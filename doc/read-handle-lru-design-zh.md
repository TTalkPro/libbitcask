# ReadHandle LRU 缓存设计

> `Cask::read_file` 按 `file_id` 懒打开只读 `DataFile` 并缓进 `read_files_`。
> 句柄既占 1 个 fd（`DataFile::read` / `fold` 的 pread 路径），也占 1 个
> sealed mmap（详见 `sealed-mmap-read-design-zh.md`），所以本 LRU 同时
> 界定 fd 数与 mmap 映射数。本设计在 `include/bitcask/cask.hpp` 与
> `src/cask/cask.cpp` 中落地。

## 1. 动机

- 大库（data/2 GiB 文件数）`get` 路径会触达几十~上百个 `file_id`——
  不限上限时每个被读过的文件常驻 1 fd + 1 mmap，`ulimit -n` /
  `vm.max_map_count` 是硬墙。
- 多读者并发 `get` 时 `read_files_` 是 `unordered_map<file_id, ReadHandle>`，
  多线程都需要安全查 / 改 / 删——不能拿一把全局 mutex 串行读热路径。

目标：

1. 给 `read_files_` 加上限（可配；缺省按 `RLIMIT_NOFILE` 软上限自动推导）。
2. 上限触发时淘汰**空闲**句柄（`use_count == 1`）——在途读者持
   `shared_ptr<DataFile>` 续命，与 P6 mmap 共享引用计数的同款续命模式。
3. 命中更新 LRU 序**不**抢独占锁——用单调时钟 `read_clock_` 近似 LRU，
   读热路径只拿共享锁。

## 2. 数据结构（`include/bitcask/cask.hpp`）

```cpp
struct ReadHandle {
    std::shared_ptr<fileops::DataFile> df;
    mutable std::atomic<std::uint64_t> atime{0};
    ReadHandle(std::shared_ptr<fileops::DataFile> d, std::uint64_t a)
        : df(std::move(d)), atime(a) {}
};

mutable std::shared_mutex            read_cache_mu_;
std::unordered_map<std::uint32_t, ReadHandle> read_files_;
std::atomic<std::uint64_t>           read_clock_{0};   // 单调访问计数
```

- `ReadHandle` 持 `shared_ptr<DataFile>`：淘汰时仅去掉 map 里的引用，
  在途读者仍持 `shared_ptr` → `DataFile` 不析构 → fd / mmap 都不撤。
- `atime` 是 `std::atomic<uint64_t>`：命中在**共享锁**下 store 即可
  （不改 map 结构，atomic store 自带线程安全）。
- `read_clock_` 单调递增：每次命中 `fetch_add(1, relaxed)`，新 atime =
  当前值。无锁即可写。

## 3. 上限配置（`CaskOptions`）

```cpp
struct CaskOptions {
    // 0（默认）→ 自动：RLIMIT_NOFILE 推导安全上限（约一半，下限 64）
    // kUnlimitedReadHandles → 不限（旧默认行为，最大吞吐、无淘汰 churn）
    // 其它 N → 显式上限
    static constexpr std::size_t kUnlimitedReadHandles =
        static_cast<std::size_t>(-1);
    std::size_t max_read_handles = 0;
    // ...
};
```

| 取值 | 含义 |
|------|------|
| `0`（默认） | 自动推导：安全上限 = `max(nofile_soft / 2, 64)` |
| `kUnlimitedReadHandles`（即 `(size_t)-1`） | 不限（旧默认行为） |
| 其它 `N` | 显式上限 = `N` |

`Cask::resolve_read_handle_cap` 是纯函数，签名：

```cpp
[[nodiscard]] static std::size_t
resolve_read_handle_cap(std::size_t opt, std::size_t nofile_soft) noexcept;
```

实现（`src/cask/cask.cpp`）：

```cpp
if (opt == CaskOptions::kUnlimitedReadHandles) return 0;   // 显式不限
if (opt != 0) return opt;                                  // 显式上限
const std::size_t derived = nofile_soft / 2;
return derived < 64 ? 64 : derived;                        // 自动 + 下限
```

- `kUnlimitedReadHandles` 在 evict 语义里翻译为 `0`（不限），与「自动
  模式」同为 `0`——两路靠 `opts_` 记录 `max_read_handles == 0` 区分
  「不淘汰」（`cap == 0` 直接 return）。
- `nofile_soft` 的来源见 §4。

## 4. 自动上限推导：`RLIMIT_NOFILE`

`Cask::open` 在装配 `opts_` 后立即调用 `resolve_read_handle_cap`：

```cpp
{
    std::size_t nofile = 1024;  // getrlimit 失败 / RLIM_INFINITY 的保守兜底
    struct ::rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        nofile = static_cast<std::size_t>(rl.rlim_cur);
    }
    cask->opts_.max_read_handles =
        resolve_read_handle_cap(opts.max_read_handles, nofile);
}
```

- `getrlimit(RLIMIT_NOFILE, &rl)` 失败（极少见）或 `rlim_cur == RLIM_INFINITY`
  → 兜底 `1024`（与「保守」相称）。
- 「约一半」是给 active writer / WAL / hint / meta / lock 等非缓存 fd 留
  余量；下限 64 防极低 ulimit 下 cap 太小导致频繁淘汰 churn。

## 5. 命中 / 淘汰路径

### 5.1 命中（`Cask::read_file`）

```cpp
{
    std::shared_lock lk(read_cache_mu_);
    auto it = read_files_.find(file_id);
    if (it != read_files_.end()) {
        // 命中：共享锁下置 atime（原子 store）
        it->second.atime.store(
            read_clock_.fetch_add(1, std::memory_order_relaxed),
            std::memory_order_relaxed);
        return it->second.df;
    }
    if (active_data_ && file_id == active_file_id_) {
        return active_data_;
    }
}
```

- 命中路径只持**共享锁**——多读者并发 `get` 同一个 `file_id` 互不阻塞。
- `atime` store + `read_clock_` fetch_add 都在共享锁下；不改 map 结构，
  原子操作自带线程安全。
- `active_data_` 走旁路（不在 `read_files_`），不受淘汰影响（active
  永远 pread，不 mmap——见 `sealed-mmap-read-design-zh.md` §3）。

### 5.2 miss → lazy open（独占锁）

```cpp
std::unique_lock lk(read_cache_mu_);
auto it = read_files_.find(file_id);
if (it != read_files_.end()) return it->second.df;       // 双检

if (active_data_ && file_id == active_file_id_) {
    return active_data_;
}

auto path = fileops::mk_data_filename(dirname_, file_id);
auto df = fileops::DataFile::open(path, fileops::DataFile::Mode::kRead);
if (!df) return nullptr;
auto sp = std::make_shared<fileops::DataFile>(std::move(*df));
read_files_.try_emplace(
    file_id, sp, read_clock_.fetch_add(1, std::memory_order_relaxed));
evict_read_handles_locked();
return sp;
```

- 共享锁 → 独占锁之间可能有其它读者已 open；双检避免重复建。
- `DataFile::open(kRead)` 走 sealed mmap 路径（见 `sealed-mmap-read-design-zh.md`），
  返回的 `shared_ptr<DataFile>` 锚定 fd + mmap。
- 刚插入的 `sp` 在 caller 返回前仍由局部变量持有（`use_count == 2`），
  淘汰会跳过它（只淘空闲）——避免自淘汰。

### 5.3 淘汰（`Cask::evict_read_handles_locked`）

```cpp
void Cask::evict_read_handles_locked() {
    const std::size_t cap = opts_.max_read_handles;
    if (cap == 0 || read_files_.size() <= cap) return;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> idle;  // {atime, file_id}
    idle.reserve(read_files_.size());
    for (auto& [fid, h] : read_files_) {
        if (h.df.use_count() == 1) {
            idle.emplace_back(h.atime.load(std::memory_order_relaxed), fid);
        }
    }
    const std::size_t over = read_files_.size() - cap;
    if (idle.size() <= over) {
        for (auto& [at, fid] : idle) read_files_.erase(fid);
        return;
    }
    std::partial_sort(idle.begin(), idle.begin() + static_cast<std::ptrdiff_t>(over),
                      idle.end());
    for (std::size_t i = 0; i < over; ++i) read_files_.erase(idle[i].second);
}
```

- 前置条件：caller 已持 `read_cache_mu_` **独占**锁（见 `Cask.hpp` 注释）。
- `cap == 0` 表示「不限」（由 `kUnlimitedReadHandles` 翻译而来）→ 早退。
- 只淘 `df.use_count() == 1` 的**空闲**句柄（仅 map 持有）。在途读者
  （`use_count > 1`）跳过——它们的 fd 正在被 pread，erase 也不能立即
  释放，留到下次；故 `cap` 是软上限（可能短暂超过 `cap`）。
- `partial_sort` 排序最旧的 `over` 个，`over = size - cap`。

## 6. 状态机

```
                            ┌───────────────┐
              get(file_id)  │ read_files_   │ 命中 → 更新 atime (shared lock)
       ────────────────────▶│ (LRU 缓存)    │ miss  → DataFile::open (exclusive)
                            └───────────────┘              │
                                   │                       ▼
                                   │              ┌────────────────┐
                                   │              │ try_emplace    │
                                   │              │ (use_count==2) │
                                   │              └────────────────┘
                                   │                       │
                                   │                       ▼
                                   │              ┌────────────────┐
                                   │              │ evict_idle()   │
                                   │              │ (use_count==1) │
                                   │              └────────────────┘
                                   ▼
                          ┌───────────────┐
                          │ merge unlink  │ erase entry from read_files_
                          │ (S13-F1 收尾) │ 在途 shared_ptr 续命 → 析构时
                          └───────────────┘ close fd / munmap
```

| 触发 | 路径 | 锁 |
|------|------|-----|
| `get` 命中 | `read_file` 共享锁下 store atime | shared |
| `get` miss | `read_file` 升级独占 → lazy open → evict | exclusive |
| merge unlink | `read_files_` 擦除该 file_id | exclusive |
| `close` | `read_files_.clear()` | exclusive |
| 内部 `info().read_handles` | 共享锁下读 `read_files_.size()` | shared |

## 7. 并发与边界

- **不维护真正的 LRU list**：命中在共享锁下置 `atime`，淘汰在独占锁下
  扫描 `atime` 最旧者。读热路径不抢独占锁（方案 (b)）。方案 (a)
  「命中走独占锁」未采用——会退化成读串行。
- `active_data_` 不在 `read_files_`——active writer 句柄独立成员，不受
  淘汰影响。
- `CaskIter::pin_files()` 在 fold 启动时独立 pin 一份「目录下全部
  data file」只读句柄到 `pinned_files_`（`include/bitcask/cask.hpp`），
  不进 `read_files_`——淘汰不影响在跑的 fold。
- merge unlink 的 erase 与 LRU 淘汰并存：都仅去 map 引用，依赖
  `shared_ptr<DataFile>` refcount 续命 fd + mmap。
- `CaskOptions::kUnlimitedReadHandles`（`opt = (size_t)-1`）在
  `resolve_read_handle_cap` 内映射为 `cap = 0`；`evict_read_handles_locked`
  早退——行为与现状（旧默认）一致。

## 8. 内省

```cpp
[[nodiscard]] std::size_t read_handle_count() const {
    std::shared_lock lk(read_cache_mu_);
    return read_files_.size();
}
```

- `StatusInfo::read_handles`（`cask.hpp`）经此接口填充，用于外部观测 /
  断言 fd + mmap 总数。
- 返回值是**当前 map 大小**——实际 fd + mmap 数 = `read_handle_count()`
  + 在途 / 缓外的引用（use_count > 1 的句柄未计入），是一个下界。

## 9. 风险

- **淘汰刚要复用的句柄** → 重新 `open` 的代价（mmap + madvise）。cap
  按工作集调：默认推导的「约一半 ulimit」在大库（多文件）下偏紧，
  小库（少文件）下偏松。
- **在途读者续命**：`use_count > 1` 时不淘汰，可能短暂超过 `cap`——
  cap 是软上限。对大库 / 高并发场景 OK，evict 是 best-effort 控上限。
- **`atomic<atime>` store 开销**：共享锁下每次命中一次 relaxed store
  + `read_clock_` fetch_add。在 Mixed 基准热路径内可观测但非瓶颈
  （256 分片 KeyDir 是主开销源）。

## 10. 测试

- **上限生效**：打开 `> cap` 个 file 并各读一次 → 断言
  `read_handle_count() ≤ cap`（含弱上限：in-flight 期间可能短暂超出）。
- **淘汰正确性**：淘汰后再读该 file → 返回正确值（重新 `DataFile::open` +
  mmap + pread，CRC 校验通过）。
- **共享指针续命**：一个线程持 `shared_ptr<DataFile>` 读，另一线程触发
  淘汰该 file_id → 读线程照常读完、最后引用析构才真正 close fd / munmap。
  （同 O10 UAF 修复 / merge unlink 续命模式。）
- **`kUnlimitedReadHandles`**：opt = `kUnlimitedReadHandles` → cap = 0 →
  `evict_read_handles_locked` 早退，行为同不限。
- **RLIMIT_NOFILE 兜底**：mock getrlimit 失败 / `RLIM_INFINITY` →
  兜底 `nofile = 1024` → cap = `max(512, 64) = 512`。

