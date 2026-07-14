# Bitcask 并发与共享语义

本文是 libbitcask 的并发契约**用户向**说明：文档承诺的线程安全保证必须在
代码中真实存在。每条锁声明都对应当前代码里的同步原语（mutex / shared_mutex
/ atomic / atomic_thread_fence / seqlock / RCU 指针 / tbb::concurrent_hash_map
/ tbb::concurrent_bounded_queue / tbb::task_arena / 文件锁 `O_EXCL`）。

> **同 handle 多线程契约**（S11 通用 C++ 库定位，README 引用的「单句柄多线
> 程安全」声明）的完整内部审计由
> [`docs/design/thread-safety.md`](../docs/design/thread-safety.md) 负责；本文聚
> 焦的是「同一个 handle / 同一个进程 / 跨 OS 进程共享同一 bitcask 目录」的
> 锁层与并发语义——锁声明在哪、把哪些不变量串起来。

源码导航：

- `Cask` 锁层：`include/bitcask/cask.hpp`、`src/cask/cask.cpp`
- `KeyDir` 分片与屏障：`include/bitcask/keydir.hpp`、`src/keydir/keydir.cpp`
- `KeyDirRegistry`（同进程内 KeyDir 共享）：
  `include/bitcask/keydir_registry.hpp`、`src/keydir/keydir_registry.cpp`
- 文件锁（跨进程隔离）：`include/bitcask/file_lock.hpp`、
  `src/lock/file_lock.cpp`
- 异步索引 MapReduce 流水线：`include/bitcask/thread_pool.hpp`
- HNSW 并发协议：`include/bitcask/hnsw.hpp`、`src/vector/hnsw.cpp`
- InvertedIndex：CoW posting：`include/bitcask/inverted.hpp`、
  `src/bm25/inverted.cpp`
- Search 进程级 arena：`include/bitcask/search_arena.hpp`、
  `src/search/search_arena.cpp`

---

## 1. 三种 open 模式

`Cask::open` 在 `src/cask/cask.cpp` 内部按阶段拆为
`acquire_open_locks()` → `check_or_create_meta()` → `create_search_infra()`
→ keydir 装配 → 索引 lane 注册。

`acquire_open_locks()` 的三路分支（对应 `CaskOptions` 的 `read_write` 与
`merge_only` 字段）：

| `read_write` | `merge_only` | 拿什么文件锁                            | active writer | 备注 |
|---|---|---|---|---|
| `false`      | `false`      | **不拿任何锁**                          | 不创建        | 只读快照；同进程各 handle 独立扫盘 |
| `true`       | `false`      | `bitcask.write.lock`（`O_CREAT\|O_EXCL`） | 创建          | 常规 writer |
| `true`       | `true`       | `bitcask.merge.lock`（独立文件）        | 不创建        | 周期 merge 专用，与 writer 并行不互斥 |

代码定位：

- 「不拿锁」分支——`acquire_open_locks()` 中 `if (!opts_.merge_only) return {};`
  直接返回，详见 `src/cask/cask.cpp` 的 `Cask::acquire_open_locks`。
- 「write.lock」分支——经 `acquire_writer_lock(dirname_)`，写入
  `<pid>\n`（随后 `ensure_active_writer` 把 active file 路径补上）。失败
  时 EEXIST → `try_remove_stale_lock` 探测 + 回收，再重试一次；二次
  EEXIST → `CaskError::kWriteLocked`。详见 `src/cask/cask.cpp` 的
  `acquire_writer_lock` 与 `try_remove_stale_lock`。
- 「merge.lock」分支——同样 `O_CREAT|O_EXCL` 拿 `bitcask.merge.lock`；额外
  以**只读**方式打开 `bitcask.write.lock` 拍下 live writer 的 active file
  id（`merger_writer_active_id_`），供 `needs_merge` 排除 live writer 正在
  写的文件。详见 `src/cask/cask.cpp` 的 `Cask::acquire_open_locks`。

文件锁的 `O_EXCL` 原子性在 Linux 上由内核保证（NFS 上有历史 bug，**不可**
在网络盘上跑——见 `file_lock.hpp` 文件头注释）。锁内容是 `<pid>\n`（可选
追加 `<active_file_path>`），stale-lock 回收以 `kill(pid, 0)` 探活。

---

## 2. 同进程内：共享 KeyDir（KeyDirRegistry）

**结论**：同一个进程里，多次 `Cask::open` 同一目录会共享**同一个**内存
KeyDir。第一个 open 付扫盘代价，后续全部 refcount + 直接拿 `shared_ptr`。

### 代码路径

`Cask::open`（`src/cask/cask.cpp`）中关键片段：

```cpp
if (registry != nullptr) {
    auto a = registry->acquire(cask->keydir_name_);   // ← name = dirname 字符串
    if (a.status == keydir::AcquireStatus::kNotReady) {
        // 50 ms 间隔轮询 40 次，最多等 2 秒
    }
    cask->keydir_ = a.keydir;                          // ← 拿到 shared_ptr<KeyDir>
    if (a.status == keydir::AcquireStatus::kCreated) {
        // ★ 只有第一个开它的人才进这条分支
        if (auto r = cask->load_keydir_from_disk(); !r) return std::unexpected(r.error());
        cask->keydir_->mark_ready();
    }
}
```

`KeyDirRegistry::acquire`（`src/keydir/keydir_registry.cpp`）在
`std::scoped_lock lock(mutex_)` 内完成：

```cpp
auto it = entries_.find(key);                  // key = dirname 字符串
if (it != entries_.end()) {
    if (!it->second.keydir->is_ready()) {
        return {kNotReady, nullptr};           // 别人正在初始化，等
    }
    it->second.refcount += 1;                  // ← 共享！只 +1 计数
    return {kReady, it->second.keydir};
}
auto kd = std::make_shared<KeyDir>();
auto saved_it = saved_biggest_file_id_.find(key);
if (saved_it != saved_biggest_file_id_.end()) {
    kd->increment_file_id_at_least(saved_it->second);  // file_id 永不回退
}
entries_.emplace(key, Slot{kd, 1});
return {kCreated, kd};                         // ← 这个 caller 负责扫盘
```

### 三种 acquire 状态

| acquire 返回 | 触发条件                       | 该 caller 干什么 |
|---|---|---|
| `kCreated`  | name 在 registry 里不存在       | 必须 `load_keydir_from_disk` 扫盘建索引，然后 `mark_ready` |
| `kReady`    | name 已存在且就绪                | 直接拿 shared_ptr，refcount +1，不扫盘 |
| `kNotReady` | name 已存在但还在被别人初始化     | 50 ms 间隔轮询 40 次，最多等 2 秒 |

> 关键：第一个 open 的是谁不重要（writer / reader / merger 都可能）——谁先
> 到谁付扫盘成本，后到的全部白嫖。

### 行为示例

```c
// T0：进程启动后第一次 open
opts.read_write = 0;
bitcask_open("/data/db", &opts, &R1, &fault);
// → acquire 返回 kCreated
// → load_keydir_from_disk 扫所有 .bitcask.data + hint
// → mark_ready
// → registry: {"/data/db" → {kd_ptr, refcount=1}}

// T1：第二个 open（读或写都行）
opts.read_write = 1;
bitcask_open("/data/db", &opts, &W, &fault);
// → acquire 返回 kReady
// → 不扫盘，秒返回
// → registry: {"/data/db" → {kd_ptr, refcount=2}}

// T2：再来 N 个 reader
bitcask_open("/data/db", &opts, &R2, &fault);  // refcount=3
bitcask_open("/data/db", &opts, &R3, &fault);  // refcount=4

// T3：W 写入
bitcask_put(W, k, v, 0, &fault);
// → Cask::put 内部 mutex write_mu_ 串行化
// → write_and_keydir → keydir_->put 拿目标 key 所在 shard 的 unique_lock
// → 所有 reader 立刻可见（共享同一个 KeyDir 实例）

// T4：R2 读
bitcask_get(R2, k, &res, &fault);  // ✅ 看见 W 刚写的
```

四个 Ref 共享：

- 同一个 `shared_ptr<KeyDir>`（`cask.hpp` 的 `Cask::keydir_`）
- 同一个 `KeyDirRegistry::entries_` 哈希表
- 同一个 `KeyDir::shards_`（256 个分片各自的 mutex + ankerl::unordered_dense::map）
- 同一个全局标量 atomic 集（epoch_/key_count_/key_bytes_/next_ord_/biggest_file_id_/...）
- 同一个 fstats 槽位表（RCU 指针表 + atomic 计数）

### 关闭与持久化

```c
bitcask_close(R1);  // refcount 4 → 3，shared_ptr 还活着
bitcask_close(R2);  // 3 → 2
bitcask_close(R3);  // 2 → 1
bitcask_close(W);   // 1 → 0
// ★ 此时 KeyDir 才真正析构
// ★ saved_biggest_file_id_ 记下 biggest_file_id + 1
```

`KeyDirRegistry::release` 在 `std::scoped_lock lock(mutex_)` 内：refcount
归零时 `saved_biggest_file_id_[name] = max(saved, keydir->biggest_file_id() + 1)`
持久化下来，**保证 file_id 跨 open/close 永不回退**——避免「老 file_id 被
keydir 当成新 entry」的灾难（见 `src/keydir/keydir_registry.cpp`）。

### name 不规范化的小坑

`name` 是原样字符串比较，**没有 canonicalize**：

```c
bitcask_open("/data/db",  &opts, &a, &fault);   // slot key = "/data/db"
bitcask_open("/data/db/", &opts, &b, &fault);   // slot key = "/data/db/"  ← 不同！
```

会建两个独立的 KeyDir，两次扫盘，互相看不到对方的写入。跟 legacy 行为
一致，调用方需要自己保证路径字符串规范化。

---

## 3. 跨 OS 进程：能开但语义弱

不同 OS 进程（比如两个独立服务进程，或服务进程 + 命令行工具）：

```
进程 A: bitcask_open(Dir, &opts, &A, &fault), opts.read_write=1  → 拿 bitcask.write.lock
进程 B: bitcask_open(Dir, &opts, &B, &fault), opts.read_write=0  → ✅ 不拿锁，open 成功
进程 C: bitcask_open(Dir, &opts, &C, &fault), opts.read_write=0  → ✅ 同上
```

- **拿锁**：write.lock 是 OS 级 `O_CREAT|O_EXCL`，跨进程 enforce「最多
  一个 writer」；只读 open 不拿锁，不冲突。
- **内存独立**：每个 OS 进程有独立的进程内 `KeyDirRegistry`、独立的
  KeyDir。reader 进程在 open 时 `load_keydir_from_disk` 自己扫一遍，得
  到一个**快照**——之后 writer 进程的 put 这个 reader **看不见**。
- **reader 怎么看到新数据**：必须 close + reopen 重新扫盘。对大目录
  （百万 key 级别）每次几百 ms 到几秒，不实用。

跨进程「带快照」的只读 open 适合做：

- 离线备份 / 导出
- 一致性快照分析
- 命令行工具临时查询

不适合做「实时 read replica」——bitcask 不是为这个设计的。

---

## 4. 写写冲突的行为

第二个 writer 试图 open：

```c
opts.read_write = 1;
bitcask_open(Dir, &opts, &W1, &fault);          // ok，拿到锁
bitcask_open(Dir, &opts, &W2, &fault);          // fault.code == BITCASK_ERR_WRITE_LOCKED
```

返回 `CaskError::kWriteLocked`——来自 `O_CREAT|O_EXCL` 的 `EEXIST`，被
`acquire_writer_lock` 翻译。

但**如果 W1 进程 crash 了**锁文件还在磁盘上：W2 open 时 `try_remove_stale_lock`
读锁文件拿到 W1 的 pid，`kill(pid, 0)` 探活，`ESRCH` → unlink stale lock →
重试 acquire → 成功。详见 `src/cask/cask.cpp` 的 `try_remove_stale_lock`。

竞态窗口：从读 pid 到 unlink 之间另一个 writer 可能写了新锁，我们会
误删他的。legacy 也有同样的 race，实际暴露面极小，只发生在 crash
recovery 路径。

---

## 5. merger 是个特例

merger 用 `merge_only` 选项 open（由 `bitcask_merge()` / `Cask::merge()` 内
部触发，不是对外直接 API），拿的是 `bitcask.merge.lock`（独立锁），**不
阻塞 writer**：

```
进程 A: writer       → 拿 bitcask.write.lock
进程 A: merger (merge_only=1) → 拿 bitcask.merge.lock，不抢 write.lock
进程 B: reader       → 不拿锁
```

所以稳态可以是：**1 writer + 1 merger + N readers** 同时活跃，互不
阻塞。这是 bitcask 跑生产负载的标准并发栈。

merge 的实现见 `include/bitcask/merger.hpp` 的 `run_merge`：自身**不取
任何 mutex**——CAS 更新 keydir 与 unconditional read 走的是 KeyDir 自带
的分片锁 + atomic 路径。`Cask::merge` 的并发契约（`cask.hpp` 注释）要求
**caller 保证同目录同时仅一次 merge 在跑**——由外部 `bitcask.merge.lock`
仲裁，不是 merger 内部自己管。

### merge 对读写的影响（正确性 vs 性能）

merge 设计成**对读写无阻塞**，靠的不是单一锁、而是 keydir 分片锁 + CAS +
文件生命周期管理：

| 路径 | 影响 | 为什么安全 |
|------|------|-----------|
| **写** | 不阻塞 | merge 不抢 `bitcask.write.lock`；并发改同一 key → keydir 的 `conditional_remove` / merge 路径 `put(newest_put=false, old_file_id, old_offset)` 的 CAS 失败，writer 赢；merge 拷贝沦为死字节下轮再清；writer 发现 `biggest_file_id` 被 merger 推进就 `roll_active` 到更大 id（详见 `Cask::put` 中 `if (active_data_ && active_file_id_ < keydir_->biggest_file_id()) roll_active();`）。无写丢失。 |
| **点 get** | 不阻塞 | merge 「先写新文件 + CAS keydir、**最后**才 unlink 旧文件」；单次 `bitcask_get` 调用从 keydir.get 到 pread 是同步的，要读到被删文件得被 OS 抢占跨越整个 merge——实践可忽略。`Cask::get` 还做了一层防御：read_file lazy open 失败时重查 keydir 重试一次（`src/cask/cask.cpp` 的 `Cask::get` 注释 S13-F5）。 |
| **fold** | 不阻塞（**S13** 修复） | 见下。 |

**fold 的文件句柄快照（S13）**：fold 跨越多次 `bitcask_iter_next` 调用、
墙钟时间长，是真正可能撞上 merge unlink 的路径。`CaskIter::pin_files`
在 `start()` 阶段 pin 一份「目录下全部非 active data 文件」的只读句柄
快照（实现见 `src/cask/cask_iter.cpp`）：

- `next()` 优先从 pin 的句柄 pread；merge 即便 unlink 了旧文件，已 open 的
  fd 让 inode 在 Linux 上存活，fold 照常读到。
- `release()` 时才关掉这些 fd——被 fold pin 住的旧文件，磁盘空间要等
  fold 结束才真正回收。
- 代价：每个并发 fold 占用「文件数」量级的 fd（与 legacy riak bitcask 的
  readable_files 快照一致，fold 本就重）。
- `pin_files` 拍 active_data_ 快照时持 `parent_->read_cache_mu_` 的
  `shared_lock`（`cask_iter.cpp` 的 `CaskIter::pin_files`），与
  `Cask::ensure_active_writer` / `roll_active` 的 `unique_lock` 互斥，
  避免对同一 `shared_ptr` 的并发读写（`shared_ptr` 对象本身的并发
  读写非线程安全）。

> 这一层修复前是个真实 bug：fold 走共享 `read_file` 缓存读 value，merge
> 无条件 unlink，长 fold 会因旧文件消失而中途报 `{error,_}`。

**性能影响才是 merge 的真实代价**（不是阻塞）：

- merge 回读旧文件 + 写新文件，跟正常读写**抢磁盘 IO / CPU**。
- **索引模式下最重**：每次 merge 后**同步**全量 `rebuild_index`（回读
  所有 live 文档、重新分词、建全新 InvertedIndex）+ `save_snapshot`。
  纯 KV 无此项。
- `bitcask_merge` 是同步调用——调用方通常在独立线程中执行 merge，不阻塞
  读写路径。

### file_id 分配：writer 与 merger 各写各的文件

merge 期间「正在写新文件」的有两方，但写的是**不同文件、不同句柄**，不
共享：

- **writer** 的 `put` append 到自己的 active 文件（`active_data_`）。
- **merger** 的 `run_merge` 新建自己的输出文件（`out_data`）。

file_id 由 keydir 的**单调计数器** `increment_file_id()` 统一分配
（`biggest_file_id_ += 1`，writer 的 `ensure_active_writer` 与 merger 共用
它；详见 `src/keydir/keydir.cpp` 的 `KeyDir::increment_file_id`）。

典型时序（当前 biggest = N，writer active = N）：

```
① merger 开 run_merge → increment_file_id() → N+1，输出写到 N+1
② writer 下次 put 发现 active(N) < biggest(N+1)
   → roll_active → increment_file_id() → N+2，新 active = N+2
```

所以「merger 写 N+1、writer 写 N+2」在「merger 先分配」的时序下成立，且
字面连号。但**具体谁拿 N+1 取决于谁先调** `increment_file_id`；真正保证
的不变量是：

> **writer 的 active file_id 永远被顶到 merger 输出之上。**

机制：writer 每次 put 前查 `biggest_file_id()`（atomic relaxed 读），发现
被 merger 推进了就主动 `roll_active` 到 ≥ biggest（`Cask::put` 与
`Cask::put_batch` / `Cask::put_doc` 都有这条 pre-roll 路径）。这条不变量
是正确性的核心——keydir 用 **file_id 大小判 newest/staleness**：merger 搬
的是旧数据快照（语义更旧，必须更小 id），writer 的新写必须更大 id，于是
并发改同一 key 时 writer 的值（大 id）天然胜出，merger 搬进 N+1 的那份
被判 stale、沦为死字节下轮清。若 writer 误写进 ≤ merger 的 id，keydir 会
当 merge-race 拒掉（`kAlreadyExists`）→ 由主动 roll + put 后的 roll-retry
兜住（`Cask::write_and_keydir` 内的重试逻辑），不会 silent drop。

---

## 6. 单 handle 多线程（核心契约）

**S11 通用 C++ 库定位之后**：同一个 `Cask` handle 可被多线程安全共享，无
需每线程一个实例。

### 读路径并发

读路径无外部锁——`get` / `search_*` / 批量搜索的并发性由四层同步原语
保证：

1. **KeyDir get**（`src/keydir/keydir.cpp` 的 `KeyDir::get`）：目标 key
   所在分片 `Shard::mu` 的 `unique_lock`（写者用 mutex 不是 shared_mutex——
   S5 由 rwlock 切到 mutex 是有意的：临界区足够短且消除写者偏好停车；详
   见 `keydir.hpp` 的 `Shard::mu` 注释）。`fold` 态 miss 时嵌套
   `meta_mu_` 的 `shared_lock` 查 `pending_`。
2. **DataFile 句柄**（`src/cask/cask.cpp` 的 `Cask::read_file`）：命中走
   `Cask::read_cache_mu_` 的 `shared_lock`（多读者并发），`read_clock_`
   atomic 维护近似 LRU 单调计数；miss 时升级 `unique_lock` 做 lazy open
   并调 `evict_read_handles_locked` 淘汰超限空闲句柄。`DataFile::read`
   用 `pread`，thread-safe（POSIX `pread` 不动 fd offset）。
3. **HNSW search**（`src/vector/hnsw.cpp`）：`copy_neighbors` 走 per-node
   seqlock（`NodeChunk::locks[slot]` 的 `std::atomic<uint32_t>`）；读者双
   读序号一致才采信——torn 读被 seq 复读检测丢弃。数据字读写均经
   `std::atomic_ref`（relaxed，TSan 干净）。节点块发布经
   `std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_`（裸指针 +
   release/acquire——避原子引用计数开销）。`count_` 与 `entry_meta_` 是
   `std::atomic`，search 开头以自己 load 的 count 为可见边界。
4. **InvertedIndex search**（`src/bm25/inverted.cpp`）：tbb::concurrent_hash_map
   提供桶级锁，查询期可并发迭代（`PostingMap::const_accessor` 是 RAII 锁
   包装）。`vocab_mtx_` 是 `shared_mutex`，fast path 是 `shared_lock` 读
   排序词典快照（`shared_ptr<const vector<string>>`，零拷贝发布）。`ensure_vocab`
   入口 `acquire` 读 `vocab_dirty_`，false 直接 fast path；true 升级
   `unique_lock` 增量重建。

`parallel_scan`（`src/cask/cask.cpp` 的 `Cask::parallel_scan`）自己 spawn
`std::thread` 把快照 key 分段并发 `get`——底层就是上面 (1)+(2)。每段独立
不相交 key，`fn` 必须线程安全。

### 写路径串行化（**write_mu_**）

**S11-W1 关键设计**：`Cask::put` / `put_batch` / `remove` / `put_doc` /
`sync` / `close_write_file` / `backup` 在 `src/cask/cask.cpp` 的入口处都
持有 `std::mutex write_mu_`（`cask.hpp` 的 `Cask::write_mu_`），串行化整
个写序列。**同一 handle 可被多线程并发写而不损坏数据**——写在文件层本
就串行（append-only active file），锁不损吞吐。

| 方法 | 入口加锁点（`src/cask/cask.cpp`） |
|---|---|
| `Cask::put`             | `std::unique_lock<std::mutex> wlk(write_mu_)` |
| `Cask::put_batch`       | 同上 |
| `Cask::remove`          | 同上 |
| `Cask::put_doc`         | 同上 |
| `Cask::sync`            | `std::lock_guard<std::mutex> wlk(write_mu_)` |
| `Cask::close_write_file` | 同上 |
| `Cask::backup`           | 同上（备份期间挡住写者，让 active 文件可安全 hardlink） |

`write_mu_` 内部又可能取：

- `read_cache_mu_`（`Cask::ensure_active_writer` / `roll_active` 替换
  `active_data_` shared_ptr 时；详见 `src/cask/cask.cpp`）。
- keydir 内部锁（`write_and_keydir` → `keydir_->put` → 目标分片
  `unique_lock` + 嵌套 `meta_mu_` 的 `unique_lock`）。

`write_mu_` **不与读路径冲突**：读路径不取 `write_mu_`（`get` /
`search_*` / `parallel_scan` 都直接走 keydir 与 `read_cache_mu_`），所以
读写并发安全（见 `cask.hpp` 注：「读路径不取 write_mu_（get/搜索保持无
锁/共享锁，吞吐不变）」）。

更高写并发 → **按目录分片多个 Cask 实例**（单 append WAL 的横向扩展手段，
详见 README）。

### 索引提交移出 `write_mu_` 临界区（**H1**）

写路径持 `write_mu_` 时只做：校验 → roll → encode → `write_buffered` →
keydir apply → 组提交（fsync）。索引任务（`IndexOp::Add` / `Delete`）的
`submit_index_task` **在 `write_mu_` 释放之后**才调用（`Cask::put` 中
`wlk.unlock(); submit_index_task(...)` 的顺序）。背压由 IndexPool 有界
队列（capacity 10240，详见 §10）提供——队列满时 push 阻塞**本写者**，
不再冻结全部写路径。

### WriteOpGate 与 close 协调

写路径全程受 `WriteOpGate`（`cask.hpp` 的 RAII）保护：

- ctor：`writes_in_flight_.fetch_add(1, seq_cst)`
- dtor：`writes_in_flight_.fetch_sub(1, seq_cst)`，归零时 `notify_all`

`Cask::close()` 的核心等待循环（`src/cask/cask.cpp` 的 `Cask::close`）：

```cpp
if (closed_.exchange(true)) return;       // 幂等门
for (auto n = writes_in_flight_.load(seq_cst); n != 0;
     n = writes_in_flight_.load(seq_cst)) {
    writes_in_flight_.wait(n, seq_cst);   // 等所有在途写者退出
}
```

seq_cst 序保证写者「inc 后读 closed_」与 close「写 closed_ 后读计数」
构成 store-buffer 形状——RMW 的全序 + seq_cst load 保证两侧不会同时读
到旧值（见 `cask.hpp` 注）。`closed_`（`std::atomic<bool>`，默认
memory_order_acquire）让公共方法 fail-fast 返回 `CaskError::kClosed`。

### 同义词 / 日志回调：open-time 不可变

`CaskOptions::synonym_map`（`shared_ptr<const SynonymMap>`）与
`log_fn`（`std::function<void(LogLevel, std::string_view)>`）都是
**open-time 配置、构造后只读**——同义词词典构造时一次性拷贝（不可变
shared_ptr），运行期没有 setter；运行期唯一可改的是 `rebase_needed_`
等 ckpt 内部状态（受 plugin 内部锁保护）。这条不变性消除了「配置项里
唯一的 reader-vs-writer 竞态源」（见 `cask.hpp` 中 synonym_map 注释）。

### close() 生命周期契约

`Cask::close()` 标 `noexcept`，二次幂等。完整步骤：

1. `closed_.exchange(true)` 置位（兼作幂等门）。
2. `writes_in_flight_.wait(0, seq_cst)` 等待所有写者退出（含锁外的索
   引提交尾段）——**这是 H1 引入的防御性收敛**，把「close 与并发
   在途写」的 UB 收敛为「阻塞等待」（见 `cask.hpp` 注）。
3. `try/catch` 兜底：落最后一批 `maybe_group_commit(force=true)` →
   `active_hint_->finalize()` → 持 `read_cache_mu_` 的 `scoped_lock`
   清理 `active_data_` / `read_files_`。
4. `index_pool_->unregister_lib(index_lane_)`：先 flush 排空本 lane，
   再从 `lanes_` 移除（不停池——池由 registry 持有）。
5. 若 `text_` 且 `read_write`：`save_search_ckpt_paired` 强制全量 base
   + `write_keydir_snapshot`。
6. `registry_->release(keydir_name_)`（refcount -1）。
7. 析构 plugins / docmap / hybrid。
8. `write_lock_->release_quiet()`——`FileLock` 析构走「先 unlink 后 close」
   顺序，让仍持 fd 的 reader 能从老 inode 读到一致内容（见
   `src/lock/file_lock.cpp` 的 `release_quiet`）。

**契约**：caller 须保证 close 时刻**没有其它线程仍在调用**
`get` / `put` / `remove` / `sync` / `iter` / `search_*`（即
`writes_in_flight_` 已经归零）。H1 提供了 UB → 阻塞等待的兜底，但不
消除「业务上不该并发 close」的契约。`closed_` 设位后**新发起的**公共
调用 fail-fast 返回错误码——不会解引用已释放状态。

---

## 7. KeyDir：256 分片 + MVCC

`include/bitcask/keydir.hpp` 与 `src/keydir/keydir.cpp` 是 KV 层的核心并
发结构。设计目标是「读写高并发 + 一致性 fold + merge race 安全」。

### 7.1 256 分片

```cpp
static constexpr std::size_t kShards = 256;          // S5: 16 → 64 → 256
struct alignas(64) Shard {
    mutable std::mutex mu;                            // S5: rwlock → mutex
    alignas(64) ankerl::unordered_dense::map<std::string, Entry,
                                             StringHash, std::equal_to<>>
        entries;                                      // 稠密哈希表，零拷贝 hash
};
mutable std::array<Shard, kShards> shards_;
```

- **256** 是经验值：分片越多，单分片碰撞概率越低、写者停车传染面越小、
  并发度越高（详见 `keydir.hpp` 的 `kShards` 注释）。
- 每分片一把 `std::mutex`（不是 `shared_mutex`）：临界区足够短，mutex
  性能更好且消除 rwlock 的写者偏好停车问题。
- `alignas(64)` 把 map 头与锁字分到不同 cache line（map 头独占缓存行；
  见 `Shard` 注释）。

### 7.2 锁全序（屏障 v2）

**严格遵守**（`keydir.hpp` 文件头）：

```
barrier_mu_ → gate_mu_ + gate_cv_ → meta_mu_ → 单个 shard（任意时刻 ≤1 把）→ fstats_grow_mu_
```

- `barrier_mu_` (`std::mutex`)：屏障间互斥，跨整个屏障持有。
- `barrier_active_` (`std::atomic<bool>`)：写者闸门标志（释放序发布）。
- `gate_mu_` (`std::mutex`) + `gate_cv_` (`std::condition_variable`)：
  写者退避等待的 cv 配套锁。
- `meta_mu_` (`std::shared_mutex`)：fold 期间的 pending_/iter 协调状态
  ——冷路径（仅 fold 期间触碰）。
- `shards_[i].mu` (`std::mutex`)：256 把分片锁，任意瞬间至多持 1 把。
- `fstats_grow_mu_` (`std::mutex`)：仅新 file_id 槽位构造时碰。

> **历史原因**：旧 `lock_all_shards()` 同时持 257 把锁，撞 TSan 死锁检测
> 器的 64 持锁硬上限（`compiler-rt sanitizer_deadlock_detector.h:67
> CHECK`，实测 `KeyDir.DeepCopyPreservesOrd` 在 `detect_deadlocks=1`
> 下崩溃），已删除。BarrierGuard v2 改为 RAII 屏障 + 逐分片
> 加锁-放锁排干（详见 `src/keydir/keydir.cpp` 的 `BarrierGuard` 注释）。

### 7.3 两处反向嵌套例外（带无环论证）

**例外 ①** —— **热路径 shard → meta**：

| 操作 | 嵌套点 |
|---|---|
| `KeyDir::get` | `src/keydir/keydir.cpp` 中 `unique_lock slock(sh.mu)` 之后 miss + fold 态时嵌套 `shared_lock mlock(meta_mu_)` |
| `KeyDir::put_probe` | 同上，fold 态时嵌套 `unique_lock mlock(meta_mu_)`（pending_ 插入） |
| `KeyDir::remove` | 同上 |

**方向**：`shard → meta`（与全序一致）。

**例外 ②** —— **屏障内 meta_shared → shard**：

`KeyDir::apply_pending_to_entries_barrier` 在持 `meta_mu_` shared 期间
嵌套分片锁（`src/keydir/keydir.cpp`），把 pending_ 逐条 apply 进各分片
entries。

**方向**：`meta → shard`（**与全序相反**）。

**无环论证**（`keydir.hpp` 文件头）：

1. 方向②仅存在于屏障内——彼时写者（meta unique 的全部使用者）已被闸
   门出清，仅剩读者走方向①且对 meta 只拿 shared。
2. ②也只拿 meta shared；shared-shared 相容，无 unique 排队者。
3. meta 获取不可能阻塞——无「持 shard 等 meta / 持 meta 等 shard」环。
4. 屏障外只有方向①——同样无环。

### 7.4 fold 的 sibling chain + pending hash（MVCC）

`keyfolders_`（`std::atomic<uint64_t>`，alignas(64) 单独 cache line）记
录当前活跃 fold 数。当 `keyfolders_ > 0` 时：

- **已存在 key 的新写**：在分片内升级 `SingleEntry` → `MultiEntry`（一
  条 newest-first 的 sibling 链）。分片锁内完成，不触 meta。
- **新 key 的写入 / fold 期间临时 key 的 tombstone**：经 `meta_mu_`
  unique 写入 `pending_`（`std::unordered_map`，meta 锁保护）。
- **不变量（S2 起）**：`key ∈ 某分片 entries ⟹ key ∉ pending_`——已
  存在 key 的新版本一律走 sibling 链，绝不进 pending。`get` / `put` /
  `remove` 的「entries 优先、miss 再查 pending」探测顺序依赖该不变量
  （详见 `src/keydir/keydir.cpp` 的 `KeyDir::get` 注释）。

最后一个 fold release 时：

1. 阶段二：`apply_pending_to_entries_barrier` 把 pending_ 逐条应用进各
   分片 entries（持 meta shared + 屏障内例外）。
2. 阶段三：`collapse_multi_entries_barrier` 把各分片 `MultiEntry` 折
   回 `SingleEntry`（持 BarrierGuard，逐分片加锁-折叠-放锁）。
3. 清 `pending_` 与 `has_pending_` atomic。

IterHandle 自己持 `KeyDir*` 裸指针；Cask 侧通过 `CaskIter::keydir_pin_`
（`shared_ptr<KeyDir>`，见 `cask.hpp`）保证迭代器存活期间 KeyDir 续命
（X1 修复：防止 close 后 reset keydir_ 导致裸指针悬空 UAF）。

### 7.5 全局标量全部 atomic（M6-S1）

```cpp
alignas(64) std::atomic<std::uint64_t> epoch_{0};
std::atomic<std::uint64_t> key_count_{0};
std::atomic<std::uint64_t> key_bytes_{0};
std::atomic<std::uint64_t> next_ord_{0};     // alloc_ord / advance_ord 无锁

alignas(64) std::atomic<std::uint64_t> keyfolders_{0};
std::atomic<std::uint32_t> biggest_file_id_{0};
std::atomic<bool> has_pending_{false};
std::atomic<bool> is_ready_{false};
std::atomic<size_t> fstats_size_{0};
std::atomic<bool> iter_mutation_{false};
```

缓存行分组：写热行（epoch_/next_ord_/key_count_/key_bytes_）与读热行
（keyfolders_/biggest_file_id_）分到不同 cache line，避免 false sharing
（S2 实测关键）。

### 7.6 fstats：无锁热路径 + RCU 指针表（M6-S1 + S13-F8）

`KeyDir::update_fstats`（`src/keydir/keydir.cpp`）：

```cpp
auto add = [](std::atomic<uint64_t>& a, int32_t inc) {
    a.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(inc)),
                std::memory_order_relaxed);
};
add(f.live_keys, live_inc);
// ...
```

- 7 个 `std::atomic` 字段全 relaxed RMW（`AtomicFStats`，`alignas(64)`
  每元素独占 cache line，否则 merge + active 并发写不同文件会假共享）。
- 增长路径：deque `fstats_` + 旁挂 RCU 指针表 `fstats_ptrs_`（`std::atomic<AtomicFStats**>`）
  ——旧表退休不释放，总内存 < 2× 终表 ≈ 16B/file_id，有界。读者
  `fstats_slot(idx)` 前置条件 `idx < fstats_size_.load(acquire)`，
  size 的 release 发布在指针表填充/替换之后——acquire 读者必见新表。
- 关键修复（S13-F8）：直接 `fstats_[idx]` 会与 `emplace_back` 的 deque
  内部块表重分配构成 UAF（TSan 实证抓出）；RCU 指针表闭合。

### 7.7 conditional_remove TOCTOU

实现分两阶段（peek + remove），peek 瞬间持分片锁 + 嵌套 meta shared，
释放；remove 重新取分片 unique lock 检查命中——remove 内部 re-check 当
前状态，幂等安全。详见 `src/keydir/keydir.cpp` 的
`KeyDir::conditional_remove`（两阶段协议 + 锁全序均在该函数附近注释）。

---

## 8. ReadHandle LRU 缓存与 `max_read_handles`

`Cask::read_files_` 是按 `file_id` 缓存的 `DataFile` 句柄表，读路径懒打
开（`src/cask/cask.cpp` 的 `Cask::read_file`）。

| 成员 | 类型 | 作用 |
|---|---|---|
| `read_cache_mu_`        | `std::shared_mutex` | 命中走 shared；lazy open / 淘汰走 unique（`const` 内省也需锁） |
| `ReadHandle::df`       | `std::shared_ptr<DataFile>` | 在途读者持引用计数，merge unlink 不析构 |
| `ReadHandle::atime`    | `std::atomic<uint64_t>`     | 近似 LRU 命中时间（在 shared_lock 下 store 安全） |
| `read_clock_`          | `std::atomic<uint64_t>`     | 全局单调访问计数（每命中 `fetch_add`） |

**`max_read_handles` 上限**（`CaskOptions::max_read_handles`，cask.hpp）：

| 取值                              | 行为 |
|---|---|
| `0`（默认）                       | 由 `RLIMIT_NOFILE` 软上限推导安全上限（约一半，下限 64）。详见 `Cask::resolve_read_handle_cap`。 |
| `kUnlimitedReadHandles`           | 不限（旧默认行为：最大吞吐、无淘汰 churn，caller 自负 fd 预算） |
| 其它 N                            | 显式上限 |

`Cask::evict_read_handles_locked` 在 `read_cache_mu_` 独占锁下淘汰最旧
的**空闲**句柄（`use_count() == 1`）——在途读者的 `shared_ptr` 引用计数
让 fd/mmap 随最后引用析构才释放。`cap` 是**软上限**（仅当有空闲句柄可
淘汰时才收缩；满载时允许超额）。

---

## 9. HNSW 单写者 + 多读者无锁发布

`include/bitcask/hnsw.hpp` 与 `src/vector/hnsw.cpp` 的并发协议由 5 类原
语组成：

### 9.1 节点块无锁发布

```cpp
std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_{};  // 写者 release / 读者 acquire
std::atomic<uint32_t> count_{0};                              // 发布水位
std::atomic<uint64_t> entry_meta_{0};                         // 高 32 = level+1, 低 32 = entry id
std::atomic<uint64_t> max_inserted_ord_{...};
```

为什么裸指针而不是 `shared_ptr`：`HnswIndex::copy_neighbors` 在热路径
每次都 load，原子引用计数开销不可接受。裸指针 + release/acquire 由
`~HnswIndex()` 单线程 delete（彼时无并发读者）兜底——见 `hnsw.hpp` 注释
（`kMaxChunks` 上限 64M 节点）。

### 9.2 per-node seqlock（S13-P7）

```cpp
std::unique_ptr<std::atomic<uint32_t>[]> locks;  // 每节点一个 seq 序号
```

写者：`seq → 奇`（进入，relaxed fetch_add）→ atomic_thread_fence
(release) → relaxed 写邻接数据 → `seq → 偶`（release 发布）。
读者：`acquire` 读 seq（奇则 `cpu_pause` 退避）→ `atomic_ref` relaxed
读邻接 → `acquire` fence → 复读 seq 一致才采信。数据字读写均经
`std::atomic_ref`（relaxed，TSan 干净，UB-free）。

**升级动机**：此前是自旋锁——读者对锁字节做 exchange（写操作），HNSW
流量高度偏向 hub 节点 → 并发查询时锁缓存行核间乒乓。seqlock 让读者只
读不写，零共享行写。

### 9.3 单写者声明

```cpp
std::atomic<bool> writer_active_{false};

void HnswIndex::insert(...) {
    const bool was_active = writer_active_.exchange(true);
    assert(!was_active && "HnswIndex::insert: single writer only");
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } guard{...};
    // ...
}
```

引擎范围内 insert **只在 IndexPool reducer 线程**执行（与异步索引管
线串行，详见 §10）；rebuild 路径（HNSW 全量重建）通过 `RunFn` 任
务经 reorder buffer 串行化到 reducer 线程。多写者不支持——debug assert
明文声明。

### 9.4 visited：thread_local 版本化数组

`src/vector/hnsw.cpp` 的 `thread_local VisitedTable t_visited`——每读者
线程一份 `{marks, epoch, owner}`。`owner` 是 HnswIndex 的**全局自增实
例 id**（非 this 指针——指针 delete/new 后可复用，会让陈旧 marks 与新
实例的 epoch 假性匹配）。owner 切换时整组清零 + epoch 归零；同实例内
epoch 自增免清零，回绕时整组清一次。

### 9.5 VectorPlugin 的整图换指针

```cpp
std::atomic<std::shared_ptr<HnswIndex>> hnsw_;  // vector_plugin.hpp
```

- 读路径：`hnsw_.load(acquire)` 拿当前快照，多查询共享同一图。
- 写路径：merge 重建时**旁路建新图**（reducer 线程内执行 RunFn，新图
  完整就绪），最后 `hnsw_.store(new_ptr, release)`。旧图由在途读者的
  `shared_ptr` 引用计数续命，析构由 GC 自然清理。

---

## 10. InvertedIndex：CoW posting + 并发查询

`include/bitcask/inverted.hpp` 与 `src/bm25/inverted.cpp` 的并发模型：

### 10.1 tbb::concurrent_hash_map（桶级锁）

```cpp
using PostingMap = tbb::concurrent_hash_map<std::string,
                                            std::shared_ptr<PostingList>>;
struct Shard {
    PostingMap inverted;
    // ...
};
static constexpr std::size_t kShardCount = 64;   // 按 term hash % 64
std::array<Shard, kShardCount> shards_;
```

- 64 个分片，按 term hash `% kShardCount` 路由（term hash 分桶，平衡
  写冲突）。
- `PostingMap::accessor` / `const_accessor` 是 RAII 桶锁包装：写者持
  `accessor` 改对应 term 的 `PostingList`；读者持 `const_accessor` 拷贝
  `shared_ptr<PostingList>` 出来再放锁。
- `tbb::concurrent_hash_map` 支持并发迭代——查询路径裸 `find` +
  `const_accessor` 安全。

### 10.2 CoW posting list（P2-min）

`PostingList` 是不可变的（`shared_ptr<const PostingList>` 发布）：

- **写者**（reducer 线程）：`mutable_pl(acc->second)` 拿非 const 引用
  ——若 `use_count() == 1`（常态）原地改；若 `> 1`（有 phrase 读者持
  引用）就 `make_shared<PostingList>(*old) + push_back` 克隆替换
  （CoW）。详见 `src/bm25/inverted.cpp` 的 `mutable_pl`。
- **读者**（查询线程）：phrase / near 持 `shared_ptr<const PostingList>`
  零拷贝读；持引用期间写者走 CoW，互不阻塞。

`FlatPostings`（`inverted.hpp`）是查询期从 PostingList 拷贝出的扁平
快照（仅 `ords` + `tfs` + blocks，positions 不拷）——分配 N+1 次 → 2
次，省 ~40B/条（详见 `PostingList::snapshot_flat` 注释）。

### 10.3 排序词典 CoW

```cpp
mutable std::shared_mutex vocab_mtx_;
mutable std::shared_ptr<const std::vector<std::string>> vocab_;          // 主基线
mutable std::shared_ptr<const std::vector<std::string>> vocab_extra_;    // 增量层（S24-M9）
mutable std::vector<std::string> vocab_delta_;                          // raw delta
mutable std::atomic<bool> vocab_dirty_{true};
```

- 写者：add_doc 插入新 term 时 `unique_lock vlock(shard.vocab_mtx_)` 下
  push 到 `vocab_delta_`，然后 `release-store true` 到 `vocab_dirty_`。
- 读者：`ensure_vocab(shard_idx)` 入口 `acquire-load` `vocab_dirty_`：
  - **fast path**（false）：`shared_lock` 读 `vocab_/vocab_extra_` 快照
    返回（零重建开销）。
  - **dirty path**（true）：升级 `unique_lock` 重建（增量并入 extra；
    超阈值时归并成新 base）→ `release-store false`。下次进入 fast
    path。
- 发布协议：`shared_ptr<const vector<string>>` 换指针——读者持旧
  `shared_ptr` 零拷贝继续用，写者换上新 `shared_ptr` 不影响在途读者。

### 10.4 全局 atomic 统计

```cpp
std::atomic<uint64_t> live_doc_count_{0};
std::atomic<uint64_t> sum_doc_len_{0};
std::atomic<uint64_t> max_indexed_ord_{...};  // 崩溃恢复幂等保护
```

- S10.1 去锁（之前用 `stats_mutex_`，与查询裸读构成 UB race）；atomic
  既消 race 又免锁。
- `max_indexed_ord_` 是崩溃恢复幂等保护——replay 重放已在快照里的
  `(ord, term)` 时，`ord ≤ 水位` 整文档丢弃，保证 PostingList 严格升序
  无重复（intersect / find / 封块都依赖）。

### 10.5 compact（死点压实）

`InvertedIndex::compact`（`src/bm25/inverted.cpp`）逐 term 持
`PostingMap::accessor` 写锁、调用 `PostingList::compact_flags` 重建。
**非查询热路径**：compact 是 merge 收尾 / 后台 GC 才走的冷路径，与查询
路径互斥（持写锁期间 reader 走 CoW 的旧版本）。

---

## 11. IndexPool：异步索引 MapReduce 流水线

`include/bitcask/thread_pool.hpp` 的 `IndexPool` 是 **registry 级共享**
的异步索引线程管理器（不是每 Cask 一个）——所有同 registry 的 Cask 复
用同一对 map/reduce 线程。

### 11.1 N map worker + 1 reducer 模型

```
                   ┌─ map worker 1 (std::thread)
                   ├─ map worker 2 (std::thread)
                   ├─ ...
                   ├─ map worker N (std::thread)         真数据并行 G1
                   │
IndexTaskQueue ──► │                                    背压由 queue 容量 + reorder_cap 共同提供
(tbb::concurrent_  │
 bounded_queue,    │
 cap=10240)        │
                   │
                   ▼
                   reorder buffer (per-lane map<ord, ReorderEntry>)
                   │
                   ▼
                   reducer (std::thread)                  按 ord 严格升序 apply
                                                          库内单写者 I3
```

N = `std::thread::hardware_concurrency()`（至少 2），由
`KeyDirRegistry::index_pool` 在首次注册时懒创建（`src/keydir/keydir_registry.cpp`）。

### 11.2 线程数与库数解耦（G2）

每个 open 的 search 库注册一条 `IndexLane`（`shared_ptr<IndexLane>` 进
`lanes_` 表）：map/reduce 回调闭包 + reorder buffer + ord 水位都封在 lane
内。所有 lane 共用同一组 N+1 个线程。N 是 `std::thread::hardware_concurrency`
取定——N+1 与库数无关。

### 11.3 三把锁 + 单锁不变量（死锁防护）

```cpp
std::mutex start_mu_;          // 启动：started_ + 建线程
std::mutex reorder_mu_;        // lanes_ + 各 lane pending/next_apply_ord + reorder_inflight_
std::mutex flush_mu_;          // flush_cv_ 配套（仅 reduce 时持，瞬时）
```

**不变量**：任一线程任一时刻最多持其中一把。**无锁嵌套 ⇒ 无加锁顺序
⇒ 不可能死锁**。关键点（`thread_pool.hpp` 注）：

- reducer apply 前 `lk.unlock()` 释放 `reorder_mu_`，再 `dec_in_flight`
  （取 `flush_mu_`）。
- `register_lib` 先放 `reorder_mu_` 再 `ensure_started`（取 `start_mu_`）。
- cv 唤醒（`notify`）持各自锁，不跨锁。

### 11.4 背压（D4）

```cpp
inline constexpr std::size_t kDefaultIndexQueueCapacity = 10240;
inline constexpr std::size_t kDefaultReorderInflightCap = 16384;
```

- queue 满 → put 阻塞（queue 内部 backoff）。
- reorder 在途达上限 → map worker 停 pop（`map_cv_.wait`），queue 累
  积 → put 阻塞。

**H1 后**（cask.hpp 注）：写路径在 `write_mu_` 释放后才
`submit_index_task`——push 阻塞只挡本写者，不再全队冻结。

### 11.5 提交 → map → reduce → apply

`IndexPool::submit`（`thread_pool.hpp`）：

```cpp
void submit(IndexLane* lane, IndexTask task) {
    if (!lane || stopped_.load(acquire)) return;
    if (task.op != IndexOp::Sentinel) {
        auto prev = lane->submitted_ord_hwm.load(relaxed);
        while (task.ord > prev &&
               !lane->submitted_ord_hwm.compare_exchange_weak(
                   prev, task.ord, seq_cst, relaxed)) {}
        lane->in_flight.fetch_add(1, relaxed);
    }
    task.lane = lane;
    queue_.push(std::move(task));           // 满则阻塞（背压）
}
```

map worker 循环（`map_worker_loop`）：等背压名额 → `queue_.pop()` →
`process_task`（Add 调 `lane->map_fn`，其余直接构造 entry） →
`push_reorder`（在 `reorder_mu_` 下入 lane 的 pending map）。

reducer 循环（`reducer_loop`）：在 `reorder_mu_` 下等就绪 entry → 拷
lane 的 `shared_ptr`（防 UAF）→ `unlock` → `lane->reduce_fn(entry)` →
`lane->applied_ord.store(...)` → `++lane->next_apply_ord` → `dec_in_flight`。
**严格按 ord 升序**：单 lane 内 I2/I3 单写者保证。

### 11.6 RunFn 通道（merge 收尾 / HNSW 重建 / checkpoint）

`IndexOp::RunFn`（S13-F6）让 reducer 在静态点执行任意闭包——典型用途：

- `merge` 收尾的 HNSW 重建（VectorPlugin 的 `rebuild`）——必须 reducer
  线程，因为 `tbb::concurrent_hash_map` 遍历与插入并发不安全。
- `checkpoint` 序列化（`Cask::checkpoint`）——同样必须 reducer 静态点。
- `auto_checkpoint`（S14-1）——`roll_active` 封口触发，ord 增量达阈值
  异步提交，失败仅 `log_warn`（best-effort 加速，正确性恒由 data fold
  兜底）。

闭包经 reorder buffer 按 ord 序与 Add/Delete 串行——reducer 始终是单
写者上下文。

### 11.7 lane 生命周期

- `register_lib`：在 `reorder_mu_` 下建 `shared_ptr<IndexLane>` 入
  `lanes_`；`ensure_started` 在 `start_mu_` 下惰性建 N 个 map worker +
  1 个 reducer（保证 reducer 先在 `reorder_cv_` 上等，再让 worker 开始
  推 entry）。
- `unregister_lib`：先 `flush(lane)`（`flush_cv_.wait(in_flight==0
  && applied_ord≥submitted_ord_hwm)`），再在 `reorder_mu_` 下 `erase`
  ——保证 lanes_ 在 lane 还有在途任务时不被释放。
- `~IndexPool`：`stop()`（CAS stopped_ + 每 worker 一个 sentinel →
  `notify_all`）→ join 所有 worker → 置 `got_sentinel_` → join reducer
  → `~IndexLane` 自然清理。

### 11.8 Search 池（inter-query 并发，独立）

批量查询（`search_*_batch`）走 `search::parallel_for_queries`
（`src/search/search_arena.cpp`）：

```cpp
tbb::task_arena& search_arena() {
    static tbb::task_arena* arena = [] {
        unsigned hc = std::thread::hardware_concurrency();
        int slots = static_cast<int>(hc > 1 ? hc : 2);
        return new tbb::task_arena(slots);
    }();
    return *arena;
}
```

- 进程级共享 `tbb::task_arena`（故意泄漏，never-destroyed——规避
  静态析构与 `TbbLifetime::finalize` 的顺序坑；task_arena 不持有线程，
  成本可忽略）。
- `parallel_for_queries(n, body)`：n≤1 直跑，>1 进 arena 用
  `tbb::parallel_for` 把 [0, n) 分配给 N 个 slot 并发跑——`body(i)` 写
  各自结果槽（槽间不重叠 → 无需锁）。每条查询内部仍串行（BM25 WAND
  + HNSW 图遍历都是顺序依赖）。
- 单条查询不走池（`n=1` 早退）；批量查询共用一次 `prepare_search`
  flush（覆盖全批调用前的写）。

---

## 12. merge 与索引重建（与读写并发的正确性）

merge 在调用方线程上同步跑（典型做法是放进独立 dirty 调度器线程）。
merge 的内部排序契约（`src/cask/cask.cpp` 的 `Cask::merge` 注释）：

```
Phase 1 — Data compaction:
  1. run_merge()  重写活 record 到新文件, CAS 更新 KeyDir

Phase 2 — Index maintenance (search_ 存在时):
  2. write_keydir_snapshot()  捕获 ord 水位
  3. flush IndexPool          排干待处理索引任务
  4. compact()                阈值压实死 posting
  5. save bm25 snapshot + index sidecar
  6. rebuild_hnsw + flush     同步重建 HNSW 图（reducer 线程内 RunFn）
  7. save hnsw snapshot

Phase 3 — Cleanup:
  8. erase read_files_ cache + unlink old data/hint
  9. trim_fstats
 10. write_keydir_snapshot()  最终状态快照
```

关键约束：

- **Phase 2 的 flush 必须在 compact 之前**——保证 Index 覆盖全部已分
  配 ord，且 IndexPool worker 无在途任务（否则在途 task 持旧 ord 可能
  写错位置）。
- **bm25/sidecar/hnsw snap 落盘顺序必须与 close() 一致**——共用 `save_checkpoint_paired`。
- **Phase 3 的 unlink 必须在 Phase 2 之后**——否则 HNSW rebuild 读不到源
  数据。

Phase 2 的并发安全：

- `flush IndexPool`：`IndexPool::flush(lane)` 等 `in_flight == 0 &&
  applied_ord ≥ submitted_ord_hwm`，保证 reducer 已 apply 完所有已
  submit 的索引事件（执行至其 ord 时刻）。
- `compact()`：与查询路径互斥（持写锁期间 reader 走 CoW 旧版本）。
- `rebuild_hnsw`：reducer 线程内 `VectorPlugin::rebuild` 旁路建新图，
  最后 `hnsw_.store(new_ptr, release)`（atomic 换指针，旧图续命至
  in-flight reader 退出）。

**merge 与 backup**：见 `cask.hpp` 注释：`backup` 须保证与 merge 不并发
（merge 收尾会 unlink 输入文件；与同目录 merge 的单实例约束同级）。

---

## 13. 总结表

### 跨 OS 进程

| 角色   | 锁文件                | 数量上限 | 备注                                |
|---|---|---|---|
| writer | `bitcask.write.lock`  | 1 / 目录 | 跨进程 enforce（`O_CREAT\|O_EXCL`） |
| merger | `bitcask.merge.lock`  | 1 / 目录 | 跟 writer 并行（独立文件）          |
| reader | 无                    | ∞        | 各进程各快照，open 后不更新          |

### 同进程内（同 `KeyDirRegistry`）

| 角色   | 共享 KeyDir？ | 写锁并发              | 看到 live 数据？ |
|---|---|---|---|
| writer | ✅ shared_ptr | `write_mu_` 串行化（库内单写者）；内部 keydir 拿分片 unique | n/a |
| merger | ✅ shared_ptr | 不取 `write_mu_`；CAS via keydir + 文件 lifecycle | ✅ |
| reader | ✅ shared_ptr | 不取 `write_mu_`；keydir 分片 mutex 短临界区 | ✅ 立即可见 |

### 同 handle 多线程

| 操作 | 并发语义 | 关键同步原语 |
|---|---|---|
| `get` / `search_*`（并发多线程） | ✅ 真并发 | `read_cache_mu_` shared、keydir 分片 mutex、HNSW seqlock + atomic 发布、InvertedIndex tbb::concurrent_hash_map + shared_ptr CoW |
| `put` / `remove` / `put_doc`（并发多线程） | ✅ 多线程安全 | `write_mu_` 串行化整个写序列；锁内嵌套分片 unique + meta unique |
| 读写并发 | ✅ 安全 | 读不取 `write_mu_` |
| `merge` | ✅ 与读写并发 | 不取 `write_mu_`；CAS via keydir；HNSW `atomic<shared_ptr>` 整图换 |
| `parallel_scan` | ✅ 多线程并发 | 内部 spawn `std::thread` 分段并发 `get` |
| 同义词词典 | ✅ open-time 不可变 | `shared_ptr<const SynonymMap>`，运行期无 setter |
| `close()` | ⚠️ 生命周期 | `closed_` atomic 标志；H1 `writes_in_flight_.wait` 收敛在途写者 |
| `CaskIter` | ⚠️ 每线程一个 | 自身不持锁；同一对象不可并发使用；多 iterator 间并行安全 |

---

## 14. 部署模型推荐

**典型单机多线程部署**：一个 OS 节点跑一个 `bitcask` 实例服务多个调用
线程的并发读写。配置上：

- 一个 handle：`bitcask_open(Dir, &opts, &cask, &fault)`，`opts.read_write = 1`，
  多个调用线程**直接共享**它做并发 get/put/search（无需每线程一个
  handle）——`write_mu_` 串行化、读路径无锁。
- 应用层用独立线程或 cron 触发周期 `bitcask_merge(cask, &fault)`。

运行期间：

- 全部读看到最新写入（KeyDir 共享 + 读路径无锁；搜索 near-real-time）。
- **多线程并发写同一 handle 安全**（S11-W1：内部 `write_mu_` 串行化）。
  写在文件层本就串行 → 锁不损吞吐；**更高写并发 → 按目录分片多个
  Cask 实例**（单 append WAL 的横向扩展手段）。
- merge 跟读写并行不阻塞（独立 `bitcask.merge.lock` + keydir 分片锁
  协调）。
- 进程重启 → 第一个 `bitcask_open` 付扫盘代价 → 之后全部 refcount。

**不推荐的反模式**：

- 多个 OS 进程同时打开同一个 dir 都想做实时读——reader 看不到对方
  writer 的更新；如果非要这么做请用 RPC 或者把 bitcask 包成单点服务
  做单点写入路由。
- 在 NFS / 网络盘上跑——`O_EXCL` 在 NFS 上有历史 bug。
- 频繁 open + close 同一个 dir——每次 open 可能触发扫盘（如果 refcount
  归零过），大目录代价高。
- 多线程并发调用 `close()` 与 `get`/`put`/`search_*`——契约是 close
  时刻无在途操作；H1 把 UB 收敛为阻塞等待，但不消除业务上的并发约束。

---

## 附录 A. 锁全序速查

### KeyDir 内部

```
barrier_mu_ (mutex) → gate_mu_ (mutex) + gate_cv_ → meta_mu_ (shared_mutex) →
单个 shard (mutex, 任意时刻 ≤1 把) → fstats_grow_mu_ (mutex)
```

两处反向嵌套（带无环论证）：

- **热路径**：shard → meta（与全序一致），`KeyDir::get` / `put_probe`
  / `remove` 均有此嵌套。
- **屏障内**：meta_shared → shard（**反向**），仅存在于
  `apply_pending_to_entries_barrier`——彼时写者已被闸门出清，读者
  对 meta 只拿 shared，本方向也只拿 meta shared，shared-shared 相容，
  无环。

### Cask 内部

| 锁 / 原子                | 类型            | 保护对象                                                                  |
|---|---|---|
| `write_mu_`              | `std::mutex`    | put/remove/put_doc/sync/close_write_file/backup 的整个写序列（`cask.hpp` 中 `Cask::write_mu_` 成员） |
| `read_cache_mu_`         | `std::shared_mutex` | `read_files_` map 结构 + `active_data_` 替换（`cask.hpp` 中 `Cask::read_cache_mu_` 成员）             |
| `ckpt_mu_`               | `std::mutex`    | `checkpoint()` 调用间互斥（`cask.hpp` 中 `Cask::ckpt_mu_` 成员；异步 RunFn 路径下持锁跨越有界 cv 等待，见下方 P5-DL-1 说明） |
| `closed_`                | `atomic<bool>`  | close 后 fail-fast 标志（`cask.hpp` 中 `Cask::closed_` 成员）                                    |
| `active_file_id_`        | `atomic<uint32_t>` | active writer 当前 file id；写者持 write_mu_，读者 relaxed 读            |
| `writes_in_flight_`      | `atomic<uint32_t>` | WriteOpGate 写操作计数，close 用 seq_cst wait 排空                        |
| `read_clock_`            | `atomic<uint64_t>` | read LRU 单调访问计数                                                    |
| `index_errors_`          | `atomic<uint64_t>` | worker 异常计数                                                          |
| `auto_ckpt_pending_/inflight_/last_ckpt_ord_` | `atomic<...>` | 自动 ckpt 状态                                                            |
| `ckpt_rebase_needed_`    | `atomic<bool>`  | ckpt rebase 标志                                                          |
| `write_lock_`            | `optional<FileLock>` | 跨进程文件锁（write.lock 或 merge.lock）                                |

### KeyDirRegistry

| 锁       | 类型         | 保护对象                                |
|---|---|---|
| `mutex_` | `std::mutex` | `entries_` / `saved_biggest_file_id_` / `index_pool_` |

### IndexPool

| 锁 / 原子                                | 类型               | 保护对象 |
|---|---|---|
| `start_mu_`                              | `std::mutex`       | `started_` + 建线程 |
| `reorder_mu_`                            | `std::mutex`       | `lanes_` / 各 lane pending / `reorder_inflight_` |
| `flush_mu_`                              | `std::mutex`       | `flush_cv_` 配套（仅 reduce 瞬时持） |
| `stopped_`                               | `atomic<bool>`     | 全池停止 |
| `IndexLane::submitted_ord_hwm/applied_ord/in_flight` | `atomic<...>` | 单 lane 水位 |
| `IndexTaskQueue::queue_`                 | `tbb::concurrent_bounded_queue<IndexTask>` | 多生产者-多消费者 + 容量背压 |

**单锁不变量**：任一线程任一时刻最多持 `start_mu_` / `reorder_mu_` /
`flush_mu_` 之一——无锁嵌套 ⇒ 无加锁顺序 ⇒ 不可能死锁。

> **P5-DL-2（已消解，2026-07-14）**：reducer_loop 曾在每 apply 一个索引事件后
> 立即取一次 `flush_mu_` + `notify_all`（服务已删除的 `flush_upto`），再紧接
> `dec_in_flight` 又取一次——同一线程连取同锁两次，形式上偏离本不变量的
> 「释放 → 工作 → 取下一把」表述。T16（P5-DL-3）删除该 per-apply 通知块后，
> reducer 对 `flush_mu_` 的唯一触点回到 `dec_in_flight` 归零时的单次 notify。
> 之所以安全：`flush()`/`unregister_lib` 只等最终态（`in_flight==0 &&
> applied_ord>=hwm`），而 `applied_ord` 在 `dec_in_flight` 之前 store，故归零
> notify 时两谓词同时成立，无丢失唤醒。

> **P5-DL-1（有意设计，文档固化）**：`Cask::checkpoint()` 的异步 RunFn 路径
> 在**持 `ckpt_mu_`（函数级 `lock_guard`）且 WriteOpGate 在持**的临界区内，
> 对一个 per-call 局部 `done_mu`/`done_cv` 做**最长 30s 的有界 cv 等待**
> （`cask.cpp` DL-MED-2）。这偏离「`ckpt_mu_` 仅用于串行化 checkpoint 调用、
> 瞬时持有」的朴素语义——持锁跨越了一次跨线程等待。
>
> **为何不是死锁**：① `done_mu` 是每次调用新建的局部 `shared_ptr`，除本调用
> 与其提交的 RunFn 外无任何代码路径获取它，不参与任何锁序；② 等待有 30s
> 硬上界（超时即返回 kIo 错误，不永久挂死）。**后果**：reducer 线程若卡死，
> 后续 checkpoint 调用者（排队于 `ckpt_mu_`）与 `close()`（经 WriteOpGate 等
> 本调用返回）最坏被拖满 30s。**契约**：不得从 reducer 上下文调用
> `checkpoint()`——否则 RunFn 永不被执行，必然走满 30s 超时（与
> `plugin_api.hpp` 的 `run_serialized` reducer 禁令同源，见 T13）。

### HNSW

| 原语                                              | 类型                         | 作用 |
|---|---|---|
| `chunks_`                                         | `atomic<NodeChunk*>[kMaxChunks]` | 节点块无锁发布（裸指针 + release/acquire） |
| `count_`                                          | `atomic<uint32_t>`            | 节点数发布水位 |
| `entry_meta_`                                     | `atomic<uint64_t>`            | entry id + max_level 合并 |
| `max_inserted_ord_`                               | `atomic<uint64_t>`            | 写入幂等保护 |
| `NodeChunk::locks[slot]`                          | `atomic<uint32_t>[per-node]`  | per-node seqlock 序号 |
| `NodeChunk::adj[slot][i]` 的读写                  | `atomic_ref<uint32_t>`        | relaxed（TSan 干净） |
| `writer_active_`                                  | `atomic<bool>`                | 单写者声明 |

### InvertedIndex

| 原语                                  | 类型                 | 作用 |
|---|---|---|
| `shards_[i].inverted`                 | `tbb::concurrent_hash_map` | 桶级锁 + PostingList 引用 |
| `shards_[i].vocab_mtx_`               | `std::shared_mutex` | 排序词典重建 |
| `shards_[i].vocab_/vocab_extra_`      | `shared_ptr<const vector<string>>` | 词典快照（CoW 发布） |
| `shards_[i].vocab_dirty_`             | `atomic<bool>`      | 脏标志（release/acquire 配对） |
| `live_doc_count_/sum_doc_len_/max_indexed_ord_` | `atomic<uint64_t>` | 全局统计 + 幂等保护 |

### DocMap / Text / Vector / SearchCache（独立锁）

| 模块            | 锁 / 原子                                  | 类型                       |
|---|---|---|
| `index::Index`  | `mutex_`                                   | `std::shared_mutex`        |
|                  | `dirty_`                                   | `atomic<bool>`             |
| `text::TextPlugin` | `fields_mu_` / `field_names_intern_mu_` | `std::shared_mutex`        |
|                  | `DocTextLru::mu_`                          | `std::mutex`               |
|                  | `dirty_default_/dirty_fields_/rebase_needed_` | `atomic<bool>`         |
| `vec::VectorPlugin` | `hnsw_`                                | `atomic<shared_ptr<HnswIndex>>` |
|                  | `dirty_/rebase_needed_`                    | `atomic<bool>`             |
| `search::SearchCache` | `mutex_`                              | `std::shared_mutex`        |
|                  | `use_clock_`                               | `atomic<uint64_t>`         |

### FileLock（跨进程）

| 调用 | flags | 用途 |
|---|---|---|
| `FileLock::acquire(path, true)`  | `O_CREAT \| O_EXCL \| O_RDWR \| O_SYNC` | write 锁：原子互斥跨进程 |
| `FileLock::acquire(path, false)` | `O_RDONLY`                             | read 锁：读锁文件里的元数据 |

`O_SYNC`（write 锁）保证 `write_data` 立刻被其它进程的读锁看到——
bitcask 用这个机制做 stale-lock 检查（详见 `src/lock/file_lock.cpp`）。

### Search 池（inter-query）

| 资源                                | 类型                  | 作用 |
|---|---|---|
| `search_arena()`                    | `tbb::task_arena`     | 进程级共享有界 Search 池（inter-query 并发） |
| `parallel_for_queries(n, body)`     | `tbb::parallel_for`   | 在 arena 内并发跑 n 条独立查询，grainsize=1 |

---

## 附录 B. 锁交互 ASCII 时序

### open → 多线程 put/get → merge → close

```
T0: bitcask_open(dir, opts={read_write=1}, ...)
  ├── acquire_open_locks():
  │     ├── FileLock::acquire("bitcask.write.lock", true)
  │     │     └── O_CREAT|O_EXCL|RDWR|SYNC —— EEXIST → try_remove_stale_lock
  │     └── write_data("<pid>\n")
  ├── check_or_create_meta()                  # bitcask.meta v3
  ├── create_search_infra()                   # Text/Vector + IndexPool lane
  ├── registry->acquire(dirname)
  │     └── kCreated → load_keydir_from_disk()
  │     └── mark_ready()
  └── index_pool->register_lib(map, reduce, error, init_ord)
        └── ensure_started()                  # 建 N+1 个 std::thread

T1: T_writer_1 ──► Cask::put(k1, v1)
  ├── WriteOpGate gate(this)                  # writes_in_flight_++
  ├── unique_lock<std::mutex>(write_mu_)      # 写路径串行化
  ├── roll_active_if_needed()                 # read_cache_mu_ 独占
  ├── write_and_keydir(...)                   # active_data_->write + keydir_->put
  │     └── keydir_->put → shard.mu unique + meta_mu_ unique（fold 态）
  ├── wlk.unlock()                            # write_mu_ 释放
  └── submit_index_task(Add{ord})             # IndexPool（锁外提交，背压点）

T2: T_writer_2 ──► Cask::put(k2, v2)
  ├── WriteOpGate gate(this)
  ├── unique_lock<std::mutex>(write_mu_)      # 与 T1 互斥
  ├── ...（同 T1 流程，但 active_file_id_ 可能被并发 merger 推进 → roll_active）
  └── ...

T3: T_reader_1 ──► Cask::get(k1)
  ├── keydir_->get(k1)                        # shard.mu unique（mutex，非 shared_mutex）
  │     └── miss + fold 态 → 嵌套 meta_mu_ shared 查 pending_
  ├── read_file(file_id)
  │     ├── shared_lock(read_cache_mu_)       # 命中
  │     │     └── atime.store(clock, relaxed)
  │     └── miss → unique_lock(read_cache_mu_)
  │           └── DataFile::open + evict_read_handles_locked()
  └── df->read(offset, sz)                    # pread (thread-safe)

T4: T_merger ──► Cask::merge()
  ├── needs_merge()                           # 排除自己 active_id 或 merger_writer_active_id_
  ├── run_merge(input_files, ...)             # 不取内部锁
  │     └── keydir_->conditional_remove(..., old_file_id, old_offset)
  │           └── shard.mu shared（探测）+ shard.mu unique（删除）
  ├── IndexPool::flush(lane)                  # 排空在途索引任务
  ├── InvertedIndex::compact(live_checker)    # 死点压实（持分片写锁）
  ├── RunFn{RebuildHnsw} → reducer            # reducer 线程旁路建新图 + atomic 换指针
  └── erase read_files_ + unlink old data/hint + trim_fstats

T5: T_user ──► Cask::close()
  ├── closed_.exchange(true)                  # 幂等门
  ├── writes_in_flight_.wait(0, seq_cst)      # H1：等所有写者退出
  ├── try/catch:
  │     ├── maybe_group_commit(force=true)
  │     ├── active_hint_->finalize()
  │     ├── scoped_lock(read_cache_mu_)        # active_data_.reset + read_files_.clear()
  │     ├── index_pool_->unregister_lib(lane)   # flush(lane) + lanes_.erase
  │     ├── save_search_ckpt_paired            # RunFn → reducer → save
  │     └── write_keydir_snapshot
  ├── registry_->release(name)                # refcount -1
  └── write_lock_->release_quiet()             # unlink + close（先 unlink 后 close）
```

### KeyDir 屏障 v2 时序（snapshot save / fold release）

```
BarrierGuard bg(kd)
  ├── lock(barrier_mu_)                       # 屏障间互斥
  ├── lock(gate_mu_)
  │     └── barrier_active_.store(true, release)
  └── for sh in shards_: sh.mu.lock(); sh.mu.unlock();   # 排干：逐分片加锁-放锁
                                                # 写者拿到分片锁后看到 active → 退避到 gate_cv_
~BarrierGuard bg
  ├── lock(gate_mu_)
  │     └── barrier_active_.store(false, release)
  ├── gate_cv_.notify_all()                   # 唤醒退避写者
  └── unlock(barrier_mu_)
```

读者（get / next / info / save_snapshot 的无锁遍历）不受影响——屏障
期间照常并发，与 barrier 内的无锁遍历是读-读并发安全。

---

## 附录 C. 与 thread-safety.md 的分工

本文档与 [`docs/design/thread-safety.md`](../docs/design/thread-safety.md) 都
覆盖 `Cask` 的并发语义，**受众不同**：

| 维度       | `concurrency-zh.md`（本文档）                       | `thread-safety.md`              |
|---|---|---|
| 受众       | 用户 / 集成方 / README 引用的契约来源                | 内部设计审计 / SQA             |
| 焦点       | 单 handle 多线程 + 跨进程共享 KeyDir + 文件锁仲裁   | 单 handle 各接口的锁边界与边界条件 |
| 细节       | 锁 X 保护 Y 的代码定位（符号级）                    | 每个方法的并发合同 / 死锁论证 / 已知边界 case |
| 历史与原因 | 较少（聚焦现状）                                    | 多（含 S11 / S13 / S14 等演进与设计权衡） |
| 索引       | 三种 open 模式 / KeyDirRegistry / 文件锁 / 各模块   | handle 各 API + merge / backup / checkpoint |

两份文档可能交叉——保持术语一致（`write_mu_` / `read_cache_mu_` / 256 shards
/ `atomic<NodeChunk*>` / `IndexPool` N+1 / `merge_only` 等术语代码标识符
**完全对齐**）。

---

**文档版本**：2026 Phase 1 并发契约重写版
**最后更新**：2026-07-06
**维护者**：libbitcask C++ 团队