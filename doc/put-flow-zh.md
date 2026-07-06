# `put(K, V)` 完整调用链

从 C API 入口一路到磁盘字节落定，再异步灌进索引流水线。每一步都标注源
符号，方便对照阅读。

本文描述的是 `bitcask_put()` 的成功路径与关键 race 处理；磁盘字节细节
见 [`format-zh.md`](format-zh.md)；线程模型 / 锁序见
[`concurrency-zh.md`](concurrency-zh.md)；异步索引 MapReduce 设计见
[`async-index-pipeline.md`](../docs/design/async-index-pipeline.md)。

---

## 1. C API 入口

符号：`c_api/bitcask_c.cpp::bitcask_put`

1. 校验参数（`cask` / `key.data` / `value.data` 非空）。
2. 把 `bitcask_slice_t` 包成 `std::span<const std::byte>`（零拷贝 view，
   不复制 `key` / `value` 字节）。
3. 调 `h->cask->put(key_span, val_span, tstamp, expiry_at)`，`h` 是持有
   `bitcask::Cask*` 的不透明句柄；`expiry_at` 由 `tstamp` 后的一个
   `uint32_t` 字段承载（C API 层把 per-key 过期时刻一并翻译进 4 参数）。
4. 把 `std::expected<void, CaskFault>` 翻成 `bitcask_error_t`，并把
   详情写入 out-param `bitcask_fault_t`（`fault == NULL` 时静默丢弃）。

`bitcask_put` 在**调用方线程上同步执行**——单次 put 是确定的小工作量，
不需要把控制权让出去；长耗时的 `merge` 由调用方自己调度。同一 handle
多线程并发写由库内 `Cask::write_mu_` 串行化；写本就单 append WAL，
更高写并发请按目录分片多实例。

---

## 2. `Cask::put` —— 写入编排

符号：`src/cask/cask.cpp::Cask::put`

签名：

```cpp
std::expected<void, CaskFault>
Cask::put(std::span<const std::byte> key,
          std::span<const std::byte> value,
          std::uint32_t tstamp = 0,
          std::uint32_t expiry_at = 0);
```

执行序列：

| # | 动作 | 失败映射 | 备注 |
|---|------|----------|------|
| 1 | 构造 `WriteOpGate`（`Cask::write_mu_` 外守卫，确保 close 与在途写不竞态） | — | H1 收敛：与 `close()` 的互斥等待 |
| 2 | `std::unique_lock<std::mutex>(write_mu_)` | — | S11-W1：写路径互斥 |
| 3 | `is_closed()` 检查 | `kClosed` | S11-W3 fail-fast |
| 4 | `!opts_.read_write \|\| opts_.merge_only` | `kReadOnly` | |
| 5 | `key.size() > format::kMaxKeySize`（`format::kMaxKeySize = 0xFFFF`，见 `format.hpp`） | `kKeyTooLarge` | |
| 6 | `value.size() > format::kMaxValueSize`（`format::kMaxValueSize = 0xFFFFFFFFu`，见 `format.hpp`） | `kValueTooLarge` | |
| 7 | `tstamp == 0` → 取 `now_sec_default()`（当前 Unix 秒） | — | |
| 8 | `about = format::kHeaderSize + key.size() + value.size()`（`format::kHeaderSize = 23`，4B CRC + 1B type + 4B tstamp + 8B ord + 2B key_sz + 4B val_sz） | — | 用原始 `value.size()`，DocValue 多出的几字节忽略 |
| 9 | `roll_active_if_needed(about)` | `kIo` / `kWriteLocked` | 见 §4 |
| 10 | race 检查：`active_data_ && active_file_id_ < keydir_->biggest_file_id()` → `roll_active()` | `kIo` / `kWriteLocked` | 并发 merger 把 biggest 推过去时主动切文件，否则下一步 keydir 拒收 |
| 11 | `ord = keydir_->alloc_ord()`（全局单调递增 `uint64_t`） | — | |
| 12 | 构造 `OrdSkipGuard`（错误路径补 `Skip` 防 reorder buffer stall，S13-F2） | — | 失败自动析构补洞 |
| 13 | `thread_local std::vector<std::byte> encoded; encoded.clear();` + `codec::encode_doc_value(encoded, {.text=value, .expiry_at=expiry_at})` | — | thread_local 复用，消每次 put 的 encoded 堆分配 |
| 14 | `persisted = write_and_keydir(key, encoded, tstamp, ord)`（见 §3） | 见 §3 | |
| 15 | `og.disarm()`（成功 ⇒ ord 已被 Add 任务覆盖，无须补 Skip） | — | |
| 16 | `gc = maybe_group_commit()` | `kIo`（fsync 失败） | P4 组提交（见 §5） |
| 17 | `wlk.unlock()`（**索引提交移到锁外**，H1） | — | 见 §6 |
| 18 | `submit_index_task(IndexTask::make(IndexOp::Add, ...))` | — | 锁外异步入队 |
| 19 | `maybe_submit_auto_checkpoint()` | best-effort | S14-1：roll 封口的异步 ckpt |
| 20 | 返回 `gc` 错误或 `{}` | — | |

---

## 3. `write_and_keydir` —— data → hint → keydir 三步落定

符号：`src/cask/cask.cpp::Cask::write_and_keydir`

返回：`std::expected<Cask::PersistedRecord, CaskFault>`
（`PersistedRecord = {ord, offset, total_size, file_id}`）。

成功路径：

| 子步 | 调用 | 失败映射 |
|------|------|----------|
| 3.1 | `active_data_->write(RecordType::kDoc, tstamp, ord, key, encoded)` —— `pwrite(current_offset_, buf)` 后推进 | `kIo`（`errnum` 取 errno） |
| 3.2 | `active_hint_->write(tstamp, total_size, offset, /*tomb=*/false, key)` —— 顺序 `write(buf)` + 更新 `running_crc_` | `kIo` |
| 3.3 | `keydir_->put(key, active_file_id_, total_size, offset, tstamp, /*now=*/0, /*newest=*/true, /*expired=*/0, /*v=*/0, ord)` | — |
| 3.4 | 返回 `PersistedRecord{ord, offset, total_size, active_file_id_}` | — |

Merge race 处理（3.3 返回 `keydir::PutResult::kAlreadyExists`）：

| 子步 | 动作 | 失败映射 |
|------|------|----------|
| 3.5 | `roll_active()` 切新 file（hint finalize + `active_data_.reset()` + `active_hint_.reset()` + 自动建新文件） | `kIo` / `kWriteLocked` |
| 3.6 | `ord2 = keydir_->alloc_ord()` | — |
| 3.7 | 构造 `OrdSkipGuard g2(this, ord2)`（S13-F2 守卫 ord2） | — |
| 3.8 | 第二次 `active_data_->write(kDoc, ...)` | `kIo` |
| 3.9 | 第二次 `active_hint_->write(...)` | `kIo` |
| 3.10 | 第二次 `keydir_->put(... ord2)` | — |
| 3.11 | 二次仍 `kAlreadyExists` → 上报 `kAlreadyExists`（`ord` 由 caller 守卫补 Skip，`ord2` 由 `g2` 析构补 Skip） | `kAlreadyExists` |
| 3.12 | 否则：`submit_index_task(IndexTask::make(IndexOp::Skip, {}, ord, ...))` 填原始 ord 的洞（必须在 caller 提交 ord2 的真任务前入队，队列 FIFO 保序） | — |
| 3.13 | `g2.disarm()`（`ord2` 由 caller 的真任务 Add 覆盖） | — |
| 3.14 | 返回 `PersistedRecord{ord2, offset2, total_size2, active_file_id_}` | — |

`ord` 在 keydir 竞争中落败、数据已写但 keydir 未收录：原始 ord 的 posting
不会出现在倒排里（这是正确语义），同时 `Skip` 任务保证 reorder buffer
不会因 ord 空洞永久 stall。

---

## 4. Active writer 管理

### 4.1 `roll_active_if_needed` / `roll_active`

符号：`src/cask/cask.cpp::Cask::roll_active_if_needed` / `Cask::roll_active`

```
Cask::roll_active_if_needed(about):
  if !active_data_              → ensure_active_writer()
  if size + about ≤ max_file_size → no-op
  else                          → roll_active()

Cask::roll_active():
  1. maybe_group_commit(force=true)         // P4：落旧文件尾批
  2. active_hint_->finalize()                // 写 hint trailer（CRC32）
  3. active_data_.reset()  // 持 read_cache_mu_ 排他
     active_hint_.reset()
  4. auto_ckpt_pending_ = true              // S14-1：封口 = ckpt 锚点
  5. ensure_active_writer()                 // 拿新 file_id + 建新文件
```

`CaskOptions::max_file_size = 2 GiB`（`cask.hpp`）为软上限；`put_batch`
允许巨批突破（见 §8）。

### 4.2 `ensure_active_writer`

符号：`src/cask/cask.cpp::Cask::ensure_active_writer`

```
1. if active_data_ → return
2. if !opts_.read_write              → kReadOnly
3. if opts_.merge_only               → kReadOnly（merger 自己用 keydir->increment_file_id 分配输出文件）
4. if !write_lock_:
     fl = acquire_writer_lock(dirname_)
        // O_CREAT|O_EXCL 创建 bitcask.write.lock
        // EEXIST → try_remove_stale_lock：
        //   读锁 → 解析 pid → kill(pid, 0) 探活
        //   死了 → unlink + 重试
        //   活着 → kWriteLocked
     write_lock_ = std::move(*fl)
5. active_file_id_ = keydir_->increment_file_id()   // 单调递增分配
6. data_path = mk_data_filename(dirname_, active_file_id_)
   hint_path = mk_hint_filename(data_path)
7. df = DataFile::open(data_path, Mode::kCreate, opts_.o_sync)
   hf = HintFile::open(hint_path, Mode::kCreate, opts_.o_sync)
8. { std::unique_lock lk(read_cache_mu_); active_data_ = make_shared<DataFile>(...); }
   active_hint_ = make_unique<HintFile>(...)
9. write_lock_->write_data("<pid> <active_data_path>\n")
   // best-effort：失败仅 log_warn，merge_only 句柄可能误把 active 纳入候选
```

---

## 5. `maybe_group_commit` —— P4 单写者组提交

符号：`src/cask/cask.cpp::Cask::maybe_group_commit`

```
if opts_.o_sync || opts_.sync_every_n == 0 || !active_data_ → no-op
++writes_since_sync_              // 非 force 路径
if force ? (writes_since_sync_ > 0) : (writes_since_sync_ ≥ opts_.sync_every_n)
  active_data_->sync()            // fsync(2)
  writes_since_sync_ = 0
```

`CaskOptions::sync_every_n` 默认 0（关闭），`o_sync` 默认 false。
组提交只对 **active data file** 调 fsync；hint 不在此 fsync（可由
`fold(data)` 重建，崩溃回退）。

---

## 6. 索引提交 —— `submit_index_task`

符号：`src/cask/cask.cpp::Cask::submit_index_task`

```
if !index_pool_ || !index_lane_ → return     // KV 模式 / 未启用搜索
index_pool_->submit(index_lane_, std::move(task))
```

`IndexTask::make(IndexOp::Add, key_view, ord, text_view, file_id, offset,
total_size, tstamp, doc_len)` 构造 payload（见
[`thread_pool.hpp`](../../include/bitcask/thread_pool.hpp)），由
`IndexPool` 内部：

1. 入 `tbb::concurrent_bounded_queue<IndexTask>`（容量 10240，见
   `IndexTaskQueue`），背压由 bounded 实现。
2. N 个 map worker 并行调 `prepare_index_task`（注册序闭包），对每条
   IndexTask 产出 `plugin::PreparedPtr`（Text 切词 + BM25 posting +
   Vector 量化）。
3. per-lane reorder buffer 按 `ord` 排序，`reducer` 单线程串行 apply：
   - 调 `TextPlugin::on_put`（`InvertedIndex::add_doc`）——按 term hash
     分片锁插入 posting。
   - 调 `VectorPlugin::on_put`（`HnswIndex::insert`）——单写者
     (`atomic<NodeChunk*>` 发布 + per-node 自旋锁)。

整条流水线在 worker 线程上跑，调用方线程 `put` 立即返回；可见性遵循
near-real-time 契约，查询前 `Cask::flush_index()` / `drain_plugins()` 排
空在途任务。失败语义详见 [`async-index-pipeline.md`](../docs/design/async-index-pipeline.md)。

---

## 7. 完整时序示意

```
调用方线程                       C API / C++                                  磁盘
─────────────────────────────────────────────────────────────────────────────────────
bitcask_put(cask, K, V, 0, 0, &fault)
  └─→ bitcask_put  (C API)
        ├─ WriteOpGate gate(this)             ← H1：close 互斥
        ├─ unique_lock(write_mu_)             ← S11-W1
        ├─ is_closed()? kClosed
        ├─ size 校验     → kKeyTooLarge / kValueTooLarge
        ├─ tstamp = now_sec_default()         ← 若 0
        ├─ roll_active_if_needed(about)
        │     └─ ensure_active_writer()
        │           ├─ acquire_writer_lock()  → kWriteLocked
        │           ├─ DataFile::open(data_path, kCreate, o_sync)
        │           └─ HintFile::open(hint_path, kCreate, o_sync)
        ├─ race 检查 (file_id < biggest)       → roll_active() 切文件
        ├─ ord = keydir_->alloc_ord()         ← 单调递增 u64
        ├─ encode_doc_value(encoded, {.text=value, .expiry_at})
        ├─ write_and_keydir(key, encoded, tstamp, ord)
        │     ├─→ DataFile::write(kDoc, ...)
        │     │     └─ pwrite(off, [CRC|Type|Tstamp|Ord|KS|VS|K|DocValue])
        │     │                                       ━━━━ <id>.bitcask.data
        │     ├─→ HintFile::write(tstamp, sz, off, tomb=false, key)
        │     │     └─ write([Tstamp|KS|TSz|Off|K])     ━━━━ <id>.bitcask.hint
        │     └─→ KeyDir::put(...)              ← 持 K 所属分片锁
        │           └─ entries_[K] = {fid,off,tsz,ep,ts,ord}
        ├─ maybe_group_commit()                ← sync_every_n > 0 → fsync
        ├─ wlk.unlock()                        ← H1：索引提交移到锁外
        ├─ submit_index_task(IndexOp::Add)     ← 入队，不阻塞
        │     └┄(异步) IndexPool worker (reducer 串行 apply)
        │           ├─ TextPlugin::on_put → InvertedIndex::add_doc
        │           └─ VectorPlugin::on_put → HnswIndex::insert
        ├─ maybe_submit_auto_checkpoint()      ← S14-1 异步 ckpt
        └─ return gc 错误或 {}
        return BITCASK_OK
（索引在 worker 线程异步建立；put 返回不代表索引已就绪，
  查询前 flush_index / drain_plugins 排空在途任务）
```

### 字节布局

data record（`format::kHeaderSize = 23`）：

| 偏移 | 字段 | 值 |
|------|------|----|
| 0 | CRC32 | 算 `Type..Value` 的 zlib CRC，写到这里（4 B） |
| 4 | Type | `u8`：`0 = kDoc`，`1 = kTombstone` |
| 5 | Tstamp | 小端 `u32`（4 B） |
| 9 | Ord | 小端 `u64`（8 B）—— 单调递增的写入序号 |
| 17 | KeySz | 小端 `u16`（2 B） |
| 19 | ValueSz | 小端 `u32`（4 B） |
| 23 | Key | 原样字节 |
| 23 + KS | Value | DocValue 打包字节（kDoc 时） |

CRC 覆盖 `Type..Value`（即偏移 4 起的所有内容），不包含 CRC 字段本身；
这与 legacy 从 `Tstamp` 开始计算不同。

DocValue 编码（kDoc value）—— 普通 `put(K, V)` 写入时只含 text 段：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | Ver | 当前 = 3 |
| 1 | Flags | `bit0 = has_vector` `bit1 = has_text` `bit2 = has_meta` `bit4 = has_fields` |
| ? | 段 | 按 vector → text → meta → fields 顺序由 Flags 决定存在；长度 / 计数全 varint |

hint record（18 B 固定 header）：

| 偏移 | 字段 | 值 |
|------|------|----|
| 0 | Tstamp | 小端 `u32` |
| 4 | KeySz | 小端 `u16` |
| 6 | TotalSz | data record 整条字节数 |
| 10 | Tomb\|Offset | 小端 `u64`：`(tomb << 63) \| offset` |
| 18 | Key | 原样字节 |

### 落盘顺序

固定 **data → hint → keydir**。有意为之：

- data 写到一半 crash → 文件尾是损坏字节，下次 open 的 `fold(data)` 会
  跳过、`out_last_valid_end` 报告位置、cask 自动 truncate 修复。
- data 写完但 hint 没写完 → hint trailer CRC 校验失败，cask 忽略 hint
  退回 `fold(data)` 重建，data 是权威。
- data + hint 都写完但 keydir 没更新（比如 cask 进程崩溃）→ 下次 open
  重建 keydir 时会扫到这条 record，自然恢复。

---

## 8. `Cask::put_batch` —— 1 MiB 块批写

符号：`src/cask/cask.cpp::Cask::put_batch`

签名：

```cpp
struct BatchItem {
    std::span<const std::byte> key;
    std::span<const std::byte> value;
};
std::expected<void, CaskFault>
Cask::put_batch(std::span<const BatchItem> items, std::uint32_t tstamp = 0);
```

执行序列：

| # | 动作 | 备注 |
|---|------|------|
| 1 | `WriteOpGate` + `unique_lock(write_mu_)` | 与 `put` 同 |
| 2 | `is_closed()` / `!read_write` / `merge_only` 校验 | |
| 3 | `items.empty()` → 直接返回 `{}` | 零长度批视为成功 |
| 4 | 全批前置校验（key / value 大小，累加 `about`） | 校验失败零副作用 |
| 5 | `tstamp == 0` → `now_sec_default()` | |
| 6 | `roll_active_if_needed(about)` + race 检查（与单条 put 同） | 整批进同一 active 文件，巨批允许超 `max_file_size` |
| 7 | 逐条 `keydir_->alloc_ord()` + `encode_doc_value(encoded, {.text=value})` + `active_data_->write_buffered(kDoc, tstamp, ord, key, encoded)` | 写入 `batch_buf_`，**不立即 pwrite**；offset 是逻辑偏移（含未落盘缓冲），确定性不依赖落盘时机 |
| 8 | `active_data_->flush_batch()` —— 把 `batch_buf_` 一次 pwrite 出去 | 批的提交点 |
| 9 | `!opts_.o_sync && opts_.sync_every_n > 0` → `active_data_->sync()`（fdatasync 语义） + `writes_since_sync_ = 0` | 整批视作一次组提交 |
| 10 | 逐条 `active_hint_->write(tstamp, total_size, offset, tomb=false, key)` | hint 不在 §9 的 fsync 范围 |
| 11 | 逐条 `keydir_->put(...)`；返回 `kAlreadyExists` 时走单条 `write_and_keydir`（内部 roll + 重试），原始 ord 由 `BatchOrdGuard` 析构补 `Skip` | merge race 处理 |
| 12 | 重写路径走 `maybe_group_commit(force=true)` 后 `wlk.unlock()` | 锁内只补 Add 提交（队列背压兜底） |
| 13 | 锁外 `flush_adds()` 逐条 `submit_index_task(IndexOp::Add)` + `maybe_submit_auto_checkpoint()` | H1 同单条 put |

### `write_buffered` / `flush_batch`

符号：`include/bitcask/data_file.hpp::DataFile::write_buffered` / `DataFile::flush_batch`

`DataFile::kBatchFlushBytes = 1u << 20`（**1 MiB**）—— `batch_buf_` 累
积 ≥ 1 MiB 触发一次 pwrite；`flush_batch()` 强制立即把剩余缓冲落盘
并清空（容量保留复用）。批路径（`merge` 输出、`put_batch`）专用；单条
`put` 走 `DataFile::write`（每次 pwrite），不允许走批量 API（WAL 语义要
求每条立即可见，详见 `data_file.hpp` 注释）。

### 批语义契约

- **成功返回 ⟹ 整批已写入且全部可见**（keydir apply 在 data flush 之
  后，本进程内 all-or-nothing；读者不会观察到批的中间态）。
- **失败返回 ⟹ 整批在本进程内不可见**（keydir 未动）。磁盘上可能残留
  批的前缀（每条 record 独立自洽，崩溃重启 fold 后可见）——与连续单条
  put 的崩溃语义一致；本 API 不提供跨崩溃的原子性。
- **durability 与单条 put 的 sync 策略对齐**：
  - `o_sync`：fd 是 O_DSYNC，flush 的单次 pwrite 即 durable；
  - `sync_every_n > 0`：整批视作一次组提交，立即 fdatasync；
  - 其余（`sync_every_n == 0`）：同单条 put，由 caller 的 `sync()` 控制。
- **校验在任何写发生前完成**——全批 key/value 大小校验，失败零副作用。

---

## 9. `Cask::put_doc` —— 结构化文档

符号：`src/cask/cask.cpp::Cask::put_doc`

签名：

```cpp
std::expected<void, CaskFault>
Cask::put_doc(std::span<const std::byte> key, const DocInput& doc,
              std::uint32_t tstamp = 0);
```

`DocInput` 字段（`include/bitcask/cask.hpp`）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `text` | `span<const std::byte>` | 必填（多字段时可空作默认字段） |
| `meta` | `span<const std::byte>` | 可选 |
| `vector` | `span<const float>` | V3.1 文档向量（空 = 无） |
| `fields` | `vector<pair<string, span<byte>>>` | S8.6 多字段 |
| `expiry_at` | `uint32_t` | S13-D5 per-key 过期时刻（0 = 永不过期） |

执行序列与 `put` 高度相似，差异点：

- DocValue 编码携带 text + meta +（如有）vector +（如有）fields 四段。
- 向量先经 `vec_plugin_->normalize_for_write` 归一化（cosine 模式下
  存储即归一化值），再写入；`int8` 量化由 `opts_.vector_quantized` 控制。
- `expiry_at` 进 DocValue 头部，读端在 `derive_from_storage()` 读出赋给
  `GetResultView::expiry_at`；`get/iter` 时与 `now_sec` 比对过期即视作
  `kNotFound`，空间在 merge 时回收。
- 索引任务同样经 `submit_index_task(IndexOp::Add)` 进入流水线。

---

## 10. `tstamp` / `expiry_at` 语义

| 字段 | 来源 | 语义 |
|------|------|------|
| `tstamp` | caller 传入（`put` / `put_doc` / `put_batch`），0 时取 `now_sec_default()` | record 的逻辑时间戳，用于 merge 选 candidate、`needs_merge` 决策、调试追溯 |
| `expiry_at` | `put` / `put_doc` 透传；`put_batch` 不支持 | per-key 过期时刻（绝对 Unix 秒；0 = 永不过期）。读路径过期即 `kNotFound`；merge 清 keydir。**与 `CaskOptions::expiry_secs` 叠加**：任一判过期即过期。旧版本库读带 TTL 的记录 = 忽略 TTL（永不过期，静默降级） |

---

## 11. 持久性 / 可见性矩阵

| `CaskOptions` 配置 | put 返回时… |
|---|---|
| 默认（`o_sync = false`, `sync_every_n = 0`） | 字节在 OS page cache，未必落盘；调用方自己择时 `sync()` |
| `o_sync = true` | data + hint 都用 O_SYNC 打开，每次 write 写穿到磁盘 |
| `sync_every_n = N`（非 o_sync） | 每累计 N 条写后 `maybe_group_commit` 对 **active data** fsync 一次（组提交）；hint 不在此 fsync（可由 `fold(data)` 重建） |

读路径单调可见：`keydir_->put` 拿**对应分片**的 `unique_lock`
（`KeyDir::kShards = 256`，见 `keydir.hpp`），put 返回后任何后续
`get(K)` 立刻能看到新值（即使 OS 还没刷盘——只要进程不死就有效）。

---

## 12. 失败模式

| `CaskError` | 触发点 | 含义 |
|---|---|---|
| `kClosed` | `Cask::put` 入口 `is_closed()` 检查 | 已 close 的 handle 上发起的写 |
| `kReadOnly` | §2 step 4 | 没开 `read_write` 或 `merge_only` 句柄 |
| `kKeyTooLarge` | §2 step 5 | `key.size() > 0xFFFF` |
| `kValueTooLarge` | §2 step 6 | `value.size() > 0xFFFFFFFFu` |
| `kWriteLocked` | `acquire_writer_lock`（§4.2） | 别人持有 `bitcask.write.lock` 且活着 |
| `kIo` | §2 step 9 / §3.1 / §3.2 / §3.5 / §3.8 / §3.9 / §5 fsync / §8 `write_buffered` / `flush_batch` | pwrite / fsync / 文件创建失败 |
| `kAlreadyExists` | §3.11 | merge race 二次重试仍失败（极罕见） |

---

## 13. 关键不变量

- **file_id 单调递增**：`KeyDir::increment_file_id()` 是唯一分配器，
  `active_file_id_` 永远不小于历史任何已分配 id。配合 keydir 的
  staleness 检查防止「老 file_id 写入覆盖新值」（§2 step 10 主动 roll）。
- **ord 单调递增**：每次 `put` / `put_doc` / `put_batch` 调
  `keydir_->alloc_ord()` 获取全局单调递增序号，永不复用。用于 tie-breaking、
  有序遍历、异步索引的 `reorder buffer` 按 ord 排序。
- **DocValue 编码格式**：`type = kDoc` 的 value 必须是 `encode_doc_value`
  输出的打包格式（Ver + Flags + 可选段），否则 CRC 校验会失败。
- **data 在 hint 之前**：data 是权威，hint 只是加速重建的索引；hint
  损坏可以从 data 重建，反之不行。
- **keydir 在最后更新**：keydir 是 put 成功的唯一可见性边界；任何在
  keydir 更新前 crash 的 put 都不会被后续 get 看到（虽然字节可能已经
  在磁盘上）。

---

## 14. 源码导航

| 符号 | 角色 |
|------|------|
| `c_api/bitcask_c.cpp::bitcask_put` | C API 入口（slice → span + fault 翻译） |
| `src/cask/cask.cpp::Cask::put` | 写入编排主体（单条） |
| `src/cask/cask.cpp::Cask::put_doc` | 结构化文档写入 |
| `src/cask/cask.cpp::Cask::put_batch` | 1 MiB 块批写（聚合 flush） |
| `src/cask/cask.cpp::Cask::remove` | 软删除（墓碑 record） |
| `src/cask/cask.cpp::Cask::ensure_active_writer` | 锁 + 文件准备 |
| `src/cask/cask.cpp::Cask::roll_active` / `Cask::roll_active_if_needed` | 切文件 |
| `src/cask/cask.cpp::Cask::maybe_group_commit` | P4 单写者组提交 |
| `src/cask/cask.cpp::Cask::write_and_keydir` | data + hint + keydir 三步落定 |
| `src/cask/cask.cpp::Cask::submit_index_task` | 锁外提交异步索引任务 |
| `src/cask/cask.cpp::Cask::maybe_submit_auto_checkpoint` | S14-1 自动 ckpt |
| `src/fileops/data_file.cpp::DataFile::write` | 单条 pwrite data record |
| `src/fileops/data_file.cpp::DataFile::write_buffered` / `DataFile::flush_batch` | 1 MiB 块批写 |
| `src/fileops/hint_file.cpp::HintFile::write` | 落 hint record |
| `src/fileops/codec.cpp::encode_data_record` / `encode_doc_value` / `encode_hint_record` | 字节编码 |
| `src/keydir/keydir.cpp::KeyDir::put` | 内存索引更新 |
| `src/keydir/keydir.cpp::KeyDir::alloc_ord` / `KeyDir::increment_file_id` | 序号 / file_id 分配 |
| `src/search/text_plugin.cpp::TextPlugin::on_put` | BM25 倒排 apply（reducer 线程） |
| `src/search/vector_plugin.cpp::VectorPlugin::on_put` | HNSW insert（reducer 线程） |
| `include/bitcask/data_file.hpp::DataFile::kBatchFlushBytes` | `1u << 20`（1 MiB） |
| `include/bitcask/format.hpp::kHeaderSize` / `kMaxKeySize` / `kMaxValueSize` | 23 / `0xFFFF` / `0xFFFFFFFFu` |
| `include/bitcask/cask.hpp::Cask::put_batch` / `put_doc` / `put` / `BatchItem` / `DocInput` | API 签名 |
| `include/bitcask/thread_pool.hpp::IndexTask` / `IndexOp` / `IndexTask::make` | 异步索引任务 |