# OKI-only keydir 架构改造深度分析

> 状态：分析 / 提案（未实施）
> 关联文档：`doc/keydir-disk-resident-design-zh.md`（S36，Level B）、`doc/ordered-key-index-design-zh.md`（Level A）、`doc/api-cpp.md §11.3`
> 背景：讨论"OKI 是否能替代 keydir"时提出的 8 项挑战，本文逐条深度分析并给出务实路线图。

## 0. 待分析的 8 项挑战

1. OKI 从派生缓存升级为可信主索引或至少强校验主索引；
2. 写入事务 / memdelta / flush 的崩溃一致性；
3. 无哈希缓存时的热路径性能；
4. ord 分配、统计、MVCC、iterator 的迁移；
5. recovery 不再依赖全量内存 keydir 重建；
6. merge 的活性判断完全走 OKI；
7. 快照格式与 checkpoint 协议重设计；
8. 缓存淘汰、冷查延迟、写放大重新调优。

## 总判断：一半是"已经解决了的"，一半才是真正的新工程

首先要纠正一个容易误导的框架：**"OKI-only" 不等于 "OKI 变成唯一真相"**。这个系统的真相永远是 data 文件 + ord 全序，OKI 无论怎么升级都应该是**可重建的派生物**。所以 "OKI-only keydir" 的准确定义是：

> OKI 成为**唯一的查找权威**（lookup authority），内存哈希表退化为可选热缓存甚至完全消失；但 data 文件仍是唯一持久真相，OKI 仍可从头重建。

用这个定义去审视 8 点，会发现 Level B 已经解决了其中约 60% 的问题，真正剩下的是 4 块硬骨头：**流式 recovery、无哈希写路径、merge 的 sort-merge join、快照协议收敛**。

## 1. OKI 升级为"可信主索引"——问题被高估了

**现状**：Level B 已经做到了"组合视图是权威"。`src/keydir/keydir.cpp:456-471` 的 get 路径：哈希 miss → `oki_->locate(key)`；merge 也已从 `keydir_.get` 切到 `keydir_.locate`（S36-5）。也就是说 **"OKI 说了算"这件事在 Level B 已经成立**，只是外面还包着一层哈希缓存。

**真正要做的**不是"让 OKI 可信"（watermark + trusted manifest + wm 覆盖检查已经提供了强校验：`snap_loaded && oki.wm() < snap_next_ord → rebuild`），而是**去掉双权威带来的组合逻辑**：

- 现在每个读路径都是"哈希命中走 epoch/seqlock 协议，miss 走 OKI locate，两边结果要按 ord 做 LWW 合并"——这是 `keydir.cpp` 里大量 `if (point_query_enabled())` 分支的来源（15+ 处）。OKI-only 后这些分支全部塌缩成单路径。
- **风险点**：可信的前提是 "OKI 的 keyset ⊇ 活跃 key 集合" 这个不变量在任何崩溃点都成立。目前靠 "memdelta 先于 data 可见性提交 + flush 水位推进" 保证。去掉哈希后，这个不变量从"性能问题"升级为"正确性问题"——哈希在时 OKI 缺行只是慢，哈希没了 OKI 缺行就是**丢 key**。所以需要的不是新的信任机制，而是把现有不变量的测试从"性能回归"升级为"崩溃注入必测项"（每个 flush/unlink 边界 kill -9）。

**结论**：难度中等，主要是删代码 + 补崩溃测试，不是新发明。

## 2. 写入事务 / memdelta / flush 崩溃一致性——基本已有，需补一个缺口

**现状的提交序列**（Level B）：

```
data append (含 ord) → keydir 内存更新 + OKI memdelta append → 返回用户
                                    ↓ 异步/阈值触发
                    keydir snapshot (BCKS) → OKI flush (runs + manifest BCOM)
```

崩溃恢复靠 hint/data 尾部 fold 补齐水位之后的部分。**这套协议不依赖哈希表的存在**——fold 的输入是 hint v5（已含 ord+loc），输出目标是 OKI memdelta。所以 OKI-only 后协议骨架不变。

**真正的缺口有两个**：

1. **flush 与 memdelta 的原子切换**。现在 `flush_mu_` 串行化了 flush/load/rebuild，但 "run 落盘 + manifest 更新 + memdelta 截断" 三步之间崩溃，靠 manifest 版本号（BCOM v1/v2/v3）+ 水位单调来兜底。哈希在时，兜底失败的后果是缓存冷启动；哈希没了，兜底失败 = 打开失败或静默丢 key。需要给 manifest 提交加**双缓冲 + CRC 校验 + 明确的 last-good 回退**（类似 `docmap.ckpt.prev` 的做法），并且回退后必须强制尾部 fold 而不是信任旧水位。
2. **memdelta 的内存上界即丢数据窗口**。`kFlushByteLimit=64MiB`（约 140 万行 @45B）。哈希在时这 64MiB 丢了可以从哈希快照+fold 补；OKI-only 时它就是 recovery 必须重放的最大尾部量——这没问题（data 是真相），但意味着 **flush 频率直接决定打开时长**，64MiB 的阈值可能要按"目标恢复时间"反推调小。

**结论**：协议不用重设计，但 manifest 提交的容错等级要提高一档。

## 3. 无哈希缓存的热路径性能——最容易被低估，而且不只是读

**读侧**：热 get 从 seqlock 哈希的 ~100ns 级掉到 OKI locate 的 memdelta 二分 + bloom + block cache（tmpfs 实测 p50 4-5µs，SSD 冷查预算 ≤300µs）。**50-3000 倍的差距**。

**但真正的隐藏成本在写侧**。`src/keydir/keydir.cpp:989-990`：

```cpp
if (!ctx.found && oki_->point_query_enabled()) {
    const auto cold = oki_->locate(key);
```

Level B 下只有**哈希 miss 的 put**才付这次冷查（判断 logical_new vs overwrite 的 fstats 记账 + LWW/复活门控 `cold.ord > ord → kAlreadyExists`）。OKI-only 且零缓存时，**每一次 put 都变成冷查**——因为需要知道这个 key 之前是否存在、旧 ord 是多少。写入吞吐直接被 OKI 点查延迟封顶，而 Bitcask 的核心卖点恰恰是"写 = 一次顺序 append"。

三个出路：

- **a) 保留小热缓存**——那就退化成 Level B 小 budget，说明"纯 OKI-only 零缓存"在性能上被 Level B 严格支配（strictly dominated）。这是反对"彻底去掉哈希"的最强论据。
- **b) 写路径免存在性检查**：把 fstats 的 logical_new 记账改成**延迟/近似**（compaction 时精确重算），LWW 门控推迟到读时按 ord 裁决（LSM 的做法：写不管旧值，读时归并）。可行，但改变了 `kAlreadyExists` 的语义（CAS/条件写会受影响），且 fstats 从实时精确变成最终一致——这是 API 语义变更，不是纯内部优化。
- **c) memdelta 兼作写缓存**：新写 key 必然在 memdelta 里，重复写热 key 时 locate 命中内存二分，不碰盘。对 Zipfian 写分布这能救回大半，但对均匀写分布无效。

**结论**：8 点里唯一可能"无解只能取舍"的点。零缓存 OKI-only 的合理场景只剩"写少读少、内存极度受限"，而这个场景 Level B 小缓存同样覆盖。

## 4. ord 分配、统计、MVCC、iterator 迁移——杂活多，但有一个真问题

- **ord 分配**（`next_ord_` 原子量）：与哈希无关，只需持久化在缩小的元数据快照里（见第 7 点）。**简单**。
- **fstats**（key_count/key_bytes/biggest_file_id）：依赖"这次写是新增还是覆盖"的判断，与第 3 点写路径耦合。若走 3b 路线就变成最终一致；若保留缓存就照旧。**中等，跟 3 绑定**。
- **MVCC/epoch**：关键事实——**epoch 是会话内的，从不持久化**（设计 D2 明确拒绝为 OKI 引入 epoch，改用 (ord, run-gen) 格）。epoch/limbo 延迟回收机制服务的是**内存哈希槽位的并发安全**，哈希没了它大部分可以整体删除，读者并发安全改由 OKI 的 run 不可变性 + block cache 引用计数承担（LSM 的 MemTable/SST 生命周期管理）。**这是简化而非迁移**，但要把 `barrier_mu_ → gate_mu_ → meta_mu_ → shard` 的锁序协议整个换成 OKI 侧的快照获取协议，涉及所有读者路径。**工作量大但方向是删繁就简**。
- **iterator**：现在是 sibling chain + pending_ hash + epoch-limbo 的组合（`keydir.cpp` ColdIter ~1476 起已有哈希+OKI 归并的雏形）。OKI-only 后变成标准的 **memdelta 有序视图 ⋈ runs 多路归并**——`src/fileops/oki_state.cpp:576-614` 的 `sorted_view_` 缓存机制已经存在。真正的语义问题：**迭代器快照的一致性级别**。哈希 epoch 提供的是"迭代期间槽位不被回收"，OKI 侧要提供"迭代期间 runs 不被 compaction 删除"（引用计数 pin 住 run 文件）。现有 compaction 已有类似 pin 机制，扩展即可。**中等**。

**结论**：没有不可逾越的障碍，MVCC 部分甚至是净简化，但改动面广、回归测试成本高。

## 5. recovery 不再依赖全量内存 keydir 重建——最有价值、也最现成的一块

**现状的讽刺之处**：`src/cask/cask_recovery.cpp:682-700`：

```cpp
std::vector<oki::OkiState::DeltaRow> rows;
rows.reserve(keydir_->info().key_count);   // ← 全量内存峰值！
// 遍历内存 keydir → push_back → oki.rebuild(...)
```

而 `src/fileops/oki_state.cpp:438` 的 rebuild **内部已经是外排**（S36-1：64MiB 分段 spill + k 路归并）。也就是说**外排能力已经造好了，但喂给它的数据源却是全量内存哈希**——Level B 号称 100M keys 1.14GB 加载，这个峰值里哈希重建本身就占大头。

**OKI-only 的 recovery 应该是**：

```
读 OKI manifest（last-good）→ 加载 runs（mmap/稀疏索引，O(1) 内存）
→ 从 oki.wm() 对应的 data/hint 位置起，流式 fold 尾部
→ 尾部行直接进 memdelta（有 64MiB 上界，超了就 flush 成 run）
→ 完成。全程 RSS = block cache + memdelta + 常数
```

hint v5 已含 ord+loc，fold 不需要读 data 正文；最坏情况（manifest 全损）才需要全量扫 hint → 外排 → rebuild，而这条路的机器（外排）已经存在，只差把"遍历内存 keydir"换成"流式扫 hint 文件"。

**收益量化**：100M keys 场景，打开内存峰值从 ~1.14GB 降到 ~300MB（256MiB block cache + 64MiB memdelta），且**打开时间从 O(全量 key 数) 降到 O(尾部未 flush 量)**——正常关闭后重开几乎瞬时。这才是 OKI-only 相对 Level B 的**决定性优势**，比省那 1GB 哈希内存重要得多。

**结论**：8 点里性价比最高的一块，而且**不需要 OKI-only 也可以先做**——Level B 下把 rebuild 数据源改成流式扫 hint，就能提前兑现大部分收益。强烈建议把这一步单独拆出来先行。

## 6. merge 活性判断完全走 OKI——判断已就位，缺的是访问模式

**现状**：S36-5 已经把 merge 活性判断切到 `keydir_.locate`（组合视图），正确性问题（被驱逐的活 key 误判为死 → unlink 丢数据）**已经解决**。relocation 也已经是 "OKI memdelta 先 append 重定位行，再 unlink 输入文件"。

所以 OKI-only 后 merge 的**语义**不用动，动的是**性能形态**：现在是对输入文件每条记录做一次 `locate`（点查 ×N，随机访问 OKI runs）。100M keys、merge 一个 2GB 段就是几千万次点查。

**正确的形态是 sort-merge join**：merge 输入记录按 key 排序（或直接用 hint 扫描 + 外排），OKI runs 天然按 key 有序 → **两边各一个顺序游标做归并连接**，随机 IO 归零，复杂度 O(N+M) 顺序读。这正是 LSM compaction 的标准做法，也是"这更像 LSM/WiscKey"这个判断的具体体现之一。

**注意点**：join 期间 OKI 还在接受新写入（memdelta 在动），需要 pin 一个一致性视图（runs 集合 + memdelta 快照，`sorted_view_` 版本号机制可复用）；join 结果里 ord 比重定位前更新的记录要以 (ord, run-gen) 格裁决——这个格的机制 D2 已经设计好了，直接沿用。

**结论**：中等难度，模式改造而非协议改造，收益是大库 merge 吞吐数量级提升。

## 7. 快照格式与 checkpoint 协议重设计——本质是"提交点搬家"

**现状**：两套持久化状态、两个提交点：

```
kv.keydir.ckpt (BCKS v3 全量 / v4 子集)  ← keydir 的提交点
kv.oki.manifest (BCOM v3) + runs         ← OKI 的提交点
顺序约束：keydir snapshot 先，OKI flush 后（cask.cpp:937）
不变量：keydir_covered ≤ search_covered
```

Level B 的 v4 子集快照是个过渡态补丁：只存缓存里那部分，冷 key 靠 trusted manifest + wm 覆盖检查兜底。**OKI-only 后这个补丁可以转正为终态**：

- **OKI manifest 成为 keydir 的唯一提交点**。`next_ord_`、key_count、fstats、biggest_file_id 这些全局标量作为 manifest 的元数据段一起原子提交（它们才几百字节）。
- `kv.keydir.ckpt` **整体退役**（或兼容期只写元数据桩）。BCKS v3/v4 的解析代码保留用于降级/迁移。
- 与搜索插件的 checkpoint 协议**不用改**：`keydir_covered ≤ search_covered` 的不变量里 keydir_covered 改为取 OKI wm，语义等价——docmap/bm25/vec 的水位机制本来就独立。
- **迁移路径**：BCOM 加 v4（内嵌元数据段），打开时 v3+v4 双读、写只写 v4，两个版本周期后删 v3。stamp_mode 的升降级逻辑（downgrade 清 stamp）要相应扩展：**从 OKI-only 降回哈希模式必须触发全量 hint 扫描重建哈希**，这个降级成本要写进文档。

**结论**：难度中低，主要是协议版本管理的细活。真正的收益是**消灭双提交点之间的崩溃窗口分析**——现在每次改 checkpoint 顺序都要论证两个提交点交错崩溃的 6 种组合，收敛成一个后这类论证消失。

## 8. 缓存淘汰、冷查延迟、写放大重新调优——运营性问题，但有一笔账要算清

- **淘汰策略**：现有哈希缓存是条目级 LRU；OKI 侧是 256MiB block cache（块级 LRU）+ bloom。OKI-only 后调优对象变成：block 大小（现在的设计对点查友好，对 scan 会造成读放大）、bloom 假阳率（每多 1% FPR ≈ 每 100 次冷查多 1 次无效盘 IO）、memdelta flush 阈值（第 2 点，与恢复时间挂钩）。这些都是 LSM 世界的成熟问题，有现成方法论。
- **写放大的账**（100M keys 估算）：
  - 每次 put：data 记录 + memdelta ~45B → 可忽略；
  - memdelta → run：64MiB 一批，顺序写，~1×；
  - **run compaction 是大头**：`kCompactRunLimit=8`，leveled 式归并下稳态写放大约 4-8×（45B/行 × 100M ≈ 4.5GB 的 OKI 总量，每次全量 compaction 重写 4.5GB）。对比：Bitcask 本体的 data merge 写放大已经存在，OKI 的 4.5GB 相对 data 体量（假设 value 平均 1KB = 100GB）**只占 ~4%**。
  - **结论：写放大不是 OKI-only 的否决项**，因为 OKI 行只有 45B，比 WiscKey 的 key+指针还小。真正要盯的是 compaction 的**空间放大**（归并期间新旧 runs 并存，峰值 2×4.5GB）和 IO 带宽抢占（要限速，避免影响前台 append 的 fsync 延迟）。
- **冷查延迟 SLA**：SSD ≤300µs 的预算意味着 bloom 挡掉绝大多数不存在的 key、存在的 key 平均 1 次 block 读。NVMe 上没问题；**HDD 上 OKI-only 基本不可用**（10ms 寻道 × 每次冷查），这应该写成明确的部署约束。

## 综合裁决与路线图

**把 8 点按"真实工作量 × 收益"重排**：

| 点 | 实际状态 | 难度 | 独立收益 |
|---|---|---|---|
| 5 流式 recovery | 外排已就绪，只差换数据源 | ★★ | **最高**（打开内存/时间数量级下降）|
| 7 提交点收敛 | v4 子集快照已是半成品 | ★★ | 高（消灭双提交点崩溃分析）|
| 6 merge join | 语义已就位，改访问模式 | ★★★ | 高（大库 merge 吞吐）|
| 1 单权威化 | Level B 已 90% 到位 | ★★ | 中（删 15+ 处双路径分支）|
| 2 崩溃一致性 | 协议已有，补 manifest 容错 | ★★ | 中（正确性加固）|
| 4 杂项迁移 | MVCC 部分净简化 | ★★★ | 低（跟随性工作）|
| 3 写路径冷查 | **无完美解，只能取舍** | ★★★★ | 负（性能倒退）|
| 8 调优 | LSM 成熟方法论 | ★★ | 运营性 |

**关键洞察**：收益最高的 5、7、6 三项**都不要求彻底去掉哈希**。所以务实路线是：

1. **第一步（Level B 增强，不改架构）**：recovery 数据源从"遍历内存 keydir"改为"流式扫 hint → 现有外排"；manifest 提交点内嵌全局元数据。→ 兑现第 5、7 点的绝大部分收益，Level B 从"省内存的缓存模式"升级为"可独立恢复的主索引"。
2. **第二步**：merge 改 sort-merge join；此时 OKI 已是事实上的主索引，哈希纯粹是加速器。
3. **第三步（可选终态）**：提供 `keydir_cache_entries = 0` 的"零缓存模式"给极端内存受限场景，接受第 3 点的写路径代价（用 3c 的 memdelta 兜底）。**不建议把零缓存设为默认**——一个小哈希缓存（比如 10 万条 ~8MB）就能让 90% 的热负载避开冷查，纯 OKI-only 被它严格支配。

**场景适配**：16GB 内存 / 10 万条目的客户端场景与此改造无关（keydir 仅 ~10MB）。本分析的价值在于：若产品未来要支撑 1 亿级条目，**不需要推倒重来走 LSM**——现有 Level B + 第一、二步，就能在保留 Bitcask 写模型的前提下拿到 LSM 级别的内存曲线和恢复速度。"更像嵌入式 LSM/WiscKey"是演化的终点描述，而不是必须一次性支付的架构税。

## 附：必须保持的不变量清单

- data 文件的 ord 顺序 == recovery fold 顺序；
- LWW 由 ord 全序裁决；merge 同 ord 冲突用 (ord, run-gen) 格；
- OKI keyset ⊇ 活跃 key 集合（任何崩溃点）；
- `oki_hooks_in_flight_` bracket 保护 flush/rebuild 与写入的交错；
- fstats 逻辑计数（logical_new vs overwrite）语义；
- 搜索插件水位独立，`keydir_covered ≤ search_covered`。

---

## 附录 A：OKI 与 keydir 写盘时的冲突分析

> 代码锚点：`src/cask/cask.cpp:878-1012`、`src/fileops/oki_state.cpp`（flush/locate）、`src/keydir/keydir.cpp:2198`（save_snapshot）、`src/cask/cask_recovery.cpp:660-703`。

**结论：文件层面没有冲突，真正的冲突发生在三个层面——运行时锁/IO 争用、水位/覆盖序的逻辑约束、崩溃窗口。**

### A.0 文件层面：天然无冲突

两者写完全不同的文件，各有各的原子提交点：

| | keydir | OKI |
|---|---|---|
| 文件 | `kv.keydir.ckpt`（BCKS v3/v4） | `kv.oki.seg-<gen>` runs + `kv.oki.manifest`（BCOM） |
| 提交点 | 快照文件整体替换 | manifest 写入（唯一 commit point，`oki_state.cpp:255`） |

不存在互相覆盖、写同一文件的问题。

### A.1 运行时并发冲突（锁与资源争用）

**A.1.1 两个 flush 触发源抢 `flush_mu_`**

- **写者线程阈值 flush**：`maybe_flush_oki()`（`cask.cpp:988`），memdelta 超 1M 行/64MiB 时**持有 `write_mu_` 内联执行**；
- **checkpoint 侧搭车 flush**：`write_keydir_snapshot()`（`cask.cpp:943`），reducer 线程执行。

两者由 `OkiState::flush_mu_` 互斥——正确性无冲突，但延迟有冲突：checkpoint flush 正在做全量排序 + 写 run + fsync（可能还触发 `compact_all_locked` 全归并）时，写者线程的阈值 flush 卡在 `flush_mu_` 上，而它**手里还攥着 `write_mu_`** → 全体写者被阻塞。最实际的冲突：**一次大 flush/compaction 造成前台写停顿（write stall）**。

**A.1.2 keydir 快照要"写者静止"，OKI flush 不要**

`save_snapshot`（`keydir.cpp:2198`）走 `BarrierGuard` 写者屏障 + `keyfolders_==0` 检查——快照期间写者被挡在屏障外，活跃 fold 时直接放弃快照。OKI flush 不需要静止：S36-2 用"拷贝前缀"而非 swap-out（`oki_state.cpp:172`），IO 期间新 append 照常进 `delta_`，提交后只删已固化前缀。checkpoint 站点时间线：

```
[屏障：写者停顿] keydir 快照 → [屏障释放：写者恢复] OKI flush（与写者并发）
```

残余冲突在 `mu_` 的短临界区：flush 的"拷前缀"和"删前缀+重建 delta_idx_"两段持 `mu_`，与每次 put 的 `append` 互斥。前缀大时（64MiB）是毫秒级 memcpy + 索引重建，写者有毛刺但有界。

**A.1.3 读者基本不被卷入（设计已规避）**

`locate`（`oki_state.cpp:506`）**不取 `flush_mu_`**：memdelta 走 `mu_` 短查，runs 走 `runs_snap_` 原子快照 + 不可变 reader。flush/compaction 期间冷查不被阻塞，旧 run 由 shared_ptr 引用计数保活。锁序纪律 `flush_mu_ → mu_ → view_mu_`，与 keydir 侧 `barrier → gate → meta → shard` 分属两套，交叉点由 `oki_hooks_in_flight_` bracket 保护。

**A.1.4 IO 带宽冲突**

两者写同一块盘：keydir 快照是一次大 buffer 写 + fsync；OKI 是 run 写 + fsync + 目录 fsync + manifest，超 8 个 run 还触发全归并。compaction 的突发顺序写会抢占前台 data append 的 fsync 延迟——目前靠"阈值罕见 + best-effort"缓解，**没有显式限速**。

### A.2 逻辑序冲突（水位/覆盖约束——设计核心）

**A.2.1 双提交点顺序约束：keydir 快照必须先，OKI flush 必须后**

`cask.cpp:937-944`：keydir 快照让 recovery **跳过字节水位之前的 fold**，这些被跳过的行必须已在 OKI runs 里（`wm ≥ 快照实载覆盖界`），否则 Level B 下被逐出的冷 key 就是**数据洞**。顺序反了 = 丢 key。这是两者写盘之间最强的逻辑耦合。

**A.2.2 水位采集时刻冲突（S14-1 反转问题）**

水位在**提交时刻**采集（`collect_snapshot_watermarks`），快照本体在 reducer **执行时刻**才写。若执行时才取水位，会被并发写者推进，反转 `keydir_covered ≤ search_covered` 不变量 → fold 从超前水位起跳、search 丢区间。快照 entries 比水位**新**无害（fold 幂等覆盖），比水位**旧**才致命——方向性约束。

**A.2.3 持久字节冲突（S36-5 B1：run 不得引用会蒸发的字节）**

OKI 行带 `loc`（file_id/offset）指向 data 文件。若 run 提交时引用了 active 文件**尚未 fsync** 的字节，掉电后 run 指向空洞。两条 flush 路径用不同手段解决同一冲突：

- **checkpoint flush**：`collect_snapshot_watermarks` 把 active 水位钳到 `durable_bytes()`（顺手 `sync_fd_only`），flush 内做"持久性分拣"——`loc.offset + total_sz > wm` 的行**持留（held）**在 memdelta，不进 run（`oki_state.cpp:186-205`）；
- **阈值 flush**：不带水位参数，改为 flush 前先把 active 文件整体 `sync()`，sync 失败则放弃本轮。

残余形态：持留行滞留队头，若持久水位长期不推进（sync 一直失败），memdelta 无法瘦身 → 反复触发阈值检查。

**A.2.4 同 ord 搬迁行的裁决冲突**

merge relocation 用 `append_update` **免水位门**写入旧 ord 新位置的行。flush 排序必须 stable、同 key 同 ord 按 **(ord, 到达序)** 取末——搬迁行必须胜过被搬迁行。keydir 快照侧无此问题（只存最终状态），这是 OKI 独有的、与 merge 写盘交织的冲突点。

### A.3 崩溃窗口冲突（两个提交点之间掉电）

| 崩溃点 | 后果 | 兜底 |
|---|---|---|
| keydir 快照已写、OKI flush 未做 | `oki.wm() < snap_next_ord` → **缺口** | `finish_oki_recovery:676` 检测 → **全量重建 OKI**（安全但慢；Level B 主要残余窗口）|
| run 写完、manifest 未提交 | 孤儿 run | manifest 是唯一提交点，open/下次 flush 清理；delta 未动，零恢复动作 |
| manifest 已提交、前缀未删 | 行同时在 delta 和 run | 幂等：同 ord 同 key，(ord,到达序) 格裁决一致 |
| 快照写失败 / flush 写失败 | best-effort 降级 | 快照失败→下次全量 fold；flush 失败→open 端重建 |
| 模式戳不一致（level_b stamp） | 降级/升级交错 | `stamp_mode` + manifest v3 戳检查，不一致→重建 |

不对称性：**keydir 快照失败只是"慢一次启动"，OKI flush 失败在 Level B 下是"冷 key 在重建完成前不可查"**——同样 best-effort，严重性不同。这正是第 7 点主张"提交点收敛到 OKI manifest"的动机。

### A.4 小结

- **不会发生的冲突**：文件覆盖、读者被 flush 阻塞、flush 期间数据不可见（S36-2 已消除）。
- **被设计化解但留有代价的冲突**：双 flush 源抢锁 → 写停顿；水位/持久字节约束 → 持留行与采集时刻纪律；双提交点 → 崩溃后全量重建窗口。
- **尚未显式管理的冲突**：compaction 与前台 fsync 的 IO 带宽争用（无限速）、大前缀 `mu_` 临界区的写毛刺。

---

## 附录 B：OKI-only 后能否废除 hint 文件——可行性 / 性能 / 数据丢失

> 代码锚点：`src/cask/cask.cpp:199/1089/1306`（hint 写入时机）、`src/cask/cask_recovery.cpp:210/363`（fold(data) 回退）、`src/merge/merger.cpp`（merge 输出 data+hint）。

**结论：技术上可以（WiscKey/LSM 方向），不会造成永久数据丢失，但会把"廉价的保险"换成"昂贵的恢复"——多数场景得不偿失。**

### B.1 hint 与 OKI runs 信息上不等价

| 维度 | hint 文件 | OKI runs |
|---|---|---|
| 组织 | **按 data 文件**、按 append 序（== ord 序） | **跨文件**、按 key 排序 |
| 完整性 | **全量**：含所有被覆盖的死记录 | **去重**：每 key 只留 (ord,到达序) 胜出行 |
| 写入时机 | 每次 put 顺带写（`cask.cpp:1306`），**不 fsync**（`cask.cpp:1089`），roll/close 时 finalize trailer | 阈值/ckpt 批量 flush，run + manifest **fsync** |
| 内容 | key/ord/tomb/loc（v5） | key/ord/tomb/loc + bloom + 稀疏索引 |

OKI 行内容是 hint 的超集，但**排序 + 去重**丢掉两样东西：

1. **每文件的完整记录清单**（死记录也在内）——merge 枚举输入、per-file fstats（total_keys/total_bytes、dead 比例）依赖它；
2. **append 序的廉价回放流**——recovery fold 依赖它。

### B.2 为什么技术上可以

**无 hint 的恢复路径已经存在**：`cask_recovery.cpp:210/363`，hint 缺失或 trailer CRC 不过 → 回退 `fold(data_file)` 全量扫描。data 记录自描述（key/ord/tstamp/CRC），所以：

```
OKI-only 无 hint 的 recovery =
  读 OKI manifest → 加载 runs
  → 从 wm 对应位置起 fold(data 本体) 补尾部
  → manifest 全损时全量扫 data 重建（现有外排机器可复用）
```

data 永远是真相，hint 从来不是持久性的必要条件——它本来就是"可重建、不 fsync"的加速缓存。去掉它，**持久性模型不变**。

### B.3 必须接管 hint 的三项职责

1. **尾部回放**：最后一次 OKI flush 之后的记录只能扫 data 本体（读 key+value）而非 hint（只读 ~45B/行）；
2. **全量重建源**：OKI 损坏时的兜底从"扫 hint"变成"扫全部 data"；
3. **merge 枚举 + per-file 统计**，两条路：
   - 扫 data 本体枚举输入记录（读放大）；
   - **OKI-driven merge**：从 OKI runs 筛 `loc.file_id == 输入文件` 的活行，按 offset 排序后直接按 loc 拷贝——可行（loc 含 offset+total_sz），但 per-file 的 live_bytes/total_keys 需在 manifest 元数据段维护计数器（去重的 OKI 推不出"含死记录"的 total），merge 候选选择才有依据。

协议上还需：OKI flush 频率成为**恢复时长的硬约束**（尾部 = 未 flush 行数 × value 大小；行数有 64MiB memdelta 上界但**字节数没有**）；held 持留行会进一步拉长尾部。

### B.4 性能：写路径略赚，恢复/merge 大亏

- **写路径（略改善）**：每次 put 少一次 ~45B hint 顺序写（缓冲、不 fsync），索引持久化完全批量化到 OKI flush——LSM 写模型。但收益很小：hint 写本就廉价，OKI 侧 sort + compaction 写放大（4-8×）一点没省。
- **恢复（显著恶化，量化）**：
  - 正常打开：尾部成本从 `行数 × 45B` 变成 `行数 × (key+value)`。value 平均 1KB → **读放大 ~20×**；10KB → ~200×。1.4M 行尾部 = 读 1.4GB+ data 而非 64MB hint。
  - 最坏重建（manifest 损坏/缺口）：100M keys、1KB value = **扫 100GB data vs 4.5GB hint**，打开时间分钟级变小时级，且污染 page cache。
  - hint 方案下最坏路径几乎从不触发（hint 每文件独立、trailer CRC 校验、坏了只影响单文件）。
- **merge（显著恶化或需重构）**：扫 data 枚举连死记录的 value 也要读。OKI-driven merge 可避免，但每次 merge 要全扫 OKI（4.5GB）+ 新增 per-file 计数器机制——第 6 点 sort-merge join 之外的又一笔改造。
- **冷 get / 空间**：冷查路径不变；空间省 ~4.5GB hint（100M keys，约占 data 4%）。

### B.5 数据丢失：永久丢失不会，但防线变薄

**不会永久丢**的根据：

- data 记录自描述 + 逐条 CRC，`fold(data)` 是现成、被测试覆盖的路径；
- hint 本来就不 fsync、本来就允许丢——去掉它没有降低任何持久性保证；
- 尾部重放幂等（ord 全序 LWW）；wm 门（`ord < wm_ → skip`）只要保守就安全——现有 flush 的 cover 只从**已固化行**计算（held 行不计入），方向正确。

**三个风险等级上升**：

1. **失去纵深防御**：现在 OKI 坏了有 hint 兜底、hint 坏了有 data 兜底——三层；去掉 hint 变两层，OKI 成为**查找单点**：任何 run/manifest 损坏 = 全库 data 扫描（可用性事故，虽不丢数据）。
2. **wm/held 逻辑从"性能问题"变"正确性 + 可用性双重问题"**：wm 若错误超前（行没进 run 却推进水位），尾部重放会跳过这些行。有 hint 时还能靠 hint 交叉审计/廉价重建；无 hint 时只能靠全量 data 扫描发现和修复。
3. **merge unlink 窗口的恢复代价**：relocation 行在易失 memdelta、输入文件已 unlink 时崩溃 → OKI 指向已删文件。此窗口**现在就存在**，hint 在时靠输出文件 hint 廉价重建；hint 没了只能全量 data 扫描。不丢数据，但恢复代价放大。

### B.6 建议

1. 保留 hint，可降级为**可选/惰性**（如 `hint_enabled=false` 给极端写敏感场景，明确告知恢复代价）；
2. 若真要走向 OKI-only，先做 **OKI-driven merge + manifest 内嵌 per-file 计数器**，再谈废 hint——顺序不能反；
3. 无论哪种形态，**"OKI 损坏 → 全量 data 扫描重建"必须作为一等公民路径保留并定期演练**（fold(data) 机器现成）；
4. 16GB / 10 万条场景：hint 总量才几 MB，此问题无实际影响。

---

## 附录 C：OKI manifest（`kv.oki.manifest`，BCOM）存储内容

> 代码锚点：`include/bitcask/oki_run.hpp:428-446`（结构体）、`src/fileops/oki_run.cpp:879-916`（读写）。

manifest **不存任何 key 数据**，只存"哪些 run 文件有效 + 覆盖到哪了"的元数据，是 OKI 的**唯一 commit point**。

### C.1 逻辑内容（三样）

```cpp
struct OkiManifest {
    std::vector<OkiManifestEntry> runs;  // ① 有效 run 清单
    std::uint64_t wm = 0;                // ② 联合覆盖水位
    bool level_b = false;                // ③ Level B 模式戳
};

struct OkiManifestEntry {
    std::uint64_t gen;         // run 文件代号 → 文件名 kv.oki.seg-<gen>
    std::uint64_t cover_ord;   // 该 run 覆盖上界（排他：run 内最大 ord + 1）
    std::uint8_t  format_ver;  // run 格式版本（1=BCOK v1, 2=v2 带 loc+bloom）
};
```

- **① run 清单**：当前有效的 `kv.oki.seg-<gen>` 文件。不在清单里的 run = 孤儿（崩溃残留），open 时清理。gen 同时是裁决格的一部分——同 key 同 ord 时高 gen 胜。
- **② wm（watermark）**：排他上界，"尚未被 runs 覆盖的最小 ord"。用途：recovery 尾部重放起跳点（fold 只收 `ord ≥ wm`）；缺口检查（`oki.wm() < snap_next_ord` → 全量重建）。排他语义刻意：首个合法 ord 是 0，含上界无法区分"覆盖了 ord 0"和"什么都没覆盖"。
- **③ level_b 戳**：声明"这份 OKI 由挂钩在线的 Level B 写者维护，run 内 loc 可信"。Level B 开启遇无戳目录必须全量重建起步；Level A 写者 open 时清戳（降级）。

### C.2 二进制布局（v3，全部小端）

```
[magic "BCOM" u32][ver u32=3][flags u8 (bit0=level_b)]
[count u32]
count × [gen u64][cover_ord u64][format_ver u8]   ← v1 条目无最后 1 字节
[wm u64]
[crc32 u32 覆盖除尾 8 字节外全部][magic "BCOM" u32]
```

典型大小：≤9 个 run（`kCompactRunLimit=8`）时 **~180 字节**。版本惰性选择：全 v1 run → BCOM v1（老读端可读）；含 v2 run → v2；Level B → v3。读端任何校验不过（magic/CRC/长度/未知 flag 位）→ `nullopt` = 整体弃用 OKI，重建兜底。

### C.3 写入纪律（为什么它是 commit point）

`atomic_write_bytes(fsync_dir=true)`：临时文件写 + fsync + rename + 目录 fsync。flush 全流程：

```
run 落盘 + fsync → 开 Reader 验证 CRC → write_manifest（原子）→ 才删 memdelta 前缀
```

- manifest 写成功**之前**崩溃 = 本次 flush 没发生（run 成孤儿被清理，delta 原状）；
- manifest 写成功**之后**崩溃 = 前缀没删也无害（行在 delta 和 run 里重复，(ord,到达序) 格裁决幂等）。

**一个文件的原子替换决定整批数据的可见性**——这就是"唯一 commit point"的含义。

### C.4 它不存什么（呼应第 7 点）

- **不存 key/ord/loc 行数据**——在 runs 里；
- **不存 `next_ord_`、key_count、fstats、biggest_file_id**——这些全局标量目前在 `kv.keydir.ckpt`（BCKS）里。第 7 点"提交点收敛"即把这几百字节标量塞进 BCOM v4 元数据段，OKI manifest 即可取代 keydir 快照成为唯一提交点。

### C.5 与其他 manifest 的区分

目录里另有 `index.manifest`（搜索插件 checkpoint 提交点，docmap/bm25/vec 水位）和 `segments.manifest`（BM25 段清单），职责不同。本文讨论的崩溃窗口/水位约束均围绕 `kv.oki.manifest`。
