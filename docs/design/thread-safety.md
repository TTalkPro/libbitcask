# 内部线程安全审计（As-Built 状态记录）

> 受众：项目维护者。配套用户向契约见 `doc/concurrency-zh.md`（重点在
> 同 handle 多线程安全承诺），本文专做 **W1 + W2 + W3 加固设计落到代码
> 后的当前实现状态** 审计——每条同步原语、每条不变量、每条锁全序都对应
> 到具体头文件 / 源文件里的符号位置。

本文不再做方向评估（W1 / W2 / W3 已全部落地），任务是回答：

> 「承诺的并发契约在代码里能否被找到对应同步原语？缺口在哪里？」

术语：本文沿用 `concurrency-zh.md`（Phase 1 用户向文档）同一套名字
（`write_mu_` / `read_cache_mu_` / `shared_mutex` / `flock` /
`KeyDirRegistry` / `IndexPool` / HNSW atomic publish 等），侧重点是
「**为什么这套原语在这里、对应哪条不变量、锁序怎么走**」，而
`concurrency-zh.md` 偏用户契约（用 threading model 表格告诉调用方何
时安全）。

---

## 1. 范围与基线

### 1.1 审计对象

| 模块             | 头文件                                       | 源文件                                       | 并发角色 |
|------------------|----------------------------------------------|----------------------------------------------|----------|
| `Cask` 门面     | `include/bitcask/cask.hpp`                  | `src/cask/cask.cpp`                          | handle 级串行化；读无锁；写互斥 |
| `KeyDir`         | `include/bitcask/keydir.hpp`                | `src/keydir/keydir.cpp`                      | 256 分片锁 + 屏障 + MVCC |
| `KeyDirRegistry` | `include/bitcask/keydir_registry.hpp`       | `src/keydir/keydir_registry.cpp`             | 同进程 KeyDir 共享 / IndexPool 持有 |
| `FileLock`       | `include/bitcask/file_lock.hpp`             | `src/lock/file_lock.cpp`                     | 跨进程 flock（基于 `O_EXCL`）|
| `IndexPool`      | `include/bitcask/thread_pool.hpp`           | `src/thread_pool/`                           | 异步索引 MapReduce |
| `HnswIndex`      | `include/bitcask/hnsw.hpp`                  | `src/vector/hnsw.cpp`                        | 单写者 + 多读者无锁发布 |
| `InvertedIndex`  | `include/bitcask/inverted.hpp`              | `src/bm25/inverted.cpp`                      | CoW posting + tbb::concurrent_hash_map |
| `Plugin API`     | `include/bitcask/plugin_api.hpp`            | `src/cask/cask.cpp::CaskPluginHost`          | `run_serialized` 经 RunFn 通道 |

### 1.2 设计基线

- **同 handle 多线程安全**（S11 通用 C++ 库定位）：同一个 `Cask` 实例可
  被多线程共享，所有公共方法的并发语义在 README + 各方法 doxygen 注释
  + `concurrency-zh.md` 已言明。
- **三类 open 模式的并发约束**与 `concurrency-zh.md` §1-§5 一致——
  read_write 拿 `bitcask.write.lock`、merge_only 拿 `bitcask.merge.lock`、
  只读不拿锁。
- **跨进程隔离**靠 `flock`（`O_CREAT|O_EXCL` 文件锁的进程间互斥语义），
  同进程多 handle 共享 KeyDir 靠 `KeyDirRegistry`。

### 1.3 W 阶段总览

| 阶段 | 目标                              | 状态     | 落地标志（详细见下） |
|------|-----------------------------------|----------|----------------------|
| W1   | 写路径内部串行化                  | ✅ 已实现 | `write_mu_`（`cask.hpp` 的 `std::mutex`）；覆盖 put / remove / put_doc / sync / close_write_file / backup |
| W2   | 读 / 搜索并发确认 + 注释订正      | ✅ 已实现 | 读路径无锁；搜索路径全部 shared_lock / atomic publish；doxygen 注释订正为「并发读安全」 |
| W3   | 生命周期硬化                      | ✅ 已实现 | `closed_` atomic + `WriteOpGate`（`writes_in_flight_`）+ `ckpt_mu_` |

W4（并行扫描）作为增值 API 已实现——`Cask::parallel_scan`（doxygen
注释 + README 一行），不在本审计展开。

> 以下 §2-§6 按符号逐项审计实现细节，并对照历史 §2 race condition 清单
> （原文件 §2 节）逐条给出现状 = **已修复 / 仍开放 / N/A**。

---

## 2. W1 — 写路径内部串行化（已实现）

### 2.1 `write_mu_` 的覆盖范围

`Cask::write_mu_`（`cask.hpp` 的 `std::mutex`，成员）声明见
`cask.hpp` 的 S11-W1 注释：覆盖 put / remove / put_doc / sync /
close_write_file 的整个写序列（包括 `ensure_active_writer` /
`roll_active` / `maybe_group_commit` / `write_and_keydir`）。

实现侧入锁点（`src/cask/cask.cpp` 的方法入口）：

| 方法                      | 入锁点                                                                                     |
|---------------------------|--------------------------------------------------------------------------------------------|
| `Cask::put`               | `std::unique_lock<std::mutex> wlk(write_mu_)`                                              |
| `Cask::put_batch`         | 同上                                                                                       |
| `Cask::put_doc`           | 同上                                                                                       |
| `Cask::remove`            | 同上                                                                                       |
| `Cask::sync`              | `std::lock_guard<std::mutex> wlk(write_mu_)`                                               |
| `Cask::close_write_file`  | 同上                                                                                       |
| `Cask::backup`            | 同上（`std::lock_guard<std::mutex> wlk(write_mu_)`，备份期间挡住写者，active 文件可 hardlink）|

`flush_index` 不纳入——读 / 搜索路径也调它，纳入会让搜索串行化；
`IndexPool::flush` 自带 `flush_cv_` 同步（见 §6.1），本就线程安全。

### 2.2 锁全序

```
write_mu_ (mutex, Cask 级)
  └─► read_cache_mu_ (shared_mutex)
        └─► DataFile 内部 / posix pread (per-call 无状态)
  └─► KeyDir shard mutex（put path）
        └─► KeyDir meta_mu_ (shared_mutex, fold 期间)
  └─► fstats_grow_mu_ (mutex, 仅新 file_id 槽位构造时碰)
```

读路径不取 `write_mu_`——`get` / 搜索 / `parallel_scan` 直接走
`read_cache_mu_` 的 shared_lock + KeyDir 分片锁 → **无反向依赖 → 无死
锁**。

### 2.3 H1：`submit_index_task` 移出 `write_mu_` 临界区

**问题**（修复前）：`IndexPool::submit` → 内部 `tbb::concurrent_bounded_queue::push`
在队列满（cap = `kDefaultIndexQueueCapacity`，10240）时**阻塞**写者——
临界区里阻塞意味着同 handle 的全部写者冻结。

**修复**（`src/cask/cask.cpp` 的 `Cask::put` 等）：常规写路径
（`IndexOp::Add` / `Delete`）在 `write_mu_` **释放之后**调用
`submit_index_task`。reorder buffer 内部按 ord 升序 apply（`IndexPool`
的 reducer 循环，见 §6.1），与到达序无关——所以锁外提交不破坏索引一致性。

例外：`IndexOp::Skip`（`OrdSkipGuard` 失败补偿路径）的提交可能在锁内
（罕见路径，可接受）。

### 2.4 `WriteOpGate` 与 close 协调

`Cask::WriteOpGate`（`cask.hpp` 内部 RAII 守卫）：

- **ctor**：`writes_in_flight_.fetch_add(1, std::memory_order_seq_cst)`
- **dtor**：`writes_in_flight_.fetch_sub(1, std::memory_order_seq_cst)`，归零时 `notify_all`
- **覆盖范围**：put / put_batch / remove / put_doc 整段 + **含释放
  `write_mu_` 之后的索引提交尾段**——这是 H1 引入的细节：锁外补提交的
  IndexTask 仍在 WriteOpGate 的生命期内。

`Cask::close()`（`src/cask/cask.cpp`）的核心等待循环：

```
closed_.exchange(true)                       // 幂等门
for (n = writes_in_flight_.load(seq_cst);    // 等写者全部退出
     n != 0;
     n = writes_in_flight_.load(seq_cst))
    writes_in_flight_.wait(n, seq_cst);
```

`seq_cst` 全序保证：写者「inc 后读 closed_」与 close「写 closed_ 后读
计数」构成 store-buffer 形状——RMW 全序 + seq_cst load 阻止两侧同时读
到旧值（见 `cask.hpp` 的 `WriteOpGate` 注释）。

### 2.5 历史 race condition：写者 vs 写者（已修复）

| 共享可变态                                          | 原状态                                       | 现状态 |
|------------------------------------------------------|----------------------------------------------|--------|
| `DataFile::write` 的 `current_offset_`/`write_buf_`/`batch_buf_` | 成员非 atomic，无保护                       | ✅ 由 `write_mu_` 保护（覆盖 put / put_doc / put_batch 整段，append-only）|
| `writes_since_sync_`（组提交计数）                   | 无保护                                       | ✅ 同上，在 `maybe_group_commit` 里同一 `write_mu_` 临界区访问 |
| `active_file_id_` / `active_data_`                  | reset 在 `read_cache_mu_`，整序列无互斥      | ✅ 写在 `write_mu_` 内；读在 `read_cache_mu_` shared |
| keydir put / LWW                                    | 已安全（分片锁 + atomic ord / file_id）      | ✅ 不变 |
| 索引任务提交                                        | MPSC + reorder buffer，已为并发写就绪         | ✅ 不变 |

**结论**：W1 把「外部串行契约」完全内化为库内互斥——多线程并发写同 handle
安全（数据不坏，TSan 已实证），单写吞吐不受锁影响。

---

## 3. W2 — 读 / 搜索并发确认（已实现）

### 3.1 KeyDir：256 分片 + MVCC

**结构**（`include/bitcask/keydir.hpp`）：

```cpp
static constexpr std::size_t kShards = 256;
struct alignas(64) Shard {
    mutable std::mutex mu;                          // S5: rwlock → mutex
    alignas(64) ankerl::unordered_dense::map<...>   // StringHash transparent
        entries;
};
mutable std::array<Shard, kShards> shards_;
```

**为什么不是 `shared_mutex`**（`keydir.hpp` 的 `Shard::mu` 注释）：
临界区足够短，mutex 性能更好且消除 rwlock 的写者偏好停车问题。

**全局标量全 atomic**（`keydir.hpp` 私有区）：

| 字段                 | 类型                          | 同步作用 |
|----------------------|-------------------------------|----------|
| `epoch_`             | `alignas(64) std::atomic<uint64_t>` | fold snap 旧 / 新判据（写热行，独占缓存行） |
| `key_count_`         | `std::atomic<uint64_t>`       | 写入 / 删除时 RMW |
| `key_bytes_`         | `std::atomic<uint64_t>`       | 同上 |
| `next_ord_`          | `std::atomic<uint64_t>`       | `alloc_ord` / `advance_ord` 无锁；写热行 |
| `keyfolders_`        | `alignas(64) std::atomic<uint64_t>` | fold 计数；仅屏障内修改；读热行独立 cache line |
| `biggest_file_id_`   | `std::atomic<uint32_t>`       | put 时 CAS-max 推进；merger / writer 协调 |
| `has_pending_`       | `std::atomic<bool>`           | pending_ 表是否存在的无锁镜像（持分片锁后 relaxed 读即够新）|
| `is_ready_`          | `std::atomic<bool>`           | 注册表初始化协议 |
| `fstats_size_`       | `std::atomic<size_t>`         | fstats 数组发布水位 |
| `iter_mutation_`     | `std::atomic<bool>`           | 写痕迹标志（无锁镜像，避免热路径摸 meta_mu_）|

**缓存行分组**（S2 实测关键）：写热行（`epoch_` / `next_ord_` /
`key_count_` / `key_bytes_`）与读热行（`keyfolders_` / `biggest_file_id_`）
分到不同 cache line，避免 false sharing——Mixed 基准实测主要损耗源
（`keydir.hpp` 的注释实测记录）。

### 3.2 BarrierGuard v2 写者闸门

旧 `lock_all_shards()` 同时持 257 把锁，撞 TSan 死锁检测器
`compiler-rt/sanitizer_common/sanitizer_deadlock_detector.h` 的
64 持锁硬上限——**已删除**。

替代：RAII `BarrierGuard`（`src/keydir/keydir.cpp` 的内部类）：

- **ctor**：拿 `barrier_mu_` → `barrier_active_.store(true, release)`
  → 拿 `gate_mu_` 短暂（确保后续写者必见 `barrier_active_=true`）→ 放
  `gate_mu_`
- **扫描阶段**：逐分片**加锁 → 放锁**排干在途写者（任意瞬间只持 1 把）
- **真正的屏障段**：在 `barrier_mu_` 持有期间执行跨分片原子操作（如
  `pending` 合并、`MultiEntry` 折叠）
- **dtor**：`barrier_active_.store(false, release)` → `notify_all` 唤醒
  `gate_cv_` 上的退避写者 → 放 `barrier_mu_`

写者在拿到分片锁后 `if (barrier_active_.load(acquire))` 检查 → 真则
放分片锁到 `gate_cv_` 退避。

读者（get / next / `conditional_remove` peek）**不受屏障影响**——继续
并发。

### 3.3 KeyDir 锁全序

```
barrier_mu_ (mutex)
  → gate_mu_ (mutex) + gate_cv_
  → meta_mu_ (shared_mutex)
  → 单个 shard (mutex, 任意时刻 ≤1 把)
  → fstats_grow_mu_ (mutex)
```

两处反向嵌套例外（`keydir.hpp` 文件头无环论证）：

1. **热路径 `shard → meta`**（`get` / `put_probe` / `remove` miss 时折叠
   态嵌套 `meta_mu_`）——与全序一致。
2. **屏障内 `meta_shared → shard`**（`apply_pending_to_entries_barrier`
   持 meta shared 期间逐 key 嵌套该分片锁）——与全序相反。

无环论证：方向② 仅存于屏障内——彼时所有 meta unique 的使用者
（`put_probe` / `remove` 折叠态）已被闸门出清，仅剩读者走方向①且对
meta 只拿 shared；② 也只拿 meta shared——`shared-shared` 相容，无
unique 排队者。屏障外只有方向①——同样无环。

### 3.4 fstats：无锁热路径 + RCU 指针表（M6 + S13-F8）

`KeyDir::AtomicFStats`（`keydir.hpp` 私有区，`alignas(64)` 每元素独占
cache line，避免 merge + active 并发写不同文件时假共享）：

```cpp
struct alignas(64) AtomicFStats {
    std::atomic<uint64_t> live_keys{0};
    std::atomic<uint64_t> total_keys{0};
    std::atomic<uint64_t> live_bytes{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint32_t> oldest_tstamp{0};
    std::atomic<uint32_t> newest_tstamp{0};
    std::atomic<uint64_t> expiration_epoch{kMaxEpoch};
    std::atomic<uint8_t>  present{0};
};
mutable std::mutex fstats_grow_mu_;
mutable std::deque<AtomicFStats> fstats_;
std::atomic<size_t> fstats_size_{0};
std::atomic<AtomicFStats**> fstats_ptrs_{nullptr};
std::vector<std::unique_ptr<AtomicFStats*[]>> fstats_ptr_arrays_;
```

**关键修复（S13-F8，TSan 实证抓出）**：`deque::operator[]` 遍历内部块
指针表（map），与 `emplace_back` 触发的 map 重分配构成无锁读者 UAF。
修复：deque 仍是元素所有者，旁挂 RCU 指针表 `fstats_ptrs_` ——扩容
时在 `fstats_grow_mu_` 下建新表、`release` 发布，旧表退休不释放
（在途读者可能仍持有；总内存 < 2× 终表 ≈ 16B/file_id，有界）。

无锁读者一律经 `fstats_slot(idx)`，前置条件 `idx < fstats_size_.load(acquire)`。
size 的 `release` 发布在指针表填充 / 替换之后——acquire 读者必见新
表（见 `src/keydir/keydir.cpp` 的 `grow_fstats_locked`）。

### 3.5 MVCC fold：sibling chain + pending hash

`KeyDir::keyfolders_ > 0` 时：

- 已存在 key 的新写：分片内升级 `SingleEntry` → `MultiEntry`（newest
  first 兄弟链）。
- 新 key 的写 / fold 期间临时 tombstone：经 `meta_mu_` unique 写
  `pending_`。
- **不变量**：key ∈ 某分片 entries ⟹ key ∉ pending_（S2 起）——
  get / put / remove 的「entries 优先、miss 再查 pending」探测顺序依
  赖该不变量。

最后一个 fold release 时：

1. **阶段二**：`apply_pending_to_entries_barrier` 把 pending_ 逐条
   应用进各分片 entries（持 meta shared + 屏障内例外 meta→shard）。
2. **阶段三**：`collapse_multi_entries_barrier` 把各分片 MultiEntry
   折回 SingleEntry（持 BarrierGuard，逐分片加锁 → 折叠 → 放锁）。
3. 清 pending_ + `has_pending_` atomic。

### 3.6 `conditional_remove` TOCTOU

`src/keydir/keydir.cpp` 的 `KeyDir::conditional_remove` 分两阶段：

1. **peek**：分片 unique + 嵌套 meta shared，匹配即释放
2. **commit**：再次取分片 unique 检查命中——caller 拿到 `kOk` 时不保
   证当前已不存在（探测后状态可能变），但对 merge 语义足够；commit
   内部 re-check 保证幂等安全。

### 3.7 历史 race condition：读者 vs 写者（已修复 / 已确认安全）

| 共享可变态                                          | 原状态                                       | 现状态 |
|------------------------------------------------------|----------------------------------------------|--------|
| `get` / `get_owned` 路径                            | 结构级已安全（KeyDir 分片锁 + pread 线程安全）| ✅ 不变；doxygen 注释订正 |
| `cache_` / `doc_texts_`                             | shared_mutex + 内置 mutex                    | ✅ 写者 reducer 持 unique，读者持 shared |
| 倒排 `tbb::concurrent_hash_map`                     | 桶级锁                                       | ✅ 不变；PostingList 改 `shared_ptr<const>` CoW 发布 |
| `Index`（`docmap` / live / doc_len）                | shared_mutex                                 | ✅ 读 shared_lock / 写 unique_lock |
| `analyzer`                                          | const 纯函数                                 | ✅ cppjieba `Cut` const 线程安全 |
| `search_vector` / `search_*_batch`                  | HNSW `atomic<shared_ptr>` 快照 + inter-query 并发 | ✅ 不变（见 §5） |

doxygen 注释 `搜索方法「线程安全:否」→「是（并发读安全）」` 修
订在 `cask.hpp` 各方法注释 + README 一行——**W2 注释订正**。

---

## 4. W3 — 生命周期硬化（已实现）

### 4.1 `closed_` atomic 标志

`Cask::closed_`（`cask.hpp` 的 `std::atomic<bool>`，S11-W3 注
释）。`close()` 顶头 `if (closed_.exchange(true)) return;`——兼作
幂等门。

公共方法入口 fail-fast（统一返 `CaskError::kClosed`，S12-5 后从
`kInvalidOption` 拆出来独立枚举）：

- 数据面：`put` / `put_batch` / `remove` / `put_doc` / `get` / `get_owned` / `sync` / `close_write_file` / `backup` / `merge` / `checkpoint`
- 搜索集中守 `run_search_one` / `run_search_batch`（所有 `search_*` 方法都过这道门）
- 内省：`status`（已关闭返零值快照，不解引用 `keydir_`）/ `is_empty_estimate`（返 `true`）/ `is_frozen`（返 `false`）/ `needs_merge`（返 `needs=false`）
- 迭代器：`CaskIter::start` 守 `parent_->is_closed()`

**契约**：caller 须保证 close 时刻没有其它线程仍在调用 get / put /
remove / sync / iter / search_*——这是资源句柄的标准约定。`closed_`
是 best-effort fail-fast：不**做**完整 rundown。W3 + H1 把它收敛为
「已发起的调用返回错误码，不会解引用已释放状态；与 close 并发在途
的调用仍是 caller 责任」。

### 4.2 `WriteOpGate`（H1 闭环）

§ 2.4 已展开。补完契约细节：

- 写路径入口新建 `WriteOpGate` 守卫整段，含锁外的索引提交尾段。
- close 设 `closed_` 后，先 `writes_in_flight_.wait(0, seq_cst)`
  等写者退出，再拆资源（`active_data_` / 索引 lane 等）。
- 这一收敛把「close 与并发在途写」的 UB 收敛为「阻塞等待」——
  队列背压中的 push 必然返回（池由 registry 持有，close 不停池），
  closed_ 置位后新写者在入口即退 → 计数单调排空。

### 4.3 `ckpt_mu_`：checkpoint 间互斥

`Cask::ckpt_mu_`（`cask.hpp` 的 `std::mutex`）。`checkpoint()` 调用
间互斥（手动的多次 `checkpoint()` 串行化；ckpt 实际写入统一在 reducer
线程 RunFn 内做）。

不与 `write_mu_` 交叉——checkpoint 不取 `write_mu_`，写路径不取
`ckpt_mu_`。

### 4.4 历史 race condition：迭代器 / close 语义

| 场景                                            | 原评估                                       | 现状态 |
|--------------------------------------------------|----------------------------------------------|--------|
| 同一 `CaskIter` 的 `start` / `next` / `release` | cursor 是有状态游标，本就不并发              | N/A——语义保留，doxygen 明确「同一对象不可并发使用」|
| 不同 `CaskIter` 并发使用同一 `Cask`             | 已安全                                       | ✅ 不变（X1：`keydir_pin_` 让 IterHandle 跨 close 存活；`pin_files()` 让 fd 跨 merge unlink 存活，详见 §6.2） |
| `close` vs 在途 get / put / search_*            | UAF / 解引用已释放状态                       | ✅ 已闭合（`WriteOpGate` + `closed_` fail-fast） |
| `CaskIter` 在 `close` 后析构                     | IterHandle 内部裸指针 UAF                    | ✅ X1 修复：`keydir_pin_ = shared_ptr<KeyDir>` 在 IterHandle 生命周期内续命 KeyDir |
| `close` 与并发写者的 race                        | UB                                          | ✅ 收敛为阻塞等待 |

---

## 5. 进阶实现机制（覆盖 `concurrency-zh.md` 全文要点）

### 5.1 读路径并发——同 handle 多线程

`get` / `get_owned` 路径无外部锁；并发由四层同步原语保证：

| 层                           | 同步原语                                                                                          | 触及共享态 |
|------------------------------|---------------------------------------------------------------------------------------------------|------------|
| KeyDir `get`                 | 目标 key 分片 `Shard::mu` unique（短临界区）；fold 态 miss 时嵌套 meta_mu_ shared                  | KeyDir     |
| `read_file`（`src/cask/cask.cpp`） | `read_cache_mu_` shared_lock；命中 `shared_ptr<DataFile>` 引用计数让 fd / mmap 跨 merge unlink 续命 | DataFile 缓存 |
| `pread`                      | POSIX 系统调用，无状态，每次显式传 offset → 多线程并发 pread 同一 fd 安全                        | OS         |
| 已 pin 文件                  | `CaskIter::pin_files()` 在 `start()` 时拍目录快照，`next()` 优先从 pin 句柄读                    | fd 表      |

`parallel_scan`（`src/cask/cask.cpp` 的 `Cask::parallel_scan`）自己
spawn `std::thread` 把快照顾 key 分段并发 `get`——底层就是上述 (1)+(2)。
每段独立不相交 key，`fn` 由 caller 保证线程安全。

### 5.2 搜索并发——`cache_` / `doc_texts_` / 倒排 / HNSW / Index

| 结构                          | 同步原语                                                                                          | 多读者 |
|-------------------------------|---------------------------------------------------------------------------------------------------|--------|
| `SearchCache::cache_`         | `shared_mutex`                                                                                    | ✅ shared_lock |
| `DocTextLru::doc_texts_`      | 内置 mutex                                                                                         | ✅ reader short critical section |
| `InvertedIndex` 倒排桶       | `tbb::concurrent_hash_map<std::string, std::shared_ptr<PostingList>>`；64 分片 term hash         | ✅ 并发迭代 + `const_accessor` 持引用出锁 |
| PostingList 发布              | `shared_ptr<const PostingList>`——写者 CoW（`use_count()==1` 原地改，否则克隆替换）                | ✅ 读持引用期间写者 CoW |
| 倒排排序词典                  | `shared_mutex` 护 `vocab_mtx_`；`shared_ptr<const vector<string>>` 换指针发布；`vocab_dirty_` atomic fast path | ✅ fast path 无锁读 |
| `Index`（docmap / live / doc_len） | `shared_mutex`                                                                                | ✅ shared_lock（fill_is_live 批量持锁直读数组）|
| HNSW                          | chunk 目录 `std::array<std::atomic<NodeChunk*>, kMaxChunks>` release / acquire；per-node seqlock；整图 `atomic<shared_ptr<HnswIndex>>` | ✅ atomic 换图 → 旧图读者引用计数续命 |
| analyzer                      | const 纯函数（cppjieba `Cut` const 线程安全）                                                       | ✅ 无状态 |

### 5.3 HNSW 单写者 + 多读者无锁发布

```cpp
// hnsw.hpp 私有区
std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_{};
std::atomic<uint32_t> count_{0};          // 发布水位
std::atomic<uint64_t> entry_meta_{0};     // 高 32 = level+1, 低 32 = entry id
std::atomic<uint64_t> max_inserted_ord_{...};
std::atomic<bool> writer_active_{false};  // 单写者 assert 守门
```

**节点块发布**：裸指针 + release/acquire——为什么不用 `shared_ptr`：
`copy_neighbors` 热路径每次都 load，原子引用计数开销不可接受。
`~HnswIndex()` 单线程 delete 兜底（彼时无并发读者），`kMaxChunks`
上限 64M 节点。

**per-node seqlock**（S13-P7，替代旧自旋锁）：`lock[i]` 是
`std::atomic<uint32_t>` 序号——偶数稳定、奇数正在写。读者双读序号
一致才采信（torn 读被重试丢弃），数据字全走 `std::atomic_ref`
relaxed（UB-free、TSan 干净）。

**升级动机**：旧自旋锁让读者的 `copy_neighbors` 对锁字节做 exchange
（写动作）——HNSW 流量高度偏向 hub 节点 → 并发查询时锁缓存行核间乒
乓。seqlock 让读者只读不写，零共享行写。

**单写者声明**：`writer_active_` atomic 在 `insert` 入口做 exchange，
debug assert 明文——多写者不支持；引擎范围内 insert 只在
`IndexPool` reducer 线程执行。

**整图换指针**：`std::atomic<std::shared_ptr<HnswIndex>> hnsw_`
（`include/bitcask/vector_plugin.hpp` 私有）。merge 重建时旁路
构建新图（reducer 线程内 RunFn），最后 `release`-store 换指针。
旧图由在途读者的 `shared_ptr` 引用计数续命，析构由 GC 自然清理。

### 5.4 InvertedIndex CoW posting

详见 `concurrency-zh.md` §10。补完审计要点：

- `mutable_pl(acc)`：写者持 `PostingMap::accessor` 后取非 const
  引用——若 `use_count()==1` 原地改；若 `>1`（phrase 读者持引用）就
  `make_shared<PostingList>(*old) + append` 克隆替换。读者持
  `shared_ptr<const PostingList>` 零拷贝读。
- `live_doc_count_` / `sum_doc_len_` 改 `atomic`（S10.1 去锁，
  避免与查询裸读构成 UB race）。
- `max_indexed_ord_`（崩溃恢复幂等保护）原子——replay 重放已在快
  照里的 `(ord, term)` 时丢弃。

---

## 6. IndexPool 与 CaskPluginHost

### 6.1 IndexPool 三锁不变量

`include/bitcask/thread_pool.hpp` 的 `IndexPool` 是 registry 级共享
的异步索引线程管理器（**不是每 Cask 一个**）——所有同 registry 的
Cask 复用同一对 N+1 线程（map workers + reducer）。

```cpp
std::mutex start_mu_;    // started_ + 建线程
std::mutex reorder_mu_;  // lanes_/pending/next_apply_ord/reorder_inflight_
std::mutex flush_mu_;    // flush_cv_ 配套
```

**不变量**：任一线程任一时刻最多持其中一把。**无锁嵌套 ⇒ 无加锁
顺序 ⇒ 不可能死锁**（`thread_pool.hpp` 注释的 criterion 5 审计）。

关键死锁防御点（`thread_pool.hpp` 注释）：

- reducer apply 前 `lk.unlock()` 释放 `reorder_mu_`，再 `dec_in_flight`
  （取 `flush_mu_`）。
- `register_lib` 先放 `reorder_mu_` 再 `ensure_started`（取 `start_mu_`）。
- cv 唤醒持各自锁，不跨锁。

**队列**：`tbb::concurrent_bounded_queue<IndexTask>`，capacity =
`kDefaultIndexQueueCapacity`（10240）。map worker 跑 `process_task` →
`push_reorder`（在 `reorder_mu_` 下入 lane 的 pending map）。

**reducer 循环**：在 `reorder_mu_` 下等就绪 entry → 拷 lane 的
`shared_ptr`（防 UAF，holds-alive 模式） → `unlock` → `lane->reduce_fn(entry)`
→ `lane->applied_ord.store(...)` → `++lane->next_apply_ord` → `dec_in_flight`。

**背压（D4）**：reorder 在途上限 `kDefaultReorderInflightCap`（16384）
→ 达限 map worker 停 pop → queue 满 → put 阻塞。

### 6.2 CaskPluginHost 与 run_serialized

`Cask::CaskPluginHost`（`include/bitcask/plugin_api.hpp` /
`src/cask/cask.cpp` 的 `Cask::CaskPluginHost` 实现）实现
`plugin::PluginHost` 接口：

```cpp
class CaskPluginHost final : public plugin::PluginHost {
public:
    std::optional<std::string> read_at(plugin::RecordLoc loc) override;
    void run_serialized(std::function<void()> fn) override;
    void log(plugin::LogLevel, std::string_view) override;
};
```

`run_serialized`（`src/cask/cask.cpp`）：取 `ord` → 构造
`IndexTask{ op = RunFn, ord, fn }` → 经 `submit_index_task` 投递到
reducer。reducer 在 RunFn 静态点执行闭包（典型用途：merge 收尾的
HNSW 重建、checkpoint 序列化、`docmap_->compact_chunks()` 等必须
reducer 线程内的操作）。

为什么必须有这条通道：`concurrent_hash_map` 的遍历与插入并发不安
全（S13-F6 实证）——必须 reducer 静态点串行。所有「变异单写者状态」
的操作（merge 收尾 / checkpoint 序列化 / HNSW 重建）都经此通道
提交，闭包经 reorder buffer 按 ord 序与 Add / Delete 串行化。

### 6.3 RunFn 序号门（`IndexOp::Skip` + `OrdSkipGuard`）

写路径 `alloc_ord` 后、真任务提交前的任何错误 return 都必须给该
ord 补一条 `IndexOp::Skip`——否则 reducer 的 `next_apply_ord` 出现
永久空洞，此后 `flush` / merge / close 全部在 `flush_cv_` 上永久阻
塞（一次 ENOSPC 即卡死 handle）。

`Cask::OrdSkipGuard`（`cask.hpp` 内部 RAII）：

- **ctor**：拿 ord
- **disarm()**：真任务（或等价 Skip）已覆盖该 ord 后调
- **dtor**：armed 时自动 submit `IndexTask::make(IndexOp::Skip, {}, ord, ...)`

reducer 收到 `Skip` 等同「该 ord 已 apply」，直接推进 `next_apply_ord`。

---

## 7. KeyDirRegistry 与 FileLock

### 7.1 KeyDirRegistry

`include/bitcask/keydir_registry.hpp` 的 `KeyDirRegistry::mutex_`
（`std::mutex`）保护 `entries_` / `saved_biggest_file_id_` / `index_pool_`
全部成员（`std::scoped_lock lock(mutex_)`，`src/keydir/keydir_registry.cpp`）。

**三状态 `AcquireStatus`**（`kCreated` / `kReady` / `kNotReady`）协议
已实现，与 `concurrency-zh.md` §2 一致：

- `kCreated`：调用方是初始化者，必须 `load_keydir_from_disk` 后
  `mark_ready()`。
- `kReady`：共享 KeyDir，refcount + 1。
- `kNotReady`：名字存在但还在被别人初始化，调用方应重试 / 等待。

**`saved_biggest_file_id_[name]`**：refcount 归零时记下
`biggest_file_id + 1`——保证 file_id 跨 open / close 永不回退。

**`index_pool_` 懒创建**：registry 级共享的双池挂 registry 下（S6-P3
共享所有权）。`unregister_lib` 时不停池（其它库还在用）；registry
析构 → `~IndexPool` → `stop()` join 所有线程。

### 7.2 FileLock（flock）

`include/bitcask/file_lock.hpp` / `src/lock/file_lock.cpp`：

- 写锁：`O_CREAT | O_EXCL | O_RDWR | O_SYNC`，mode 0600。EEXIST 表示
  已有别人持有（stale 检查由 caller 在 EEXIST 后自己做——`try_remove_stale_lock`）。
- **先 unlink 后 close**（`release_quiet`）：让仍持有 fd 的 reader 还
  能从老 inode 读到一致内容；如果反过来，新同名锁文件可能被旧 reader
  读出 garbage（legacy 的既定顺序，照搬）。
- 内容是 `<pid>\n`，可选追加 `<active_file_path>`。

> FileLock 对象自身没有内部锁——`acquire` 每次产出新对象；同对象并
> 发 `write_data` 会撕裂内容（非原子），caller 须自己保证同一对象
> 串行使用。跨对象 `acquire` 由 `O_EXCL` 仲裁——并发 acquire 同一锁
> 文件名由内核原子性保证 safe。

### 7.3 跨进程边界——能开但语义弱

与 `concurrency-zh.md` §3 一致：

- **拿锁**：write.lock 是 OS 级 `O_CREAT | O_EXCL`，跨进程 enforce
  「最多一个 writer」；只读 open 不拿锁，不冲突。
- **内存独立**：每个 OS 进程有独立的进程内 `KeyDirRegistry`、独立的
  KeyDir。reader 进程 open 时 `load_keydir_from_disk` 自己扫一遍，
  得到一个**快照**——之后 writer 进程的 put 这个 reader 看不见。
- **reader 怎么看到新数据**：必须 close + reopen 重新扫盘。大目录
  每次几百 ms 到几秒，**不适合做实时 read replica**。

---

## 8. W 阶段交付物总账（as-built）

### 8.1 W1 交付（已实现 100%）

| 项                                                       | 落地位置                                                                                                                                  | 状态 |
|----------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|------|
| `Cask` 加 `std::mutex write_mu_`                         | `cask.hpp` 的 `std::mutex write_mu_;`                                                                                                     | ✅    |
| put / remove / put_doc / sync / close_write_file 入口加锁 | `src/cask/cask.cpp` 的方法入口 `std::unique_lock/std::lock_guard<std::mutex> wlk(write_mu_)`                                              | ✅    |
| backup 也加锁                                            | `src/cask/cask.cpp` 的 `backup`                                                                                                           | ✅    |
| `flush_index` 不纳入                                     | 读 / 搜索路径也调它，纳入会让搜索串行化——已审计不入锁                                                                                    | ✅    |
| 锁序确认：`write_mu_` 最外层 → `read_cache_mu_` / KeyDir 锁 | `cask.hpp` 的 S11-W1 注释明文                                                                                                              | ✅    |
| merge 不触 `write_mu_`                                   | merger 写自有输出文件，经 KeyDir `shared_mutex` 协调                                                                                       | ✅    |
| H1：索引提交移出 `write_mu_` 临界区                      | `src/cask/cask.cpp` 的 `Cask::put` 等：常规写 `wlk.unlock(); submit_index_task(...)`                                                      | ✅    |
| TSan 测试：`ConcurrentWritersSharedCaskNoCorruption`     | tests 已存在（详见各并发套件）                                                                                                              | ✅    |

### 8.2 W2 交付（已实现 100%）

| 项                                                          | 落地位置                                                                                                                                | 状态 |
|-------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|------|
| 读 / 搜索 TSan 套件                                         | `W2ConcurrentSearchAndWriteNoRace`（4 读 × 6 模式 + 2 写并发）                                                                          | ✅    |
| 搜索方法 doxygen「线程安全:是」修订                        | `cask.hpp` 各 `search_*` 方法注释                                                                                                       | ✅    |
| 写方法 doxygen「线程安全:是」修订                          | `cask.hpp` 各 `put` / `remove` / `put_doc` / `sync` / `close_write_file` 方法注释                                                       | ✅    |
| 同义词词典 setter 移除                                      | `CaskOptions::synonym_map`（`shared_ptr<const SynonymMap>`），open-time 注入；运行期无 setter                                            | ✅    |
| `Cask::open()` 顶部「线程模型」段落重写                     | `cask.hpp` 文件头：通用 C++ 库 / handle 多线程安全                                                                                       | ✅    |
| `set_synonym_map` 等运行期 setter 已审计移除                | 仅 `synonym_map` 一例，已结构化为 open-time 不可变；`log_fn` 同为 open-time 不可变                                                       | ✅    |

### 8.3 W3 交付（已实现 100%）

| 项                                                       | 落地位置                                                                                                                                | 状态 |
|----------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|------|
| `std::atomic<bool> closed_` + `is_closed()`              | `cask.hpp` 的 `std::atomic<bool> closed_{false};` 和 `is_closed()`                                                                      | ✅    |
| `close()` 顶 `exchange(true)`（幂等门）                  | `src/cask/cask.cpp` 的 `Cask::close`                                                                                                    | ✅    |
| 公共方法 fail-fast 返 `kClosed`                          | 数据面 / 搜索（集中守 `run_search_*`）/ 内省 / Iter.start                                                                               | ✅    |
| 不做完整 rundown                                         | 契约写明：close 时刻须无在途操作                                                                                                        | ✅    |
| `WriteOpGate`（H1 闭环）                                 | `cask.hpp` 的 `WriteOpGate`；含锁外索引提交尾段；close `writes_in_flight_.wait(0, seq_cst)`                                              | ✅    |
| `OperationsAfterCloseReturnErrorNotUb` 测试              | 测试已存在（详见各并发套件）                                                                                                              | ✅    |

---

## 9. 未完成项 / 仍开放

经本审计逐符号核验，**W1 / W2 / W3 / W4 全部已实现**，没有「仍开放」的
race condition 在主路径上。下列条目属于**已知残留风险 / 调用方契约**
而非 race condition bug：

1. **`CaskIter` 跨 `Cask` 对象生命周期存活仍是 UB**（`cask.hpp` 的 X1
   注释）：`parent_` 是裸 `Cask*`（非 weak / shared）；若 `Cask` 对象
   被销毁而 iterator 还存活，则 `next()` 访问 `parent_->opts_/dirname_/read_file`
   即悬空 UAF。X1 的 `keydir_pin_` 兜 KeyDir 生命周期，但不兜
   `Cask` 对象本身。已知结构性问题，留待 zero-copy 重构时用
   `weak_ptr` / owning 句柄解决。

2. **`close()` 与并发在途调用的契约**：caller 须保证 close 时刻无
   在途调用；`closed_` 是 best-effort 防误用，非完整 rundown。详见
   `cask.hpp` 的「S11-W3」注释。

3. **`flush` 在大库上的等待**：索引 ckpt 序列化经 RunFn 在 reducer
   线程按 ord 执行，大库可达秒~分钟级——期间 reducer 停摆、队列积
   压（H1 后背压只阻塞提交中的写者，不再冻结全部）。已实现的有界
   等待，**业务上需注意**：长 checkpoint 期间 search_* 返回陈旧索引。

4. **NFS / 网络盘上不可靠**：`O_CREAT|O_EXCL` 在 NFS 上有历史 bug，
   bitcask 不该跑在网络盘上（`file_lock.hpp` 文件头明文警告）。

5. **`synonym_map` 运行期更换**= 重开库；没有运行期 setter（结构化
   保证，无 race）。

---

## 10. 审计方法学说明

本审计的目的是把承诺的并发契约映射到**代码里真实存在**的同步原语：

1. **入口侧**：每个公共方法的 doxygen 注释「线程安全：是」+ 锁要求
   行 → 抓到入锁点。
2. **共享态侧**：每个共享成员（mutex / atomic / shared_mutex）抓
   声明位置与所有用锁点。
3. **路径侧**：从每个公共方法入口沿调用图走完直到外部 IO（pread /
   pwrite / open），确认所有触及的共享态都在某把锁或 atomic 下。
4. **锁序**：从每条加锁点导出锁全序，确认无环（特别是 KeyDir 的两
   处反向嵌套例外 + IndexPool 的「单锁」不变量 + Cask 的
   `write_mu_` 最外 / 读不取）。
5. **降级 / 失败路径**：失败补偿路径上的 Skip / disarm / 已 disarm
   guard 都需保留提交口（`OrdSkipGuard`）。

TSan 测试覆盖（既已存在，本审计不复述）：

- `ConcurrentWritersSharedCaskNoCorruption`（8 线程写同 handle）
- `W2ConcurrentSearchAndWriteNoRace`（4 读 × 6 模式 + 2 写并发）
- `OperationsAfterCloseReturnErrorNotUb`（close 后 fail-fast）
- `ParallelScanVisitsAllKeysOnce`（2000 key 删 1/10，每 key 恰一次）
- `crash_recovery` 套件（TSan 零 race）

---

## 附录 A. 关键同步原语清单（按符号）

| 符号                       | 类型                     | 所在                       | 作用 |
|----------------------------|--------------------------|----------------------------|------|
| `Cask::write_mu_`          | `std::mutex`             | `cask.hpp`                 | 写路径互斥（W1）|
| `Cask::read_cache_mu_`     | `std::shared_mutex`      | `cask.hpp`                 | `read_files_` + `active_data_` 缓存 |
| `Cask::closed_`            | `std::atomic<bool>`      | `cask.hpp`                 | fail-fast 标志（W3）|
| `Cask::writes_in_flight_`  | `std::atomic<uint32_t>`  | `cask.hpp`                 | close 等待在途写者（H1）|
| `Cask::ckpt_mu_`           | `std::mutex`             | `cask.hpp`                 | checkpoint 间互斥 |
| `Cask::active_file_id_`    | `std::atomic<uint32_t>`  | `cask.hpp`                 | writer / reader 间 hint（S13-F4）|
| `KeyDir::shards_[i].mu`    | `std::mutex`             | `keydir.hpp`               | 256 分片锁 |
| `KeyDir::meta_mu_`         | `std::shared_mutex`      | `keydir.hpp`               | fold / pending 协调 |
| `KeyDir::barrier_mu_`      | `std::mutex`             | `keydir.hpp`               | 屏障间互斥 |
| `KeyDir::gate_mu_`         | `std::mutex`             | `keydir.hpp`               | 写者退避的 cv 配套锁 |
| `KeyDir::gate_cv_`         | `std::condition_variable`| `keydir.hpp`               | 写者退避等待 |
| `KeyDir::barrier_active_`  | `std::atomic<bool>`      | `keydir.hpp`               | 写者闸门标志（release / acquire）|
| `KeyDir::fstats_grow_mu_`  | `std::mutex`             | `keydir.hpp`               | fstats 槽位增长串行 |
| `KeyDir::fstats_ptrs_`     | `std::atomic<AtomicFStats**>` | `keydir.hpp`         | fstats RCU 指针表（S13-F8）|
| `KeyDir::fstats_size_`     | `std::atomic<size_t>`    | `keydir.hpp`               | fstats 发布水位 |
| `KeyDir::epoch_` 等标量    | `std::atomic<uint64_t>` 等 | `keydir.hpp`             | 全局标量（写热 / 读热 cache line 分组）|
| `KeyDirRegistry::mutex_`   | `std::mutex`             | `keydir_registry.hpp`      | 注册表 + IndexPool 持有 |
| `IndexPool::start_mu_`     | `std::mutex`             | `thread_pool.hpp`          | started_ + 建线程 |
| `IndexPool::reorder_mu_`   | `std::mutex`             | `thread_pool.hpp`          | lanes_/pending/next_apply_ord |
| `IndexPool::flush_mu_`     | `std::mutex`             | `thread_pool.hpp`          | flush_cv_ 配套 |
| `IndexPool::queue_`        | `tbb::concurrent_bounded_queue` | `thread_pool.hpp` | MPSC 有界（cap = 10240）|
| `HnswIndex::chunks_`       | `std::array<std::atomic<NodeChunk*>, kMaxChunks>` | `hnsw.hpp` | 节点块无锁发布 |
| `HnswIndex::count_`        | `std::atomic<uint32_t>`  | `hnsw.hpp`                 | 发布水位 |
| `HnswIndex::entry_meta_`   | `std::atomic<uint64_t>`  | `hnsw.hpp`                 | 入口点合并发布 |
| `HnswIndex::writer_active_`| `std::atomic<bool>`      | `hnsw.hpp`                 | 单写者 assert |
| `NodeChunk::locks[]`       | `std::unique_ptr<std::atomic<uint32_t>[]>` | `hnsw.hpp` | per-node seqlock（S13-P7）|
| `VectorPlugin::hnsw_`      | `std::atomic<std::shared_ptr<HnswIndex>>` | `vector_plugin.hpp` | 整图换指针 |
| `InvertedIndex::vocab_mtx_`| `std::shared_mutex`      | `inverted.hpp`             | 排序词典 publish |
| `InvertedIndex::vocab_*`   | `std::shared_ptr<const vector<string>>` | `inverted.hpp` | 排序词典 CoW 发布 |
| `InvertedIndex::vocab_dirty_` | `std::atomic<bool>`   | `inverted.hpp`             | 排序词典 fast path 标志 |
| `PostingMap`               | `tbb::concurrent_hash_map` | `inverted.hpp`            | 64 分片桶级锁 |
| `PostingList`              | `shared_ptr<const PostingList>` 发布 | `inverted.hpp`     | CoW 冻结语义 |

## 附录 B. 锁全序速查

### Cask 门面

```
write_mu_ (mutex) 写路径最外层
  └─► read_cache_mu_ (shared_mutex) lazy open / 淘汰
        └─► DataFile 内部 fd 句柄（pread 无状态）
  └─► KeyDir shard mutex
        └─► KeyDir meta_mu_ (shared_mutex) 折叠态
  └─► fstats_grow_mu_ (mutex) 槽位增长

读路径不取 write_mu_：
read_cache_mu_ (shared_mutex)
  → DataFile pread
→ KeyDir shard mutex (shared)
  → KeyDir meta_mu_ shared (fold miss)
```

### KeyDir 内部

```
barrier_mu_ (mutex)
  → gate_mu_ (mutex) + gate_cv_
  → meta_mu_ (shared_mutex)
  → 单个 shard (mutex, 任意时刻 ≤1 把)
  → fstats_grow_mu_ (mutex)
```

两处反向嵌套例外（带无环论证）：

- **热路径 `shard → meta`**（与全序一致）
- **屏障内 `meta_shared → shard`**（`apply_pending_to_entries_barrier`）

### IndexPool

```
start_mu_  →  reorder_mu_  →  flush_mu_            顺序无要求
任一线程任一时刻最多持一把 ⇒ 无嵌套 ⇒ 无加锁顺序 ⇒ 不可能死锁
```

---

## 附录 C. 与 `concurrency-zh.md` 的关系

| `concurrency-zh.md`（用户向） | 本审计（维护者向）                                       |
|-------------------------------|----------------------------------------------------------|
| 锁层在哪些文件 / 函数         | 为什么这套原语在这里、对应哪条不变量                       |
| 同 handle 多线程契约表        | doxygen 注释依据、锁序推导、不变量论证                     |
| W1+W2+W3 已实现的现状         | **本审计是该现状的事实依据**；每条都映射到符号位置         |
| 跨进程 / `flock` 行为        | FileLock `O_EXCL` 语义、stale-reclaim、先 unlink 后 close |
| 部署模型推荐                 | 同：典型单机多线程部署、按目录分片扩展写并发                |
