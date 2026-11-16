# OKI 分级归并（Size-Tiered Suffix Compaction）设计

状态：设计稿（未实现） · 2026-11-16
关联：`doc/ordered-key-index-design-zh.md` §5.2（极简两层归并）、`src/fileops/oki_state.cpp`（`compact_runs`）、`include/bitcask/oki_merge_cursor.hpp`

## 1. 结论

把 OKI 的 run 归并从「run 数 > 8 就把**全部** run 重写成一个」改成**分级后缀归并**：

```text
run 序列（按 gen 升序）： R0(底座, 最大) R1 R2 … Rn(最新, 最小)
每次只归并「最新的一段连续后缀」Rj..Rn，选段规则是几何的：
    从 Rn 往前累加，前一个 run 不比已累加的后缀大太多就并进来
后缀延伸到 R0（底座）时才是「全归并」，才允许丢墓碑
```

效果：

- 索引写放大从 **O(N / 每次 flush 行数)** 降到 **O(log(N / 每次 flush 行数))**，与库的总规模基本脱钩；
- **不改盘上格式**：manifest、run 文件、恢复流程都不变；新旧版本库可以互相打开同一目录；
- 代价是常驻 run 数从「≤ 9」变成「≤ log₂(N/F) + 1」（1 亿 key 时约 8～12 个），点查多几次内存 bloom 判定。

## 2. 现状与问题

### 2.1 现状（本轮修复后）

- memdelta 达到 1M 行 / 64MiB，或任一提交点（close / checkpoint / merge 收尾 / 备份）→ flush 出一个新 run；
- flush 提交后 run 数 > `kCompactRunLimit`（8）→ `compact_runs`：三段式（锁内快照输入 + 预留 gen → 锁外 `OkiMergeCursor` 归并写新 run → 锁内校验并替换），IO 段不持 `flush_mu_`；写线程上的阈值 flush 把归并交给后台线程；
- 全归并 ⟹ 墓碑真正丢弃（同 key 的 put 行与墓碑行必在同一次归并里）。

### 2.2 问题：每次都搬一遍不变的底座

每次归并都重写**整个索引**（O(N)），但两次归并之间新进来的只有约 8 个 run（8·F 行，F = 单个 run 的行数）。每写入一行，平均被重写的次数：

```text
写放大 ≈ N / (8·F)
```

F 不是常数：阈值 flush 是 1M 行，但 OKI flush 也搭在 base checkpoint、close、merge 收尾上——搜索模式默认每 65536 条触发一次自动 checkpoint（`auto_checkpoint_min_docs`），run 往往远小于 1M。

| N（活 key） | F = 1M | F = 64K | 单次全归并重写量（~15B/行） |
|---|---|---|---|
| 1000 万 | ~1.3× | ~19× | ~150MB |
| 1 亿 | ~12× | ~190× | ~1.5GB |
| 10 亿 | ~125× | ~1900× | ~15GB（分钟级） |

本轮已把归并挪到后台、IO 不持锁，所以它不再卡住写者和读者；但磁盘带宽和写放大仍随 N 线性增长。搜索模式下 1 亿 key 量级就已经明显浪费。

## 3. 目标与非目标

**目标**

1. 写放大与总规模脱钩：≈ log₂(N/F) 量级。
2. 盘上格式零改动（manifest v4、run v3 不变），升级/降级都不需要迁移。
3. 正确性不变量（gen 优先级、墓碑语义、提交一致性）与现状同等强度，且可测试。
4. 常驻 run 数有硬上限，点查读放大可控。

**非目标**

- 不做 leveled（分层、key 区间不重叠）——需要 manifest 记录层级和区间、点查逐层查，改动大，十亿级以上再议；
- 不做按 key 区间的分区归并；
- 不改 memdelta flush 阈值、不改 run 文件格式。

## 4. 设计

### 4.1 关键约束：只能归并「连续后缀」

OKI 的胜出规则是 max (ord, 来源序)：同 ord 时 gen 更高的 run 胜。同 ord 的多行只来自 merge 搬迁（`relocate` → `append_update`）：旧位置在较老的 run，新位置在较新的 run。

若归并**不连续**的一组 run（例如 R3 和 R5，跳过 R4），输出拿到新的最大 gen，R3 里的旧位置就会压过 R4 里的新位置——输入文件退役后索引悬空。若归并**中间一段**（R2..R4，后面还有 R5），输出 gen 必须落在 (gen(R4), gen(R5)) 之间，整数 gen 做不到。

因此规定：**每次只归并 Rj..Rn 这一段后缀**，输出取新的最大 gen。

- 输入之后在归并期间新 flush 出的 run，其 gen 由「在途预留」机制保证大于输出 gen（现有 `inflight_gens_`），依然排在输出之后；
- 比 Rj 更老的 run 不动。

这样「gen 越大越新」的不变量在任何时刻都成立，`locate`（gen 降序首命中）、`OkiMergeCursor`、`make_read_view`、恢复流程都不需要改。

### 4.2 选段规则（几何后缀）

记 run 大小 sᵢ = `OkiRunReader::entry_count()`（行数，含墓碑行），R0..Rn 按 gen 升序。

```text
j = n, acc = s_n
while j > 0 and s_{j-1} <= K * acc:      # K = 扇入系数，默认 1
    j -= 1; acc += s_j
if n - j + 1 >= 2: 归并 Rj..Rn
```

直观理解（K = 1）：从最新的 run 往前看，只要前一个 run 不比后面已攒起来的总量大，就把它并进来。这就是二进制计数器式的合并——每行每次被重写时所在 run 的规模至少翻倍，所以一行最多被重写约 log₂(N/F) 次。

- K 越大越激进（run 数更少、写放大更高）；K → ∞ 退化为现在的全归并。默认 K = 1，留作调参点。
- 何时检查：每次 flush 提交后（与现在的触发点相同）。flush 持锁算出是否需要归并，归并本身仍走三段式（阈值 flush 交后台，其余提交点在释放锁后同步做）。
- 后台线程收尾时复查并续跑（沿用本轮已实现的循环）。

### 4.3 硬上限与全归并

- **run 数硬上限** `kMaxRuns`（默认 24）：几何规则在病态输入下（例如 flush 大小剧烈波动）仍可能积累较多 run；超过上限时强制把后缀扩到满足上限为止。
- **全归并**（后缀覆盖到 R0）照旧由几何规则自然触发：后缀累加量追上底座（K = 1 时即新增量 ≥ 底座）时。
- **墓碑占比过高时的显式全归并**：墓碑行只在全归并时回收。若在后缀归并时发现「输出中墓碑行 / 总行」超过阈值（默认 25%），把下一次归并标记为全归并。墓碑计数在归并过程中顺带统计，不需要改 run 格式。
- 保留一个内部开关 `CompactionPolicy::kFull`，用于 A/B 基准对比和回退。

### 4.4 墓碑处理

| 归并范围 | 墓碑行 | 原因 |
|---|---|---|
| 后缀包含 R0（全归并） | 丢弃（同现状） | 同 key 所有更老的行都在输入里，put 与墓碑一起消失 |
| 不含 R0 的后缀 | **必须保留**，原样写入输出 | 更老的 run 里可能还有该 key 的 put；丢掉墓碑会让它「复活」 |

实现上：`OkiMergeCursor::next(row, include_tombs = !full)`，输出 run 对墓碑行写 `tomb=true`、不带位置（与 flush 产出的墓碑行同形）。

同 key 在后缀内既有 put 又有更新的墓碑时，后缀内的胜出者是墓碑，put 行被抵消；输出只留一条墓碑行。这正是现有 flush 的去重语义。

### 4.5 提交与并发（沿用三段式）

在 `compact_runs` 的基础上泛化：

1. **锁内**：按 4.2 / 4.3 选出后缀 Rj..Rn（以及是否全归并），快照这些 Reader，预留输出 gen（登记为在途）。
2. **锁外**：`OkiMergeCursor` 归并后缀，按 4.4 处理墓碑，写出输出 run，fsync，打开 Reader 校验。
3. **锁内**提交前校验：
   - 输入 gen 仍全部在册，且仍构成「当前 run 序列中、所有新于输出 gen 的 run 之前」的一段连续区间（期间只可能在其后追加新 run）；
   - 期间若发生 rebuild（run 集合被整体替换）、或别的归并改动了输入 → 放弃，删除输出；
   - 新 manifest = 当前 manifest 中 Rj 之前的 run + 输出 + 期间新增的更高 gen run；元数据 / fstats / data 水位 / wm 原样沿用（与本轮修复后一致）。
4. 发布 run 快照 → sweep 输入文件（在途构建的 gen 受保护）→ 块缓存清掉死 gen。

读视图无锁构建（「先取 delta、后取 runs」）的正确性论证不变：归并只替换 run，不动 delta。

同一时刻至多一个归并（`compact_mu_`）。后缀归并期间又来一次 flush 并要求归并：等当前归并完成后由后台循环复查。

### 4.6 与其它机制的交互

- **merge 搬迁行**：同 ord 的新位置总在更新的 run（或 delta）里；后缀归并保持相对顺序，搬迁裁决不受影响。
- **rebuild**：产出单一 run（即新的 R0），之后从头开始分级。
- **恢复**：manifest 与 run 格式不变，data-only 恢复、`recover_row` 的视图比对、账本模式补 tail 全部不变。
- **v1 run**：含 v1 输入时输出降为 v1（同现状）；只要 v1 run 还没被 rebuild 淘汰，点查不可用的语义就不变。
- **降级兼容**：旧版本库打开新目录时，看到的是若干按 gen 升序的 v3 run（非底座 run 里带墓碑行，和 flush 出的 run 同形），完全可读；旧版本下次归并会做全归并，也正确。

## 5. 预期收益

### 5.1 写放大（每写入一行平均被重写次数）

| N | F | 现状（全归并） | 分级（K = 1，≈ log₂(N/F)） |
|---|---|---|---|
| 1000 万 | 1M | ~1.3× | ~3.3× |
| 1000 万 | 64K | ~19× | ~7.3× |
| 1 亿 | 1M | ~12× | ~6.6× |
| 1 亿 | 64K | ~190× | ~10.6× |
| 10 亿 | 1M | ~125× | ~10× |
| 10 亿 | 64K | ~1900× | ~14× |

注意第一行：千万级、run 又大的场景下，分级反而比全归并多写。这是对数与线性的交叉点，绝对量都很小（GB 级以下）；如需完全不劣化，可在 N 较小时（例如底座 < 16·F）直接走全归并（4.3 的开关就能表达）。

### 5.2 读放大

常驻 run 数 ≤ log₂(N/F) + 1（且受 `kMaxRuns` 限制）：1 亿 key、F = 64K 时约 12 个，现状 ≤ 9 个。点查每多一个 run 多一次内存 bloom 判定（~1% 误判率才会多一次块读），range 归并多一路游标，影响很小。

### 5.3 空间

墓碑行在非全归并中保留，空间回收比现状慢一些；由 4.3 的墓碑占比触发兜底。

## 6. 实现计划

| 阶段 | 内容 | 涉及 |
|---|---|---|
| T-1 | 纯函数 `choose_compaction(sizes, K, max_runs) → {j, full}`；单元测试（边界、硬上限、K→∞ 退化为全归并） | `oki_state.cpp`（或新 `oki_compaction_policy.{hpp,cpp}`） |
| T-2 | `compact_runs` 泛化为后缀归并：输入选择、墓碑保留、提交校验（连续性、期间新增 run） | `oki_state.cpp` |
| T-3 | 触发与统计：flush 后按策略判定；后台循环复查；`compaction_stats()`（归并次数、全归并次数、重写行数、墓碑行数）接入 `StatusInfo` | `oki_state.{hpp,cpp}`、`cask.cpp` |
| T-4 | 测试调整与新增（见第 7 节），基准对比，更新 `doc/ordered-key-index-design-zh.md` §5.2 | `tests/`、`bench/`、`doc/` |

需要调整的既有测试（它们锁定了「第 9 次 flush 塌成 1 个 run」的旧行为）：

- `OkiRecoveryTest.RunCompactionCollapsesRunsAtThreshold`
- `OkiRecoveryTest.FullCompactionDropsTombstoneRows`（改为显式触发全归并，或断言「全归并后」而非「第 N 次 flush 后」）
- `OkiRegressionTest.CompactionKeepsManifestMetadata`、`AsyncCompactionKeepsReadViewsComplete`（run 数上限从 `kCompactRunLimit + 1` 改为 `kMaxRuns`）

## 7. 测试计划

1. **策略单元测试**：给定 run 大小序列验证选段；模拟 10⁴ 次随机大小 flush，断言 run 数始终 ≤ `kMaxRuns`、每行累计重写次数 ≤ ⌈log₂(N/F_min)⌉ + 常数。
2. **模型对拍**（核心）：随机 put / remove / 同 ord 搬迁 / flush / 后缀归并 / 全归并，与一个内存 map 模型逐 key 对比 `locate`、迭代结果。重点场景：
   - put 在底座、墓碑在小 run，后缀归并不含底座 → key 仍不可见（墓碑必须保留）；
   - 搬迁行与原行分处后缀内外 → 新位置胜出；
   - 归并期间并发 flush / rebuild → 提交放弃或正确拼接。
3. **崩溃**：在「输出 run 已写、manifest 未提交」处中断 → 重开后输出作为孤儿被清理，数据完整。
4. **并发**：沿用 `AsyncCompactionKeepsReadViewsComplete` / `ReadViewNeverTornByRebuild`，TSAN 下循环跑。
5. **基准**：`OkiState` 直驱，N = 10⁷ / 10⁸，F = 64K / 1M，统计归并重写行数 ÷ flush 行数、归并总耗时、点查 p50/p99、range 吞吐；与 `CompactionPolicy::kFull` 对比。

## 8. 风险与回退

| 风险 | 应对 |
|---|---|
| 非连续归并导致搬迁位置被旧行压过 | 4.1 的后缀约束 + 提交时连续性校验 + 模型对拍专项场景 |
| 后缀归并误丢墓碑导致复活 | 4.4 只有含 R0 才丢；对拍专项场景 |
| run 数在病态 flush 序列下膨胀 | `kMaxRuns` 硬上限 |
| 墓碑长期滞留占空间 | 墓碑占比触发全归并 |
| 小库写放大略升 | 底座较小时直接全归并 |
| 需要回退 | `CompactionPolicy::kFull` 内部开关；盘上格式未变，回退无需迁移 |

## 9. 待定问题

1. K 的默认值（1 还是 2）需要基准数据定；
2. 是否把 K / `kMaxRuns` 暴露成 `CaskOptions`（建议先不暴露，内部常量 + 基准调参）；
3. 墓碑占比阈值（25%）是否需要按 key 规模自适应。
