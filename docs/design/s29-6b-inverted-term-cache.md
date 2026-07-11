# S29-6B 设计:倒排读路径「零共享写」——thread_local term 快照缓存(generation 失效)

> 状态:**已落地(2026-07-11)**。S29-6 一期(KeyDir SeqShardTable + epoch-RCU)
> 收官时留下的二期:BOW 查询扩展性。
> 实测(3 reps):BOW 1t 7.43→6.75µs(-9%)、4t 7.42→6.46µs(-13%,1→4 线程
> 延迟完全平坦=零争用)、16t -17%;SearchWhileIndexing(恒 miss+WAND)+2%
> 噪声边缘;SearchHybrid 持平。验收:clang 580/580、TSan 子集 95/95 +
> 并发压力 ×8 轮、ASan 580/580、build-rel 构建过。
>
> **⚠️ 修正原估(落地时实证)**:S29 基线「BOW 1→4 线程零扩展」是对 bench
> 计数器的误读——google benchmark 线程化基准的 items_per_second ≡ 1/每线程
> 每迭代 real time,**不是聚合 QPS**(固定迭代数 × 墙钟核实:4 线程跑 4×
> 迭代量墙钟不变;bench 注释已订正)。基线 4 线程聚合扩展已有 ~3.6×/4;
> 真实病灶是下述 RMW 弹跳造成的 **5-18% 查询延迟膨胀**,非灾难性串行化。
> 本设计仍成立(把膨胀清零、延迟平坦化、单线程也受益),只是收益量级从
> 「解锁扩展性」修正为「消争用膨胀 + 免快照拷贝」。
>
> 背景:每查询每词:TBB `const_accessor` find(桶锁 RMW ×2)+ shared_ptr
> 拷贝/释放(控制块 RMW ×2)≈ 32 次共享 cacheline RMW,全线程打同 16 条
> cacheline → RFO 弹跳。

## 1. 路线选择:为什么是缓存而不是换表(SeqShardTable + epoch)

TASK.md S29-6 二期列了两条路:「换表或 thread_local term→snapshot 缓存」。选**缓存**:

1. **换表只消一半 RMW**。SeqShardTable 乐观读消掉桶锁,但读者拿 posting 数据
   仍需 shared_ptr 引用计数(CoW 协议的读者在场信号)——热词控制块照样弹跳。
   要彻底零 RMW 就得废 use_count 协议换 epoch 裸指针 + PostingList 全部 5 个
   vector 换 limbo 分配器/自建 FlatArray + per-PL seqlock——多会话工程,UAF 面大。
2. **BOW 路径按定义是小查询**。`total_postings < kWandThreshold(1024)` 才走
   score_bow_topk;≥1024 走 WAND,评分微秒级计算主导,每词 4 次 RMW 占比可
   忽略,**不需要**零 RMW。⇒ 需要治的恰是「小查询高频重复」场景,正是缓存的
   甜区,且缓存条目被 1024 行上限天然封顶(≤12KB/词)。
3. **S27 段模型放大缓存收益**。查询走 [封口段集 + building]:封口段 posting
   仅 compact 时变(罕见)→ gen 几乎不变 → **永久命中**;building 段持续写
   → 恒 miss,但它小,慢路径(现路径)本来就便宜。
4. 缓存是**纯私有数据 + 一个原子 gen 的共享读**:不动 CoW 协议、无 TSan 豁免、
   无 UAF 面、可运行期一键关闭退回现路径。

不做换表的代价:WAND 大查询、phrase/near、bool 仍走桶锁——它们计算主导,
留作观测项;若未来实测成瓶颈,换表方案(设计见一期 §6.3-A)依然可叠加。

## 2. 设计

### 2.1 失效信号:per-shard generation

`InvertedIndex::Shard` 增 `alignas(64) std::atomic<uint64_t> gen_{0}`(独占
cacheline,避免与读者共享行造成伪失效)。**每个 posting 可见变更点**在完成
变更后 `fetch_add(1, release)`:

- `add_doc`(逐 term,释放 accessor 后 bump 所属 shard)
- `apply_delta`(同上)
- `compact`(压实过该 shard 任一 key → bump 一次)
- `finalize_all_postings`(blocks 重建进快照 → 每 shard bump)
- `deserialize` / `load`(入口与出口各全量 bump,覆盖失败半填状态)

`remove_doc` 不 bump:它只改全局统计(atomic,查询每次现读),不碰 posting;
删除语义由 live_checker 查询期过滤,与缓存正交。

### 2.2 缓存本体:`bitcask::bm25::TermSnapshotCache`(thread_local)

开放寻址定长槽表(256 槽 pow2,窗口内线性探测 8 步,**无 rehash 无堆表**):

```
Entry { index_id, gen, df, use_seq, has_rows, occupied, term(string), fp(FlatPostings) }
```

- key = (index_id, term)。`index_id` 来自进程级单调计数器(构造时分配,
  **永不复用** → 指向已析构索引的残留条目永不假命中,也无需析构清扫)。
- **命中判据**:id+term 相等 **且** `entry.gen == 当前 shard.gen_`(acquire load)。
  gen 不等 = 陈旧,视作 miss(slot 可被同 key upsert 刷新)。
- 三类条目:
  - 全量:`has_rows=true, fp=snapshot_flat 拷贝, df=fp.size()`(BOW 标量路径产出,
    结构上 ≤1024 行);
  - 缺席:`has_rows=true, fp 空, df=0`(term 不存在于该索引,负缓存);
  - df-only:`has_rows=false, df=n`(doc_freq 对大 term 产出,不搬行)。
- **同查询钉住**:`begin_query()` 递增线程内 query_seq;probe/upsert 把
  entry.use_seq 记为当前 seq;**淘汰跳过 use_seq==当前 seq 的槽**——防止同一
  查询后插入的 term 覆写先前命中、仍被 views 引用的条目。窗口全被钉住时
  upsert 返回 nullptr,caller 回退 tps_pool 私有槽(恒正确)。
- 内存上界:256 槽 × ≤12KB(全量条目封顶) ≈ 3MB/线程最坏,典型远小
  (热词都是小 posting)。vector 容量随覆写复用。

### 2.3 查询接线(`InvertedIndex::search`)

```
qseq = cache.begin_query()
Phase 1(零锁):逐词 gen=shard.gen_(acquire) → cache.probe(id, term, gen)
  全命中且全 has_rows:
    total==0 → 返回空(全负缓存)
    total <  kWandThreshold → 直接 score_bow_topk(缓存 fp 的视图)——零锁零拷贝
    total >= kWandThreshold → 落 Phase 2(WAND 需要 PostingList 指针)
  任一 miss / df-only → Phase 2
Phase 2(现 S29-1 路径不变):单趟 find 拿 pls → WAND 或标量。
  标量分支的 snapshot_flat 改为落进 cache.upsert 槽(带 pre-find 读的 gen);
  未命中词写缺席条目;upsert 失败(窗口钉满)回退 tps_pool。
  WAND 分支不产缓存(不做无谓大拷贝)。
```

`score_bow_topk` 的词条目从值持有 `ScoredTerm{term, fp}` 改**视图**
`ScoredTermView{const string* term, const FlatPostings* fp}`——wildcard/fuzzy
两个调用方各自套一层视图数组(行为零变化),search 命中路径零拷贝。

`doc_freq`(分段查询 stage-1 逐段逐词调用,S29-5 遗留的「同段同词两次 find」):
probe 命中(gen 相等)→ 直接返回 df;miss → 现锁路径,结果 upsert 为 df-only
条目(不覆写行:同 key 旧行已因 gen 不等失效,清 has_rows)。stage-2 search
标量分支随后把行补齐——同段同词稳态 **0 次 find**。

### 2.4 内存序论证

- 写者:posting 变更(桶写锁内)→ 释放 accessor → `gen_.fetch_add(release)`。
- 读者:`gen_.load(acquire)` 读到新值 G ⇒ 与 fetch_add(release) 构成
  synchronizes-with ⇒ G 对应的变更对后续 find 可见。缓存条目命中要求
  entry.gen == G;条目由「先读 gen=G₀,后 find/snapshot」产生,快照内容
  ≥ G₀ 状态(只可能更新,不可能更旧)⇒ **缓存声称的 gen 恒不晚于内容的
  实际版本**,过保守方向,安全(至多一次无谓刷新)。
- 读者看到旧 gen(bump 尚未传播):等价于查询串行化在该写之前——search
  对并发写本无线性化承诺;read-your-writes 由上层 drain/refresh 屏障保证
  (drain happens-before 查询 ⇒ gen 新值可见 ⇒ 缓存必失效)。
- 缓存本体 thread_local,零共享;唯一共享触点是 gen_ 的 load(纯读,
  read-only 稳态下 cacheline 保持 SHARED,线性扩展)。**无 TSan 豁免需求。**

### 2.5 回退开关

进程级 `std::atomic<bool>`(默认开):`InvertedIndex::set_query_cache_enabled`。
关闭 ⇒ probe 恒 miss、upsert no-op ⇒ 字节级回到现路径。

## 3. 正确性风险清单(测试对位)

| 风险 | 兜法 / 测试 |
|---|---|
| 漏 bump ⇒ 永久陈旧 | §2.1 完整枚举 mutable_pl/emplace 全部调用点;测试:add_doc/compact/deserialize 后查询立刻反映 |
| 同查询条目被后续插入覆写 | use_seq 钉住;缓存单元测试构造窗口冲突 |
| 跨索引串味 | index_id 单调不复用;双索引同 term 测试 |
| 负缓存挡新词 | add_doc 插新 term 也 bump shard;测试:miss→add→查询命中 |
| df-only 降级覆盖全量条目 | upsert 刷新时清 has_rows;doc_freq→search 顺序测试 |
| 命中/慢路径分数不一致 | 快照字节同源(snapshot_flat)⇒ 位级一致;测试:同查询两次结果逐位相等 |
| 并发写者 vs 读者 | gen 为原子;数据面走现锁路径;TSan:SearchWhileIndexing 型压力 + compact 并发 |

## 4. 验收

- build-rel + build-clang 双树全量 ctest;TSan 倒排/搜索/并发子集;ASan 全量。
- `BM_Inverted_QueryThroughputBOW`:聚合 QPS 1→4 线程 ≥3×(现 0.9×);
  单线程不劣化(免每查询 8 次 snapshot 拷贝,预期反而升)。
- `BM_Inverted_SearchWhileIndexing`(写并发,恒 miss):不劣化(probe 开销
  ≈ 一次 hash + 一次原子 load / 词)。
