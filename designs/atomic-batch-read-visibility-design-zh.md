# 原子批对并发读者整批可见 设计

状态：**已实现（S63，2026-11-20；12.0.1 已发版）**。方案 A 已拍板并落地；第 8 节各项已定。
来源：下游反馈 [`feedbacks/2026-11-20-atomic-batch-no-read-isolation.md`](../feedbacks/2026-11-20-atomic-batch-no-read-isolation.md)
（keelson 压测撞上；bitcask 图层审代码确认同一根因）。
关联：`src/cask/cask.cpp` `Cask::put_batch_atomic`、`include/bitcask/index_authority.hpp`（写权威，`writer_mu_`）、
`include/bitcask/oki_state.hpp`（active 行 + 已发布视图，`mu_`）、`doc/atomic-batch-design-zh.md`（S35 崩溃原子性）、
S46 条件写（`feedbacks/2026-11-17-conditional-atomic-batch.md`）。

## 1. 结论（先说推荐）

- **推荐方案 A：整批在权威索引里一次性发布。** 批内各 key 的行在同一个临界区里进入 OKI 的 active 段，读者要么看到整批、
  要么一条都看不到。读路径不用改，不加锁，不加重试。
- 保证的是**原子可见**，不是快照隔离：读者读到批内任何一个 key 的新值之后，再读批内其它 key 一定也是新值（或更新的批）。
  两次 `get` 之间插进来一个完整的批仍然可能，这种情况由下游的头条件挡住——这正是「头即版本」设计的前提。
- 盘上格式不变。C API 不变。C++ 只给 `OkiState` / `IndexAuthority` 各加一个非虚成员函数，**不破 ABI**，建议按 PATCH 发
  （暂定 **12.0.1**，未发版）。
- 同批顺带做：`put_batch_atomic` 注释改准；把「只含 `kCheck` 的批 = 写锁下校验一组版本」写成文档承诺；补回归用例。
- 已知代价：大批应用期间点查会被挡在 `OkiState::mu_` 外，时长与批大小成正比（见 5.1）。

## 2. 现状与病因

### 2.1 今天批是怎么生效的

`Cask::put_batch_atomic`（`cask.cpp:2450`）在 `write_mu_` 内：

1. 条件判定（S46）→ 编码 → 批头 + 成员写缓冲 → 一次 flush（崩溃原子性的提交点）；
2. `final_ops`（`cask.cpp:2554`）是按 key 字节序的 `std::map`，每个 key 只留批内最后一个 op，`apply_order` 由它生成；
3. 按 `apply_order` **逐条**调 `authority_->apply_put` / `apply_remove`（`cask.cpp:2823` 起）。每一条各自取一次
   `IndexAuthority::writer_mu_`，内部再取一次 `OkiState::mu_` 追加一行。

读路径 `read_doc`（`cask.cpp:1780`）只做 `authority_->state().locate(key)`：取 `OkiState::mu_` 查 active 哈希，再看已发布视图。
不取 `write_mu_`，也不取 `writer_mu_`。

所以第 3 步每应用完一条，那个 key 就对读者可见。批的中间态持续「剩余成员数 × 单条应用耗时」。

### 2.2 两个容易误解的点

| 下游以为 | 实际 |
|---|---|
| 批对读者是原子的（`cask.hpp:765` 注释「进程内 all-or-nothing」） | 只对**崩溃恢复**和**条件判定**是 all-or-nothing；对并发读者逐条可见 |
| 可以靠 op 顺序控制谁先可见（注释「批内 op 依序 apply」） | 生效顺序是 **key 字节序**；「依序」只管同 key 多次时谁是最终值 |

`txn.hpp:8` 写的「不提供隔离性」是对的，但下游多数直接用 `put_batch_atomic`，看的是 `cask.hpp` 的注释。

### 2.3 复现

写了一个回归用例验证病因（用例暂存在开发机镜像，未进仓库）：一个批写 `头 + 400 个填充 key + 尾`，全部写成同一个版本号；
3 个读者线程反复「读头 → 读尾」，断言尾的版本不落后于头。在 12.0.0（`62dd725`）GCC Release 上：

| 用例 | 12.0.0 结果 |
|---|---|
| 先读头后读尾，尾不得落后 | **失败**：107100 次读里 73485 次读到「新头 + 旧尾」 |
| 含删除的批（偶数轮删尾），头与删除同批切换 | **失败**：5073 次读到半个批 |
| 先读尾后读头 | 通过（按 key 序应用，头总是先生效，反向读看不出来） |
| 4 线程「读头 → 读尾 → 带头条件写回」计数 | 通过，未复现丢更新 |

最后一条没复现的原因：各线程大部分时间堵在 `write_mu_` 上，读发生在自己的批刚返回时，下一个写者还在编码 / 写盘阶段，
撞不进应用窗口。它不能说明没问题——丢更新只需要读者在窗口内读到一次新头。实现时这条用例要改成「盯着头变化再读块」才有判别力（见第 7 节）。

## 3. 要保证什么

**S63 保证（进程内，共享同一个 `IndexAuthority` 的所有句柄）：**

> 一次成功的 `put_batch_atomic` 对 `get` / `get_owned` / `read_doc` / `CaskIter` / `CaskRangeIter` 是一次性生效的：
> 不存在某个时刻，批内一部分 key 读到新值而另一部分读到批前的值。

由此推出下游要的性质：读者先后读批内两个 key，如果第一个读到了这个批写的值，第二个读到的一定是这个批或更晚的批写的值。
「头 + 块、只对头设 `kOrdEquals`」因此成立：

- 读到新头 ⇒ 块也是新的；
- 读到旧头、之后批才生效 ⇒ 块可能是新的，但写回时头条件不成立，整批被拒。

**不保证的（写进文档）：**

1. **不是快照。** 两次 `get` 之间可以插进完整的批。要多 key 一致读，用头条件写回，或用只含 `kCheck` 的批校验（第 6 节）。
2. **`CaskRangeIter` 仍是 per-key 弱一致**（它逐 key 回查取值）。迭代途中生效的批，可能前半段 key 是旧值、后半段是新值——
   这是迭代器既有契约，不属于「半个批」；只保证不会出现「批前值排在批后值之后被读到」的倒退。`CaskIter` 是视图快照，整批全有或全无。
3. **检索索引（文本 / 向量 / meta 过滤）不在内。** 批内 `kPutDoc` 的索引登记仍在锁外异步提交，`search_*` 看到的时刻晚于
   `get`，且各文档不同步。要读自己写的，沿用 `flush_index()`。
4. **`put_batch`（非原子批）不变**，仍逐条生效。
5. **跨进程读者**（另一个进程里的只读句柄）不在本项范围，本设计没有核查那条路径。

## 4. 方案

### 4.1 方案 A：一次性发布（推荐）

权威索引里，「对读者可见」等于「行进了 `OkiState` 的 active 段」。active 段和点查哈希都由 `OkiState::mu_` 保护，
`locate`、`make_read_view`、flush 取切面都要拿这把锁。所以只要**整批的行在一个 `mu_` 临界区内追加**，读者就看不到中间态。

三层各加一个入口：

**`OkiState::apply_batch(span<const BatchRow>)`**

```cpp
struct BatchRow {
    std::string_view key;
    std::uint64_t ord;
    bool tomb;
    RowLoc loc;               // 墓碑忽略
    std::uint32_t dur_file;   // 仅墓碑：data 记录落点
    std::uint64_t dur_end;
};
void apply_batch(std::span<const BatchRow> rows);
```

一次 `lock_guard(mu_)`：逐行 `push_back` + 分配连续的到达序号 + 登记点查哈希；`delta_version_` 只加一次。
单行逻辑与 `append_update` 相同。

**`IndexAuthority::apply_batch(span<const BatchApply>, vector<size_t>& rejected)`**

```cpp
struct BatchApply {
    std::string_view key;
    bool remove;
    std::uint32_t file_id;      // remove：墓碑记录的落点
    std::uint32_t total_size;
    std::uint64_t offset;
    std::uint64_t tstamp;
    std::uint64_t ord;
};
[[nodiscard]] PutResult apply_batch(std::span<const BatchApply> items,
                                    std::vector<std::size_t>& rejected);
```

一次 `unique_lock(writer_mu_)`，分两段：

1. **裁决段（不改任何状态）**：对每个成员 `probe_for_write`。put 成员的裁决口径与 `apply_put` 逐字相同——
   不可判定 ⇒ 返回 `kIndexUnavailable`；`file_id < biggest_file_id_` 或视图里已有更大 ord ⇒ 记入 `rejected`。
   只要有一个被拒，返回 `kAlreadyExists`，**零应用**。
2. **应用段**：按各自的 prior 记账（`install_live` / `retire_live` / `adjust_file`，与 `apply_put` / `apply_remove` 相同，
   含「同 ord 同落点已在视图」的幂等跳过），攒出行，调一次 `OkiState::apply_batch`，最后逐个 `note_applied`。

前置条件：`items` 内 key 两两不同。`put_batch_atomic` 的 `final_ops` 已经保证。先取齐 prior 再记账依赖这一点。

**`Cask::put_batch_atomic` 第 ⑥ 步**改成：

```text
items = apply_order 里每个成员的 (key, 类型, 落点, ord)
循环（最多 3 次）：
    pr = authority_->apply_batch(items, rejected)
    kOk               → 跳出
    kIndexUnavailable → 返回 kIndexCorrupt（零成员生效）
    kAlreadyExists    → roll_active()；被拒成员各分配新 ord、单条重写到新 active 文件，
                        更新 items 里的落点与 ord；再试
第 3 次仍被拒 → 返回 kAlreadyExists
```

之后的索引任务提交、`outcome->ords` 回填、组提交与今天相同；被重写成员的旧 ord 仍由 `BatchOrdGuard` 补 Skip。

### 4.2 方案 B：读路径 seqlock 重试（不推荐）

批开始应用时把一个原子计数器加到奇数，应用完加到偶数；读者读前读后比对，奇数或变了就重试。

- 单次 `get` 只读一个 key，一次读本身没有「半个批」可言；要挡的是**跨多次 `get`** 的不一致，库看不见读者的读集边界。
  真要做成，只能让读者在「有批正在应用」时等批结束——等价于方案 A 的效果，但每次点查多一次原子读，还要处理写者被抢占时读者空转。
- 迭代器、range、条件判定各处读路径都要加同一套逻辑，漏一处就是一个新洞。

### 4.3 方案 C：只改文档（不推荐单独做）

把契约写准，让下游给整个读集加条件。反馈第 2 条的兜底。问题是每个下游都要知道这件事并自己做对；
bitcask 图层三处、keelson 一处已经各踩一遍。文档修正在方案 A 里照做（第 6 节），但不拿它当修复。

### 4.4 对比

| | A 一次性发布 | B seqlock | C 只改文档 |
|---|---|---|---|
| 读路径改动 | 无 | 每处读路径 | 无 |
| 点查额外开销 | 无（大批期间等锁，见 5.1） | 每次多一次原子读 + 可能重试 | 无 |
| 下游要改吗 | 不用 | 不用 | 都要 |
| 改动面 | 3 个函数 | 读路径全部 | 注释 |
| ABI | 纯新增 | 纯新增 | 无 |

## 5. 方案 A 的细节与风险

### 5.1 大批期间读者等锁

今天每行各取一次 `mu_`，读者可以在行与行之间插进来；改后整批一次持锁。持锁时间 ≈ 行数 × 单行追加（一次 `std::string` 拷贝 +
一次哈希插入，百纳秒量级）：400 行的批约几十微秒，4 万行的批到毫秒级。期间 `locate` / `make_read_view` 等待。

这是原子可见的直接代价，方案 B 也躲不开（读者同样要等批结束）。可以减的部分：`apply_batch` 进临界区前把 `DeltaRow`（含 key 拷贝）
在锁外构造好，锁内只做 move + 哈希插入。建议实现时就这么做，并用现有基准（`bench/`）量一组「大批 + 并发点查」的 p99，结果写进落地记录。

### 5.2 flush / 一致切面不会把批切开

- flush 的冻结切面：带快照时是 `capture_commit_cut` 在 **`writer_mu_`** 内取的 `current_seq()`；不带快照时在 **`mu_`** 内遍历 active。
  批的应用横跨这两把锁各一个完整临界区，切面只能落在批前或批后。
- 这顺带修掉一个今天存在的小缝：逐条应用时，一致切面可以落在批中间，manifest 记下的覆盖水位对应「半个批已入账」的状态
  （崩溃恢复靠批头区间兜住，不会读错，但水位口径不干净）。
- 批内行的到达序号连续，`frozen_through` 的过滤（丢弃 `seq ≤ frozen_through` 的 active 行）对整批同进同出。

### 5.3 受控重建

`rebuild_from_data` 全程持 `writer_mu_`。批的字节在应用前已经 flush，所以重建要么在批应用之前跑完（扫到了整批记录，随后
`apply_batch` 走幂等跳过），要么在之后。不会出现重建只收了半个批。

### 5.4 merge 竞争的重写路径

今天被拒成员走 `write_and_keydir` 单条重写并立即应用，其余成员照常逐条应用。改后被拒成员先重写落盘（不应用），再整批重试，
所以**这条路径上读者可见性同样是整批的**。

没变的：重写出来的记录落在批头声明的区间之外，这几条的**崩溃原子性**仍然是今天的样子（既有行为，`cask.cpp` 注释
「区间之外的独立完整记录」）。本项不动它。

与今天的一处差异：今天第一次重写直接写进当前 active 文件，再被拒才换文件；改后被拒就先 `roll_active()`。
该路径只在 merge 恰好把 `biggest_file_id` 推过批文件时触发，多换一次文件可以接受。

### 5.5 `kIndexUnavailable`

今天应用到一半遇到不可判定，已应用的成员保留、其索引任务补交，后面的成员丢在盘上等重建。改后裁决段先于任何状态变更，
不可判定 ⇒ 零成员生效，行为更干净。批的字节已经落盘，后续受控重建会把整批收进视图——与今天「后半批」的处境相同。

### 5.6 没动的地方

单条 `put` / `remove` / `put_doc`、组提交、`put_batch`、恢复重放、merge 搬迁都不经过新入口。`IndexAuthority::apply_put` /
`apply_remove` 保留。

## 6. 文档与契约

**`cask.hpp` `put_batch_atomic` 注释**改写两句、加一段：

- 「除进程内 all-or-nothing 外」→ 写明三件事各自的范围：条件判定全批零副作用、崩溃后全有或全无、**对并发读者一次性生效**（S63 起）。
- 「批内 op 依序 apply」→「同 key 在批内多次出现时最后一个 op 生效；各 key 之间没有可观察的先后」。
- 新增「不保证」清单（第 3 节 1～5）。

**只含 `kCheck` 的批**（反馈第 3 条）写成承诺：

> 批内全部是 `kCheck` 时，本调用在写锁内校验全部条件后返回，不写任何字节、不分配 LSN、不升级 `bitcask.meta`。
> 可用它确认一组先前读到的 `(key, ord)` 此刻仍是当前版本。

今天的实现已经是这样（`cask.cpp:2539` 提前返回，早于 meta 懒升级）；`ConditionalBatchTest` 第 4 条覆盖了「不写」。
要补的是注释、`c_api/bitcask_kv.h` 对应说明、以及一条「不升 meta 版本」的断言。

**专门的 `validate(keys, ords)` 入口**（不要求可写句柄）：建议**不做**。校验要有意义就得和写者互斥，只读句柄拿不到写者的
`write_mu_`（跨进程更拿不到）；同进程可写句柄用 `kCheck` 批已经够。列在第 8 节待定。

**`txn.hpp:8`**「不提供隔离性——事务中间态对并发读者可见」改成与新契约一致：提交对读者一次性生效，但不是快照隔离。

## 7. 测试

新文件 `tests/atomic_batch_visibility_test.cpp`（第 2.3 节的用例定稿后入库）：

1. 先读头后读尾，尾的版本不落后（12.0.0 上红）。
2. 先读尾后读头，同上（可见性与 key 序无关）。
3. 含删除的批：头与删除同批切换（12.0.0 上红）。
4. 「头 + 块、只对头设条件」的读-改-写不丢更新。读者改成盯着头的 LSN 变化、一变就读块，保证能撞进应用窗口；
   定稿前先确认它在 12.0.0 上红。
5. `CaskIter`：迭代途中并发写批，单个迭代器内同一批的 key 版本一致。
6. merge 竞争路径：用 `set_pre_apply_hook_for_test` 在应用前推高 `biggest_file_id`，断言被拒成员重写后整批一次可见、
   `outcome->ords` 等于随后 `get` 的 ord。
7. 只含 `kCheck` 的批不升 `bitcask.meta` 版本。

回归面：`atomic_batch_test` / `atomic_batch_doc_test` / `conditional_batch_test` / `s62_meta_multivalue_test` / S61 `update_doc` /
`oki_only_contract_test` 全绿；GCC Release 全量 + ASan 全量；新用例加跑 TSan。

## 8. 已定项（2026-11-20 拍板）

| # | 问题 | 结论 |
|---|---|---|
| 1 | 做方案 A，还是只改文档（C） | 方案 A |
| 2 | 版本号：行为修复 + C++ 纯新增成员函数 | **12.0.1**（PATCH，未发版） |
| 3 | 要不要专门的 `validate` 入口 | 不做，文档承诺 `kCheck` 批 |
| 4 | `put_batch`（非原子批）要不要也整批可见 | 不做，它没有这个承诺；想要的下游改用 `put_batch_atomic` |
| 5 | 检索索引与 KV 同步可见 | 不在本项；需要时另立项 |
| 6 | 跨进程只读句柄的批可见性 | 本项不核查；下游有需求再提 |

## 9. 任务拆分（已落地）

| 步 | 内容 | 验收 |
|---|---|---|
| S63-1 | `OkiState::apply_batch`（锁外构造行） | 单测：批内行到达序号连续；并发 `locate` 不见半批 |
| S63-2 | `IndexAuthority::apply_batch`（裁决 / 应用两段） | 账本与逐条 `apply_put` / `apply_remove` 结果逐字段相同 |
| S63-3 | `put_batch_atomic` 第 ⑥ 步改造 + merge 竞争重写 | 第 7 节用例 1～6；既有批用例全绿 |
| S63-4 | 注释 / C 头 / `txn.hpp` / CHANGELOG `[Unreleased]` | 第 7 节用例 7 |
| S63-5 | 基准：大批 + 并发点查 p99（5.1） | 数字写进 TASK.md 落地记录 |
| S63-6 | 回填反馈文件「回没回」、`feedbacks/README.md` 末列 | — |
