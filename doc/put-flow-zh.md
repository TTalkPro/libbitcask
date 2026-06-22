# put(K, V) 完整调用链

从 C API 入口一路到磁盘字节落定。每一步都标了源码位置，方便对照阅读。
本文描述的是 `bitcask_put()` 的成功路径与关键 race 处理；磁盘格式细节
见 [`format-zh.md`](format-zh.md)。

---

## 第 0 层：C API 入口

`c_api/bitcask_c.cpp::bitcask_put`

1. 校验参数（`cask` / `key.data` / `value.data` 非空）
2. 把 `bitcask_slice_t` 包成 `std::span<const std::byte>`（zero-copy view，
   不复制 `key` / `value` 字节）
3. 调 `h->cask->put(key_span, val_span, tstamp)`，`h` 是持有
   `bitcask::Cask*` 的不透明句柄
4. 把 `std::expected<void, CaskFault>` 翻成 `bitcask_error_t`，并把
   详情写入 out-param `bitcask_fault_t`（`fault == NULL` 时静默丢弃）

`bitcask_put` 在**调用方线程上同步执行**——单次 put 是确定的小工作量，
不需要把控制权让出去。长耗时的 `merge` 由调用方自己决定：开一个
worker 线程（或子进程）调 `bitcask_merge`，write 路径仍由调用方串行化
（caller-side 锁，库内不做多写者同步）。

---

## 第 1 层：Cask::put — 写入编排

`src/cask/cask.cpp::Cask::put`

```
1. 权限校验
   - read_write == false 或 merge_only == true → kReadOnly
2. 大小校验
   - key.size() > kMaxKeySize(65535)        → kKeyTooLarge
   - value.size() > kMaxValueSize(~4 GiB)   → kValueTooLarge
3. tstamp 默认值
   - tstamp == 0 时取当前 Unix 秒
4. 估算本次 record 字节数
   - about = kHeaderSize(23) + key.size() + value.size()
     ★ 用**原始 value.size()**（编码尚未发生；DocValue 多出的几字节忽略）
5. roll_active_if_needed(about)
   - 没 active writer → ensure_active_writer()
   - 写入会撑爆 max_file_size → roll_active()
6. ★ 关键 race 检查：active_data_ && active_file_id_ < keydir.biggest_file_id()
   → roll_active()
   - 并发 merger 抢先把 biggest_file_id 推过去了
   - 不主动 roll 的话第 9 步 keydir.put 会被 staleness 检查拒掉，put 静默丢失
7. ★ 分配 ord
   - ord = keydir_->alloc_ord()（全局单调递增序号，atomic fetch_add）
8. ★ DocValue 编码
   - thread_local encoded（append 语义，clear 后复用，消每次 put 堆分配）
   - codec::encode_doc_value(encoded, {.text = value})
   - encoded = [Ver=3][Flags][Len:varint][text bytes]
9. write_and_keydir(key, encoded, tstamp, ord)：data → hint → keydir 三步落定
   a. active_data_->write(kDoc, tstamp, ord, key, encoded)      → 落 data record
   b. active_hint_->write(tstamp, total_sz, offset, false, key) → 落 hint record
   c. keydir_->put(key, active_file_id_, total_sz, offset, tstamp,
                   0, /*newest_put=*/true, 0, 0, ord)
   d. keydir 返回 kAlreadyExists（极少见的二段 race）→ roll_active + 重试一次；
      二次仍失败 → 上报 kAlreadyExists
10. submit_index_task(IndexOp::Add, ...)
    - 索引模式下入队 IndexPool（单 worker 异步 on_write）；无 IndexPool 则 no-op
    - 注意：put 路径**不**同步调 search_->on_write，全部异步（见 concurrency-zh §6）
11. maybe_group_commit()
    - sync_every_n>0 且非 o_sync 时：累计写满 N 条 → active_data_->sync() 组提交
```

---

## 第 2 层：active writer 准备（首次写或切文件时）

`Cask::ensure_active_writer` (`src/cask/cask.cpp`)

```
1. 如果 active_data_ 已存在 → 直接返回
2. 如果 write_lock_ 空（之前 close_write_file 释放过）：
   - acquire_writer_lock(dirname)
     · O_CREAT|O_EXCL 创建 bitcask.write.lock
     · EEXIST → try_remove_stale_lock：
         读现有锁 → 解析 pid → kill(pid, 0) 探活
         死了 → unlink 重试
         活着 → 返回 kWriteLocked
3. active_file_id_ = keydir_->increment_file_id()  ← 单调递增分配
4. 创建 <file_id>.bitcask.data（O_CREAT|O_EXCL，o_sync 模式带 O_SYNC）
5. 创建 <file_id>.bitcask.hint（同上）
6. 把 "<pid> <active_data_path>\n" 写到 bitcask.write.lock
   → merger 读这个内容知道「不能合这个文件」
```

---

## 第 3 层：磁盘字节布局

### `DataFile::write` (`src/fileops/data_file.cpp`)

`codec::encode_data_record` 在内存 buffer 里组装（23 B 固定 header）：

```
偏移   字段      值
───────────────────────────────────────
0      CRC32     算 Type..Value 的 zlib CRC，写到这里（4 B）
4      Type      u8：0=kDoc，1=kTombstone
5      Tstamp    小端 u32（4 B）
9      Ord       小端 u64（8 B）—— 单调递增的写入序号
17     KeySz     小端 u16（2 B）
19     ValueSz   小端 u32（4 B）
23     Key       原样字节
23+KS  Value     DocValue 打包字节（kDoc 时）：[Ver=3][Flags][可选段…]（段长 varint）
```

CRC 覆盖 `Type..Value`（即偏移 4 起的所有内容），不包含 CRC 字段本身。
这与 legacy 从 `Tstamp` 开始计算不同。

然后 **`file_.pwrite(current_offset_, buf)`**——注意是 pwrite 不是
write，文件虽然带 O_APPEND 打开，但这里显式追踪偏移，方便 torn-write
修复时 truncate。

写完 `current_offset_ += total`，返回 `{offset, total_size}` 给上层。

### DocValue 打包格式（kDoc value）

`codec::encode_doc_value` 将 value 编码为：

```
偏移   字段              字节数   说明
───────────────────────────────────────────
0      Ver               1        当前 = 3
1      Flags             1        bit0=has_vector bit1=has_text bit2=has_meta bit4=has_fields
       各段按 vector→text→meta→fields 顺序、由 Flags 决定存在；长度/计数全 varint：
         vector?  [Dim:varint][f32×Dim 小端]
         text?    [Len:varint][bytes]
         meta?    [Len:varint][bytes]
         fields?  [FieldCount:varint] ×{[FieldId:varint][ValLen:varint][value]}
```

普通 `put(K,V)` 写入时只有 text 段（原始 value 作为 UTF-8 文本）。完整布局与
field id（schema interning）见 `format-zh.md` §五。

### `HintFile::write` (`src/fileops/hint_file.cpp`)

`codec::encode_hint_record` 组装（18 B 固定 header）：

```
偏移   字段          值
─────────────────────────────────────
0      Tstamp        小端 u32
4      KeySz         小端 u16
6      TotalSz       data record 整条字节数（含 23 B header）
10     Tomb|Offset   小端 u64：(0 << 63) | offset      ← 普通 put，tomb=0
18     Key           原样字节
```

然后 `file_.write(buf)` 顺序追加，并
`running_crc_ = crc32_update(running_crc_, buf)`——为最终 close 时写
trailer 用。

---

## 第 4 层：内存索引更新

`KeyDir::put` (`src/keydir/keydir.cpp`)

```
0. this_epoch = epoch_.fetch_add(1) + 1  ← 全局逻辑时钟，作为本次写入的版本号
   （在取分片锁前分配；epoch_ 是 atomic）
1. unique_lock(sh.mu)  ← 该 key 所属分片的锁（kShards=256 把之一，非单一全局锁）
2. 探测 key（S2 顺序：先本分片 entries_，miss 且 fold 态再嵌套 meta shared 查 pending_）
3. 三大分支：
   (A) key 不存在 / 是墓碑：
       - merge race 检查：newest_put 且 file_id < biggest_file_id_ → kAlreadyExists
       - 没 fold → entries_[key] = SingleEntry{..., ord}  ← 最常见路径
       - 在 fold → 走 pending 表
   (B)/(C) key 存在：
       - staleness 检查（newest_put=true 模式：file_id >= biggest_file_id_）
       - 没 fold → 直接覆盖
       - 在 fold → 升级为 sibling 链 MultiEntry，新 revision 插链头
4. 更新 fstats：
   - 新 file → 创建一条；live++、total++、live_bytes+=sz、total_bytes+=sz
   - 同 file 覆盖 → 旧 file live--、新 file live++
5. biggest_file_id_ = max(biggest_file_id_, file_id)
```

`ord` 参数在第 3 步写入 SingleEntry/MultiEntry，用于 tie-breaking
和 SearchLayer 的文档跟踪。

---

## 完整时序示意

```
调用线程                 C API / C++                   磁盘
────────────────────────────────────────────────────────────
bitcask_put(cask, K, V, 0, &fault)
  └─→ bitcask_put (C API)
        └─→ Cask::put
              ├─[校验 + DocValue 编码 + alloc_ord]
              ├─→ DataFile::write
              │     └─ pwrite(off, [CRC|Type|Tstamp|Ord|KS|VS|K|DocValue])
              │                                         ━━━━ <id>.bitcask.data
              ├─→ HintFile::write
              │     └─ write([Tstamp|KS|TSz|Off|K])     ━━━━ <id>.bitcask.hint
              ├─→ KeyDir::put（持 K 所属分片锁）
              │     └─ entries_[K] = {fid,off,tsz,ep,ts,ord}
              ├─[可选]→ submit_index_task(IndexOp::Add)   ← 入队，不阻塞
              │           └┄(异步) IndexPool worker → SearchLayer::on_write
              │                                          └─ InvertedIndex::add_doc
              └─→ maybe_group_commit()  ← sync_every_n 组提交
        return BITCASK_OK
        （索引在 worker 线程异步建立；put 返回不代表索引已就绪，
          搜索路径靠 flush_index/prepare_search 排空在途任务）
```

DocValue 编码将原始 value 打包为 `[Ver=3][Flags][Len:varint][text bytes]`——
普通 put 只含 text 段。写入 record 的 type = kDoc，ord = alloc_ord()
返回的单调递增序号。

落盘顺序固定：**data → hint → keydir**。这是有意为之：

- 如果 data 写到一半 crash → 文件尾是损坏字节，下次 open 的
  `fold(data)` 会跳过、`out_last_valid_end` 报告位置、cask 自动 truncate 修复
- 如果 data 写完但 hint 没写完 → hint trailer CRC 校验失败，cask 忽略
  hint 退回 `fold(data)` 重建，data 是权威
- 如果 data + hint 都写完但 keydir 没更新（比如 cask 进程崩溃）→ 下次
  open 重建 keydir 时会扫到这条 record，自然恢复

---

## 持久性 / 可见性

| `CaskOptions` 配置                  | put 返回时…                                            |
|---|---|
| 默认（`o_sync=0, sync_every_n=0`）  | 字节在 OS page cache，未必落盘；调用方自己择时 `sync()`   |
| `o_sync=1`                          | data + hint 都用 O_SYNC 打开，每次 write 写穿到磁盘       |
| `sync_every_n=N`（非 o_sync）       | 每累计 N 条写后 `maybe_group_commit` 对 **active data** fsync 一次（组提交）；hint 不在此 fsync（可由 fold(data) 重建） |

读路径是单调可见的：`keydir.put` 拿 `unique_lock` 串行化所有写者，
put 返回后任何后续 `get(K)` 立刻能看到新值（即使 OS 还没刷盘——只要
进程不死就有效）。

---

## 失败模式

| 错误                                | 触发点                          | 含义                                       |
|---|---|---|
| `BITCASK_ERR_READ_ONLY`        | step 1                          | 没开 read_write，或 merge_only 句柄          |
| `BITCASK_ERR_KEY_TOO_LARGE`    | step 2                          | key > 65535 字节                            |
| `BITCASK_ERR_VALUE_TOO_LARGE`  | step 2                          | value > kMaxValueSize (~4 GiB)              |
| `BITCASK_ERR_WRITE_LOCKED`     | step 5 (ensure_active_writer)   | 别人持有 bitcask.write.lock 且活着            |
| `BITCASK_ERR_IO` (errnum=ENOSPC) 等 | step 9 (data/hint write) / 11 (group commit fsync) | pwrite / fsync 失败          |
| `BITCASK_ERR_ALREADY_EXISTS`   | step 9d                         | 二段 race 重试仍失败（极罕见）                |

---

## 关键不变量

- **file_id 单调递增**：`KeyDir::increment_file_id()` 是唯一分配器，
  active_file_id_ 永远不小于历史任何已分配 id。配合 keydir 的 staleness
  检查防止「老 file_id 写入覆盖新值」。
- **ord 单调递增**：每次 `put` 调用 `keydir_->alloc_ord()` 获取全局单调
  递增序号，永不复用。用于 tie-breaking、有序遍历和 SearchLayer 文档跟踪。
- **DocValue 编码格式**：`type = kDoc` 的 value 必须是 `encode_doc_value`
  输出的打包格式（Ver + Flags + 可选段），否则 CRC 校验会失败。
- **data 在 hint 之前**：data 是权威，hint 只是加速重建的索引；
  hint 损坏可以从 data 重建，反之不行。
- **keydir 在最后更新**：keydir 是 put 成功的唯一可见性边界；任何在
  keydir 更新前 crash 的 put 都不会被后续 get 看到（虽然字节可能已经
  在磁盘上）。

源码导航：

- `c_api/bitcask_c.cpp::bitcask_put` — C API 入口（slice → span + fault 翻译）
- `src/cask/cask.cpp::Cask::put` — 写入编排主体
- `src/cask/cask.cpp::ensure_active_writer` — 锁 + 文件准备
- `src/fileops/data_file.cpp::DataFile::write` — 落 data record
- `src/fileops/hint_file.cpp::HintFile::write` — 落 hint record
- `src/fileops/codec.cpp::encode_data_record` / `encode_doc_value` /
  `encode_hint_record` — 字节编码
- `src/keydir/keydir.cpp::KeyDir::put` — 内存索引更新
- `src/keydir/keydir.cpp::KeyDir::alloc_ord` — ord 分配
- `src/search/search_layer.cpp::SearchLayer::on_write` — 索引更新（索引模式）
