# k-way 交集 + 块级元数据 + Block-Max WAND（BM25 V2 检索加速路线）

> 对应代码：本路线三大件 —— K1 k-way 交集（`InvertedIndex::bool_search`
> 局部的 `run_must_intersect` lambda，内分 k==1 退化为 live 过滤直拷、
> k==2 走 `intersect_u64` SIMD/galloping 内核、k≥3 走 leapfrog）、B1/v5
> 块级元数据 `(max_tf, min_dl)`（`PostingBlock` 字段定义见
> `include/bitcask/inverted.hpp`，运行时维护见 `PostingList::note_appended`
> / `seal_full_blocks` / `finalize`）、disjunctive top-k 的 Block-Max WAND
> 主循环（`InvertedIndex::search_wand`）。k-way 块跳跃与 must-only 合取
> BMW 共用 `BmwCur` 游标 + `block_ub` lambda（`bool_search` 内的 B1 分支
> 局部类）。底层指针/游标接口 `advance(target)` 由 `bool_search` 的 K1
> 段定形（leapfrog 的 galloping + 二分收尾），是后续块级元数据与 BMW 的
> 共同上游。
>
> 前置阅读：`doc/wand-blockmax-zh.md`（已实现的 `search_wand` 算法逐步
> 讲解 + 变量映射表）、`doc/inoue-simd-intersection-zh.md`（SIMD 块
> 过滤及 §8 设计评审）。
>
> 本文解释三个递进的概念——k-way 交集、posting 块级元数据、
> Block-Max WAND——以及它们为什么排在 AVX-512 内核之前，以及这三件
> 在当前实现在哪里、对应哪些代码符号。**已实现的 `search_wand` 算法
> 逐步讲解 + 现有代码映射表**见 [`wand-blockmax-zh.md`](wand-blockmax-zh.md)。

## 1. 背景与动机

当前 BM25 查询路径是「完整 must 交集 → 全部评分 → 排序取 top-k」。
两个问题：

1. **pairwise 物化交集**（K1 落地前的基线）：k 个 must 词做 k-1 轮
   两两交集，每轮分配中间 vector、完整重读上一轮结果。
2. **完整交集本身就是无用功**：top-k 查询要的是分数最高的 k 篇文档，
   却把全部命中文档都求了出来、评了分。工业引擎
   （Lucene/Elasticsearch）靠 WAND / Block-Max WAND / MaxScore
   在遍历阶段就跳过注定进不了 top-k 的文档，典型只触碰百分之几的
   posting——这是任何交集内核优化都给不出的量级。

`doc/inoue-simd-intersection-zh.md` §8.2.1 还指出：Inoue 块过滤是
**区间驱动**的跳跃（块 [min,max] 区间不相交才能跳），对均匀散布的
真实 doc ID 分布基本失效。本文的块级元数据提供的是**目标值驱动**与
**分数驱动**的跳跃，不依赖值域成簇假设。

## 2. 第一步：`run_must_intersect` k-way 化

### 2.1 现状（pairwise）

```
acc = list1
acc = intersect_u64(acc, list2)   // 物化新 vector，丢掉旧 acc
acc = intersect_u64(acc, list3)   // 再物化一个
...
```

k 个词 k-1 轮；第一轮中间结果可能很大（两个热词的交集），
后续每轮反复搬运它。

### 2.2 k-way（leapfrog）

k 个游标在 k 条列表上同时推进，一遍出结果、零中间物化：

```
candidate = 各游标当前值的最大者
把每个游标 advance 到 ≥ candidate 的位置
    ├─ 所有游标的值都等于 candidate → 命中，输出，candidate 推进
    └─ 某个游标跳过了 candidate → 以它的新值为新 candidate，继续
```

核心原语是 `advance(cursor, target)`——「跳到 ≥ target 的第一个
位置」。这个接口正好接住 §3 的块级元数据：advance 跳多远取决于
元数据的粒度。

### 2.3 当前实现的三路分发

`InvertedIndex::bool_search` 内 K1 段（`run_must_intersect` lambda）
按 must 词数分发：

| k 形态 | 实现路径 | 原因 |
|---|---|---|
| **mk == 1** | 直接从最短列表拷 live 过滤后的 ords | 无交集，最简退化 |
| **mk == 2** | `intersect_u64`（SIMD pairwise：旋转内核 / AVX-512 Inoue；大悬殊走 galloping） | 两次 live 过滤拷贝 + SIMD 内核的两列表具体实现已比 leapfrog 快 |
| **mk >= 3** | leapfrog：每轮取 `curs[0]` 的当前 ord 驱动，其余 `advance(curs[j], v)` 对齐 | k-1 轮中间 vector 物化 + 多列表互相 gallop 的合并收益 |

所有路径统一消费**前置批量化**的 `tp.live`（`fill_is_live` 一次
取齐，每条 posting 一位）与**按 `PostingList::kBlockSize == 128`
粒度的懒填充** `tp.dls`（K1 段此刻仍全量预取，留作 K1+/B1+ 的腾挪
空间——见 §6 的 `ensure_block` lambda）。

改动范围：仅 `bool_search` K1 段局部，posting 存储不动。
独立收益：消除 k-1 次中间分配与搬运（大候选集上 k≥3 时收益显著）。

## 3. 第二步：posting 块级元数据（每 128 ord 存 (max_tf, min_dl)）

### 3.1 结构与维护

每条 posting list 加一层骨架数组，每 `PostingList::kBlockSize == 128`
条 posting 一个条目 `PostingBlock`（定义于 `include/bitcask/inverted.hpp`）：

```
posting:  [ord0 ... ord127][ord128 ... ord255][ord256 ...]
骨架:     {base_ord, end_ord, max_tf,         {base_ord, end_ord, max_tf, ...
            min_dl, start_idx, count}          min_dl, ...}
```

`PostingBlock` 的字段语义：

| 字段 | 类型 | 用途 |
|---|---|---|
| `base_ord` | `uint64_t` | 块第一条 posting 的 ord（块边界二分键） |
| `end_ord` | `uint64_t` | 块最后一条 posting 的 ord（同上 + 给块跳跃提供 `block_end()` 目标） |
| `max_tf` | `uint32_t` | 块内最大 `tf` —— 经典 BMW 输入 |
| `min_dl` | `uint32_t` | v5 impacts 引入：块内最小 `dl`。1 = 旧快照/dl 未知时的 admissible 回退 |
| `start_idx` / `count` | `size_t` | 行下标区间（SoA 下标至 `ords[]` / `tfs[]` / `dls[]`） |

**三阶段维护**（构造期由块大小决定走哪条）：

1. **增量封块**（`PostingList::seal_full_blocks`）：add_doc 追加后
   `note_appended()` 调用，把已攒满 `kBlockSize` 的整块封进 `blocks[]`。
   ord 严格单调 → 末尾追加 O(1) 摊还。
2. **最终化**（`PostingList::finalize`）：可能含不满的尾块时，先 clear
   再重建为规范集（含部分尾块）。
3. **死点压实**（`PostingList::compact_flags`）：S22 后改 SoA 原地
   双指针压实，`max_tf` 重算（不再用 `note_appended` 增量维护的值
   —— 一致性优先），然后 `seal_full_blocks`。

### 3.2 `max_tf` 单字段 —— 目标值驱动的跳跃（max_tf）

`advance(cursor, target)` 先在骨架上扫：`max_tf`-独立的「块 [base_ord,
end_ord] 与 target 不交」判据可直接适配 K1 的 `advance`，但当前
`run_must_intersect` 的 leapfrog 走的是 ord 列二分（`std::lower_bound`）
+ galloping，不依赖 `max_tf` 本身做跳跃——`max_tf` 在 K1 段未参与
跳跃，真正起作用要到 §4 的 BMW 块上界。

不过 **`end_ord` 在 B1 段的合取 BMW 中**充当 `block_end(curs)` 的目
标——见 §6.2 的 `advance(curs[0], next + 1)`，其中 `next = min over
terms of block_end(curs[m])`，由 `blocks[m].end_ord` 给出。

### 3.3 `min_dl` 单字段 —— 块级分数上界（v5 impacts）

BM25 tf 归一化项（`bm25_kernels.hpp::bm25_score_scalar`）的分母
含 `b * dl / avgdl`，`dl` 单调 ⇒ 用块内最小 `dl` 作为下界，**配合
`max_tf` 算出块级 BM25 分数上界**：

```
ub_tf_norm = max_tf * (k1+1) / (max_tf + k1*(1 - b) + b * min_dl / avgdl)
block_ub   = idf * (ub_tf_norm + delta)
```

这就是 `bm25/inverted.cpp` 内 `upper_bound_from(max_tf, idf, params,
avgdl, min_dl)` 闭包（`search_wand` / `search_wand` 类 B1 路径 / B1
合取 BMW 共享），由它驱动 §4 的块跳跃判据与 §6.2 的 `block_ub` lambda。

**与 Lucene `impacts` 的关系**：Lucene 的 `impact` 在块级预存「最大
贡献分 + 所需 idf 范围」三元组（分位下界），是单对「max_tf + min_dl」
的上界 + 区间对偶。本实现选「单对简化」原因：块的最高贡献分依赖
idf/avgdl/params 等查询期统计，预存会随统计漂移失去 admissibility；
`(max_tf, min_dl)` 与统计无关，查询期用当前统计算上界，天然 admissible。

### 3.4 v5 Admissibility 不变量（已写入 LiveChecker 文档）

查询期 `doc_len(ord)` 必须等于 add_doc 时记录的 `Σtf`（SearchLayer
同源天然成立）——若查询期 dl 更小，上界不再 admissible。实测：
BoolMustHot 100k 1024 词下 `min_dl` 实际值的范围与 `Σtf` 完全匹配
（无须额外代码路径保证）。

### 3.5 落地值

| 基准 | K1 后 | B1+min_dl | 累计 vs K1 前 |
|---|---|---|---|
| BoolMustHot/4096 | 49.1μs | **7.63μs** | 5.8× |
| BoolMustHot/100k | 1431μs | **324μs** | 4.6× |
| BoolMustSkewed/100k | 1451μs | **603μs** | -58% |
| BoolMustHot3/100k | 695μs | 674μs | -33% |

剪枝如预期触发：均匀形态 `min_dl == 实际 dl` ⇒ `block_ub == θ`
⇒ 堆满后整块跳过。

## 4. 第三步：WAND → Block-Max WAND（BMW）

> 本节是设计动机层面的介绍。**已实现的 `search_wand` 算法逐步讲解 +
> 变量映射表**见 [`wand-blockmax-zh.md`](wand-blockmax-zh.md)。

### 4.1 WAND（Broder et al., CIKM 2003）

top-k 检索维护当前堆门槛 θ（第 k 名的分数）。每个词预存
**全局分数上界 `list_upper_bound`**（它对任何文档的最大可能贡献）。遍历时，
若某文档能拿到的上界之和 < θ，该文档不评分、不访问，
游标批量跳过。θ 随堆变满不断抬高，越跳越狠。

**弱点**：全局上界太松。一个词在某篇文档 tf=50，全局上界就被
这一个离群值撑大，整条 100K 列表都显得「有希望」，跳不动。

### 4.2 Block-Max WAND（Ding & Suel, SIGIR 2011）

上界不用全局的，用**当前块 max_tf + min_dl 算出的局部上界**：

```
查询: "数据库" AND "优化"，θ = 8.5（当前第 10 名分数）
游标走到 ord ≈ 50000 附近：
  "数据库" 所在块 max_tf=2, min_dl=80 → 块内上界 3.1
  "优化"   所在块 max_tf=1, min_dl=80 → 块内上界 2.8
  3.1 + 2.8 = 5.9 < 8.5
  → 这 128 篇文档无论如何进不了 top-10
  → 两个游标直接跳到 min(块 end_ord) + 1，一篇都不评分
```

**这就是对称热词场景的解法**：两个 100K 热词在值域上没有任何跳跃
空间，但在分数上有——绝大多数块的局部上界够不到 θ，整块被跳过。
工业实测 top-k 查询典型只触碰百分之几的 posting。

### 4.3 MaxScore（同思路的替代算法）

按上界把词分为 essential / non-essential 两组：non-essential 列表
不参与游标推进，只在候选文档上做点查。实现比 BMW 简单，
工业上两者都在用（Lucene 8+ 默认 BMW 变体）。选型可后置，
两者依赖的块级元数据相同。

## 5. 实施顺序与依赖关系

```
k-way 化（§2）            → 提供 advance(target) 接口
    ↓
块级元数据（§3）          → 让 advance 跳得动（end_ord）
    ↓                       + 提供分数上界（max_tf + min_dl）
BMW / MaxScore（§4）      → 按分数整块跳过，top-k 只触碰少量 posting
```

三步每步独立有收益（去物化 / 大粒度 skip / top-k 跳过），
且互为前提，投入不浪费。

**与 AVX-512 内核的优先级关系**（结论同
`doc/inoue-simd-intersection-zh.md` §8.3）：AVX-512 优化的是
「块内精确匹配」这最后一小段；BMW 落地后绝大多数块根本不进入
精确匹配——先做内核就是给一条 BMW 准备绕开的路铺豪华路面。
顺序应为：k-way → 块元数据 → BMW/MaxScore，AVX-512 等部署
微架构确定（含 VP2INTERSECT 评估）后再议。

**与块压缩的协同**（见 inoue 文档 §8.5）：块级元数据的 128-ord
分块与 FOR/PFor 块压缩天然同构——一份分块投入同时解决
skip 地基与 u64 flat posting 的带宽问题。（**当前落地**：v6 落盘
格式 `bm25/inverted.cpp` `kInvVersion` —— ord 列 FOR 128/块，
`TFs`/`dls` 列 VByte varint 整组编码；`min_dl` 上界逻辑不变，旧
快照 v1..v5 不支持载入。）

## 6. B1 / v5 实施设计（must-only 合取 BMW）

### 6.1 适用门与核心决策

**适用门**：`should_terms` 与 `must_not_terms` 均空、`must_terms` 非空、
k > 0 → 走 `bool_search` 的 B1/v5 合取 BMW 分支（第一段 lambda 内联
在 `bool_search` 函数体里，未抽到成员）；否则回退原路径（eager fill
+ 完整交集 + 全量评分）。

**核心决策**：

1. **idf 改基于 df（=列表长）而非 live_df**：live_df 需要 O(n) 全列表
   live 扫描，与 BMW 的亚线性目标矛盾。df 含未压实的死 posting，与
   Lucene docFreq 语义一致（删除近实时近似，merge 后收敛）。**无删
   除时 live_df == df，与原路径分数位级一致**（等价性测试基础）。
   极端删除比下 idf 可为负——上界仍 admissible（`ub < θ ⇒ 真实分 < θ`），
   剪枝正确性不破，记录为良性边角。
2. **live/doc_len 懒取**：`ensure_block` lambda 按 `kBlockSize == 128`
   粒度，首次触达才 `fill_is_live` + `fill_doc_lens`（每块一次虚调用
   + 一次锁）。未触达的块零成本——这是 10× 类收益的来源（原路径
   eager 填全列表）。**对比**：`search_wand` 的 `ensure_dls` 走更
   细的「单块位 `dls_filled`」缓存（per-pivot 比 per-term 缓存细），
   且小列表（≤ 32 块 ≈ 4K posting）直接全量预取——
   实测 4K 档位 per-pivot 分支 + 位图反而 +13%~28%。
3. **剪枝**：leapfrog 对齐出候选 v 后，若堆已满：
   `Σ_t block_ub(curs[t]) ≤ θ` → 各游标 `advance(curs[0], min(block_end) + 1)`
   整段跳过（不评分、不查 live）。
4. **尾块**（未 seal，无块元数据）：上界退化用列表级 `max_tf` +
   `min_dl = 1`，admissible。
5. **评分公式与原路径逐运算一致**（分数位级不变约定）；跨词累加顺序
   为列表长升序（与原路径的词名序不同，k≥3 时浮点结合性可差最后
   1 ulp——测试用 `FLOAT_EQ` + 集合断言）。

### 6.2 BMW 段落当前实现的算法步骤映射

下表是 `bool_search` 内 B1/v5 段（局部 `BmwCur` + 三个 lambda）的
代码符号与论文算法的「**一一对应**」：

| 论文算法步骤（Ding & Suel 2011） | 当前代码符号（`bm25/inverted.cpp` `bool_search`） |
|---|---|
| 块级 `(max_tf, min_dl)` 元数据（v5 impacts 简化对） | `PostingBlock{max_tf, min_dl}`（`include/bitcask/inverted.hpp`）+ `note_appended` / `seal_full_blocks` / `finalize` 三阶段维护 |
| 计算块分数上界 | `upper_bound_from(max_tf, idf, params, avgdl, min_dl)`（`inverted.cpp` 匿名命名空间） + `block_ub(BmwCur&)` lambda（块粒度缓存 + `ub_done` 位图） |
| K1 k-way leapfrog 列出候选 v | `while (!exhausted && curs[0].i < curs[0].tp->fp.size())` 主循环：`curs[0]` 为最短列表驱动，j 从 1..nterms 逐游标 `advance(curs[j], v)` 对齐 |
| `advance(target)` 原语 | `advance(BmwCur& c, std::uint64_t target)` lambda —— `ords` 列裸指针 + galloping（指数探查 + `std::lower_bound` 收尾） |
| 各块 ub 求和 | `for (auto& c : curs) ub += block_ub(c)` 在「堆满」之后做 |
| 块跳跃判据：`Σ ub ≤ θ` | `if (ub <= heap.top().first)` —— 注意 `<=` 而非绝对 epsilon 容差（idf 极小时分数量级 ~1e-4，任何 `+ 1e-6` 都成巨大相对误差） |
| 块终点 | `block_end(BmwCur& c)` lambda = `b < fp.blocks.size() ? fp.blocks[b].end_ord : fp.ords.back()`（尾块退化） |
| 跳过目标 = `min(block_end) + 1` | `next = std::min({block_end(curs[0]), ..., block_end(curs[nterms-1])})` 后 `advance(curs[0], next + 1)` |
| 候选 v 评分 | live 过滤 → 公式与原路径逐运算一致：`c.tp->fp.tfs[c.i]` + `c.tp->dls[c.i]` + `c.idf * (tf_norm + params.delta)` 累加 |
| 维护 top-k 小顶堆与 θ | `std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap` —— 仅当 `score > heap.top().first` 才替换（注意严格优于语义，平分保留旧条） |
| idf 计算 | `c.idf = log(1 + (N - df + 0.5)/(df + 0.5))` —— 此处 df = list 长，非 live_df（§6.1 决策 1） |
| live/doc_len 懒取 | `ensure_block(BmwCur& c)` lambda —— `c.block_filled[b]` 标志位 + `fill_is_live` / `fill_doc_lens` 各一次虚调用 |

## 7. v6 收尾：FOR 块压缩 + VByte 与 SoA 内存镜像

S22 批次（commit `bf4da8c` —— PostingList AoS→SoA + inverted_wal
退役）后，`PostingList` 的内存布局与 v6 落盘格式天然对齐：

- 落盘 v6 格式本就列式（`save`/`deserialize` 写 ord 列、tf 列、dl
  列分开编码后 VByte 整组）；
- 内存 SoA `ords[]` / `tfs[]` / `dls[]` / `pos_data[]` + `pos_off[]`
  是其天然镜像——**零额外格式迁移**；
- `block_upper_bound` 不再每次重扫全表：`PostingList::max_tf` 字段
  在 `note_appended` 增量维护 / `compact_flags` 重算后，直接读即可；
- `find` / `has` 退化为对 `ords` 二分；
- `snapshot_flat` 退化为 `assign = memcpy`。

这意味着 §3 描述的「块级元数据」与 §6.2 的 BMW 表**全文对当前
代码有效**——符号未变动，符号背后的数据布局变了但观感一致。

## 8. 参考

- Broder et al.: «Efficient Query Evaluation using a Two-Level
  Retrieval Process», CIKM 2003（WAND）。
- Ding & Suel: «Faster Top-k Document Retrieval Using Block-Max
  Indexes», SIGIR 2011（Block-Max WAND）。
- Turtle & Flood: «Query Evaluation: Strategies and Optimizations»,
  IPM 1995（MaxScore）。
- Lucene `WANDScorer` / `MaxScoreBulkScorer`——工业实现参考；Lucene
  `impacts` 结构（块级「最高贡献分 + idf 区间」）的简化对——本实现的
  `(max_tf, min_dl)`。
- Inoue et al.: 块过滤 SIMD 交集——在 `intersect_u64` 内的 AVX-512
  Inoue kernel 实现（详细见 `doc/inoue-simd-intersection-zh.md`）。
