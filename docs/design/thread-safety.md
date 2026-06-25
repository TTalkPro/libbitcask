# 线程安全化分析与工作方向（Thread-Safety Hardening）

> 来源：2026-06-25 对 `Cask` / `CaskIter` 对外 API 的并发契约审计。
> 目的：把当前「调用方负责串行化写」的外部契约,评估是否/如何**内化**为
> 接口级线程安全,并指定分阶段工作方向。
>
> ⚠️ 本文是**方向性设计稿**,非实现记录。
> **定位（2026-06-25 定向,见 §6）**：libbitcask 作为**通用 C++ 库**,而非
> 仅服务 Erlang/NIF「一进程一 Cask」模型。该定位下 W1+W2+W3 为必做组。

## 1. 当前并发契约

**单写者 + 无锁多读者**,由调用方保证。

- 设计前提（cask.hpp 顶部「线程模型」）：「一个 Erlang 进程持有一个 Cask
  resource」,因此 Cask **自身不做内部并发写控制**。
- 底层 KeyDir 可在同目录多个 Cask 间共享（`KeyDirRegistry` 管 refcount）;
  多写者要求调用方自己串行化。
- 读路径无锁：keydir get + `DataFile::pread` 线程安全,`read_files_` cache 受
  `read_cache_mu_` 保护。

## 2. 竞态点精确定位（已核实代码）

### A. 写者 vs 写者 —— 真正的缺口

| 共享可变态 | 位置 | 现状 |
|---|---|---|
| `DataFile::write` 的 `current_offset_` / `write_buf_` / `batch_buf_` | `data_file.cpp` | **无保护**（成员,非 atomic）;两写者并发即数据损坏 |
| `writes_since_sync_`（组提交计数） | `cask.cpp` `maybe_group_commit` | **无保护** |
| `active_file_id_` / `active_data_` | `ensure_active_writer` / `roll_active` | reset 已在 `read_cache_mu_` 下,但**整个写序列无互斥** |
| keydir put / LWW | `keydir.cpp` | ✅ 已安全（`shared_mutex` + 原子 `ord`/`file_id`） |
| 索引任务提交 | `IndexPool` | ✅ 已安全（MPSC 队列 + **S6 reorder buffer 本就支持任意到达序按 ord apply**） |

→ **结论：多写者唯一缺的是「active 文件写序列」的互斥。** keydir 与索引池
（S6 双池 + reorder buffer）早已为并发写提交设计就绪。

### B. 读者 vs 写者 —— 基本已安全

- `get` / `get_owned`：✅ **真安全**（`active_data_` 指针受 `read_cache_mu_` 守、
  `pread` 线程安全、keydir `shared_mutex`）。
- `search_text` / `phrase` / `near` / `bool_search` / `search_fields` / `search_fuzzy`
  / `search_wildcard`：✅ **结构级已安全**（S6/S7 TSan 已证：`cache_`/`doc_texts_`
  各 `shared_mutex`、倒排/HNSW `shared_lock`、analyzer const）。头文件「线程安全:否」
  是**保守注释**,非真不安全。
- `search_vector` / `search_*_batch`：✅ 已明确安全（HNSW `atomic<shared_ptr>` 快照;
  批量入口专为 inter-query 并发设计）。

### C. 迭代器 / close —— 语义 / 生命周期问题（非数据竞争 bug）

- 同一 `CaskIter` 的 `start`/`next`/`release` 并发：cursor（有状态游标）语义本就
  模糊,价值低。**不同** CaskIter 对象并发使用同一 Cask 已安全。
- `close` vs 在途操作：use-after-free / use-after-close,本质是生命周期,标准
  资源句柄都要求 caller 不在使用时关闭。

## 3. 工作方向（分阶段）

### W1 — 写路径内部串行化【核心】

加 `std::mutex write_mu_`,覆盖写序列：`put` / `remove` / `put_doc` / `sync` /
`close_write_file` / `flush_index`（连同其内部的 `ensure_active_writer` /
`roll_active` / `maybe_group_commit` / `write_and_keydir`）。

- **语义**：把「外部串行契约」内化为「内部互斥」。写本来就串行 → **吞吐不变**
  （只加 ~20ns 锁开销,相对 pwrite/fsync 的 µs–ms 可忽略）;真多写者 caller 获得正确性。
- **不动读路径**（读不取 `write_mu_`,保持无锁）。
- **锁序**：`write_mu_` 最外层 → 内部再取 `read_cache_mu_` / keydir 锁（写路径不持
  读锁,无反向依赖 → 无死锁）。merge 走独立路径（keydir `shared_mutex` 协调）,
  `write_mu_` 不触 merge。
- **验证**：新增「N 线程并发 put/remove 同一 Cask」TSan 测试。
- **风险：低**。

### W2 — 读/搜索并发确认 + 注释订正【便宜,收尾】

- 补「并发文本搜索 + 并发写」TSan 测试;把 `search_text` 等的「线程安全:否」订正为
  准确描述（「并发读安全;与写的可见性遵循 near-real-time 契约」）。
- **风险：零**（测试 + 文档）。

### W3 — 生命周期硬化【低优先,便宜】

- 加 `std::atomic<bool> closed_`,公共方法入口检查 → 已关闭返回 `kInvalidOption`
  而非 UB（fail-fast,尤其防 C API 多线程 host 误用）。
- **不做**完整 rundown（关闭时等在途操作完成）——成本高、价值低。
- **风险：低**。

### W4 — 迭代器并行扫描【可选,按需】

- 不让单 iterator 并发（语义模糊）;如需并行遍历,提供「keyspace 分区 + N 个独立
  iterator」的高层 API。

## 4. 明确否决的方向

**细粒度写并发**（预分配 offset + 并发 pwrite 到不相交区间）：data 文件是 WAL
（每记录即时 durable）,并发 pwrite + offset 分配 + roll + 组提交协调极复杂,且
**单 append log 写是 IO-bound,串行化不是瓶颈**——高风险换不到吞吐。否决;W1 的
单写锁才是对的粒度。

## 5. 推荐顺序与工作量

| 阶段 | 收益 | 风险 | 工作量 |
|---|---|---|---|
| **W1** 写路径互斥 | **高**（通用库 handle 多写安全契约,见 §6） | 低 | ~半天 + TSan 测试 |
| **W2** 搜索注释订正 + 配置类审计 | 中（消除误导 + 解锁查询并发） | 零 | ~2–3h |
| **W3** close guard | 中（防 UAF,通用库必备） | 低 | ~2h |
| **W4** 并行扫描 | 按需 | 中 | 视需求 |

## 6. 目标定位：通用 C++ 库（2026-06-25 定向）

**不再只服务 Erlang/NIF「一进程一 Cask」模型,而是作为通用 C++ 库。** 这把
线程安全从「可选」变成「契约」：通用库的使用者不会去读埋在头文件里的「单写者
契约」,默认期望「同一个 handle 多线程用是安全的」。**一个会在并发写时静默损坏
数据的存储库,不合格。**

### 6.1 目标并发契约（对标 RocksDB / LMDB 等通用库）

| 维度 | 目标 | 当前 | 缺口 |
|---|---|---|---|
| 多线程并发**读**同一 handle | 安全 | ✅ 已满足 | — |
| 多线程并发**写**同一 handle | 安全（内部串行） | ❌ 静默损坏 | **W1** |
| 读 与 写 并发 | 安全（near-real-time 可见性） | ✅ 已满足（注释保守） | W2 文档 |
| 迭代器 | 每线程一个（同 std 容器迭代器） | ✅ 已满足 | 文档化 |
| 关闭后误用 | fail-fast 而非 UB | ❌ UB | W3 |
| 配置类（`set_synonym_map`）与查询并发 | 安全或明确「需先于并发配置」 | ⚠️ 未明确 | W2 审计 |

> 写吞吐说明（实测校准，见 §9）：**单写线程吞吐不受 `write_mu_` 影响**（uncontended
> 锁 ~20ns ≪ pwrite）。**多写线程堆同一 handle 不提速、反降**——put 临界区极短
> （~1µs），高争用下 futex 唤醒开销压过临界区（threads 1→8：980k→46k/s）。这不是
> bug：data 是单 append WAL 文件层本就串行，往一个 handle 堆写线程只增争用。需要
> 更高写并发 → **按目录分片多个 Cask 实例**（各自独立 WAL + 锁，标准横向扩展）。

### 6.2 结论（通用库定位下）

- **W1（写路径互斥）→ 必做**。从「廉价冗余」升级为「安全契约的必要组成」:
  通用库默认 handle 多线程写必须安全。成本可忽略（~20ns/写 vs pwrite µs–ms）,
  且 keydir + 索引池（S6 reorder buffer）已为并发写就绪,W1 只补 active 文件
  写序列这最后一块。
- **W2（搜索并发确认 + 注释订正）→ 必做**。通用库必须有**准确、显眼**的并发
  契约文档;且订正后解锁已有的查询并发能力（现保守注释让用户白白串行）。
  顺带审计 `set_synonym_map` 等配置类方法。
- **W3（close fail-fast guard）→ 必做**。通用库用户必然犯生命周期错误;atomic
  `closed_` 把静默 UB 变明确错误码。
- **W4（迭代器并行扫描）→ 可选**。每线程一个迭代器是通用库的标准约定（同 std
  容器）,文档化即可;并行扫描作为增值 API 按需。

**一句话：通用库定位下,W1+W2+W3 是一组,共同建立「多读 + 多写（内部串行）+
读写并发 + fail-fast 生命周期」的常规契约。这是合格通用存储库的下限,不是过度
工程。**

## 7. 各接口线程安全实现机制

逐组说明「触及哪些共享态 → 用什么同步原语保护 → 为何安全」。这是 api-cpp.md §9
线程模型汇总表的实现依据。

### 7.1 读：`get` / `get_owned`
- **触及**：keydir（ord/定位查找）、`active_data_` 指针、`read_files_` 句柄缓存、
  `DataFile::read`。
- **机制**：
  - keydir 查找 → 内部按 key hash **分片 `shared_mutex`**（256 分片，多读并发）。
  - `active_data_` / `read_files_` 的读取在 **`read_cache_mu_` shared_lock** 下（写路径
    的 roll/close_write_file 在同锁 unique_lock 下改它们）。
  - 实际读盘 = **`pread(2)`**：无状态系统调用，每次显式传 offset，不依赖/不修改文件
    位置 → 多线程并发 pread 同一 fd 安全。sealed 文件走 mmap 零拷贝（映射不可变）。
- `get_owned` = `get` + `to_owned()`（拷贝），机制同 `get`。

### 7.2 写：`put` / `remove` / `put_doc` / `sync` / `close_write_file`
- **触及**：`active_data_`（`current_offset_`/`write_buf_`/`batch_buf_`）、`active_file_id_`、
  `active_hint_`、`writes_since_sync_`、keydir put/remove、IndexPool 提交。
- **机制**：
  - **`write_mu_`（`std::mutex`）串行化整个写序列**——同一时刻仅一个写线程进入临界区。
    这是 W1 的核心：把「调用方串行化」内化为「库内互斥」。
  - 写线程内部再取 keydir 分片锁（put_overwrite，含原子 `ord`/`file_id` 的 LWW 冲突
    解析，与并发 reader/merger 协调）+ roll 时取 `read_cache_mu_`。
  - 索引提交走 **MPSC lock-free 队列** + per-lane **reorder buffer**（按 ord 序 apply，
    天然容忍多写线程乱序到达——见 async-index-pipeline.md）。
  - **锁序**：`write_mu_`（最外）→ `read_cache_mu_` / keydir；读路径不取 `write_mu_`
    → 无反向依赖、无死锁；写方法互不内部调用 → 无递归锁。
- 为何不损吞吐：data 是单 append WAL，文件层本就串行；锁 ~20ns ≪ pwrite/fsync。

### 7.3 全文搜索：`search_text` / `phrase` / `near` / `bool` / `fields` / `fuzzy` / `wildcard`
- **触及**：`SearchCache`（cache_）、`DocTextLru`（doc_texts_）、`InvertedIndex` 倒排、
  `Index`（docmap/live/doc_len/ord_to_ext）、analyzer。
- **机制**（全是「多读并发 + 写者经同锁协调」）：
  - cache_ = **`shared_mutex`**（命中 shared_lock 读，put unique_lock）。
  - doc_texts_ = **内置 `mutex`**（get/put 短临界区）。
  - 倒排 = **`tbb::concurrent_hash_map` 分片** + 查询持引用零拷贝读快照
    （写者 CoW，见 posting-zero-copy-design）。
  - Index = **`shared_mutex`**（is_live/doc_len/ord_to_ext 读 shared_lock；
    `fill_is_live`/`fill_doc_lens` 批量持锁数组直读）。
  - analyzer = **const 纯函数**（cppjieba `Cut` const 线程安全）→ 无状态可并发。
  - 与并发写（索引 reducer 单写线程改这些结构）经上述 shared_lock 协调；可见性
    遵循 near-real-time（`prepare_search`→flush 覆盖调用前的写）。

### 7.4 向量 / 混合：`search_vector` / `search_hybrid`
- **机制**：HNSW 图 = **`std::atomic<std::shared_ptr<HnswIndex>>`**——查询开头 `load`
  一次快照指针；merge 重建用「新图旁路构建 + 原子换指针」发布，旧图由在途读者的
  `shared_ptr` 引用计数续命到查询结束。图遍历本身无锁（图节点不可变 + visited 用
  `thread_local` 版本化数组，各线程独立）。`search_hybrid` = text 路 + vec 路串行 + RRF
  融合，复用上述两路机制。

### 7.5 批量：`search_*_batch`
- **机制**：经 `search::parallel_for_queries` 跑在**进程级共享 `search_arena`**（TBB
  `task_arena`，并发上限由 market 封顶 ≈hardware_concurrency）。每条查询是完整独立
  单元、结果槽不重叠 → 无需额外锁，复用 7.3/7.4 的单查询读机制。

### 7.6 `merge`
- **机制**：写**自有 merge 输出文件**（不碰 `active_*`/`write_mu_`）；keydir 更新
  （`on_relocate`）走 keydir 分片锁；与 put/search 经 keydir `shared_mutex` 协调。
  跨进程经 **`merge.lock`**（`O_EXCL` 文件锁）与 live writer 的 `write.lock` 互不阻塞。

### 7.7 迭代器：`CaskIter`
- **机制**：`start()` 经 keydir **BarrierGuard** 拍 key 快照（frozen pending）；X1：复制
  keydir `shared_ptr`（`keydir_pin_`）+ S13 pin 全部 data fd → **跨 close 存活**。`next()`
  对每 key 取分片 shared_lock + pread。**同一 iter 是有状态游标（`cursor_`）→ 不可并发**；
  不同 iter 各持独立快照 → 可并发。W3：close 后 `start()` 经 `parent_->is_closed()` fail-fast。

### 7.8 `parallel_scan`
- **机制**：① 串行阶段——一个 iter `drain_live_keys()` 快照 live key（仅 keydir proxy，
  **不读 value**）。② 并发阶段——N 个 `std::thread` 各 `get()` 自己的不相交 key 段 + 调
  `fn`。安全性 = 复用 7.1 的并发读机制 + 段不相交 + `fn` 由 caller 保证线程安全。

### 7.9 生命周期：`closed_`（W3）
- **机制**：`std::atomic<bool>`；`close()` 用 `exchange(true)`（兼幂等门）；公共方法入口
  `acquire` load 检查 → 已关闭返回 `kInvalidOption`。**best-effort fail-fast**：拒绝 close
  后**新发起**的调用；与 close **并发在途**的调用仍是 caller 责任（非完整 rundown）。

### 7.10 配置（**非线程安全**）：`set_synonym_map`
- **机制**：改 `synonym_map_`（`unique_ptr`）裸指针，而 `search_text`/`search_fields` 读它
  → reader-vs-writer 竞态。**判定为配置类**：契约「须先于并发查询配置或外部串行化」。
  不加锁的理由——加 `atomic<shared_ptr>` 会给「无 synonym」的查询热路径（常态）添每查询
  原子开销，为罕见的动态重配不划算。

## 8. 落地子任务清单（实施 checklist）

### W1 — 写路径互斥 ✅ 已完成（2026-06-25）
- [x] `Cask` 加 `std::mutex write_mu_`（成员）。
- [x] `put` / `remove` / `put_doc` / `sync` / `close_write_file` 入口取 `write_mu_`
      （覆盖内部 `ensure_active_writer` / `roll_active` / `maybe_group_commit` /
      `write_and_keydir` 全序列）。
- [x] **`flush_index` 不纳入**——核实它被 `prepare_search()`（读/搜索路径）调用,上锁会串行化
      搜索;IndexPool flush 自带 cv 同步本就线程安全。锁集 = 5 个真写方法。
- [x] **锁序确认**：`write_mu_` 最外层 → 内部再取 `read_cache_mu_`;读路径**不**取
      `write_mu_`。无反向依赖、无递归锁。
- [x] **merge 交互审计**：`merge()` 不触 `active_*`/DataFile 成员（写自有输出文件,经 keydir
      `shared_mutex` 协调）→ 不纳入 `write_mu_`,与写并发不变。`FieldSchema::intern` 已自带
      `shared_mutex` → 无新增 reader-vs-writer 缺口。
- [x] TSan 测试：`ConcurrentWritersSharedCaskNoCorruption`（8 线程共享 handle 并发 put+remove
      互不相交 key 段,重开逐键校验）。**已实证移除 write_mu_ 后 TSan 必报 race** → 真护栏。
- [x] C API 自动受益（包装 `Cask`）;无需改 C 层逻辑。
- 验证：Release/Debug **475/475 ctest** + TSan 零 race（95 例并发套件）。

### W2 — 读/搜索并发确认 + 注释订正 + 配置类审计 ✅ 已完成（2026-06-25）
- [x] TSan 测试：`W2ConcurrentSearchAndWriteNoRace`（4 读线程 ×6 模式 + 2 写线程并发，零 race）。
- [x] 订正搜索方法「线程安全:否」→「**是**（并发读安全）」;**连带订正 W1 后写方法**「否」→「是」;
      cask.hpp 顶部线程模型重写为「通用库 handle 多线程安全」。
- [x] **`set_synonym_map` 审计**：定为**配置类**（加锁会给无 synonym 的查询热路径添 atomic 开销）→
      契约「**须先于并发查询配置**或外部串行化」（注释 + 文档明示）。
- [x] 契约**显眼化**：`doc/api-cpp.md` §9 汇总表全面订正 + §5.3/各方法注释;README 一行订正 + docs 表
      加本文指针。
- [x] 写吞吐指引：文档明示「更高写并发 → 按目录分片多 Cask 实例」。

### W3 — 生命周期硬化 ✅ 已完成（2026-06-25）
- [x] `Cask` 加 `std::atomic<bool> closed_` + `is_closed()`;`close()` 顶 `exchange(true)`（兼幂等门）。
      公共方法入口 fail-fast：数据面 → `kInvalidOption`("cask is closed");搜索集中守
      `run_search_one`/`run_search_batch`;内省 → 安全默认值;`CaskIter::start` 守 parent。
- [x] 不做完整 rundown——文档写明「close 时刻须无在途操作」（与 close 并发在途仍 caller 责任）。
      用 `kInvalidOption`+detail（不新增 kClosed,避免 C API 枚举 churn）。
- [x] 测试：`OperationsAfterCloseReturnErrorNotUb`（close 后各 API 返错码 + 内省默认 + iter
      fail-fast + 二次 close 幂等）。Release/Debug 477/477 + TSan crash_recovery 11 零 race。

### W4 — 迭代器并行扫描 ✅ 已完成（2026-06-25）
- [x] 文档化「每线程一个 `CaskIter`」（同 std 容器迭代器约定）——已在 W2/W3 注释 + api-cpp §9 落实。
- [x] `Cask::parallel_scan(n_threads, fn)`：**原计划「N 独立 iterator 分区」不可行**（keydir 迭代器
      是单快照游标，无法切分）→ 改为「单次快照 live key（`CaskIter::drain_live_keys`，仅 key 不读
      value）→ 分段 → N 个 std::thread 并发 get + fn」。并行化读值的 pread+decode（真成本）;写串行
      不受影响。语义：n=0→hw_concurrency;并发删 kNotFound 跳过;其它错误停止;close 后 fail-fast。
- [x] 测试 `ParallelScanVisitsAllKeysOnce`（2000 key 删 1/10，每 key 恰一次 + 值正确 + close fail-fast）。
      478/478 + TSan 零 race。C++-only（C API 未绑定，C host 可自行多线程 get）。

## 9. 实测基线（benchmark，2026-06-25，Release+LTO+native，6 核 / 24 MiB L3）

跑法：`cmake --build build-rel --target bitcask_bench &&
build-rel/bench/bitcask_bench --benchmark_filter='BM_Cask_Put_Concurrent|BM_Cask_ParallelScan'`

### W1 — 并发写同一 handle（`BM_Cask_Put_Concurrent`，聚合吞吐）

| 写线程数 | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| put/s（聚合） | ~980k | ~200k | ~112k | ~46k |

- **单写不受锁影响**（~980k/s ≈ 单写基线 `BM_Cask_Put_Overwrite`）。
- **多写不升反降**：put 临界区 ~1µs，`write_mu_` 串行下 futex 唤醒/上下文切换开销压过
  临界区（短临界区高争用 mutex 退化）。**非 bug**——印证「写扩展靠分片，不靠堆线程」。
- 多写**安全**（数据不坏，TSan `ConcurrentWritersSharedCaskNoCorruption` 验证）。

### W4 — `parallel_scan` 全表扫描加速（`BM_Cask_ParallelScan`，5 万 key×128B）

| 工作线程数 | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| 扫描耗时 | 56.8 ms | 30.8 ms | 17.9 ms | 17.8 ms |
| 加速比 | 1.0× | 1.84× | **3.17×** | 3.19×（6 核饱和） |

- 读值的 keydir 查找 + pread + DocValue decode 被并行化（页缓存热后 CPU bound）；
  快照 key 串行。4 线程近饱和 6 核，8 线程过订阅无增益。
- 对比 W1：**读可扩展（parallel_scan 3.2×），写不可扩展（串行 WAL）**——这正是
  「读真并行 + 写内部串行」契约的实测体现。
