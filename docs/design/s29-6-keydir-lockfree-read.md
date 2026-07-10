# S29-6 设计:KeyDir 读路径「零共享写」(epoch-RCU + 乐观读)

> 状态:**已评审(2026-07-10),P1+P2 已落地(558/558;ASan 137/137;
> TSan 135/135;bench 零回归),P3+P4 待实施**。评审决议见 §5。
> P2 产物:`include/bitcask/epoch_reclaim.hpp`(通用注册表,seq_cst 交错
> 论证见其文件头)+ Shard::Limbo 三池 + LimboAllocator + erase 零 free 化。
> 2026-07-10 实现前审计发现原 TASK.md sketch(「每 shard seqlock + POD 乐观
> 拷贝,fold 态回退加锁」)**不成立**——本文记录三个致命场景、修正后的设计
> 与分相计划。
>
> 背景:`BM_KeyDir_Get_MultiThreaded` 1→8 线程 CPU 37.8→63.7ns。根因非阻塞争用
> (256 分片 × 短临界区,同分片碰撞率 <1%),而是**每次 get 对分片锁字做原子 RMW**
> → 该 cacheline 上一次总被别的核写过 → 每次 acquire 一次 RFO 缓存缺失(~26ns)。
> 读者零共享写(纯 load)可让锁字 cacheline 保持 SHARED 态,读路径线性扩展。

## 1. 为什么 naive seqlock 不成立(实现前审计,2026-07-10)

seqlock 的事后校验只能兜「**读到脏数据**」,兜不住「**deref 已释放内存 → 段错误**」。
读者乐观 find 期间会触碰三类可能被并发释放/改写的内存:

### 1.1 rehash/grow UAF(致命)

`Shard::entries` 是 `ankerl::unordered_dense::map`(bucket 下标数组 + 稠密 kv
vector)。并发 insert 触发增长时 realloc 两个数组并**立即 free 旧数组**——乐观
读者正在旧数组上做桶探测/线性比较 → UAF。校验发生在 deref 之后,救不了 fault。

### 1.2 热路径 erase 释放 key string 缓冲(致命)

`KeyDir::remove` 无 fold 分支**物理 erase**(`keydir.cpp:699`):析构
`std::string` key(>SSO 的 key free 堆缓冲)+ ankerl swap-with-last 搬移。
读者 find 比较 key 时 deref 该缓冲 → UAF。merge 路径 `remove_if_older`
(`keydir.cpp:989`)同理。另:grow 搬移会对旧位置的 string 对象执行 move
(源被置空,**写**旧内存)——这是脏读(seq 可兜),但 erase 的 free 是 UAF(兜不住)。

### 1.3 variant Single→Multi 升级(可回退处理)

fold 并发启动时写者把 `Entry` variant 从 SingleEntry 升为 MultiEntry(内含
vector 指针)。读者拷出的 bytes 判别若非 Single,**不得追 revisions 指针**——
判别 == Single 时用拷出的 POD bytes(无指针,安全);否则回退加锁。此类可处理。

## 2. 修正后的设计

四件套,缺一不可(P1-P3 必须同批上线,P1 单独只加内存开销):

### 2.1 P1:热路径 erase → tombstone-in-place

`remove` 无 fold 分支不再 `erase`,改为把 entry 覆写为墓碑 SingleEntry
(`is_tombstone` 判据已存在:`entry_at_epoch`/fold sibling 墓碑同款语义,get
已处理「命中但墓碑 → nullopt」)。物理回收挪到既有 quiescent 点:
- `collapse_multi_entries_barrier`(fold release,已有 erase 逻辑,扩为同时清
  no-fold 墓碑);
- merge/save_snapshot 的 BarrierGuard 期(写者已出清);
- 新增兜底:per-shard 墓碑计数超阈值(如 ≥1/8 表长)时,由**写者**在分片锁内
  顺手 sweep(写者互斥,配合 P2 的延迟释放,对乐观读者安全)。
影响面审计:fold 迭代(需跳墓碑——sibling 墓碑已跳,补 no-fold 墓碑)、
snapshot save/load、`key_count_`(remove 已单独递减,不受影响)、
`entries.size()` 语义消费方。

### 2.2 P2:epoch 读者登记 + limbo 延迟回收

- 全局 `epoch` 计数 + 固定槽读者注册表(cacheline 对齐,thread_local 槽,
  读者进入快路径时 `slot.store(cur_epoch)`、退出 `store(0)`——**只写自线程
  cacheline**,无 ping-pong)。
- per-shard limbo 列表:待释放块打上 `epoch+1` 戳;回收条件
  `戳 < min(活跃读者槽)`。写者在分片锁内顺手推进(摊销,无独立线程)。
- ankerl map 换自定义 allocator:deallocate 不 free,推入 limbo(覆盖 1.1 的
  两个内部数组)。key string 缓冲经 P1 已不在热路径 free;quiescent 点/写者
  sweep 的 free 也走 limbo(覆盖 1.2)。

### 2.3 P3:get 乐观快路径

per-shard `seq`(仅写者 bump,odd/even):
```
读者:注册 epoch → s1=seq(偶,否则回退) → 乐观 find(桶探测+key 比较)
     → 拷 Entry bytes → s2=seq → s1==s2 且判别==Single → 用拷贝;否则重试
     ≤N 次后回退加锁。fold 态(keyfolders_/has_pending_)直接回退。
写者:分片锁内 seq+1(odd) → 变更 → seq+1(even)。覆盖所有结构性与值变更:
     insert/grow/erase(sweep)/覆写/升链。
```
key 比较的 deref 安全性:比较窗口内所有可达内存的 free 都经 limbo 延迟(P2),
脏读(grow move 置空源 string)由 seq 校验拒绝 → 重试。

### 2.4 P4:验证

- TSan 全量(尤其 get/put/remove/fold 交错的既有压力用例)+ 新增乐观读者 vs
  grow/erase/fold 升链的定向压力测试;
- `BM_KeyDir_Get_MultiThreaded` 期望 8 线程 CPU 时间 ≈ 单线程(37-40ns);
- `BM_KeyDir_Mixed_MultiThreaded` 读侧改善、写侧不退化;
- ASan 树跑 sweep/limbo 路径(UAF 检出)。

## 3. 与倒排桶锁(S29-6 二期)的关系

BOW 查询的桶锁 ping-pong(TBB `const_accessor`)是独立问题;posting map 已是
CoW(`mutable_pl`),读者理论可零锁——但 TBB 桶锁不可下探,方案是换表(F14/
自建开放寻址 + 本文同款 epoch)或 thread_local term→snapshot 缓存(generation
失效)。**建议 KeyDir 先行**(本文),验证 epoch 基建后倒排复用同一套注册表。

## 4. 工作量与风险

- P1 ~1 会话(语义审计为主);P2 ~1 会话(基建+allocator);P3+P4 ~1 会话。
- 风险:KeyDir 是全库最核心并发结构(锁全序见 keydir.hpp 头注释,46K 的
  thread-safety.md 历史)。**必须整批评审 + 三 sanitizer 全量**后合入;
  任何一相单独合入无收益且引入墓碑内存开销。
- 回退开关:乐观快路径可加编译期/运行期开关(默认开),出问题一键退回纯锁。

## 5. 评审决议(2026-07-10,用户确认)

1. **TSan 策略:选 b**——乐观快路径函数标 `__attribute__((no_sanitize("thread")))`
   + 注释论证(seqlock 业界标准做法,Linux 内核同款)。代价:该函数在 TSan 树免检,
   由 ASan + 定向压力测试兜底;**写者侧不加豁免**(写者全程持锁,TSan 照常查)。
   豁免范围必须收窄到单个快路径函数,不得扩散。
2. **路线:KeyDir 先行**(本文 P1-P4),倒排桶锁二期复用 epoch 基建。
3. **墓碑回收:增量 sweep**——per-shard 墓碑计数超阈值(1/8 表长)后,后续每次
   **写操作**在分片锁内顺手清扫 K 个槽位(K=8,均摊 O(1),无单次延迟尖刺),
   叠加既有 quiescent 点(fold release / barrier)全量清。P1 期 sweep 直接 free
   (读者仍持锁,安全);P3 上线后 sweep 的 free 一律改走 limbo。
4. **回退开关:运行期**——`std::atomic<bool>`(默认开),每 get 一次分支(可忽略),
   线上出问题免重编译一键退回纯锁路径。
