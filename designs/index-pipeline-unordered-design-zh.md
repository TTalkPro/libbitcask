# 索引管线去全局顺序化（key + LSN 裁决 · 分片并发写 · 去 reducer）设计

状态：**已实现**（TASK.md S45-0～S45-6，2026-11-17；实现记录与偏差见第 10 节）
来源：2026-11-17 竞态 / 泄漏审计（基线 HEAD `4d3b506`，7.0.3）及其后的方向讨论
关联：`include/bitcask/thread_pool.hpp`（`IndexPool`）、`include/bitcask/plugin_api.hpp`、
`src/cask/cask.cpp`、`src/cask/cask_recovery.cpp`、`src/search/text_plugin.cpp`、
`src/keydir/index.cpp`（docmap）、`docs/design/s27-4-dwpt-design.md`（并行 builder 与 LSN 守卫）

> 本文取代同日的初版《索引池背压活性 + close 有界等待》（两层补丁方案）。初版的
> 止血内容收进第 3 节，其余方案作废。

## 1. 结论

审计发现的死锁（第 2 节）不是某个谓词写错，而是结构性的：**索引管线要求全局按 LSN 顺序
应用，却允许乱序提交，中间用一个有上限的重排缓冲把顺序排回来。** 这三件事放在一起，
逻辑上必然存在「消费者等的那一条被限流挡在外面」的可能。

本设计不再修补重排缓冲，而是去掉对全局顺序的需求：

| 现在 | 改后 |
|---|---|
| reducer 单线程按 LSN 严格 +1 应用 | 没有全局顺序；每条任务独立应用 |
| 正确性靠「到达顺序 = LSN 顺序」 | 正确性靠 **key + LSN 裁决**：旧版本到达即丢弃 |
| 队列 → map worker → 重排 ring → reducer | 按 key 哈希分发到 N 个分片线程，各写各的段 |
| 水位 = reducer 连续应用到的 LSN | 水位 = 「已完成 LSN 集合」的连续前缀（完成跟踪器） |
| 写失败要补 Skip 填洞 | 写失败只需在跟踪器里标记完成（不阻塞、不排队） |
| 维护操作借 reducer 线程执行（RunFn） | 维护线程 + 按库暂停（独占段） |

分两步交付：

- **止血（S45-0，可随 7.0.x 补丁发）**：最小改动堵住死锁，并修复 `close()` 超时失效。
- **重构（S45-1～S45-6）**：先让每一处应用都能乱序且幂等（reducer 仍在，行为等价），
  再换掉管线，最后让恢复重放也走新管线。

硬约束：盘上格式、公共 API、C ABI 不变（待 S45-1 逐项确认，见第 8 节）；严格的
读己之写保留。

## 2. 问题回顾

### 2.1 死锁（已复现）

管线：写线程 → 有界队列（10240）→ N 个 map worker → 每库一个重排 ring → 单 reducer。

- reducer 必须按 LSN 严格 +1 应用（原因见 2.3）。
- LSN 在 `write_mu_` 内分配，索引任务在锁外提交（H1 优化）。`put_batch` 尤其明显：
  整批 LSN 一次分完，解锁后逐条入队（`cask.cpp:2287-2288`）。入队顺序不等于 LSN 顺序。
- map worker 的背压只看 ring 在途数是否达到 `reorder_cap_`（16384），不看 reducer
  在等哪个 LSN（`thread_pool.hpp:664-667`）。

批还没提交完时，其他线程 LSN 更大的任务堆满 ring → 所有 worker 停止取任务 → 批的
下一条还在队列里 → reducer 永远等不到 → 队列写满 → 所有写线程卡在 push。同 registry
下的其他库共用同一个池，一起停摆。`search`（查询前要等索引追平）和 `close` 也挂住。

复现（附录 A；Whitespace 分词，9 个 map worker，链接 `build-gcc-release` 静态库）：

| 批大小 | 并发 put 线程 | 结果 |
|---|---|---|
| 60000 | 4 | 死锁（2/2） |
| 40000 | 8 | 死锁（2/2） |
| 30000 | 8 | 通过 |
| 20000 | 6 | 通过 |

gdb 线程栈：9 个 worker 全在 `map_worker_loop` 的条件变量上，reducer 在 `reorder_cv_`
上，`put_batch` 和 4 个 `put` 线程全在 `submit_index_task` 的队列 push 里。

### 2.2 close 的 30 秒逃生门不生效（读代码确认，未运行）

`cask.cpp:755-770`：超时判断在循环头，随后调 `writes_in_flight_.wait(n)`。
`std::atomic::wait` 没有超时，值不变就不返回。写线程被 kill 或卡在 2.1 的死锁里时，
`close()` 永久阻塞。同文件 `checkpoint()` 的注释（`:3112`）已指出这一点并改用 cv，
close 漏改。

### 2.3 为什么现在必须按序

| 依赖全局顺序的地方 | 证据 |
|---|---|
| docmap：`put_doc` 无条件覆盖，不比较 LSN；`remove` 直接抹掉条目 | `index.cpp:26-52`、`:61-83` |
| HNSW 只支持单写者 | `hnsw.hpp:23`「多写者不支持」 |
| 维护操作靠「在 reducer 线程上」获得独占 | `plugin_api.hpp` `run_serialized` 注释；`cask.cpp` 各 RunFn |
| 水位与读屏障用「连续应用到的 LSN」 | `IndexPool::flush` 谓词；`FlushRequest.watermark` |
| 插件契约写明「reducer 单写者，ord 严格升序」 | `plugin_api.hpp` `on_put` / `on_delete` 注释 |
| 启动重建 key 定位表假设段序、段内序即 LSN 序 | `text_plugin.cpp:1507-1530` |
| 恢复重放假设「插入序 == fold 序」 | `cask_recovery.cpp:461-466` |

反过来，BM25 一侧**已经不依赖顺序**：S27-4 做了每 builder 一个 building 段、段内局部
编号、`key → (段, 段内编号, LSN)` 定位表和 LSN 守卫，设计目标就是「任意到达序安全」。
段里逐文档落盘了 key 和 LSN（`segment.hpp` 的 `keys_` / `lsns_`）。S27-4 实测 2 个 builder
吞吐 1.9 倍、4 个 2.1 倍，但默认 0 个（内联），且上游仍是 reducer。

## 3. 止血（S45-0）

目标是用最小、最容易评审的改动消除两个挂死点，不追求结构正确——这部分代码会随
S45-4 删除。

### 3.1 死锁：reducer 饥饿旁路

map worker 的等待谓词加一项：

```cpp
reorder_inflight_ < reorder_cap_
    || !any_lane_ready_locked()     // 新增：reducer 无事可做时放行
    || stopped_
```

reducer 一轮扫描无就绪 lane、即将等待时，若 `reorder_inflight_ >= reorder_cap_` 则
`map_cv_.notify_all()`。两个 cv 共用 `reorder_mu_`，不新增锁。

活性：worker 全部阻塞 ⇒ 存在就绪 lane ⇒ reducer 可前进；reducer 的等待谓词正是
「无就绪 lane」，两者不可能同时成立。

代价：`reorder_cap_` 变成软上限。旁路期间 ring 会超额，极端批量下超额量可达
「并发写线程数 × 批大小」量级。止血阶段接受，并在 ring 首次超过 `2 × reorder_cap_`
时 `log_warn` 一次。不做「大批量挪回锁内提交」——那是为即将删除的结构再加一层补丁。

### 3.2 close：有界轮询

```cpp
const auto deadline = steady_clock::now() + close_wait_timeout_;   // 默认 30s，可注入
auto nap = 50us;
while (writes_in_flight_.load(seq_cst) != 0) {
    if (steady_clock::now() >= deadline) { log_error(...); break; }
    std::this_thread::sleep_for(nap);
    nap = std::min(nap * 2, 10ms);
}
```

`WriteOpGate` 析构里的 `notify_all()` 不再有等待者，删除（单线程连续写时每次析构都会
触发一次，删掉后热路径略省）。`closed_` 与计数的 seq_cst 握手顺序不变。这一项与管线
重构无关，长期保留。

## 4. 重构方案

### 4.1 裁决规则 R（key + LSN）

> 索引任务 `(key, L)` 生效，当且仅当应用时刻它仍是该 key 的最新版本；否则丢弃。
> 删除任务 `(key, T)` 同理：只有 T 仍是该 key 的最新版本时才执行删除。

配合两条结构保证，R 在任意到达顺序下给出与「按 LSN 顺序应用」相同的最终状态：

- **G1 权威先行**：写线程先写 data、更新 KV 索引（OKI），之后才提交索引任务。所以任务
  到达时，权威侧至少已包含它自己。
- **G2 同 key 串行**：任务按 key 哈希分发，同一个 key 的任务恒由同一个线程处理。

论证（同一 key 的两个版本 X < Y）：若 Y 先应用，X 到达时已非最新，丢弃；若 X 先应用，
此刻 Y 可能尚未写入，X 生效，随后 Y 到达并顶替 X。删除同理。不需要全局顺序，也不需要
跨 key 的任何协调。

「最新版本」的判定有两种实现，S45-1 选定：

| 实现 | 做法 | 优点 | 代价 |
|---|---|---|---|
| R-a 内存守卫 | docmap 的 `key → 最新 LSN` 表比较 LSN；删除保留墓碑 LSN | 纯内存，快；文本插件已有同款 | 墓碑要驻留（否则更旧的 put 会复活），与 S42 的「只存 live」目标冲突 |
| R-b 权威裁决 | 应用时查 OKI 中该 key 的最新 LSN，与任务 LSN 比较 | 单一真相源；无需墓碑 | 每次应用多一次 OKI 点查，冷 key 可能触盘 |
| R-c 混合 | docmap 有该 key 时比 LSN（内存）；docmap 无该 key 时才查 OKI | 覆盖写走内存；只有新 key / 已删 key 查权威 | 批量导入全是新 key，仍是每条一次点查（但刚写入的 key 在 memdelta 里，应为内存命中） |

初步倾向 R-c，以 S45-1 的实测为准。文本插件现有的 LSN 守卫保留，作为段级的第二道
保险和「找到被顶替的旧文档并标死」的定位手段。

幂等：同一任务重复应用（恢复重放）必须是 no-op。现有守卫是「已有 LSN **大于**我则
跳过」，相等时会重复 add；改为「大于等于」（S45-2）。

### 4.2 管线结构

```text
写线程：写 data + 更新 OKI（得到 LSN，在跟踪器登记）
   ├→ 分片 = hash(key) mod N → 该分片的有界队列
   └→ 文档带向量时，另送「串行道」队列
分片线程 s（N 个，池内共享、跨库）：
   裁决 R → 更新 docmap → 分词 → 写本库 slot[s] 的 building 段 → 登记定位 → 标记完成
串行道线程（1 个）：
   裁决 R → 向量插件 on_put / on_delete（HNSW 单写者不变）→ 标记完成
维护线程（1 个）：
   按库暂停 → 执行检查点 / run_serialized 闭包 → 恢复
```

- **没有任何线程等待某个特定 LSN。** 写线程只会被「目标队列满」挡住，分片线程只在
  「自己的队列空」时等待，二者不成环。2.1 的死锁在结构上不存在。
- 删除、失败补偿不再绕行：删除走同一分片（保证 G2）；失败路径直接在跟踪器标记完成。
- 文本插件自带的 BuilderPool 线程退役，宿主的分片线程就是 builder；
  `apply_*_in(slot, …)` 已按目标槽参数化，直接复用。
- 分片数沿用 `thread_limits.index_workers`。
- 一条带向量的文档有两个完成点（分片 + 串行道），两者都完成才算该 LSN 完成。

### 4.3 LSN 完成跟踪器

每库一个，取代 `applied_ord` / `submitted_ord_hwm` / Skip：

- `begin(L)`：LSN 分配时登记（锁内，O(1)）。
- `done(L)`：该 LSN 的全部应用点完成、或被裁决丢弃、或写失败时调用。**不阻塞、不分配、
  不抛异常**，可以安全地放在析构里（`OrdSkipGuard` / `BatchOrdGuard` 由此退役，连同其
  「析构里提交任务可能抛异常」的问题）。
- `contiguous()`：最大的 C，使得所有 LSN < C 都已 `done`。

实现为滑动位图（固定窗口，例如 2¹⁶ 位，按 LSN 取模定位），窗口基点随 `contiguous`
推进。LSN 超出窗口的 `done` 走一个小的溢出集合（mutex 保护，正常负载下为空）。

用途：

| 用途 | 现在 | 改后 |
|---|---|---|
| 读己之写（`flush_index` / `prepare_search`） | 等 `in_flight == 0 && applied >= hwm` | 入口取 `target = 下一个待分配 LSN`，等 `contiguous() >= target` |
| 检查点水位 | 提交时刻的 `peek_next_ord()`，靠 RunFn 排在其后保证覆盖 | `contiguous()`，见 4.5 |
| 序号空洞 | 必须补 Skip 任务 | `done(L)` |

读屏障顺带改善：现在的谓词在持续写入下可能长时间等不到 `in_flight == 0`；新谓词只等
调用时刻之前的写入，不受后续写入影响。

某个 LSN 永远不 `done`（写线程被 kill）时，`contiguous` 停住：检查点水位不再前进、
读屏障挂住。这与现状等价（现状是 reducer 停住），不是新问题；溢出集合超过阈值时
`log_error` 一次。

### 4.4 维护操作：按库暂停

检查点保存、段封口与合并、压实、HNSW 重建、docmap 回收等需要「该库此刻没有索引写入」。
现在靠排进 reducer 队列获得；改后由池内一个维护线程执行：

```text
pause(lane)：置暂停位 → 新提交在入口等待 → 等该库在途任务归零
执行闭包
resume(lane)：清暂停位 → 唤醒等待者
```

- `PluginHost::run_serialized(fn)` 语义不变（「独占、FIFO、fire-and-forget」），只是
  执行者从 reducer 换成维护线程。插件侧调用点不用改。
- 现有的死锁陷阱（`plugin_api.hpp` DL 陷阱 6：不得在应用回调里调 `run_serialized`）
  继续成立，形式变为：独占闭包内不得提交本库的索引任务。
- 新增两条纪律，S45-4 落地时逐点核对：
  - 持 `write_mu_` 时不得做阻塞式索引提交（`put_batch` / `put_batch_atomic` 的失败路径
    目前在锁内补交，要挪到解锁后）；
  - 独占闭包不得取 `write_mu_`。
- 首版检查点整个在暂停期内执行，与现状等价（现状是 RunFn 占住 reducer，期间该库索引
  应用停顿、靠队列缓冲）。后续可缩短为「暂停期只封口和取水位，序列化在暂停外」，
  本轮不做。

### 4.5 检查点与水位

```text
pause(lane)
wm = tracker.contiguous()        // 先取水位
各插件 flush(wm)                  // 封口各分片的 building 段、落盘段清单、docmap、向量
成对保存点（顺序不变：search 覆盖 ≥ KV 水位）
resume(lane)
```

- 先取水位再封口：水位之前完成的任务必然已在某个 building / sealed 段里，封口后落盘的
  状态覆盖 `[0, wm)` 全部。
- 落盘状态里允许包含 LSN ≥ wm 的文档（乱序完成的）。恢复时从 wm 重放会再次遇到它们，
  由规则 R 的幂等性丢弃。
- 暂停期间仍可能有写线程已拿到 LSN 但被挡在入口——它们的 LSN ≥ `contiguous()`，不在
  水位覆盖内，恢复后正常应用。这正是不能用「下一个待分配 LSN」当水位的原因。

### 4.6 恢复

- 起点不变：各组件水位的最小值；从该处按文件序 fold。
- 重放任务走与在线写相同的分片管线（并行、乱序），正确性由规则 R 和幂等保证。
  `PutEvent.replay` 标记保留，仅作执行策略提示。
- 启动重建 key 定位表改为比较 LSN，不再依赖段序。
- 可选的一致性核对（不在启动路径上）：遍历索引侧的 `key → LSN`，与 OKI 逐个比对，
  报告或修复偏差。全量核对是 O(总 key 数)，作为离线工具 / 诊断 API 提供。

### 4.7 插件契约变更

`plugin_api.hpp` 头注释与接口：

- `on_put` / `on_delete`：由「reducer 单写者，ord 严格升序」改为「**同 key 串行；不同 key
  可并发；到达序任意**；宿主已按规则 R 裁决，插件收到的都是当时最新的版本」。
- `PutEvent` / `DeleteEvent` 增加 `shard`（分片下标）；`OpenContext` 增加 `shards`（分片数），
  供插件预建每分片的 building 槽。
- 插件声明并发能力：`concurrency()` 返回「分片并发」或「串行」。串行插件的事件全部走
  串行道线程（HNSW、以及首版的 IVF / DiskANN 段式向量插件）。
- `prepare()`：保留为可选的纯函数预处理，但改为由分片线程在 `on_put` 之前同线程调用；
  独立的 map 阶段消失。
- `run_serialized`、`drain`、`flush`、`on_relocate` 等签名不变。`drain` 对文本插件变为
  空操作（不再有内部 builder 队列）。

### 4.8 背压与内存

- 每个分片队列有界（初值 1024，S45-6 校准），串行道队列有界。在途任务总量
  ≤ (N + 1) × 队列容量，硬上限，与批大小无关。
- 池跨库共享：某库的任务占满某个分片队列时，哈希到同一分片的其他库写入会被减速，
  但不会死锁。

## 5. 被删除的部件

| 部件 | 去向 |
|---|---|
| reducer 线程、`reducer_loop` | 删除 |
| 重排 ring、`ring_put` / `ring_grow`、`reorder_cap_`、`reorder_inflight_` | 删除 |
| map worker 池、`MapFn` / `ReduceFn` 两段回调 | 并入分片线程 |
| `IndexOp::Skip`、`OrdSkipGuard`、`BatchOrdGuard` | 由 `tracker.done()` 取代 |
| `IndexOp::RunFn`、为 RunFn 分配 LSN | 由维护线程 + 暂停取代 |
| `applied_ord` / `submitted_ord_hwm` / lane `in_flight` 的 flush 谓词 | 由跟踪器取代（暂停仍用在途计数） |
| `TextPlugin` 的 BuilderPool 线程与队列、`builder_threads` 配置 | 由宿主分片取代 |
| S45-0 的饥饿旁路 | 随 ring 一并删除 |
| `unregister_lib` 超时归还 ring 名额的逻辑 | 随 ring 删除；超时后 lane 的处置见第 8 节 |

## 6. 备选方案与取舍

| 方案 | 不选的原因 |
|---|---|
| 保留全局顺序，入口按 LSN 滑动窗口准入 | 能根治死锁且改动小，但保留了「全库排队等一个 LSN」的结构：单写者瓶颈、Skip 纪律、一个卡住的写线程拖停整库 |
| 严格提交定序器（写线程按 LSN 接力入队） | 热路径多一次线程间接力；S29-7 实测 futex 往返会吃掉组提交收益 |
| 撤销 H1，全部锁内提交 | 队列背压时冻结全体写者 |
| 在写线程里直接分词写段（无分片线程） | 结构最简单，但单线程批量导入失去并行分词 |
| 放宽为「稍后可见」（不保证读己之写） | 行为变更，下游有依赖；本轮保留严格语义 |

## 7. 测试

并发测试必须带**对照组**（去掉保护后能观察到违例），否则不算有效。

| 用例 | 阶段 | 内容 |
|---|---|---|
| 池层乱序死锁 | S45-0 | 小容量池（队列 8、`reorder_cap` 4、2 worker），先提交 LSN 1..N 再提交 0；`flush` 限时返回。对照：关旁路后超时 |
| 端到端批量 + 并发 put | S45-0 起长期保留 | 附录 A 负载在限时内完成，`prepare_search` 后文档数正确 |
| close 超时 | S45-0 | 测试接缝持住一个 `WriteOpGate`，注入 100ms；`close()` 约 100ms 返回并记 error |
| 乱序注入对拍 | S45-2 | reducer 仍在时，测试专用注入器在窗口内打乱应用顺序；随机 put / 覆盖 / 删除 / 复写序列，与内存模型逐 key 对比 docmap、BM25 命中、向量命中。对照：关掉裁决后必须出现幽灵或丢文档 |
| 重放幂等 | S45-2 | 同一批任务应用两遍，状态与一遍相同 |
| 跟踪器 | S45-3 | 乱序 `done` 下 `contiguous` 正确；窗口溢出；并发 TSan |
| 水位切换等价 | S45-3 | reducer 仍在时，跟踪器水位与 `applied_ord + 1` 恒等 |
| 同 key 串行 | S45-4 | 同 key 高频覆盖 + 删除，多线程写，终态等于最后一次操作 |
| 暂停独占 | S45-4 | 独占闭包执行期间断言无应用回调在跑；并发写不丢 |
| 崩溃恢复 | S45-5 | 各检查点阶段注入崩溃，重开后与模型一致；现有 crash 注入用例全绿 |

收尾：全矩阵 ctest、TSan 循环、ASan、吞吐与恢复时间基准。

## 8. 风险与待定问题

1. **docmap 是最难的一块。** 它以 LSN 为行键，带检查点增量日志（`delta_live_`、
   `removals_`、`delta_window_wm_`），`set_doc_len` / `set_meta` 按 LSN 寻址。这些在乱序
   应用下的语义需要 S45-1 逐项盘点后才能定稿 4.1 的实现选择。
2. **OKI 点查的代价和接口**（R-b / R-c）：`locate` 是否直接给出最新 LSN、热路径开销
   多大，未实测。
3. **向量插件的顺序假设**：HNSW 单写者由串行道保留；段式向量插件有 `replay_gate_`
   （`sealed_segment_vector_plugin.hpp:297`，LSN ≤ 门限即跳过），乱序下是否会误丢，
   需要逐个读。
4. **盘上格式不变是目标，不是已验证的结论。** 段已落盘 key + LSN，检查点水位语义不变，
   看起来不需要改格式；docmap 增量链若需调整则可能例外。
5. **文本与向量不再同一时刻可见**（分片与串行道各自完成）。读屏障等两者都完成，
   严格读己之写不受影响；不经屏障的查询可能短暂只命中其一。现状下两者在 reducer 内
   先后应用，同样不是原子的。
6. **`unregister_lib` 超时后的 lane 处置**：新结构下建议超时不销毁 lane 对象（移入墓碑
   列表），消除现有的已知 UAF 取舍。
7. **跨库共享分片的公平性**：一个库的大批量可能占满分片队列，拖慢其他库。是否需要
   按库配额，看 S45-6 数据。
8. **读己之写是否放宽**为可配置的「稍后可见」：本轮不做，留作后续。

## 9. 明确不做

- 不改 BM25 段格式、posting 结构、查询路径。
- 不做 HNSW 多写者。
- 不在写线程内联分词。
- 不放宽读己之写。
- 检查点不做「暂停外序列化」优化。

## 10. 实现记录（2026-11-17）

### 10.1 落地对照

| 设计项 | 实现 |
|---|---|
| §3 止血 | S45-0：饥饿旁路（仅在 reducer 已入睡时放行）+ close 有界轮询。旁路随 S45-4 删除；close 修复保留 |
| §4.1 规则 R | S45-2：R-b（OKI `locate`）为主，docmap LSN 守卫为辅（`Cask::index_put_is_current`）；删除任务不查 OKI，只删更旧版本。重放期关闭规则 R（见 §10.2-4） |
| §4.2 管线 | S45-4：`IndexPool` 重写为分片执行器（N 分片 + 串行道 + 维护线程）；宿主分发集中在新头 `index_dispatch.hpp`（`IndexDispatcher`，Cask 与契约测试共用） |
| §4.3 跟踪器 | S45-3：`lsn_tracker.hpp`（64K 位滑动位图 + 溢出 `std::set`），reducer 仍在时调试断言与 `applied_ord+1` 恒等 |
| §4.4 暂停 | Dekker 式 `active` / `paused` 原子 + 车道 cv；维护闭包自身计入 `active`（摘钩路径据此等它结束） |
| §4.5 检查点 | 闭包内取 `index_coverage()`（暂停时刻 LSN 前缀）；手动 checkpoint 先 `wait_lsn(peek_next_ord, 30s)` 保「覆盖到调用时刻」语义 |
| §4.6 恢复 | S45-5：重放车道走分片执行器；`replay_floor` 跳过已覆盖记录；墓碑重放为真实删除 |
| §4.7 契约 | `concurrency()` / `accepts_put()` / `accepts_delete()` / `PutEvent::shard` / `OpenContext::shards` / `PutEvent::replay` / `kReplayFloorMargin` |
| §5 删除部件 | 全部删除；`OrdSkipGuard` / `BatchOrdGuard` 保留类型但析构改为 `complete_index_ord`（noexcept，直接记完成） |
| §8-6 摘钩 | `unregister_lib` 超时：置 `detached`、等在执行的回调结束（同一上界）、车道入墓地 |

### 10.2 偏差与新增决定

1. **向量侧同样以 key + LSN 裁决**（用户 2026-11-17 定）：分片阶段并行完成后向量才进
   串行道，单写线程下也会乱序到达。HNSW 图退回纯图（无任何水位 / 重放门），乱序插入
   与重放幂等全部由向量插件的 key → (LSN, 墓碑) 定位表承担
   （`detail/vector_key_loc.hpp`，HNSW 与 IVF/DiskANN 共用；删除事件登记墓碑，覆盖
   水位之下的墓碑在 flush 时修剪）。**定位表随向量检查点持久化**（用户 2026-11-17 定
   方案 A）：base 写快照段 `kVecKeyLoc`（不含墓碑），delta 写窗口内新登记条目
   `kVecKeyLocDelta`，布局 `count u64; 每条 ord u64 | klen u32 | key`。重开时由
   base + 链直接重建，**不依赖 docmap**——与 BM25 段自带 (key, LSN) 对称；base 缺
   key 段（旧版检查点）或链上任一 delta 缺 key 段时回退为「节点 ord 经 docmap 反查」。
   仅追加段型、文件版本不动，7.0.x 读端静默忽略。图文件原要求节点 ord 随 id 严格递增，改为只校验头部最大值——字节布局不变；
   7.0.x 读端遇乱序文件拒收 → 全量重放重建向量（降级）。
   配套：① 恢复期 search 重放挪到 `finish_oki_recovery` **之后**单独一遍 fold，重放
   用**严格版规则 R**：OKI 已是终态，只有「命中、非墓碑、LSN 恰好相等」的版本放行，
   已删 / 已覆盖 / OKI 不认的版本一律丢弃（在线版「OKI 版本更旧则放行」的防御在重放
   时不成立——实测崩溃恢复后 OKI 对已删 key 可能报偏旧的墓碑 LSN，旧规则会放进中间
   版本成为死节点、下次重开再插一遍；该 OKI 缺陷本身也已修复——
   `IndexAuthority::rebuild_from_data` 账本模式补 tail 时，视图已是更旧墓碑也补行；
   严格规则保留，作为不依赖 OKI 墓碑 LSN 精度的防线）；② docmap 检查点的行与 meta 去掉 LSN
   上界（写全部已应用行），使「docmap 反查节点 key」覆盖乱序先完成、ord ≥ 覆盖水位
   的节点，也修复了这类行重开后丢 meta 的问题；③ 串行插件一律收到删除事件。
2. **重放起点余量 `kReplayFloorMargin = 4096`**：吸收 7.0.x checkpoint 的覆盖水位高估
   （旧水位取提交时刻 `peek_next_ord`）。插件重放门同样以 `wm − 4096` 为下界。
3. **docmap 同 LSN 重放为 no-op**（而非覆盖）：重放行的 `doc_len=0` 占位不得覆盖已回填
   的值；merge 搬迁改走新 `Index::relocate`。
4. **重放期规则 R**：初版因 fold 时 OKI tail 未补齐而关闭；终版把 search 重放移到 OKI
   收尾之后的第二遍 fold，用严格版规则 R（LSN 恰好相等才放行，见第 1 条）。重放任务在 docmap 已有该行时仍送达
   插件（组件水位可能不同，插件按 key+LSN 自判重）。
5. **文本自动压实**：应用路径不能再内联压实（多分片并发），改为经 `run_serialized`
   投递独占维护（`compact_pending_` 防重复投递）。
6. **`builder_threads` 忽略**：保留字段解析，open 时告警一次。
7. **分片队列容量 512**（初稿 1024 / 实现初版 2048）：S45-6 基准显示吞吐不随容量变，
   但读屏障与 checkpoint 延迟 ≈ 在途积压 ÷ 应用吞吐；512 使持续写入下读屏障 p50
   220 → 46 ms。串行道队列保持 4096。
8. **查询缓存失效代号**（收口中发现的既有 TOCTOU）：`SearchCache::generation()` +
   `put(..., observed_gen)`；`TextPlugin::on_delete` 的失效挪到标死之后。
9. **未做**：§8-7 按库配额（基准未见需要，见 `docs/design/s45-6-bench-zh.md`）；检查点
   「暂停外序列化」优化；读己之写放宽。

### 10.3 测试

| 用例 | 文件 |
|---|---|
| 执行器单测（LSN 完成、逆序提交、同 key 串行、分片并行、串行道、独占、背压、多库、摘钩） | `tests/thread_pool_test.cpp` |
| LsnTracker 单测 | `tests/lsn_tracker_test.cpp` |
| 插件契约（复用 `IndexDispatcher`） | `tests/plugin_contract_test.cpp` |
| 活性：原复现负载、持续写入下读屏障不饿死、close 超时 | `tests/index_pool_liveness_test.cpp` |
| 乱序裁决对拍（真并发 + 提交抖动，KV 为预言机；对照组关裁决必现违例）、重放幂等 | `tests/index_unordered_apply_test.cpp` |
| 并发写中崩溃 → 重开索引与 KV 一致（两轮） | `tests/index_crash_recovery_test.cpp` |

## 附录 A：复现程序

用法 `./repro <批大小> <put线程数> <无进展秒数>`；无进展达到阈值时打印 `DEADLOCK` 并以
2 退出。链接方式同 `tests/` 下用例（`build-gcc-release/build.ninja` 中
`bitcask_atomic_batch_doc_test` 的 `LINK_LIBRARIES`，去掉 gtest）。

```cpp
#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace bitcask;
static std::span<const std::byte> B(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
int main(int argc, char** argv) {
    const std::size_t batch_n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 60000;
    const int putters = argc > 2 ? std::atoi(argv[2]) : 4;
    const int timeout_s = argc > 3 ? std::atoi(argv[3]) : 60;
    auto dir = std::filesystem::temp_directory_path() /
               ("lb-repro-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    static oki::IndexRegistry reg;
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    search::SearchLayerConfig scfg;
    scfg.analyzer_config.type = text::AnalyzerType::Whitespace;
    opts.search_config = scfg;
    auto oc = Cask::open(dir.string(), opts, &reg);
    if (!oc) { std::printf("open failed: %s\n", oc.error().detail.c_str()); return 1; }
    auto c = std::move(*oc);

    std::vector<std::string> keys(batch_n), vals(batch_n);
    std::vector<Cask::BatchItem> items(batch_n);
    for (std::size_t i = 0; i < batch_n; ++i) {
        keys[i] = "b" + std::to_string(i);
        vals[i] = "hello world batch doc number " + std::to_string(i);
        items[i] = {B(keys[i]), B(vals[i])};
    }
    std::atomic<bool> batch_done{false};
    std::atomic<std::uint64_t> puts{0};
    std::atomic<int> finished{0};
    std::thread tb([&] {
        if (!c->put_batch(items)) std::printf("put_batch err\n");
        batch_done = true; ++finished;
    });
    std::vector<std::thread> tp;
    for (int t = 0; t < putters; ++t) tp.emplace_back([&, t] {
        std::uint64_t i = 0;
        while (!batch_done.load()) {
            std::string k = "p" + std::to_string(t) + "_" + std::to_string(i++);
            (void)c->put(B(k), B("single put doc text"));
            puts.fetch_add(1, std::memory_order_relaxed);
        }
        ++finished;
    });
    std::uint64_t last = 0; int stalled = 0;
    while (finished.load() < putters + 1) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto p = puts.load();
        stalled = (p == last) ? stalled + 1 : 0; last = p;
        if (stalled >= timeout_s) {
            std::printf("DEADLOCK: no progress for %ds: batch_done=%d puts=%llu\n",
                        stalled, int(batch_done.load()),
                        static_cast<unsigned long long>(p));
            std::fflush(stdout);
            std::_Exit(2);
        }
    }
    tb.join(); for (auto& t : tp) t.join();
    std::printf("OK: batch=%zu puts=%llu\n", batch_n,
                static_cast<unsigned long long>(puts.load()));
    c->close();
    std::filesystem::remove_all(dir);
    return 0;
}
```
