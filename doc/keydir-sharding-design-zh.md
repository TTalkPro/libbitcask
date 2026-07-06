# KeyDir 分片与 MVCC fold 设计

> KeyDir 是 bitcask 整个架构的核心：`get` 走 KeyDir 查 `(file_id, offset)`
> 后单次 `pread` 读值，`put` 改 KeyDir + 追加 data file。本设计在
> `include/bitcask/keydir.hpp` 与 `src/keydir/keydir.cpp` 中落地。

## 1. 整体目标

- **多读真并发**：`get` / `search_*` / `iter.next` 走各自分片锁，不抢全局锁。
- **写串行到分片粒度**：不同 key 的 `put` / `remove` 落到不同分片，互不阻塞。
- **fold（迭代）一致快照**：并发写继续进行，迭代器看到稳定的 epoch 视图。
- **无全局热路径锁字**：写路径上分片锁 + 一组 relaxed 原子即够，cache-line
  ping-pong 收敛到分片级。
- **可水平扩展到多 Cask**：进程内多 Cask（同/异目录）经 `KeyDirRegistry`
  共享 KeyDir；多 KeyDir 互不干扰。

## 2. 内存布局（`include/bitcask/keydir.hpp`）

```cpp
class KeyDir {
    static constexpr std::size_t kShards = 256;          // 2^n，低位 hash 路由
    struct alignas(64) Shard {
        mutable std::mutex mu;                            // S5：rwlock → mutex
        alignas(64) ankerl::unordered_dense::map<std::string, Entry,
                                                 StringHash,
                                                 std::equal_to<>> entries;
    };
    mutable std::array<Shard, kShards> shards_;

    mutable std::shared_mutex meta_mu_;                   // pending_/iter 协调

    // 屏障 v2：写者闸门（替代旧 "同时持全部分片锁" 方案）
    std::atomic<bool>       barrier_active_{false};
    std::mutex              barrier_mu_;                  // 屏障间互斥
    std::mutex              gate_mu_;                     // 写者退避 cv 配套锁
    std::condition_variable gate_cv_;

    // 全局标量（写热行 / 读热行 分组隔离）
    alignas(64) std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint64_t> key_count_{0};
    std::atomic<std::uint64_t> key_bytes_{0};
    std::atomic<std::uint64_t> next_ord_{0};

    alignas(64) std::atomic<std::uint64_t> keyfolders_{0};
    std::atomic<std::uint32_t> biggest_file_id_{0};
    std::atomic<bool>          has_pending_{false};
    std::atomic<bool>          is_ready_{false};

    std::atomic<bool> iter_mutation_{false};

    // fold 期间 pending 表（meta unique 保护）
    std::optional<std::unordered_map<std::string, SingleEntry,
                                     StringHash, std::equal_to<>>> pending_;
    std::uint64_t pending_start_epoch_ = 0;
    std::uint64_t pending_start_time_  = 0;
    std::uint64_t pending_updated_     = 0;

    // fstats 槽位（无锁热路径，详见 §6）
    struct alignas(64) AtomicFStats { /* 8 个 atomic 字段 + present */ };
    mutable std::mutex fstats_grow_mu_;
    mutable std::deque<AtomicFStats> fstats_;
    std::atomic<std::size_t> fstats_size_{0};
    mutable std::atomic<AtomicFStats**> fstats_ptrs_{nullptr};
    mutable std::vector<std::unique_ptr<AtomicFStats*[]>> fstats_ptr_arrays_;
    mutable std::size_t fstats_ptr_cap_ = 0;
};
```

要点：

- `kShards = 256`（2 的幂），hash 低位 `& (kShards - 1)` 路由到分片下标。
- `Shard` `alignas(64)` 隔离 cache line：每分片锁字独占一行，`entries` 头
  独占另一行（map 头频繁读，锁字 RMW 频繁写，必须分行）。
- 写热行（`epoch_` / `next_ord_` / `key_count_` / `key_bytes_`）与读热行
  （`keyfolders_` / `biggest_file_id_` / `has_pending_` / `is_ready_`）隔离
  两条 cache line——put 的 `epoch_` RMW 不会打飞读者需要的行。
- 256 分片下碰撞率足够低（生产 90% get + 10% put 实测 get-only 8t 仅
  比 1t 慢 8%），同分片并发读互斥可接受。

分片选择函数：

```cpp
[[nodiscard]] static std::size_t shard_for(std::string_view key) noexcept {
    return StringHash{}(key) & (kShards - 1);
}
```

`StringHash` 提供 `is_transparent`，故 `entries` 的 `find` 支持
`string_view` 异构查找——热路径零拷贝。

## 3. 256 分片并发形态

```
               ┌─────────────────────────────────────┐
   get(k1) ──▶ │ shard_for(k1)=0x0a  →  shards_[0x0a] │ ─▶ Shard::mu shared
   get(k2) ──▶ │ shard_for(k2)=0x7c  →  shards_[0x7c] │ ─▶ Shard::mu shared
   get(k3) ──▶ │ shard_for(k3)=0x0a  →  shards_[0x0a] │ ─▶ Shard::mu shared（同分片互斥）
   put(k1) ──▶ │ shard_for(k1)=0x0a  →  shards_[0x0a] │ ─▶ Shard::mu unique
   put(k4) ──▶ │ shard_for(k4)=0xff  →  shards_[0xff] │ ─▶ Shard::mu unique
               └─────────────────────────────────────┘
                                 │
                                 ▼
            epoch_ / key_count_ / key_bytes_ / biggest_file_id_
                —— relaxed atomic，无锁字
```

- **读**（`get` / `next` / `conditional_remove` 探测 / `info`）：分片锁 shared
  + 探测 `keyfolders_` 决定是否嵌套 meta shared。
- **写**（`put` / `remove`）：分片锁 unique + 探测 `has_pending_` 决定是否
  嵌套 meta unique。
- **fold**（`start` / `release` / `save_snapshot`）：屏障 v2 写者闸门
  屏障（见 §4）。
- 跨分片推进 `biggest_file_id_`：`compare_exchange_weak` CAS-max loop，
  不持全局锁。

## 4. 屏障 v2：写者闸门（`src/keydir/keydir.cpp` 的 `BarrierGuard`）

`start` / `release` / `save_snapshot` 是「写时复制 + 延迟合并」的写路径。
要拿到 keydir 的稳定快照，必须先让在途写者出清。**v1 方案**是同时持
全部分片锁（kShards=256 后即 257 把），撞上 compiler-rt
`sanitizer_deadlock_detector.h` 的 64 持锁硬上限，TSan
`detect_deadlocks=1` 下必崩——已删除。

**v2 方案**是写者闸门：持闸门期间任意瞬间至多持 1 把分片锁。

### 4.1 协议

```cpp
class BarrierGuard {
    KeyDir& kd;
public:
    explicit BarrierGuard(const KeyDir& k) : kd(const_cast<KeyDir&>(k)) {
        kd.barrier_mu_.lock();
        {
            std::lock_guard<std::mutex> g(kd.gate_mu_);
            kd.barrier_active_.store(true, std::memory_order_release);
        }
        // 排干：逐分片「加锁-放锁」——任意瞬间只持 1 把分片锁
        for (auto& sh : kd.shards_) { sh.mu.lock(); sh.mu.unlock(); }
    }
    ~BarrierGuard() {
        {
            std::lock_guard<std::mutex> g(kd.gate_mu_);
            kd.barrier_active_.store(false, std::memory_order_release);
        }
        kd.gate_cv_.notify_all();
        kd.barrier_mu_.unlock();
    }
};
```

- **ctor**：`barrier_mu_`（屏障间互斥）→ `gate_mu_` 内置 `barrier_active_=true`
  → 逐分片「加锁-放锁」排干在途写者。
- **dtor**：`gate_mu_` 内置 `barrier_active_=false` → `notify_all` → 放
  `barrier_mu_`。
- **写者侧**（`put` / `remove`）：拿到分片锁后立即 `barrier_active_(acquire)`；
  若 active 则**先放分片锁**再到 `gate_cv_` 等待，唤醒后重拿分片锁循环重查。
  ```cpp
  // put_probe / remove 里的闸门检查
  while (barrier_active_.load(std::memory_order_acquire)) {
      ctx.slock.unlock();
      { std::unique_lock<std::mutex> g(gate_mu_);
        gate_cv_.wait(g, [&] { return !barrier_active_.load(std::memory_order_acquire); }); }
      ctx.slock.lock();
  }
  ```
- **读者**（`get` / `next` / `conditional_remove` 探测 / `info`）**不**检查
  闸门——屏障期间照常并发。
- **`conditional_remove` 探测**也**不**检查闸门（只读 peek），写阶段走
  `remove()` 自带闸门检查。

排干循环的 HB 论证：

- 闸门读到 `inactive` 的写者要么 (a) 先于排干持有分片锁（排干在该分片阻塞，
  等写者整段临界区出清）；要么 (b) 晚于排干拿锁（经该分片 mutex 的
  unlock/lock 配对必见 `barrier_active_=true` 而放锁退避）。
- 所以「读到 inactive → 直写」的写一定整体先于屏障。

### 4.2 三阶段 release 协议

`IterHandle::release()` 最后一个 folder 时执行：

1. **阶段一** [`meta_mu_` unique]：`keyfolders_--`。非最后一个 folder
   → 直接返回（屏障 RAII 析构）。
2. **阶段二** [`meta_mu_` shared]：遍历 `pending_` 每条，嵌套拿该 key
   分片锁应用进 `entries`（`apply_pending_to_entries_barrier`）。
3. **阶段三** [`meta_mu_` unique，不持任何分片锁]：`pending_.reset()` 、
   `has_pending_=false`、推进 `iter_generation_`、清 `iter_mutation_`。
4. **MultiEntry 折叠**：逐分片「lock → 折叠 → unlock」
   （`collapse_multi_entries_barrier`）。

「先应用（阶段二）后清表（阶段三）」保证：读者在窗口内要么 `entries`
命中（探测顺序 entries 优先）要么 `pending_` 命中，无丢失窗口。

### 4.3 锁全序

```
barrier_mu_ → gate_mu_ → meta_mu_ → 单个 shard（任意时刻 ≤ 1 把）
              → fstats_grow_mu_
```

两处反向嵌套例外（均有无环论证）：

- **热路径**（`get` / `put` / `remove`）：持单个分片锁后嵌套 `meta_mu_`
  （shard→meta，堵 release 合并窗口 TOCTOU）。
- **屏障内例外**（阶段二）：`meta_mu_` shared 持有期间嵌套分片锁
  （meta→shard）。合法性：屏障内写者已出清、无 `meta_mu_` unique 持有
  / 等待者；读者对 `meta_mu_` 只拿 shared，shared-shared 相容且无 unique
  排队者——无环。

## 5. Entry 变体与 MVCC fold 协议

### 5.1 Entry variant

```cpp
struct SingleEntry {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t epoch    = 0;   // 分片锁内 fetch_add 的全局递增
    std::uint32_t tstamp   = 0;
    std::uint64_t ord      = 0;   // 全局单调写序号
};

struct MultiEntry { std::vector<SingleEntry> revisions; };
using Entry = std::variant<SingleEntry, MultiEntry>;
```

- `Entry` variant 避免每个 key 都背一个 vector 开销——大多数 key 命中
  `SingleEntry` 分支。
- `MultiEntry.revisions` 是 **newest-first** 顺序：链头是最新写入的 revision。
- 三个 sentinel 常量（`kMaxFileId` / `kMaxSize` / `kMaxOffset`）同时取 MAX
  是 sibling-tombstone 的判别约定（`is_sibling_tombstone`），沿用以保证
  跨实现互通。
- pending 表内墓碑只用 `offset == kMaxOffset` 一个字段判别
  （`is_pending_tombstone`），`file_id` / `total_sz` 从真实 entry 继承。

查询返回的视图：

```cpp
struct EntryProxy {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t epoch    = 0;
    std::uint32_t tstamp   = 0;
    std::uint64_t ord      = 0;
    bool          is_tombstone = false;
    std::string_view key;            // zero-copy view，仅持锁期间有效
};
```

### 5.2 fold 协议：sibling chain + pending

- `keyfolders_ > 0`（有 fold 在跑）时，已存在 key 的 `put` 覆写 / `remove`
  墓碑**不**直接覆盖（会破坏迭代器看到的快照），而是把旧 `SingleEntry`
  升级成 `MultiEntry`，新 revision 插到链头——迭代器仍然看到自己 epoch
  的那个 revision，新写入对它不可见。
- 全新 key 走独立的 `pending_` map（`meta_mu_` unique 保护），迭代器看不到。

**核心不变量**（探测顺序 entries 优先的正确性依据）：

> `key ∈ 某分片 entries` ⟹ `pending_` 不会有它的更新版本。

已存在 key 的新版本一律在分片内走 sibling 链，绝不进 pending。`get` /
`put` / `remove` 的「entries 优先、miss 再查 pending」探测顺序依赖该不变量。

### 5.3 探测顺序

| 路径 | entries 命中 | entries miss |
|------|--------------|--------------|
| `get` | shard shared → 返回 | miss 且 fold 态时嵌套 meta shared 查 pending |
| `put`（newest_put=true） | 升级 MultiEntry 插链头 | entries 无 + pending 已有 → 写 pending；否则 fold 态写 pending，否则直插 entries |
| `put`（newest_put=false / 条件 put） | CAS `(old_file_id, old_offset)` 匹配则升级链 / 否则返回 `kAlreadyExists` | 同左（折成「直接覆盖」语义） |
| `remove` | fold 态升 sibling 链插墓碑；非 fold 态 erase | fold 态进 pending 改墓碑；非 fold 态 no-op |

### 5.4 探测为什么「保持分片锁不放 + 嵌套 meta」

`get` / `put_probe` / `remove` 在分片锁内探测时**不**释放分片锁再查
`pending_`，而是在分片锁持有期间嵌套 `meta_mu_`（shared / unique 按
场景）——这是为了堵 release 合并窗口的 TOCTOU：

- 屏障 v2 下，阶段二（`apply_pending_to_entries_barrier`）把某 key
  应用进 entries 必须拿该 key 的分片锁——若本读者已持有该锁，阶段二
  必须等读者释放，期间 `pending_` 表清不掉（阶段三排在阶段二全部
  应用完成之后）。
- 「先应用后清表」⟹ 持本分片锁期间该分片 key 要么已在 entries 命中、
  要么仍留在 pending 可见——无丢失窗口。

### 5.5 `IterHandle`（迭代器）

每个 `IterHandle` 在 `start()` 时锁定一个 `iter_epoch_`——之后看到的
所有 key revision 都 `epoch <= iter_epoch_`（newest-first 链从头往后
扫第一个满足的就是那一刻的可见 revision）。handle 持有指向 parent
`KeyDir` 的非拥有指针；parent 必须比 handle 活得久（实际通过
`Cask` 的 owning `shared_ptr<KeyDir>` 保证）。

关键字段：

```cpp
class IterHandle {
    KeyDir*  parent_;
    bool     iterating_           = false;
    std::uint64_t iter_epoch_     = kMaxEpoch;
    std::string   keys_buf_;              // 扁平 key 缓冲
    std::vector<std::size_t> key_offs_;   // N+1 哨兵；empty = 无快照
    std::size_t  cursor_ = 0;
};
```

- `keys_buf_` + `key_offs_`：start 时一次性拍下「entries 全部 key 列表」
  扁平副本（每 fold 恒 2 次分配，原 `vector<string>` 是 N 次 malloc）。
- `next()`：按 cursor 推进，对当前 key 取该分片锁 shared 查 `entry_at_epoch`。
  fold 期间 `entries` key 集只增不减（`remove` 走 sibling 墓碑不 erase），
  故不需要触 `pending_`。
- `release()`：三阶段协议（§4.2），仅最后一个 folder 触发合并 + 折叠。
- 析构调 `release()`，幂等；同一 handle 不可并发调用（自身字段无锁）。
- 多 handle 之间 parent 共享但各自独立，可并行 fold。

### 5.6 pending freeze 复用

`start()` 在已有 fold 跑（`pending_` 已建立）时，可复用同一份 pending
（共享 freeze），`iter_epoch_` 取最新 epoch。受两个阈值约束：

- `maxage`（秒）：pending 起始时间到现在超过则 `kOutOfDate`。
- `maxputs`（次）：pending 累积写入数超过则 `kOutOfDate`。

任一阈值禁用（`< 0`）则不参与判定。`can_use_existing_freeze` 判定失败
时直接返回 `kOutOfDate`，caller 等待 pending 排空再重试。

## 6. fstats：无锁热路径（`KeyDir::update_fstats`）

文件级统计：每个 `file_id` 一条 `AtomicFStats`，merge 触发判断 + status
都靠它。put / remove 时增量更新。

- `std::deque<AtomicFStats> fstats_`（元素地址稳定）+ `std::atomic<size_t> fstats_size_`。
- `AtomicFStats` 8 个 `std::atomic<...>` 字段（`live_keys` / `total_keys`
  / `live_bytes` / `total_bytes` / `oldest_tstamp` / `newest_tstamp` /
  `expiration_epoch` / `present`），整条结构 `alignas(64)` 独占 cache line
  （merge + active 并发写不同文件时防假共享）。
- 读 / 累加：`idx < fstats_size_.load(acquire)` ⟹ 槽位已构造完毕，直接
  对字段做 `fetch_add(relaxed)`；`oldest` / `newest` / `expiration_epoch`
  用 `compare_exchange_weak` CAS-min/max 循环。
- 增长（新 file_id，罕见）：`fstats_grow_mu_` 串行构造新槽，发布 size。

### 6.1 RCU 指针表

S13-F8 修复：`std::deque`「元素地址稳定」不等于「`operator[]` 并发安全」
——`deque::operator[]` 要遍历内部块指针表（map），`emplace_back` 触发的
map 重分配会释放旧表，无锁读者 UAF。修复：

- `deque` 仍是元素所有者（地址稳定）。
- 旁挂 RCU 指针表 `fstats_ptrs_`（`std::atomic<AtomicFStats**>`）+ 退休
  表 `fstats_ptr_arrays_`（`std::vector<std::unique_ptr<AtomicFStats*[]>>`）。
- 扩容时在 `fstats_grow_mu_` 下建新表 → `fstats_ptrs_.store(release)` 发布
  → 旧表退休进 `fstats_ptr_arrays_`（不释放，在途读者可能仍持有；总内存
  < 2× 终表 ≈ 16B/file_id，有界）。
- 无锁读者一律经 `fstats_slot(idx)` 访问，前置条件
  `idx < fstats_size_.load(acquire)`：size 的 release 发布在指针表填充 /
  替换之后，acquire 读者必见新表。

```cpp
[[nodiscard]] AtomicFStats& fstats_slot(std::size_t idx) const {
    return *fstats_ptrs_.load(std::memory_order_acquire)[idx];
}
```

## 7. KeyDirRegistry：进程内共享

`include/bitcask/keydir_registry.hpp` 提供命名 KeyDir 注册表。同名
（一般是 bitcask 目录的绝对路径）多次 `acquire` 共享同一个底层 KeyDir，
refcount 管理生命周期；refcount 归零时把 `biggest_file_id + 1` 存进
`saved_biggest_file_id_`，保证后续重新 acquire 同名 keydir 时新分配的
file_id 不会跟历史文件冲突。

```cpp
class KeyDirRegistry {
    struct Slot {
        std::shared_ptr<KeyDir> keydir;
        std::uint32_t           refcount = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> entries_;
    std::unordered_map<std::string, std::uint32_t> saved_biggest_file_id_;
    std::unique_ptr<bitcask::IndexPool> index_pool_;  // 进程级共享索引双池
};
```

### 7.1 初始化协议

| AcquireStatus | 含义 | caller 职责 |
|---------------|------|-------------|
| `kCreated` | 全新创建；caller 是「初始化者」 | 扫盘 / 重建后必须 `kd->mark_ready()` |
| `kReady` | 已存在且就绪 | refcount 已自增，可直接使用 |
| `kNotReady` | 名字存在但尚未 `mark_ready`（或不存在） | 等待 / 重试 |

- 在 `mark_ready()` 之前，其它 `acquire` 同名 keydir 的请求会拿到
  `kNotReady`。
- `mark_ready()` 之后，所有后续 `acquire` 拿到 `kReady` 和共享指针。
- `release(name)` 必须配对调 `acquire` 时用过的 name；归零后
  把 `biggest_file_id + 1` 持久化到 `saved_biggest_file_id_`。

### 7.2 file_id 单调性

`KeyDir::increment_file_id_at_least(conditional_id)` 在 `acquire` 时把
新 KeyDir 的 `biggest_file_id_` 推到 `saved_biggest_file_id_` 之上，避免
「老 file_id 复用 → keydir 把旧 entry 误判为最新」的灾难。

### 7.3 索引双池归属

`KeyDirRegistry::index_pool()` 懒创建共享 `IndexPool`（所有同 registry
的 search 库共享这一对 Map/Reduce 线程，线程数 = `hardware_concurrency`、
下限 2）。registry 析构时停池 join 线程。

## 8. 快照格式（`KeyDir::save_snapshot` / `load_snapshot`）

`BCKS` v2 快照（`src/keydir/keydir.cpp` `kSnapMagic` = `"BCKS"`）：

```
┌────────────┬──────┬──────┬──────┬──────┬──────┬────────────┬──────┬──────┬──────┐
│ magic 32B  │ ver  │ next │ epoch│ big  │ key  │ key        │ fstats_n... │ wm_n... │
│ "BCKS"     │  32B │ _ord │  64B │ 32B  │_count│ _bytes     │ (per file)   │ (watermarks) │
│            │      │ 64B  │      │      │ 64B  │ 64B        │               │            │
└────────────┴──────┴──────┴──────┴──────┴──────┴────────────┴──────┴──────┴──────┘
                                                                     │
                                                                     ▼
                                                              entries: N 条 vbyte 变长
                                                              (klen + key + file_id_vb +
                                                               total_sz_vb + offset_vb +
                                                               epoch_vb + tstamp_32B + ord_vb)
                                                              + crc32 32B
```

- 头：`magic` (32B) + `ver` (32B)。
- 5 个标量：`next_ord` / `epoch` / `biggest_file_id` / `key_count` /
  `key_bytes`（各 64B / 32B）。
- `fstats`：长度 + 逐条 `(file_id, live_keys, total_keys, live_bytes,
  total_bytes, oldest_tstamp, newest_tstamp, expiration_epoch)`，每条
  52B。
- `watermarks`：长度 + 逐条 `(file_id, offset)`，用于 fold 跳过已覆盖
  字节。
- `entries`：长度 + 逐条 vbyte 变长编码
  （`klen_vb + key_bytes + file_id_vb + total_sz_vb + offset_vb + epoch_vb +
  tstamp_32B + ord_vb`），典型 15-20B/条（GB 级 keydir 快照 I/O 近半）。
- 尾部：`crc32(payload)` (32B)。

- 写：tmp + rename 模式；rename 前 `fdatasync` 防断电丢快照页。
- 读：v1 / v2 双分支（`kSnapVersion = 2`、兼容 `kSnapVersionV1 = 1`）；
  CRC 不通过 / 越界 / 解析失败 → 整体 `reset_all()` 返回 `nullopt`，
  caller 走全量 fold。

`apply_meta_delta`（`BKMD` v1，per-component checkpoint 内联）：

- 标量取 max（`advance_ord` / `epoch_` CAS / `increment_file_id_at_least`）。
- fstats 绝对覆盖（不 relative，不 merge）。
- 返回解析出的 watermarks，caller 拿它驱动 fold 起点。

## 9. 关键不变量

1. **分片锁任意时刻至多 1 把**（屏障 v2 排干循环保证）。
2. **`epoch_` 在分片锁内 `fetch_add`**：保证
   `entry.epoch < iter_epoch ⟺ 屏障前完成` 判据。
3. **`key ∈ entries ⟹ pending_` 不会有其更新版本**（探测顺序正确性
   依据）。
4. **`keyfolders_` 只在 `BarrierGuard` + `meta_mu_` unique 内修改**；
   写者闸门检查通过且持分片锁后 relaxed 读即足够新（§4.1 时序论证）。
5. **`fstats` 槽位发布后**才允许无锁读（`fstats_size_.load(acquire)` 门
   禁 + RCU 指针表）。
6. **`pending_` 内存生命 = `meta_mu_` 持有期间**（每次 `start` 视复用
   情况 `emplace`；`release` 阶段三 `reset`）。
7. **`saved_biggest_file_id_` 单调不减**（`release` 时取 `max`）。

## 10. 公开 API 速查

| 方法 | 锁 | 说明 |
|------|-----|------|
| `put` | shard unique + 闸门检查 | fold 态新 key 嵌套 meta unique |
| `remove` | shard unique + 闸门检查 | fold 态升 sibling 链插墓碑；非 fold 态 erase |
| `conditional_remove` | shard unique（探测阶段 shared） | CAS `(tstamp, file_id, offset)` |
| `get` | shard shared（命中）；miss + fold 态嵌套 meta shared | 探测顺序 entries 优先 |
| `next`（IterHandle） | 目标 key 分片 shared | 折叠后的扁平 key snapshot 推进 cursor |
| `start`（IterHandle） | `BarrierGuard` + meta unique | 拍 key snapshot + 分配 `iter_epoch_` |
| `release`（IterHandle） | `BarrierGuard` + meta 三阶段 | 最后一个 folder 触发 pending 应用 + 折叠 |
| `save_snapshot` | `BarrierGuard` + meta unique | 活跃 fold 拒绝（`keyfolders_ != 0` → false） |
| `load_snapshot` | `BarrierGuard` + meta unique | open 期单线程；CRC 失败 `reset_all()` + nullopt |
| `update_fstats` | 无锁热路径（必要时 `fstats_grow_mu_`） | 锁全序最末 |
| `info` | meta shared（iter 状态）+ 无锁（计数） | 读多写少，近似一致 |
| `increment_file_id` | 无锁（`fetch_add`） | 单调递增是 keydir 核心不变量 |
| `increment_file_id_at_least` | 无锁（CAS-max loop） | 重新 acquire / 异常恢复追平 |
| `alloc_ord` / `advance_ord` | 无锁（`fetch_add` / CAS-max） | 写序号分配 |


