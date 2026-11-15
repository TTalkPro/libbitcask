# S43-0 基线、语义契约与故障注入底座

> 状态：**完成**（2026-11-15）。本文件冻结 S43 的外部语义、调用面
> 规模和故障矩阵；最终 rollout 量化见
> [`s43-okionly-rollout-report-zh.md`](s43-okionly-rollout-report-zh.md)。

## 1. 当前调用面规模

以下计数来自 2026-11-15 工作树，排除本地 build 目录：

| 面积 | 文件数 | 匹配行数 |
|---|---:|---:|
| hint 相关引用（src/include/tests/bench/tools/c_api） | 24 | 146 |
| hint 生产路径引用（src/include） | — | 94 |
| KeyDir / keydir 相关引用 | 57 | 631 |
| cask 内 `keydir_` 引用 | — | 172 |
| tests 内 hint 引用 | — | 63 |

这不是删除工作量本身，但说明 S43-8 / S43-9 必须放在 S43-3 至 S43-7
的能力替换之后执行。

## 2. 外部语义契约

以下行为是 S43 全程不得静默改变的外部契约。`tests/oki_only_contract_test.cpp`
先锁定核心生命周期；更多并发和崩溃语义由现有测试继续守门。

### 2.1 点查生命周期

```text
missing key -> NotFound
put(k,v1)   -> get(k)=v1
put(k,v2)   -> get(k)=v2，ord 递增
remove(k)   -> get(k)=NotFound
put(k,v3)   -> get(k)=v3
close/reopen-> 上述最终状态不变
```

删除 hint 后重开必须仍能得到同一状态；这是去 hint 前的最低恢复契约。

### 2.2 OKI 冷视图与公共 get 对齐

Level B 目录下，`OkiState::locate(key)` 必须与 `Cask::get(key)` 的存在性和
ord 一致：

```text
public get hit   => OKI kHit 且 !tomb，ord 相等
public get miss  => OKI kMiss，或 OKI kHit 且 tomb
OKI kUnavailable => 表示索引不可用，不得解释成 miss
```

后续 S43-4 切换后，该对拍只保留在测试或显式诊断路径中。

### 2.3 atomic batch

批内 put/remove 对外部观察者原子生效。data 提交后的重开必须看到批后的
最终状态；不允许出现“remove 提交但同批 put 丢失”或反向的中间态。

### 2.4 值读取安全

任何定位来源返回 `(file_id, offset, total_sz)` 后，读回 record 必须校验：

1. record CRC；
2. record key 与请求 key 相等；
3. tombstone / TTL 语义；
4. merge relocation 后旧 loc 失败时的重试语义。

其中第 2 条是 D2 §5 的硬前置，禁止因索引损坏返回别的 key 的 value。

## 3. 性能与恢复基线

低层 run/manifest 基线已在 §3.1/§3.1.1 回填；端到端 S43-10 rollout
量化见独立报告。历史计划如下：

| 场景 | 指标 | 状态 |
|---|---|---|
| 开库 10M / 100M | 冷/热时间、峰值 RSS | ✅ run 形态见 §3.1/§3.1.1；端到端见 rollout 报告 |
| OKI run 开库 CRC | run 总大小、热/冷 open 时间 | ✅ 首测回填（见 §3.1） |
| 点查 | delta 态 / run 态 / 冷块 p50/p99 | ✅ 热点查与冷块 p50/p99 已量化 |
| 写路径 | 顺序写、均匀写、Zipfian 写吞吐 | ✅ 覆盖写、均匀写与 bounded Zipf(s=1) 已量化 |
| range/iterator | 冷扫、热扫、大 memdelta 首查 | ✅ OKI range 两档已量化 |
| 常驻内存 | 无 KeyDir 的 RSS A/B | ✅ 100k KV 探针 memdelta RSS 26.37→7.60 MiB；rebuild 峰值 37.14→17.60 MiB |
| merge | 吞吐、磁盘峰值、unlink 后状态、OKI 写放大 | ✅ 吞吐、data/OKI/hint 物理账与 0.1533 增量写放大已量化 |
| recovery | tail replay、全量 data rebuild | ✅ snapshot/full-fold 与 K10/K14/K15 gate 已量化 |

基线机器、存储介质和缓存状态必须写入报告，否则不同阶段结果不可比。

### 3.1 S43-1 首次 run 开库校验量化

测量日期：2026-11-15；主机：`/dev/sda2`（500 GB SATA/XFS，工作目录所在盘）；
构建：`build-gcc-release`；方法：直接生成真实 BCOK v2 run
（`key-%010d`、4096 B 目标块、v2 bloom/loc），连续 3 次
`OkiRunReader::open` 计时；每次 open 前用 `posix_fadvise(DONTNEED)` 释放
page cache。首轮包含少量首次冷遍历噪声，取后两次为热缓存口径。

| entries | run bytes | blocks | fadvise 冷 open | 热 open（后两次） |
|---:|---:|---:|---:|---:|
| 10,000,000 | 140,674,306 | 31,106 | 89.3 ms | 62.0 / 65.8 ms |
| 100,000,000 | 1,445,296,491 | 320,495 | 2,505.2 ms | 608.5 / 608.7 ms |

结论：**占比显著，不能长期保持 v2 eager 整文件 CRC 作为 OKI-only 的常态**。
按 D2 §4-C 应实施 run v3：稀疏索引每块记录 CRC32，开库只校验
header/索引/bloom/元数据 CRC；数据块在块缓存 loader 与直读 loader 中
首次读取时校验。

### 3.1.1 BCOK v3 开库与首读量化

测量日期：2026-11-15；主机与存储同 §3.1；构建：`build-gcc-release`。
方法：生成真实 BCOK v3 run（`key-%010d`、4096 B 目标块、v3 bloom/loc），
每次 `OkiRunReader::open` 前执行 `posix_fadvise(DONTNEED)`；首轮为冷开
口径，后两次为热开口径。首读测 10,000 个均匀分布 key，首次命中会逐块
装载 256 MiB LRU，总耗时包含这些块的首次读/解码/CRC。

| entries | v3 run bytes | blocks | fadvise 冷 open | 热 open（后两次） | 首读 10k 总耗时 | 首读 p50/p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000,000 | 139,410,699 | 30,757 | 10.6 ms | 10.3 / 9.9 ms | 58.8 ms | 5.5 / 12.4 µs |
| 100,000,000 | 1,445,480,636 | 320,146 | 413.7 ms | 442.3 / 101.9 ms | 89.8 ms | 5.8 / 23.7 µs |

结论：v3 将 100M 元数据开库从 v2 全文件 CRC 的约 2.5 s 冷开 / 0.609 s
热开，降到本测约 0.414 s 冷开 / 0.102 s 第二次热开；剩余耗时主要是
sparse index/bloom 元数据读取与校验。数据块 CRC 已延迟到首读；10k
均匀 distinct key 首读只付对应 10k 块成本，p99 仍低于 24 µs。该结果是
单次采集，未纳入峰值 RSS；S43-10 仍需在统一 benchmark gate 中复测。

### 3.2 S43-1 run v3 第一批实现记录（2026-11-15）

- 新增 BCOK v3：v2 行格式不变；sparse index entry 追加 `block_crc u32`；
  bloom 与 v2 兼容；trailer 尺寸不变，trailer CRC 改为覆盖
  header + sparse index + bloom + trailer 定宽字段的 metadata CRC。
- v3 open 不再整读 run；v1/v2 继续 eager 全文件 CRC。`Cursor::load_block`
  在块缓存路径和直读路径都先验证 block CRC；失败返回 `kCorrupt`。
- `OkiState` 新 flush/rebuild 输出 v3；compaction 在所有输入均 v2/v3 时
  输出 v3，含 v1 时仍降级 v1。manifest entry `format_ver` 支持 1/2/3。
- `Cask::read_doc` 的 key 核对保留；同时修正 cold locate 的回填时机：
  必须先核对 record key/tomb/TTL，通过后才允许 warm fill，避免错指 loc
  被污染进 KeyDir 后在下一次读返回错值。
- 新增直接守门：v3 roundtrip、块位翻转后 open 放行但读报 `kCorrupt`
  （直读与块缓存两条路径）、metadata 位翻转 open 拒收、v1/v2/v3 mixed
  manifest、Level B loc 错指后 public get 返回 `kIndexCorrupt`。
- 已补运行期坏块降级闭环：`KeyDir::locate_status` 将冷侧
  `kUnavailable` 与普通 miss 分开；`Cask::read_doc` 首次遇到时置
  rebuilding 标志，在 KeyDir 写者屏障内从 data（唯一持久真相）重建
  单个 BCOK v3 run，随后重试触发读；重建期间其他读者显式返回
  `kIndexCorrupt`，绝不折叠成 NotFound。该路径支持原子批 staging，
  成功后恢复 OKI 点查并淘汰坏 run。

- 已补语义注入矩阵：结构合法的 v3 run 分别注入 key 改写、ord 改写和
  tomb 置位，并同步重写 run/manifest CRC。key/ord 与 data 记录核对失败
  时显式返回 `kIndexCorrupt`；合法 tomb mutation 返回 NotFound，绝不
  返回旧 value。加上既有 foreign loc 与 raw bad-block 用例，loc/key/
  tomb/ord 四类覆盖齐备。
- 已补 Cask 级 mixed v1/v2/v3 降级：最高 gen 只有无位置能力的 v1 行时，
  Level B locate 显式 `kUnavailable`，首次读触发 data-only 受控重建；
  三个 mixed 输入淘汰，replacement 是单个 v3 run。
- 已补旧读端未来 run 版本降级：BCOK version 99 拒收后从 data 重建，
  replacement 是单个 v3 run。

最终全量回归：`build-gcc-release` 834/834 通过；两个 RSS probe 维持
disabled 未运行。运行期 data-only rebuild 当前采用屏障内 fold + 外排，
内存/停写上界将在 S43-3 streaming recovery 中继续收敛。

## 3.3 S43-2 BCOM v4 第一批实现记录（2026-11-15）

- BCOM v4 采用定长 metadata 段：56B 全局快照 + 64B per-file fstats。
  全局字段覆盖 `next_ord`、`biggest_file_id`、显式 `next_file_id`、
  active key 数、live bytes 与时间包络；fstats 沿用 KeyDir 口径并显式
  保存 `pending_delete` / `expiration_epoch`。reserved 与 fstats pad
  都要求 0，未知 flags、reserved 非零、CRC/长度不一致全部 fail-fast。
- 写端只在 KeyDir 捕获成功时升 v4；v1-v3 reader 保留，v3 open 不伪造
  metadata。空 memdelta 与全持留 flush 也支持 metadata-only 原子提交，
  因此 Level A 目录和 v3 目录首次迁移不会因“没有新 run”被跳过。
- KeyDir 捕获在写者屏障内完成，活跃 fold/pending 拒绝本次 v4 提交。
  Cask close/checkpoint、merge unlink 前和受控 rebuild 都把快照传给
  `OkiState::flush/rebuild`；run fsync 与 Reader 校验之后、manifest
  发布之前再校验 `next_ord >= wm/run cover` 和结构约束。
- v4 + BCKS 成功载入后才应用 metadata 标量；无 BCKS 的全量 data fold
  仍从空 KeyDir 重建，避免 fold 挂钩把逻辑计数双记。tail/墓碑高水位
  允许在恢复后正向超过上一个提交点，下一次提交再收敛为新快照。
- 已守门：v4 roundtrip/fail-fast、hint 删除后 v4 reopen、空 run
  metadata-only 提交、manifest 全损后 data/BCKS rebuild metadata
  与上一提交点相等、旧 run 清理，以及 Level A/B 模式转换回归。

### 3.3.1 S43-2 第二批收口记录（2026-11-15）

- K4 注入采用可复现目录阻塞：run fsync/Reader 校验通过后，manifest
  tmp 无法发布。失败只保留孤儿 run；delta、上一份 v4 metadata 与活跃
  key 读语义都不回退。移除阻塞重试后，同一 delta 收敛为新 commit point。
- 强制 v3→v4 迁移保留 runs/wm，但清空 metadata/fstats。open 端继续
  拒绝伪造；下一次成功 flush 的 KeyDir 捕获成为迁移源。metadata-only
  提交使无 run 的 Level A / 空目录也能迁移。
- merge 收尾对拍不再只看 pre-unlink 快照：trim 旧 input 后，下一次
  keydir snapshot 必须发布 trim 后的 v4 fstats。旧 input 消失、relocation
  output 与 active fstats 逐字段一致；per-key TTL 清理跨重启不复活。
- `pending_delete` 不只持久布尔：`expiration_epoch` 是源字段。epoch
  推进后 `pending_delete = expiration_epoch < epoch` 必须精确成立。
- held row 允许同一提交点发布 next_ord/fstats，但不允许推进 `wm` 或
  run cover；持久水位越过 loc 后，下一次 flush 才把行固化进 run。
- manifest 全损 rebuild 对拍上一提交点 metadata、清孤儿 run，并显式
  清理崩溃遗留的半截 `kv.oki.spill-*`。

定向回归 85/85 通过；最终 `build-gcc-release` 全量 ctest 841/841 通过
（ctest 总账 843，两个 RSS probe 依旧 disabled）。S43-2 收口。

## 3.4 S43-4 OKI-only public get 切换记录（2026-11-15）

- `Cask::read_doc` 已改为直接调用 `OkiState::locate`；KeyDir hash
  first、warm-fill 和成功后的缓存回填从公共读路径移除。OKI
  `kUnavailable` 不折叠成 miss：可写读者触发一次受控 data rebuild，
  并发读者显式收到 `kIndexCorrupt`。
- 旧 Level A 目录保留懒迁移：首次 public get 前不清 manifest、不改变
  recovery 形态；只有读取需要定位时才用 data rebuild 生成可信 v3/v4。
  此后写/merge 挂钩保持 OKI 新鲜，后续读取不再触发 KeyDir 定位。
- 迁移期影子对拍继续只用于 debug/测试；release 生产 get 只读 OKI。
  定向回归 58/58、全量 Release 843/843 通过；S43-10 仍需量化冷点查
  与均匀写退化，并执行 sanitizer/benchmark gate。

## 4. S43-0 故障注入矩阵

### 4.1 写入 / OKI delta

| # | 注入点 | 期望恢复行为 |
|---|---|---|
| K1 | data append 后，OKI append 前 | data 存在但索引缺口可由 tail replay 补齐 |
| K2 | OKI append 后，返回用户前 | 重放幂等，LWW 结果不变 |
| K3 | memdelta flush 的 run fsync 前 | run 为临时/孤儿，manifest 不推进 |
| K4 | run fsync 后，BCOM 提交前 | 孤儿 run 清理，delta 保留 |
| K5 | BCOM 提交后，delta 截断前 | run/delta 重复行按裁决规则幂等 |

### 4.2 recovery / checkpoint

| # | 注入点 | 期望恢复行为 |
|---|---|---|
| K6 | BCOM v3/v4 原子写中断 | 弃用新 manifest，走 last-good 或 rebuild |
| K7 | BCOM CRC / 未知版本 / 未知 flags | 不读错数据，进入受控 rebuild |
| K8 | keydir snapshot 已写、OKI flush 未写 | 当前系统重建 OKI；S43 后由 v4+tail 收敛 |
| K9 | data tail torn / bad CRC | 只丢弃未完整尾部，不跳过已完整记录 |
| K10 | recovery flush 阈值触发 | 与普通 flush 同一提交协议 |

### 4.3 merge / relocation

| # | 注入点 | 期望恢复行为 |
|---|---|---|
| K11 | output data fsync 前 | 新输出无效，旧输入仍权威 |
| K12 | output data fsync 后，relocation 前 | 新文件未引用，旧状态可用 |
| K13 | relocation append 前 | 旧 loc 仍有效 |
| K14 | relocation append 后，unlink 前 | 新旧 loc 语义幂等 |
| K15 | unlink data 后 | OKI 不得指向不可恢复位置 |

## 5. S43-0 完成定义

- [x] 调用面计数与架构风险冻结；
- [x] 核心点查 / lifecycle / batch / data-only reopen 守门测试；
- [x] OKI run 开库 CRC 首次量化；
- [x] 全量并发、TTL、search watermark、file_id 分配语义守门补齐；
- [x] 性能基线采集；
- [x] kill 注入底座覆盖 K1-K15；
- [x] S43 后续工作面清单与本仓库实际 HEAD 再次核对。
