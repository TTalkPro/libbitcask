# 批量 flush 设计（当前代码版）

本文档描述 libbitcask **当前** 代码中所有「批量缓冲 + 一次落盘」的路径，
并把原 V6.2 文档里描述的 `InvertedWal::seal_and_write` 批量改造标注为
**已退役**——`InvertedWal` 模块已在 S22 拍板删除（CHANGELOG `Removed`
段，2026-07-06），`enable_wal` / `wal_batch_size` 配置管道整体摘除。

---

## 1. 现状速览

| 路径 | 触发场景 | 缓冲粒度 | 单次落盘单元 | 文件 |
|------|----------|----------|-------------|------|
| data file 块写 | `Cask::put_batch`、merge 输出 | `DataFile::batch_buf_`（≥ `kBatchFlushBytes`） | 一次 `pwrite(2)` | `include/bitcask/data_file.hpp` / `src/fileops/data_file.cpp` |
| data file 组提交 | `Cask::maybe_group_commit` | `writes_since_sync_` 计数 | `fsync(2)`（fsync 不缓冲，仅触发落盘） | `src/cask/cask.cpp::Cask::maybe_group_commit` |
| 索引任务队列 | `Cask::put` / `put_doc` / `put_batch` / `remove` → 异步索引 | `tbb::concurrent_bounded_queue<IndexTask>`（容量 10240） | reducer 单线程 apply | `include/bitcask/thread_pool.hpp` / `src/keydir/key_pool.cpp` |
| ~~InvertedWal 批量 flush~~ | ~~`append_add_doc` / `append_remove_doc` 缓冲 N 条再写~~ | — | — | **已退役**（见 §2） |

---

## 2. InvertedWal 批量 flush（V6.2 设计，已退役）

**状态**：S22 用户拍板删除；模块文件 `inverted_wal.{cpp,hpp}`（465 行）
+ 13 例测试 + 全部钩子已摘除，理由：

- `enable_wal` 生产零调用 —— S14-4 之后 delta 链已经承担增量持久化，
  维护「WAL + 全量 ckpt」双轨没收益。
- 测试夹具不依赖（inverted_wal_test 跟着删）。
- 落盘字节层已被 data file 吞掉（data file 本身已是带 ord 的全量 WAL，
  bm25 重新打 WAL 等于把同一笔写两遍）。

代码中无残留：

```bash
$ grep -rn 'class InvertedWal\|InvertedWal(' src/ include/
# 无匹配
```

唯一相关的是 `search_config.hpp` 与本目录历史文档里提及的「以 git 历史
`inverted_wal.*` 为底重新接线」注释。S22 设计备忘见 `TASK.md` §22-B2。

### 2.1 V6.2 历史设计要点（供 git 历史 / 重新接线时参考）

原 V6.2 提出的方案（详见 git 历史 / 本文档旧版）：

- 帧格式不变：现有 `[4B len][payload][4B crc32]` O11 framing，多条
  framed entry 拼在 `batch_buf_`，一次 `fwrite` 写出整块，`replay`
  侧逐条解析，不感知批量边界。
- 阈值：`batch_size` 可配，默认 1（等价旧行为）。
- 崩溃窗口：`batch_size = 1` 不丢；`batch_size = N > 1` 最多丢 N-1 条；
  已 `fflush` 的 entry 一定持久化；不丢「已持久化但未 replay」的 entry
  语义。
- 安全性：`batch_buf_` 中的 entry 尚未进入 WAL 持久化边界 → 对应文档
  不会出现在倒排中 → 与数据文件中记录一致（data file 与 WAL 是同一 put
  调用链，crash 前 data file 的 fwrite 也可能丢失），不降低系统级一致性。

这些要点在 S22 退役时已被 delta 链吸收。

---

## 3. 当前批量路径一：data file 块写

符号：`include/bitcask/data_file.hpp::DataFile::write_buffered` /
`DataFile::flush_batch` / `DataFile::kBatchFlushBytes`

### 3.1 常量

| 常量 | 值 | 位置 |
|------|----|------|
| `DataFile::kBatchFlushBytes` | `1u << 20`（1 MiB） | `include/bitcask/data_file.hpp` |

### 3.2 接口

```cpp
class DataFile {
    // 累积缓冲：编码后的 record 追加到 batch_buf_，
    // 累计 ≥ kBatchFlushBytes 才触发一次 pwrite。
    // 返回的 offset 是逻辑偏移（含未落盘缓冲），确定性不依赖落盘时机。
    std::expected<WriteResult, DataFileFault>
    write_buffered(RecordType type, uint32_t tstamp, uint64_t ord,
                   span<const std::byte> key, span<const std::byte> value);

    // 把 batch_buf_ 里尚未落盘的字节一次 pwrite 到磁盘并清空。
    std::expected<void, DataFileFault> flush_batch();

private:
    std::vector<std::byte> batch_buf_;
    static constexpr std::size_t kBatchFlushBytes = 1u << 20;
};
```

约束（`data_file.hpp` 注释）：

- **仅用于「flush（+按策略 sync）之后才被 caller 采信」的场景**：
  merge 输出、`put_batch`（flush 后才 apply keydir）。
- **单条 `put` 不可用本 API** —— WAL 语义要求每条立即 pwrite（详见
  `data_file.hpp::write_buffered` 注释 ⑪ 否决记录）。
- `flush_batch()` 空缓冲是 no-op；`write()` 路径从不缓冲，调本函数恒
  为 no-op。
- `sync()` 内部已兜底调用 `flush_batch()`。

### 3.3 调用方

| 调用方 | 时机 | 持久化 |
|--------|------|--------|
| `Cask::put_batch` | 每条 record 入 `batch_buf_`；批末尾 `flush_batch()` | 批的提交点（见 §4） |
| `merge`（`Merger::run_merge`） | merge 输出累积 | merge 收尾统一 sync |

### 3.4 `Cask::put_batch` 的批量持久化

符号：`src/cask/cask.cpp::Cask::put_batch`

批的提交流程（关键路径）：

```
1. 全批前置校验（key / value 大小）         ← 校验失败零副作用
2. roll_active_if_needed(about)
   race 检查 (file_id < biggest) → roll_active()
3. for each item:
     ord = keydir_->alloc_ord()
     encode_doc_value(encoded, {.text=value})
     active_data_->write_buffered(kDoc, tstamp, ord, key, encoded)
4. active_data_->flush_batch()               ← 批的提交点
5. !opts_.o_sync && opts_.sync_every_n > 0:
     active_data_->sync()                    ← fdatasync，整批视作一次组提交
     writes_since_sync_ = 0
6. for each item:
     active_hint_->write(tstamp, total_size, offset, tomb=false, key)
7. for each item:
     keydir_->put(...)                       ← keydir apply 在 flush 之后
     kAlreadyExists → 单条 write_and_keydir 重写（内部 roll + 重试）
8. flush_adds() 锁外：submit_index_task(IndexOp::Add)
```

durability 与单条 put 的 sync 策略对齐：

- `o_sync`：fd 是 O_DSYNC，`write_buffered` 的 pwrite 即 durable；
- `sync_every_n > 0`：整批视作一次组提交，立即 `fdatasync`；
- 其余（`sync_every_n == 0`）：与单条 put 相同，由 caller 的 `sync()`
  控制。

批语义契约：

- **成功 ⟹ 整批已写入且全部可见**（keydir apply 在 flush 之后，本进程
  内 all-or-nothing）。
- **失败 ⟹ 整批在本进程内不可见**（keydir 未动）。磁盘上可能残留批的
  前缀（每条 record 独立自洽，崩溃重启 fold 后可见）——与连续单条 put
  崩溃语义一致；本 API 不提供跨崩溃的原子性。

详见 [`put-flow-zh.md` §8](./put-flow-zh.md)。

---

## 4. 当前批量路径二：data file 组提交

符号：`src/cask/cask.cpp::Cask::maybe_group_commit`

P4 单写者组提交：`put` / `put_doc` / `remove` 每次写后调用。

```cpp
std::expected<void, CaskFault> Cask::maybe_group_commit(bool force = false) {
    if (opts_.o_sync || opts_.sync_every_n == 0 || !active_data_) return {};
    if (!force) ++writes_since_sync_;
    const bool flush_now =
        force ? (writes_since_sync_ > 0)
              : (writes_since_sync_ >= opts_.sync_every_n);
    if (!flush_now) return {};
    if (auto r = active_data_->sync(); !r) return unexpected(io_fault(...));
    writes_since_sync_ = 0;
    return {};
}
```

`CaskOptions::sync_every_n` 默认 0（关闭）；`o_sync` 默认 false。
组提交只对 **active data file** 调 fsync；hint 不在此 fsync（可由
`fold(data)` 重建，崩溃回退）。`force = true` 用于 `roll_active()` /
`put_batch` 重写路径的尾批落盘。

写路径单线程（库内 `write_mu_` 串行化），`writes_since_sync_` 无需原子
计数。

---

## 5. 当前批量路径三：异步索引任务队列

符号：`include/bitcask/thread_pool.hpp::IndexTaskQueue` /
`IndexPool::submit` / `IndexOp`

异步索引流水线本身就是一个批量背压系统：生产者（写路径）入队，消费者
（map worker × N + reducer × 1）按 ord 序串行 apply。

### 5.1 队列容量

| 常量 | 值 | 位置 |
|------|----|------|
| `IndexTaskQueue` 默认 capacity | `10240` | `include/bitcask/thread_pool.hpp` |
| 队列实现 | `tbb::concurrent_bounded_queue<IndexTask>` | 同上 |

`concurrent_bounded_queue` 在 `push` 满时阻塞生产者 → 写路径在索引管
线堵时自然背压，无需额外信号量。

### 5.2 任务类型

```cpp
enum class IndexOp : uint8_t {
    Add,        // 记录写入（map 各插件 prepare，reduce 广播 on_put）
    Delete,     // 记录删除（reduce 广播 on_delete）
    Skip,       // S6-P1：ord 空洞填充
    RunFn,      // S13-F6：reducer 内执行任意回调
    Sentinel,   // 停止信号
};
```

### 5.3 流水线

```
写线程                IndexTaskQueue            N 个 map worker         reducer
──┬────────────  ────────────────────  ────────────────────────  ──────────
  │submit(Add) → →bounded_queue →     prepare_index_task(task) → reorder
  │                capacity=10240       (TextPlugin + VectorPlugin  buffer
  │                                       并行 prepare)         按 ord 序
  │                                                                   │
  │                                                              on_put
  │                                                          (TextPlugin +
  │                                                           VectorPlugin)
  │                                                                   ↓
  │                                                              apply 完成
```

- `prepare_index_task`：N 个 map worker 并行调（注册序闭包），每条
  IndexTask 产出 `plugin::PreparedPtr`（Text 切词 + BM25 posting +
  Vector 量化）。
- per-lane reorder buffer：按 `ord` 排序（保证 reducer 按写序 apply，
  与 `ord` 单调递增不变量一致）。
- reducer：单线程串行 apply，调用 `TextPlugin::on_put` 与
  `VectorPlugin::on_put`。
- 失败语义：worker 抛异常 → `on_index_worker_error()` 自增
  `index_errors` 计数，`status()` 暴露；写入仍成功（put 已持久化）。
- 可见性：`Cask::flush_index()` / `Cask::drain_plugins()` 排空在途任务
  → 查询前 read-your-writes 屏障。

完整设计见 [`async-index-pipeline.md`](../docs/design/async-index-pipeline.md)。

---

## 6. 批量 flush 阈值常量汇总

| 常量 | 值 | 位置 | 触发 |
|------|----|------|------|
| `DataFile::kBatchFlushBytes` | `1u << 20`（1 MiB） | `include/bitcask/data_file.hpp` | data file `batch_buf_` 累积 ≥ 1 MiB 触发一次 pwrite |
| `CaskOptions::sync_every_n` | `0`（默认） | `include/bitcask/cask.hpp` | 累计 N 次写后 `maybe_group_commit` 对 active data fsync |
| `IndexTaskQueue::capacity` | `10240` | `include/bitcask/thread_pool.hpp` | 索引任务队列 bounded 上限；满则生产者背压 |

**注**：`inverted.hpp` / `inverted.cpp` 内**不存在**任何 `kBatchFlushThreshold`
之类常量 —— 旧 V6.2 文档里的 `InvertedWal::batch_buf_` / `batch_count_`
字段及对应阈值随模块退役已移除。`InvertedIndex::add_doc` / `remove_doc`
当前直接入内存 posting 列表，无中间缓冲层。

---

## 7. 崩溃窗口与一致性

| 路径 | 崩溃时丢失 | 系统级一致性 |
|------|-----------|--------------|
| data file 块写（`write_buffered`） | `batch_buf_` 中未 `flush_batch()` 的字节 | 不变 —— flush 前的 record 对应 keydir 也未 apply |
| data file 组提交（`maybe_group_commit`） | 自上次 fsync 以来的写入（默认下 fsync 关闭，丢失整段 page cache） | 同上 |
| 索引任务队列 | reducer 未 apply 的 task | 索引滞后于数据文件；下次 `drain_plugins` / `flush_index` 可见 |
| ~~InvertedWal 批量 flush~~ | ~~batch_buf 中未 flush 的 entry~~ | **已退役**；S14-4 delta 链取代 |

---

## 8. 配置管道

| 配置 | 路径 | 默认 | 备注 |
|------|------|------|------|
| `CaskOptions::max_file_size` | `cask.hpp` | `2 GiB` | active file 软上限；`put_batch` 巨批可突破 |
| `CaskOptions::o_sync` | `cask.hpp` | `false` | O_SYNC 打开 fd |
| `CaskOptions::sync_every_n` | `cask.hpp` | `0`（关闭） | 组提交触发频率 |
| `IndexPool::queue_capacity` | `thread_pool.hpp` | `10240` | 异步索引队列容量 |
| ~~`SearchLayerConfig::wal_batch_size`~~ | — | — | **已退役**（V6.2 引入，S22 删除） |

`wal_batch_size` 字段在 V6.2 引入但「生产零调用」（CHANGELOG S22-B2），
与 `SearchLayerConfig::enable_wal` / `InvertedWal` 一起退役。

---

## 9. 不做的事

- **不做异步 flush 线程** —— data file `flush_batch` 仍同步（`put_batch`
  写满 1 MiB 或批末尾同步 flush）；索引任务的 apply 已由 IndexPool
  reducer 线程异步跑（见 §5）。
- **不做 WAL 跨线程 group-commit** —— 不存在 WAL（inverted_wal 退役）；
  data file 写由库内 `write_mu_` 串行化，单 append WAL 单写者。
- **不改 IndexTask 帧格式** —— `IndexTask` 字段（`buf/ord/key_len/...`）
  是进程内内存对象，不存在「帧格式」概念；入队 / 出队由
  `tbb::concurrent_bounded_queue` + TSan 标注同步（`annotate_release` /
  `annotate_acquire`）。

---

## 10. 源码导航

| 符号 | 角色 |
|------|------|
| `include/bitcask/data_file.hpp::DataFile::write_buffered` | data file 块写（累积到 `batch_buf_`） |
| `include/bitcask/data_file.hpp::DataFile::flush_batch` | data file 一次 pwrite + 清空 |
| `include/bitcask/data_file.hpp::DataFile::kBatchFlushBytes` | `1u << 20`（1 MiB）触发阈值 |
| `src/cask/cask.cpp::Cask::put_batch` | 1 MiB 块批写主路径 |
| `src/cask/cask.cpp::Cask::maybe_group_commit` | P4 单写者组提交（fsync 触发） |
| `src/cask/cask.cpp::Cask::roll_active` | 文件封口（`force = true` 触发尾批落盘） |
| `include/bitcask/thread_pool.hpp::IndexTaskQueue` | 索引任务 bounded 队列 |
| `include/bitcask/thread_pool.hpp::IndexTask` / `IndexOp` | 任务类型与构造 |
| `include/bitcask/cask.hpp::CaskOptions::sync_every_n` / `o_sync` / `max_file_size` | 配置面 |