# 原子批没有条件写 —— 「读了再写」的下游只能靠外部锁，否则丢更新

**报的人**：bitcask（下游 Erlang 封装，pin 7.0.3 `ce7ce13`）。起因：图层 v2 设计
（bitcask 仓库 `doc/graph-adjacency-pack-design-zh.md`）把一个顶点的全部边打包进一个
value，key 数量从「3 × 边数」降到「2 × 顶点数」；代价是每次加 / 删边都是对这个 value
的读-改-写。
**严重度**：🟡 能力缺口。不报错——并发的读-改-写静默丢更新。

## 症状

一个字都不报。两个写者各自 `get(k)` → 修改 → `put_batch_atomic([{put, k, v'}])`，
后提交的覆盖先提交的，先提交的那次修改消失。

## 位置

- `include/bitcask/cask.hpp:760` `BatchOp { type; key; value; doc; }`：没有前置条件的位置。
- `doc/multikey-txn-zh.md:178`：明确写着「不提供 CAS」。
- `GetResult::ord`（`cask.hpp:290`）已经把 key 的当前 LSN 交给了调用方，但写入侧没有
  办法引用它。

## 怎么撞上的

任何「值是聚合体」的下游都会撞上：

- 现有的：bitcask `graphdb` 的 `deg` / `degi` 度数计数器（读-判-写，文档里只能写
  「单写者语义下精确」），`bitcask_query_index` 的二级索引维护。
- 新的：图层 v2 的邻接表。加一条边 a→b 要同时重写 a 和 b 两个顶点的邻接表（同一个
  原子批）。两个写者并发往同一个顶点加边，后写的覆盖先写的，**丢的是边，不只是计数**。

## 影响面

下游现在的绕法是 BEAM 侧的锁管理进程（`bitcask_txn`，2PL + 死锁检测）：

- 每次写多一次进程往返，单边写吞吐实测约为直通写的 0.4×；
- 绕过锁管理直接写的调用方不受保护，所以文档里只能要求「同一张图要么全走事务、
  要么全走直通 + 单写者」；
- 跨进程 / 跨语言的消费者（C API、clj、C#）各自要再造一遍。

判断「我读到之后有没有人改过」必须与写入原子，只有持着 `write_mu_` 的引擎做得到。

## 建议（下游不替 libbitcask 拍板）

1. **`BatchOp` 加前置条件**，三种取值：无条件（现状）/ 期望 key 不存在 /
   期望 key 当前 LSN 等于给定值。例如：

   ```cpp
   struct BatchOp {
       ...
       enum class Expect : std::uint8_t { kNone = 0, kAbsent = 1, kOrdEquals = 2 };
       Expect expect = Expect::kNone;
       std::uint64_t expect_ord = 0;      // 仅 kOrdEquals
   };
   ```

2. **语义**：
   - 全部条件在 `write_mu_` 内、任何记录落盘之前判断；任一不成立 ⇒ 整批零副作用，
     返回专门的 fault（如 `kConditionFailed`），并带出首个不成立的 op 下标或 key——
     下游要靠它区分「冲突，重试」与「真的出错」。
   - 「不存在」包含：从未写过、已删除（墓碑）、已过期（`expiry_secs` / `expiry_at`）。
   - 批内同一个 key 出现多次时，条件一律对照**批开始前**的状态判断。
   - `kPutDoc`、`kRemove` 同样可带条件。
   - 条件只在写入时判断，落盘的仍是普通记录，恢复重放不需要感知。

3. **需要上游确认的契约**：一个 key 的 LSN 只在它被写入时改变——merge 搬迁、重开
   恢复（含 hint / keydir 检查点 / 全量 fold 重建）、OKI 重建都不改变它。下游据此把
   LSN 当版本号用；若有例外路径请指出，下游改用别的版本来源。

4. **可选**：
   - 只校验不写的 op（`kCheck`）：事务层想把「读过但没改的 key」也纳入提交校验时用。
   - 批成功后返回各 op 分到的 LSN（或首个 LSN + 连续性保证）：写者可以不重读就继续
     下一轮读-改-写。
   - C API 同批跟进（纯加法，参照 6.6.2 加 `bitcask_txn_op_ex_t` 的做法）；
     `TxnCask::commit` 是否透传由上游定。

## bitcask 这一侧怎么处理

图层 v2 的只读路径、值编码、迁移工具不依赖本条，先做。写路径在本条落地前经
`bitcask_txn` 的点锁串行（直通写 API 内部也走这把锁）；落地后改成「条件写 + 冲突
重试」，事务提交也带上条件，直通写与事务写即可混用。NIF 侧配套（`get` 带回 LSN、
批操作解析新形态、冲突错误映射）在 bitcask 仓库做。

## 状态

已修（**8.0.0 已发版**，S46）。

## 怎么修的

按建议 1–4 全做，接口形态与建议一致：

- `Cask::BatchOp` 加 `Expect expect`（`kNone` / `kAbsent` / `kOrdEquals`）+
  `expect_ord`，新 op 型 `kCheck`（只校验不写）；新错误 `CaskError::kConditionFailed`。
- `put_batch_atomic(ops, tstamp, BatchOutcome* outcome = nullptr)`：失败时
  `outcome->failed_op` 是首个不成立的 op 下标；成功时 `outcome->ords[i]` 是
  第 i 个 op 落盘记录的 LSN（`kCheck` 为 0），可直接当下一轮期望值。
- 判定在 `write_mu_` 内、任何字节进写缓冲之前，逐个调用与 `get()` 同一条读
  路径（`read_doc`）——「不存在」的口径（删除、`expiry_secs`、`expiry_at`）与
  `get()` 返回 kNotFound 逐项相同；索引不可判定时原样返回 `kIndexCorrupt`，
  不会被当成「不存在」。批内同 key 多次一律对照批前状态。
- LSN 契约（建议 3）核对过、写进了头文件：merge 搬迁（`merger.cpp`
  `copy_oki_record`）原样写回 record 的 ord；重开、OKI 重建都从 record 读回
  ord。唯一会让一个 op 的 LSN 与「批里分到的 ord」不同的是 merge race 下的
  单条重写——这种情况 `outcome.ords` 给的是重写记录的 ord，即 `get()` 随后
  返回的值，下游无需感知。
- `TxnOp` / `TxnCask::commit(ops, outcome)` 透传。
- C API 纯加法：`BITCASK_ERR_CONDITION_FAILED = 17`、`bitcask_cond_op_t`、
  `bitcask_put_batch_atomic_cond` / `bitcask_txn_commit_cond`（出参
  `out_ords` / `out_failed_op` 可空）。ord 由既有 `bitcask_get_result_t.ord` 读取。

回归：`tests/conditional_batch_test.cpp`（10 例：存在性、ord 链式改写、删除 /
过期算不存在、kCheck、批内同 key、形态校验、merge / 重开 / 全量重建后 ord 不变、
8 线程并发计数器自增精确 + 无条件对照组确实丢更新、TxnCask 透传）；C
`test_conditional_batch`。

代价：每个带条件的 op 在写锁内多一次定位 + pread（刚 `get` 过的通常命中 page
cache）；不带条件的批没有额外开销。
