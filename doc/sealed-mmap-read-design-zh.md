# Sealed DataFile mmap 只读路径设计

> bitcask data 文件在 `roll_active` / merge 收尾后即 **sealed**（不可变）。
> sealed 文件只读，结构上完美契合 `mmap(PROT_READ, MAP_SHARED)`：指针
> 直读 page cache，零拷贝 + 无 syscall，与 `pread` 共享同一缓存层。
> 本设计在 `include/bitcask/data_file.hpp` 与 `src/fileops/data_file.cpp`
> 中落地，`get` 路径消费点在 `src/cask/cask.cpp`。

## 1. 背景与动机

- 数据文件读路径是 `pread`（`include/bitcask/io.hpp` 里的
  `io::PosixFile::pread` / `pread_into`）。`pread` 享 OS page cache
  （热数据不打盘），但每次 `get`：
  1. 一次 `pread` **syscall**；
  2. page cache → 用户态**一次拷贝**。
- mmap sealed 文件可同时消掉这两项：指针直读 page cache，**零拷贝 +
  无 syscall**，且**不双缓存**（mmap 就是 page cache 的视图）。
- 已有机制：
  - `Cask::read_files_`（详见 `read-handle-lru-design-zh.md`）按
    `file_id` 懒打开只读 `DataFile` 并缓存——sealed mmap 自然落进该
    缓存，淘汰随 LRU 一起走。
  - fd + mmap 同时占进程预算（`ulimit -n` / `vm.max_map_count`）——LRU
    上限同时界定两者。

## 2. 为何只 mmap sealed（不可变）文件

active（正在 append、尺寸在长）文件对 mmap 不友好：

- **映射长度固定** → append 超出映射区不可见。
- **预映射更大区间** → 访问超过当前文件大小的页触发 `SIGBUS`（除非
  `ftruncate` 撑大，破坏「文件大小 == 数据长度」不变量）。
- **随增长重映射** → `mremap` 可能**移动虚拟地址**，在途读者持有的指针
  悬垂。

bitcask 文件 **roll 后即 sealed/immutable**——`mmap` 只用于 sealed 文件，
**active 永远 pread**（通过 `active_data_` 旁路，详见
`read-handle-lru-design-zh.md` §5.1），append 问题自然规避。

## 3. mmap 决策（`DataFile::open`）

```cpp
static std::expected<DataFile, DataFileFault>
DataFile::open(std::string_view path, Mode mode, bool sync, bool mmap_enabled);
```

mmap 触发条件（`src/fileops/data_file.cpp::open`）：

```cpp
if (mode == Mode::kRead && mmap_enabled && sizeof(void*) >= 8 &&
    initial_off > 0) {
    void* base = ::mmap(nullptr, static_cast<std::size_t>(initial_off),
                        PROT_READ, MAP_SHARED, df.file_.fd(), 0);
    if (base != MAP_FAILED) {
        df.map_base_ = static_cast<const std::byte*>(base);
        df.map_size_ = static_cast<std::size_t>(initial_off);
        ::madvise(base, static_cast<std::size_t>(initial_off), MADV_RANDOM);
    }
}
```

四个条件同时满足才建映射：

| 条件 | 含义 | 失败后果 |
|------|------|----------|
| `mode == Mode::kRead` | 只读（= sealed） | skip mmap |
| `mmap_enabled` | 调用方开启 mmap | skip mmap |
| `sizeof(void*) >= 8` | 64 位平台 | skip mmap（32 位地址空间不足） |
| `initial_off > 0` | 文件非空 | skip mmap |

- `initial_off`：对 `kRead` / `kAppend` 是 `fseek(SEEK_END)` 拿到的
  当前文件大小；`kCreate` 模式下 `initial_off = 0`，条件自然不满足。
- `MAP_FAILED` 落空 → 不置 `map_base_`，**自然走 pread 路径**（见 §6
  fallback）。
- `madvise(MADV_RANDOM)`：D3 决定——`get()` 热路径按 `offset` 随机读，
  关掉内核 readahead 避免预取浪费。

### 3.1 调用方对 `mmap_enabled` 的选择

| 调用方 | `mmap_enabled` | 原因 |
|--------|----------------|------|
| `Cask::read_file`（热读路径，`read_files_` 缓存） | `true`（默认） | 命中零拷贝 |
| `cask_recovery` 扫盘（`DataFile::open(... mmap_enabled=false)`） | `false` | 一次性 fold，mmap 收益低 |
| `cask_iter` 迭代器 `pin_files` | `false` | 折迭代 `pread` 路径，规避映射开销 |
| `merge::Merger` 读输入文件 | `false` | 整文件顺序遍历，mmap 不必要 |
| `migrate` 工具 | `false` | 离线迁移，无重复读收益 |

也就是说：**sealed mmap 仅服务于 `read_file` 缓存路径**。所有「一次性
遍历 sealed 文件」的 fold / iter / merge 路径显式 `mmap_enabled=false`，
走 `pread` + 完整 fold / CRC 校验逻辑。

## 4. mmap 字段与生命周期

```cpp
class DataFile {
    // ...其它字段...
    const std::byte* map_base_ = nullptr;   // nullptr = 未映射
    std::size_t      map_size_ = 0;
};
```

- `map_base_ != nullptr` 表示已 mmap。`mmapped()` 公开判别。
- 析构 `~DataFile` munmap（`MAP_SHARED` 私有映射同样适用；本设计全用
  `MAP_SHARED`）。
- 移动构造 / 移动赋值**转移裸指针 + 把源置空**——避免双 `munmap`：
  ```cpp
  DataFile::DataFile(DataFile&& o) noexcept
      : file_(std::move(o.file_)), path_(std::move(o.path_)),
        current_offset_(o.current_offset_), mode_(o.mode_),
        write_buf_(std::move(o.write_buf_)),
        map_base_(o.map_base_), map_size_(o.map_size_) {
      o.map_base_ = nullptr;
      o.map_size_ = 0;
  }
  ```

## 5. 读路径：`read` vs `read_mmap`

`get` 主流程（`Cask::get`，`src/cask/cask.cpp`）：

```cpp
auto df = read_file(entry->file_id);
if (!df) { /* IO error / 重查重试 */ }

if (df->mmapped()) {
    auto rv = df->read_mmap(entry->offset, entry->total_sz);
    /* CRC / kBadCrc / kShortRead → CaskFault */
    if (rv->type == format::RecordType::kTombstone) {
        return std::unexpected(err(CaskError::kNotFound));
    }
    GetResultView view(std::move(df), rv->value, rv->type, ...);
    return view;
}

auto rec = df->read(entry->offset, entry->total_sz);   // pread 路径
GetResultView view(std::move(rec));
return view;
```

两种结果源统一到 `GetResultView`：

```cpp
struct GetResultView {
    fileops::ReadRecord           storage_;     // ① owned (pread)
    std::shared_ptr<fileops::DataFile> map_holder_;   // ② mmap 持有者
    std::span<const std::byte>    value_bytes_{};
    // ...value / meta / vector / tstamp / ord / expiry_at
};
```

- **mmap 命中**：`map_holder_` 锚定 `DataFile`（持 `shared_ptr`），
  `value_bytes_` 指向映射内的 `DocValue` 字节；`storage_` 空。
- **pread 兜底**：`storage_` 持 owned 缓冲（`ReadRecord::key` /
  `value` vector），`value_bytes_` 借 `storage_.value`。
- `derive_from_storage()` 把 `value_bytes_` 解析成 `value` / `meta` /
  `vector` 三个 span，量化则 dequant 进 `vector_dequant_`——三 ctor 共用。

`GetResultView` 必须**持 `shared_ptr<DataFile>` 锚定映射**：

> view 生命内映射不撤；即便期间 merge unlink 该文件，DataFile 引用
> 计数未归零 → 映射仍在 → 指针有效。

```cpp
// Cask.hpp：move 转移 shared_ptr；析构时（最后引用）才 munmap + 释放。
GetResultView(GetResultView&& other) noexcept;
```

## 6. mmap 失败 fallback

- `mmap` 返回 `MAP_FAILED` → `map_base_` 保持 `nullptr`，`mmapped()`
  返回 `false`。
- `Cask::get` 走 `else` 分支调 `df->read(offset, total_size)`——返回
  owned `ReadRecord`，语义与未 mmap 路径一致。
- 失败原因（无需特殊处理）：`ENOMEM`（虚拟地址 / vm.max_map_count 满）、
  32 位平台、文件大小 0、`mprotect` 失败等。**全部静默回退 pread**——
 读路径永不显式返回 mmap 错误给 caller。
- 多次 `DataFile::open` 的 mmap 失败不会自动重试；下次 `read_file`
 重新 `open`（LRU 淘汰后）会再尝试一次（可能因 `vm.max_map_count`
 恢复而成功）。

## 7. merge 生命周期（核心：映射指针不悬垂）

合并是 sealed mmap 的最大考验：merge 写新文件后 **unlink 旧文件**，
若在途读者持映射指针而我们 munmap 释放 → SIGSEGV / UAF。

### 7.1 延迟 munmap 模式

- merge unlink 时**不立即 munmap**——只从 `read_files_` 擦除该 file_id
  的 `ReadHandle`（去掉缓存引用）。在途读者仍持
  `shared_ptr<DataFile>`（来自 `read_file()` 返回值或
  `GetResultView::map_holder_`）→ DataFile 存活 → 映射存活。
- Linux 上 **unlinked-but-mapped 文件仍可读**（inode 由映射续命，
  类似 open fd 续命）。
- 最后引用析构（`~DataFile`）时才 `munmap`——同 `O10` UAF 修复
  / `read-handle-lru-design-zh.md` §5.3 共享 `shared_ptr` 续命模式。

### 7.2 重新 mmap

- merge 产出的新 sealed 文件 / `roll_active` 切成 sealed 的文件
  → 下次经 `read_file`（lazy open）走 §3 的 sealed mmap 路径建新映射。
- 不预热，不预建；纯 demand-driven。

### 7.3 状态机

```
       DataFile::open(kRead)                  roll_active / merge 完成
              │                                       │
              ▼                                       ▼
      mmap(PROT_READ,MAP_SHARED)        文件 sealed（不可变）
              │                                       │
              │                              ┌────────┴────────┐
              │                              ▼                 ▼
              │                       Cask::read_file    fold / iter / merge
              │                              │                 │
              │                              ▼                 ▼
              │                       mmapped 命中     pread 路径
              │                              │           (mmap_enabled=false)
              │                              ▼
              │                      GetResultView::map_holder_
              │                              │
              │   merge unlink ─ erase read_files_ entry
              │   (不 munmap)                │
              │                              ▼
              │                      在途 reader 仍持 shared_ptr
              │                              │
              ▼                              ▼
         ~DataFile (最后引用析构) → munmap + 关闭 fd
```

## 8. SIGBUS 源与防护

只有**外部 truncate 已映射文件**才会触发 `SIGBUS`——内核向访问该页
的进程投递信号。bitcask 自身安全：

- 写路径只 `pwrite` 到 active 文件；sealed 文件**永远不 truncate / write**。
- merge 收尾 `unlink` 旧文件，不 truncate——unlink 不影响已映射的 inode。
- 恢复流程 `truncate_to` 仅作用于「发现 torn write 的 active 文件」——
  尚未 mmap，无映射指针存在。

故 `SIGBUS` 在 bitcask 自身代码路径下不会发生。理论上 OS / 磁盘故障
导致 sealed 文件被截断的极端场景会触发 `SIGBUS`——与 LevelDB 等使用
`mmap` 的系统同样风险，非本设计独有。

## 9. 32 位 / 0 字节 / 模式门禁

- `sizeof(void*) >= 8`（`>=` 不是 `==`，兼容未来 128 位平台），不满足
  → 不尝试 mmap。32 位虚拟地址空间（典型 3-4 GB 用户态）容纳不了
  大库（GB 级 sealed 文件）+ 程序本身 + heap——同 LevelDB 的处理。
- `initial_off == 0`（空文件）→ 不 mmap；`mmap` 对 0 长度在某些内核
  返回 `EINVAL`，直接门禁更安全。
- `Mode::kAppend` / `Mode::kCreate` → 不 mmap（条件 `mode == kRead`
  失败）；只走 `pwrite` 写路径。
- `mmap_enabled = false`（fold / iter / merge 路径） → 不 mmap；走
  pread。

## 10. 关键不变量

1. **只 mmap sealed 文件**；merge 只 `unlink`、**绝不原地 truncate**
   sealed 文件 → 无 `SIGBUS-on-truncate`。
2. sealed 已 finalize → **无 torn-tail**（active 才有 torn-write，
   走 pread + `truncate_to` 修复）。
3. 映射指针**绝不跨 `DataFile` 生命逃逸**：`GetResultView` /
   读者 / fold 回调都持 `shared_ptr<DataFile>` 锚定。
4. `map_base_` 唯一所有者：构造 / 移动 / 析构对 `map_base_` / `map_size_`
   的转移 / 释放严格定义，**无双 munmap**。
5. 写路径（`pwrite` 到 active）**不**触碰 `map_base_`：active 不 mmap，
   sealed 不写。

## 11. 与 ReadHandle LRU 的协同

| 关注点 | ReadHandle LRU | sealed mmap |
|--------|----------------|-------------|
| 句柄内容 | `shared_ptr<DataFile>` + `atime` | `DataFile` 内部 `map_base_` / `map_size_` |
| 谁控 fd 数 | LRU 上限 `max_read_handles` | 不单独控制（共享同一 `DataFile`） |
| 谁控 mmap 数 | LRU 上限（淘汰即触发 `~DataFile`） | 同上 |
| 谁管 lazy open | `Cask::read_file` 独占锁下 `try_emplace` | `DataFile::open` 内 `mmap` |
| 谁管淘汰 | `evict_read_handles_locked` | `~DataFile`（最后一个 `shared_ptr` 析构） |
| 并发读安全 | `read_cache_mu_` shared / 命中无锁 | `DataFile::read_mmap` 只读映射 + 纯解码，OS 层 thread-safe |
| fd 关闭时机 | LRU 淘汰 / merge unlink | `~DataFile` 内 `file_.close_quiet()` |

`sealed mmap` 与 `ReadHandle LRU` 不是两个独立上限——**LRU 上限同时
界定 fd 数 + mmap 数**。淘汰一个句柄同时释放 1 fd + 1 mmap（最后
`shared_ptr` 析构时 `~DataFile` 内 `munmap` + `close_quiet`）。

## 12. 测试

- **正确性**：sealed mmap 路径读出与 pread 路径**完全等价**（byte-for-byte）。
  全套既有 `get` / 迭代器 / merge 测试覆盖——它们不区分两条路径。
- **读中 merge unlink**（`P6MmapViewSurvivesMergeUnlink`）：一个读者持
  `GetResultView`（映射 span），并发 merge unlink 该文件 → 读者照常读、
  无 UAF / `SIGBUS`；引用释放后文件真正 munmap + 回收。
- **mmap 失败 fallback**：mock `mmap` 返回 `MAP_FAILED`（或构造
  `ENOMEM` 环境）→ `get` 静默回退 pread，返回正确值。
- **32 位 / 0 字节 / 模式门禁**：构造 32 位编译 / 0 字节文件 /
  `Mode::kAppend` → `mmapped() == false`，所有路径走 pread。
- **`mmap_enabled=false` 路径**：recovery / iter / merge 入口显式传
  false → `DataFile::open` 跳过 mmap；fold / iter 仍正确（pread 兜底）。
- **close + reopen**：sealed mmap 路径在 reopen 后仍命中（重新
  `DataFile::open` + mmap）。

