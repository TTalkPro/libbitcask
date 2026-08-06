// bitcask 内存索引（KeyDir）。
//
// KeyDir 是「key → (file_id, offset, total_sz, tstamp)」的全内存哈希表，
// 是 bitcask 整个架构的核心：put/delete 改 keydir + 追加 data file，
// get 走 keydir 拿 (file_id, offset) 直接 pread 一次磁盘。
//
// === 并发模型（M6-S2:分片 + 屏障 v2 写者闸门）===
//
// entries 按 key hash 低位切成 kShards 个分片，每分片一把 mutex；
// 全局标量（epoch_/key_count_/key_bytes_/biggest_file_id_/next_ord_/
// keyfolders_）全部 atomic（M6-S1/S2），fstats 走无锁发布路径（§设计
// doc/keydir-sharding-design-zh.md）。pending_/iter 协调状态由独立的
// meta_mu_ 保护（只在 fold 期间触碰，冷路径）。
//
// 锁全序（屏障 v2，必须严格遵守）：
//     barrier_mu_ → gate_mu_ → meta_mu_ → 单个 shard（任意时刻 ≤1 把）
//     → fstats_grow_mu_
// 任何路径任意时刻至多持 1 把分片锁（旧"同时持全部分片锁"方案因 TSan
// 死锁检测器 64 持锁硬上限废弃，见 keydir.cpp BarrierGuard 注释）。
// 两处与全序相反的嵌套方向（均有无环论证，详见 keydir.cpp）：
//   ① 热路径 get/put/remove 持单个分片锁后嵌套 meta（shard→meta，
//      S2 起的既有方向，堵 release 合并窗口 TOCTOU）；
//   ② iter release 阶段二在 meta shared 持有期间嵌套分片锁
//      （meta→shard，"屏障内例外"）。
// 无环论证：①②构成环要求一方持 shard 等 meta、另一方持 meta 等 shard。
// 方向②仅存在于屏障内——彼时写者（meta unique 的全部使用者）已被闸门
// 出清，仅剩读者走方向①且对 meta 只拿 shared；②也只拿 meta shared，
// shared-shared 相容且无 unique 排队者，meta 获取不可能阻塞——无环。
// 屏障外只有方向①——同样无环。
//
//   - 热路径（无 fold）：get/put/remove 单分片 mutex，
//     至多一把锁 + relaxed 原子。
//   - fold 期间：写已存在 key 在分片内升 sibling 链；新 key 经
//     meta_mu_ 进 pending_。
//   - fold 的 start/release/save_snapshot/load_snapshot 走
//     BarrierGuard 写者闸门屏障：置 barrier_active_ 后逐分片加锁-放锁
//     排干在途写者；写者拿到分片锁后检查闸门退避，**读者照常并发**。
//
// === fold（迭代）下的 sibling chain + pending hash ===
//
// keyfolders_ > 0（有 fold 在跑）时，put 命中已存在 key 时不能直接覆盖
// （会破坏迭代器看到的快照），而是把旧 SingleEntry 升级成 MultiEntry——
// 一条 newest-first 的 sibling 链。新 key 走单独的 pending_ map，迭代器
// 看不到。
// 最后一个 fold release 时：把 pending_ 合并回 entries_，把 MultiEntry
// 折叠回 SingleEntry。这种「写时复制 + 延迟合并」让 fold 看到的是稳定
// 快照，并发写还能继续——这是 bitcask 高并发读 / 一致性 fold 的核心机制。

#pragma once

#include "bitcask/string_hash.hpp"
#include "bitcask/index_ids.hpp"  // S27-1：Lsn/DocId 角色别名
#include "bitcask/seq_shard_table.hpp"  // S29-6 P3:seqlock 原生分片表

namespace bitcask::oki {
class OkiState;  // S33-4：OKI 运行态（挂 KeyDir 随 registry 共享）
}

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bitcask::keydir {

// 跟 legacy bitcask_nifs.c 完全对齐的 sentinel 值，用于「无限」/ unset。
inline constexpr std::uint64_t kMaxTime    = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t kMaxEpoch   = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t kMaxSize    = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kMaxFileId  = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint64_t kMaxOffset  = std::numeric_limits<std::uint64_t>::max();

// 一个 key 的某次「写入快照」。epoch 是写入时分配的全局递增计数，用于
// fold 期间区分新 / 旧 revision；wall-clock tstamp 在 ms 级别用于过期判定。
// ord 是写入时的全局单调递增序号，用于 tie-breaking 和有序遍历。
struct SingleEntry {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t epoch    = 0;
    std::uint64_t tstamp   = 0;
    std::uint64_t ord      = 0;  // 全局单调递增写序号
};

// 「sibling 链」：fold 期间被多次写过的同 key revision 列表。
// revisions[0] 是最新，往后越来越旧。fold release 时折回 SingleEntry。
struct MultiEntry {
    std::vector<SingleEntry> revisions;
};

// entries_ map 中存的实际类型；用 variant 避免每个 key 都背 vector 开销。
using Entry = std::variant<SingleEntry, MultiEntry>;

// S29-6 P2/P3:entries 的容器是自建 seqlock 原生开放寻址表
// （detail::SeqShardTable,见 seq_shard_table.hpp——self-describing 桶块 +
// 内置 epoch-limbo 延迟回收 + try_get_optimistic 乐观读配方）。原 ankerl
// unordered_dense 因内部 private 无法健全支撑乐观读退役(设计 §6.3,评审选 A)。

// 查询返回的「展开视图」。对应 legacy 的 bitcask_keydir_entry_proxy 结构。
// key 字段是 zero-copy view——指向 KeyDir 内部存储，仅在持锁期间有效；
// 调用方要保留必须自己 copy。
struct EntryProxy {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t epoch    = 0;
    std::uint64_t tstamp   = 0;
    std::uint64_t ord      = 0;  // 全局单调递增写序号
    bool is_tombstone      = false;
    std::string_view key;
};

// 文件级统计：每个 file_id 一条。merge 触发判断 + status 都靠这个。
struct FStatsEntry {
    std::uint32_t file_id          = 0;
    std::uint64_t live_keys        = 0;  // 还活着的 key 数
    std::uint64_t total_keys       = 0;  // 历史写入的 key 数
    std::uint64_t live_bytes       = 0;
    std::uint64_t total_bytes      = 0;
    std::uint64_t oldest_tstamp    = 0;
    std::uint64_t newest_tstamp    = 0;
    std::uint64_t expiration_epoch = kMaxEpoch;  // pending delete 截止 epoch
};

enum class PutResult { kOk, kAlreadyExists };

enum class StartIterResult {
    kOk,                  // 迭代成功开始；caller 必须配套调 release()
    kAlreadyIterating,    // 这个 handle 已经在迭代了
    kOutOfDate,           // pending 已过期；caller 应重试或等待
};

struct IterInfo {
    std::uint64_t iter_generation = 0;  // 累计 fold 启动次数
    std::uint64_t keyfolders      = 0;  // 当前 fold 数
    bool frozen                   = false;
    std::optional<std::uint64_t> pending_start_epoch;
};

struct KeyDirInfo {
    std::uint64_t key_count = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t epoch     = 0;
    IterInfo iter_info;
    std::vector<FStatsEntry> fstats;
};

class KeyDir;

// 单个 fold 的迭代句柄。
//
// 同一个 KeyDir 可以同时挂多个 IterHandle（并发 fold）。每个 handle 在
// start 时通过 iter_epoch_ 锁定一个 keydir 快照——之后看到的所有 key
// revision 都不晚于这个 epoch。handle 持有指向 parent 的非拥有指针；
// parent 必须比 handle 活得久（实际通过 cask 的 owning shared_ptr<KeyDir> 保证）。
//
// === 线程模型 ===
//   - 单 handle 内：start / release 自行对 parent 做写者闸门屏障
//     （BarrierGuard + meta_mu_），next 只拿目标 key 的分片锁；handle
//     自身字段（iterating_/iter_epoch_/keys_buf_/key_offs_/cursor_）
//     不受任何锁保护——caller 必须保证「不要在多线程同时调用同一个
//     IterHandle 的方法」。
//   - 多 handle 之间：parent 共享但每个 handle 独立；可并行 fold。
//   - 析构调 release()——若上层有别的线程仍在 next()，会出现数据竞争。
class IterHandle {
public:
    // 定义在 .cpp（S36-4：cold_ 的 unique_ptr 需要 ColdIter 完整类型）。
    explicit IterHandle(KeyDir* parent) noexcept;
    ~IterHandle() noexcept;

    IterHandle(const IterHandle&) = delete;
    IterHandle& operator=(const IterHandle&) = delete;
    IterHandle(IterHandle&&) = delete;
    IterHandle& operator=(IterHandle&&) = delete;

    // 开始迭代。
    //   now_sec  — 当前 wall-clock 秒，用于 freshness 判断
    //   maxage   — 允许 frozen pending 表的最大年龄（秒），负数禁用该限制
    //   maxputs  — freeze 后允许的最大写入次数，负数禁用
    // 线程安全: 否（修改 handle 自身字段）；同一 handle 不可并发调用。
    // 锁: 内部对 parent_ 做写者闸门屏障（BarrierGuard + meta unique）。
    // caller 不要持有任何 keydir 锁。
    StartIterResult start(std::uint64_t now_sec, int maxage, int maxputs);

    // 取下一项。默认跳过墓碑（legacy fold 语义）；include_tombstones=true
    // 时墓碑也作为 EntryProxy 返回（is_tombstone=true 字段）——给 fold/6
    // 的 SeeTombstones 路径用。
    // 线程安全: 否（推进 cursor_）；同一 handle 不可并发调用。
    // 锁: 内部对目标 key 的分片取 shared_lock（读）。caller 不要持锁。
    std::optional<EntryProxy> next(bool include_tombstones = false);

    // 释放迭代；幂等。如果是最后一个 folder，触发 parent 把 pending_
    // 合并回 entries_ 并折叠 MultiEntry。
    // 线程安全: 否；幂等但同一 handle 上不可与 start/next 并发。
    // 锁: 内部对 parent_ 做写者闸门屏障（BarrierGuard），meta_mu_ 分
    // 三阶段持有（见 keydir.cpp 实现注释）。
    void release();

    [[nodiscard]] bool is_iterating() const noexcept { return iterating_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return iter_epoch_; }

    // S36-4：Level B 冷枚举中途 run 读失败（IterHandle 的 next 无错误
    // 通道——nullopt 后由 caller 查此位区分「到尾」与「截断」）。
    [[nodiscard]] bool cold_error() const noexcept { return cold_error_; }

private:
    friend class KeyDir;

    // S36-4：Level B 冷枚举状态（OKI 三元组：runs 游标 + delta 拷贝；
    // 定义在 keydir.cpp——本头不引入 oki_state.hpp）。
    struct ColdIter;
    std::optional<EntryProxy> next_cold(bool include_tombstones);

    KeyDir* parent_;
    bool iterating_           = false;
    bool cold_error_          = false;
    std::uint64_t iter_epoch_ = kMaxEpoch;
    std::unique_ptr<ColdIter> cold_;  // 非空 = Level B 枚举路径

    // 迭代位置用 key copy 来表示，比 legacy 的 bucket index 多一点拷贝
    // 开销，但对 rehash 完全免疫。pin unordered_map 迭代器要求严格控
    // 制 load factor——M5 不愿意多花精力在那里。
    // S24-M7：扁平化——单一拼接缓冲 + N+1 偏移哨兵（原 vector<string>
    // 百万 key = N 次 malloc + 每 key 32B 头；现每 fold 恒 2 次分配，
    // 峰值内存约减半）。切片经 string_view 消费（entries 透明查找）。
    std::string keys_buf_;
    std::vector<std::size_t> key_offs_;  // N+1 哨兵；empty = 无快照
    std::size_t cursor_ = 0;
};

// === KeyDir 类的线程模型（统一）===
// 所有 public 方法均「线程安全 / 可重入」，内部按需获取分片锁 /
// meta_mu_（锁序见文件头：shards 下标升序 → meta_mu_ → fstats_grow_mu_）。
// caller 永远不应该在外部预先持有 keydir 的任何锁。
// 把多次调用组合成原子操作不支持——例如「get 再 put」不是原子的，需要
// 上层自行控制；M5 阶段的 cask 利用「单 Erlang 进程一个 Cask」回避了
// 这个需求。
//
// 私有的 *_barrier 后缀方法要求 caller 已持 BarrierGuard 写者闸门屏障；
// 详见每个方法附近的注释。
class KeyDir {
public:
    KeyDir();   // S33-4：构造 OkiState（定义在 .cpp——oki_state.hpp 不进本头）
    ~KeyDir();

    KeyDir(const KeyDir&) = delete;
    KeyDir& operator=(const KeyDir&) = delete;

    // ---- 写操作 ----

    // 写入或更新 key。
    //   newest_put：true 表示「无条件写」（put 流程）；
    //               false 表示「条件写」（用 old_file_id/old_offset 做 CAS，
    //               值不匹配返回 kAlreadyExists——给 merge 用）。
    //   ord：写入的全局单调递增序号，用于 tie-breaking 和有序遍历。
    // 线程安全: 是。锁: key 分片 unique;fold 态新 key 嵌套 meta unique。
    // 可重入: 否（递归会死锁）。
    PutResult put(std::string_view key,
                  std::uint32_t file_id, std::uint32_t total_sz,
                  std::uint64_t offset, std::uint64_t tstamp,
                  std::uint64_t now_sec,
                  bool newest_put,
                  std::uint32_t old_file_id, std::uint64_t old_offset,
                  std::uint64_t ord = 0);

    // 无条件删除。返回 true 表示原本有这条 key。
    //
    // S33（并行恢复 remove/put 到达序无关，见 put_insert 的复活门）：
    //   ord：墓碑的全局写序号。非 0 时记入墓碑 sentinel——之后
    //     newest_put=false 的 put 命中该墓碑，仅当 put.ord > 墓碑 ord 才
    //     允许复活（否则 kAlreadyExists）。ord=0（运行期 remove / 链重放）
    //     维持旧语义（无门禁）。
    //   insert_tombstone_if_absent：key 不存在时也插入墓碑 sentinel（仅
    //     非 fold 态生效）。并行恢复中「墓碑先于其 put 到达」的必要标记；
    //     sentinel 由 S29-6 P1 的墓碑清扫机制回收。
    // 线程安全: 是。锁: key 分片 unique;fold 态按需嵌套 meta unique。
    bool remove(std::string_view key, std::uint64_t remove_time,
                std::uint64_t ord = 0,
                bool insert_tombstone_if_absent = false);

    // 条件删除（CAS）：只有 (tstamp, file_id, offset) 匹配当前 entry
    // 才删。给 merge 跟 cask put 路径之间的 race 防护用。
    // 线程安全: 是。锁: 探测阶段分片 shared，匹配后 release 并调 remove()
    // 取分片 unique；这两阶段之间存在「探测后状态变化」的窗口（caller
    // 拿到 kOk 时不保证当前已不存在），但对 merge 的语义足够。
    PutResult conditional_remove(std::string_view key,
                                 std::uint64_t tstamp,
                                 std::uint32_t file_id,
                                 std::uint64_t offset,
                                 std::uint64_t remove_time);

    // ---- 查询 ----

    // 默认拿最新 revision；epoch != kMaxEpoch 时拿在那个 epoch 之前的
    // 最新 revision（fold 的 snapshot 语义就靠这个）。
    // 线程安全: 是。锁: key 分片 shared;miss 且 fold 态时嵌套 meta shared。
    // 注意: 返回的 EntryProxy.key 是 zero-copy view，仅在持锁期间有效——
    // 本接口返回时锁已释放，所以 key 已不可信赖；
    // caller 拿到值字段足够（key 字段当前调用方都已自带）。
    // S36-2：oki_shadow_check 开启且 epoch==kMaxEpoch 时，返回前对拍组合
    // 视图（见 set_oki_shadow_check）。
    std::optional<EntryProxy> get(std::string_view key,
                                   std::uint64_t epoch = kMaxEpoch) const;

    // ---- S36-2/3：统一点查原语（设计 doc/keydir-disk-resident-design-zh.md §5.1）----
    // 哈希缓存 → OKI 组合视图（memdelta 辅助哈希 → runs gen 降序 bloom/
    // seek + 块 LRU）。S36-3 起 Cask::get 走本原语（点查关 = 纯哈希，行为
    // 与现状逐字节相同）；S36-5 起 merge 活性/搬迁与 TTL 检查也统一到此
    // ——一份实现，杜绝三处各查一遍的漂移。组合视图来源的命中 epoch=0
    // （epoch 不落盘）。影子开启时顺带对拍（Cask::get 不再经 get()）。
    //
    // warm_fill（S36-3 读升温回填，设计 §5.1 步骤 3 + §11 问题 1）：冷侧
    // 命中时经**二次命中频度门**（4096 槽指纹表——同 key 连续两次冷命中才
    // 回填，扫描型负载天然被门挡住）把行插回哈希缓存。回填是缓存填充而非
    // 逻辑插入：不动 key_count/fstats（D4）；安全性靠分片写计数（fill 捕获
    // 的 writes 快照与插入时不一致即放弃——挡「remove 缺席 key 只写 delta
    // 墓碑」与回填旧行的竞态）+ 屏障/fold 期间直接放弃。
    // 线程安全: 是。锁: 同 get；冷侧嵌套 oki 内部 mu / 块缓存分片小锁
    // （独立锁域）；回填取分片锁（与写路径同款闸门检查，但退避即放弃）。
    [[nodiscard]] std::optional<EntryProxy> locate(std::string_view key,
                                                   bool warm_fill = false);

    // ---- S36-4：缓存预算与逐出（Level B）----
    // 条目预算（0 = 不限 = 现状全内存）。>0 时派生 per-shard 预算
    // （budget/kShards，下限 8），写路径插入超限即分片内**采样逐出**：
    // 从 clock 游标起取 ≤8 个可逐条目（SingleEntry；MultiEntry 不可逐），
    // 逐 epoch 最旧者——读路径不触碰缓存行（乐观读兼容，无读热度位），
    // 读热 key 被误逐后由读升温二次命中门回填自愈（S36-3）。设计原文的
    // CLOCK ref-bit 需要读侧写痕迹，与 seqlock 乐观读冲突，首版以采样
    // 近似（偏离已记录）。设置 >0 时同步做一次全分片出清（Level A→B
    // 转换的即时收敛）；fold 活跃期间跳过逐出（key 集只增不减不变量）。
    // 前置：OKI 点查已开启且组合视图可用（Cask::open 的 Level B 协议
    // 保证——未加载态下设置预算属误用，逐出后冷 get 无兜底）。
    void set_cache_budget(std::size_t entries);
    [[nodiscard]] std::size_t cache_budget() const noexcept {
        return cache_budget_.load(std::memory_order_relaxed);
    }

    // S36-3：单 key 物理逐出（测试/bench 用；S36-4 的采样逐出建立其上）。
    // 语义 = D4 的「逐出是缓存腾位，不是删除」：物理 erase（swap-delete +
    // limbo，乐观读者安全同 S29-6 清扫），key_count/fstats **不动**。
    // 前置：OKI 点查开启（组合视图能兜住冷 get）；fold/屏障期间拒绝
    // （MultiEntry 不可逐——设计 §11 问题 3）；**写挂钩在途时拒绝**
    // （oki_hooks_in_flight_≠0——否则逐掉「哈希已有、append 在途」的行 =
    // 冷读者回填旧行的静默回滚，实现注释有并发实证）。返回是否真的逐出。
    // ⚠ S36-5 之前 merge 活性判定仍走哈希 get——逐出态并发 merge 会把被逐
    // key 判死丢弃，测试勿组合两者。
    bool evict(std::string_view key);

    // S36-2 影子对拍（Level B 风险闸——零漂移是 S36-4 开逐出的前置门）：
    // 开启后每次 get(kMaxEpoch) 都以组合视图（OkiState::locate 冷侧）对拍
    // 哈希权威，稳定失配即 assert（NDEBUG 下计入 drifts 计数）。开启同时
    // 启用 OkiState 点查索引。前置：所有写路径携真实 ord（Cask 恒满足；
    // 直接驱动 KeyDir 且 ord 缺省 0 的旧式测试不可开）。
    // 并发协议：写路径以 oki_hooks_in_flight_ 括住「哈希更新可见 →
    // OKI append 完成」窗口；对拍失配时若计数非 0 → 跳过（无法判定），
    // 计数为 0 → 重读重试，稳定失配才判漂移。
    // 开启时机：**open 后、任何 merge 之前**（搬迁/TTL 挂钩门在点查开启
    // 上——关门期间的 merge 会把陈旧 loc 留在 run 里，之后开门即稳定
    // 失配）。debug 构建下 Cask::open 自动满足（finish_oki_recovery 末尾
    // 开启）；混用「未开门写者」的目录不可对拍（Level B 对这类目录以
    // 重建起步，S36-4）。
    void set_oki_shadow_check(bool on);
    [[nodiscard]] bool oki_shadow_check() const noexcept {
        return oki_shadow_.load(std::memory_order_relaxed);
    }
    struct ShadowStats {
        std::uint64_t checks = 0;  // 组合视图可用且完成比对的次数
        std::uint64_t skips  = 0;  // 写在途/不可用而跳过的次数
        std::uint64_t drifts = 0;  // 稳定失配（debug 下已 assert）
    };
    [[nodiscard]] ShadowStats shadow_stats() const noexcept {
        return {shadow_checks_.load(std::memory_order_relaxed),
                shadow_skips_.load(std::memory_order_relaxed),
                shadow_drifts_.load(std::memory_order_relaxed)};
    }

    // 线程安全: 是。无锁（atomic 读）。
    [[nodiscard]] std::uint64_t get_epoch() const;

    // 分配一个新的全局 ord 值（单调递增）。S27-1：这是 **LSN 的唯一发番点**
    // （写入序列号；恢复/MVCC/幂等水位）。返回类型 Lsn 标注角色，值语义不变。
    // 线程安全: 是。无锁（atomic fetch_add）。
    [[nodiscard]] Lsn alloc_ord();

    // 把 next_ord_ 至少推到 lsn + 1（用于 merge 后恢复 ord 状态）。
    // 线程安全: 是。无锁（atomic CAS-max）。
    void advance_ord(Lsn lsn);

    // ---- 迭代器工厂 ----
    // 线程安全: 是（仅构造一个 IterHandle 对象，未触碰共享状态）。
    [[nodiscard]] std::unique_ptr<IterHandle> make_iter() {
        return std::make_unique<IterHandle>(this);
    }

    // ---- 杂项 ----

    // 标记 keydir 为「就绪」——之前 acquire 同名 keydir 的线程会被解阻塞。
    // 线程安全: 是。无锁（atomic 写）。
    void mark_ready();
    // 线程安全: 是。无锁（atomic 读）。
    [[nodiscard]] bool is_ready() const;

    // 线程安全: 是。无锁（atomic 读）。
    [[nodiscard]] std::uint32_t biggest_file_id() const;
    // 线程安全: 是。无锁（atomic fetch_add）。
    std::uint32_t increment_file_id();
    // 把计数器至少推到 conditional_id；用于 registry 重新 acquire 时的恢复。
    // 线程安全: 是。无锁（atomic CAS-max）。
    std::uint32_t increment_file_id_at_least(std::uint32_t conditional_id);

    // A4-P2:当前 next_ord(成对性门比较用;原子读,无锁)。S27-1：LSN 水位。
    [[nodiscard]] Lsn peek_next_ord() const {
        return next_ord_.load(std::memory_order_relaxed);
    }

    // ---- A4:keydir 段快照(open 加速;设计 doc/recovery-unified-checkpoint-design-zh.md 附录 A)----
    // dump 当前内存态 + 调用方给的 per-file 字节水位。有活跃 fold
    // (MultiEntry 可能存在)时拒绝并返回 false(快照是纯优化)。
    // 线程安全: 是(写者闸门屏障 + meta unique;屏障期间读者照常并发)。
    // ---- S14-7：keydir 元数据 delta（"BKMD" v1）----
    // 增量 checkpoint（search.ckpt.d<seq> 的 kKeydirDelta 段）只携带小件：
    // per-file 字节水位 + 单调标量（next_ord/epoch/biggest_file_id）+
    // fstats（absolute，advisory 精度——提交时刻快照，微观并发漂移可接受，
    // merge/close 的全量快照定期校准）。**不含 entries 与 key 计数**：
    // entries 由链的 docmap 行/删除重放推进（put/remove 原生维护
    // key_count_/key_bytes_，精确）。
    void serialize_meta_delta(
        std::vector<std::byte>& out,
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watermarks)
        const;
    // 应用元数据 delta：标量取 max（单调）、fstats 绝对覆盖；返回其中的
    // 字节水位（caller 用链尾水位驱动 fold_start）。解析失败返回 nullopt。
    [[nodiscard]] std::optional<
        std::vector<std::pair<std::uint32_t, std::uint64_t>>>
    apply_meta_delta(std::span<const std::byte> bytes);

    // S14-7：ord 守卫删除——仅当当前 entry 的 ord < tomb_ord 才删除。
    // 链重放（delta 删除日志）用：顺序无关（「删后重写」的新行 ord >
    // tomb_ord，无论先后应用都不误杀）。返回是否实际删除。
    bool remove_if_older(std::string_view key, std::uint64_t tomb_ord);

    // S36-4：Level B（cache_budget>0）时写 **BCKS v4**——payload 与 v3 逐字节
    // 同构，但语义是「缓存子集 + 逻辑计数」（entries 只含未被逐出的条目；
    // key_count/key_bytes/fstats 标量是逻辑值）。Level A 照写 v3（全量）。
    [[nodiscard]] bool save_snapshot(
        std::string_view path,
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watermarks) const;
    // 校验 magic/ver/CRC 并整体重建内存态,返回水位表;任何不一致返回
    // nullopt 且清空状态(调用方走全量 fold)。仅限全新 KeyDir(open 路径)。
    // S36-4：v4（子集快照）仅当 accept_subset 且快照 next_ord ≤
    // subset_wm_limit（= OKI 联合水位——runs 覆盖快照点之前的全部行，缺席
    // 条目可由组合视图兜底）时接受；否则拒收 → 全量 fold（v4 当不了全量
    // 快照用——按子集载入再跳字节水位 = 静默丢 key）。v3 恒接受。
    [[nodiscard]] std::optional<
        std::vector<std::pair<std::uint32_t, std::uint64_t>>>
    load_snapshot(std::string_view path, bool accept_subset = false,
                  std::uint64_t subset_wm_limit = 0);

    // ---- 文件统计 ----
    // (注:fstats 的增量更新只发生在 put/remove 内,经私有 update_fstats;
    //  S1 起内部无锁,曾有的带锁公开版零调用方,O13 核实后删除。)

    // 标记某 file_id 为「等迭代结束就可删」。
    // 线程安全: 是。无锁（fstats 原子路径）。
    void set_pending_delete(std::uint32_t file_id);
    // 从 fstats 表里删一组 file_id（merge 完成后调）。返回实际删了几条。
    // 线程安全: 是。锁: fstats_grow_mu_（与槽位增长串行）。
    std::uint32_t trim_fstats(std::span<const std::uint32_t> file_ids);

    // ---- 快照 ----

    // 线程安全: 是。锁: meta shared（iter 状态）;计数走 atomic,fstats 无锁。
    [[nodiscard]] KeyDirInfo info() const;

    // S33-4：OKI 运行态（memdelta/run 集合/水位）。随 KeyDir 在 registry
    // 内共享；写挂钩在 put/remove 咽喉点（本类内部），目录路径由 Cask 在
    // load/flush/rebuild 时注入。
    [[nodiscard]] oki::OkiState& oki() noexcept { return *oki_; }

    // S29-6 P3:get 乐观快路径开关(默认开;false = 纯锁路径,回退用)。
    void set_optimistic_reads(bool on) noexcept {
        optimistic_reads_.store(on, std::memory_order_relaxed);
    }
    [[nodiscard]] bool optimistic_reads() const noexcept {
        return optimistic_reads_.load(std::memory_order_relaxed);
    }

    // ⑤ 诊断探针：遍历已存 key 统计长度分布，判断 SSO 命中率（libstdc++
    // std::string ≤15B 内联无堆分配）。零热路径开销——只读已有 entries，
    // 在真实负载（如生产数据恢复后）调用即得真实 sso/heap 占比，据此决定
    // 是否值得把 unordered_map 换成开放寻址扁平表。逐分片取锁，O(key 数)。
    struct KeyLenHistogram {
        std::uint64_t total = 0;
        std::uint64_t sso   = 0;  // ≤15B：SSO 内联，无堆分配
        std::uint64_t heap  = 0;  // >15B：每键一次堆分配
        // 桶：[0,8) [8,16) [16,24) [24,32) [32,48) [48,64) [64,128) [128,∞)
        std::array<std::uint64_t, 8> buckets{};
        // S33-1：keydir 常驻内存估算（诊断口径——entries 主结构，不含
        // pending/sibling 链/limbo 遗骸/fstats）。Level B 门禁数据来源：
        // estimated_bytes 对比进程 RSS 决定 keydir 磁盘驻留是否立项。
        std::uint64_t key_bytes       = 0;  // 活 key 字节和
        std::uint64_t entry_slot_bytes = 0; // 稠密数组容量 × sizeof(pair<string,Entry>)
        std::uint64_t bucket_bytes    = 0;  // 桶块槽数 × sizeof(Bucket) + 块头
        std::uint64_t heap_key_bytes  = 0;  // SSO 溢出 key 的堆分配估算（len+1）
        std::uint64_t estimated_bytes = 0;  // slot + bucket + heap_key（SSO key 已含于 slot）
    };
    [[nodiscard]] KeyLenHistogram key_length_histogram() const;

private:
    friend class IterHandle;

    // S36-2：conditional_remove 的锁内精确匹配参数（消灭原探测/删除两段
    // 之间的 TOCTOU——「删掉并发新写」从文档容忍变为不可能）。
    struct ExpectedLoc {
        std::uint64_t tstamp;
        std::uint32_t file_id;
        std::uint64_t offset;
    };

    // remove 本体（remove() 是「本体 + OKI 挂钩」的薄包装）。
    // expected 非空 = 锁内 CAS：live entry 的 (tstamp,file_id,offset) 不
    // 匹配则不删返回 false；victim_ord 非空时回传被删 entry 的 ord。
    bool remove_impl(std::string_view key, std::uint64_t remove_time,
                     std::uint64_t ord, bool insert_tombstone_if_absent,
                     const ExpectedLoc* expected = nullptr,
                     std::uint64_t* victim_ord = nullptr);

    // get 本体（get() 是「本体 + S36-2 影子对拍」的薄包装）。
    std::optional<EntryProxy> get_impl(std::string_view key,
                                        std::uint64_t epoch) const;
    // S36-2：影子对拍（见 set_oki_shadow_check 注释；proxy 为 get_impl
    // 已算出的哈希侧结果，失配时内部重读重试）。
    void shadow_verify(std::string_view key,
                       std::optional<EntryProxy> proxy) const;

    // S36-3：读升温回填本体（locate(warm_fill=true) 的冷命中路径调用）。
    // row = 组合视图行（epoch 由本函数分配）；w0 = locate 在哈希探测**之前**
    // 捕获的分片写计数快照——插入时不一致即放弃（协议见 locate 注释）。
    void cache_fill(std::string_view key, const SingleEntry& row,
                    std::uint64_t w0);
    // 二次命中频度门：同 key 连续两次冷命中才放行（4096 槽 32 位指纹，
    // 槽冲突 = 假阴性延迟回填，无正确性影响）。
    [[nodiscard]] bool warm_gate_second_hit(std::string_view key) const;

    // === M6-S2:分片 ===
    // 锁全序（屏障 v2,严格遵守,详见文件头）:
    //     barrier_mu_ → gate_mu_ → meta_mu_ → 单个 shard(≤1 把)
    //     → fstats_grow_mu_
    // 两处反向嵌套例外（热路径 shard→meta;release 阶段二屏障内
    // meta_shared→shard）见文件头无环论证。
    static constexpr std::size_t kShards = 256;  // S5:16→64,降低分片碰撞与写者停车传染面
    struct alignas(64) Shard {
        // 分片锁。主 hash 的值是 variant;判别用 std::get_if<Single|Multi>。
        // 透明 hash:get/put/remove 热路径用 string_view 直接查,零拷贝(O1)。
        mutable std::mutex mu;  // S5 实验:rwlock→mutex(消写者偏好停车;短临界区)
        // S29-6 P3:自建 seqlock 原生开放寻址表(替代 ankerl,设计 §6.3 选 A)。
        // 稠密存储 + 桶下标层保留(⑤ 的每键零 malloc / 零指针追逐不变);
        // erase 恒零 free(key/值遗骸进表内 limbo)、grow 旧数组延迟回收——
        // 乐观读者(get 快路径,不持锁)deref 恒安全。引用语义:值存于稠密
        // 数组,insert 增长会搬移、erase swap-with-last——entries_entry 裸
        // 指针仅在分片锁内、获取与使用之间无 insert/erase(已审计),安全。
        // 就地改写值(经 find 指针)必须包 entries.write_section()(seqlock
        // 写窗口),否则乐观读者读撕裂数据却校验通过。
        // S36-3：分片写计数——put/remove（含 miss 分支：remove 缺席 key 只
        // 写 delta 墓碑也算「动过」）在持本分片锁后 ++。读升温回填以
        // 「探测前后计数一致」为插入前提，挡住回填旧行的竞态；逐出同样 ++
        // （挡并发 fill 立即塞回）。仅回填/逐出路径消费，热路径只多一次
        // 无争用 RMW。
        std::atomic<std::uint64_t> writes{0};
        // S36-4：采样逐出的游标（近似轮转；swap-delete 使下标漂移无碍——
        // 逐出是启发式）。持本分片 mu 下读写。
        std::size_t clock_hand = 0;
        // 表头独占缓存行:find 路径读表头,别让它与锁字(每次加解锁 RMW)同行。
        alignas(64) detail::SeqShardTable<Entry> entries;
        // S29-6 P1:remove 无 fold 分支不再物理 erase,改留墓碑 SingleEntry
        // (sibling sentinel 判据)——为 P3 乐观读者保住 key string 缓冲与
        // map 槽位不被热路径 free。tombstones 计当前墓碑数,超 1/8 表长后
        // 每次写操作增量清扫 kTombstoneSweepBatch 槽(sweep_cursor 续扫);
        // fold release / barrier 的 quiescent 点全量清。两字段仅在持本分片
        // mu 下读写。
        std::size_t tombstones   = 0;
        std::size_t sweep_cursor = 0;
    };
    mutable std::array<Shard, kShards> shards_;

    // S29-6 P3:get 乐观快路径运行期开关(评审决议 ④,默认开;线上出问题
    // set_optimistic_reads(false) 一键退回纯锁路径,免重编译)。
    std::atomic<bool> optimistic_reads_{true};

    // S29-6 P1:增量墓碑清扫(前置:持 sh.mu 且 fold 未激活——fold 期间
    // entries key 集必须只增不减,见 IterHandle::next 注释)。
    static void sweep_tombstones_locked(Shard& sh);

    // S29-6 P2:攒批回收本分片 limbo(前置:持 sh.mu)。
    static void maybe_reclaim_locked(Shard& sh);

    // put() 三阶段拆分的共享上下文。锁随 ctx 移动，raw pointer
    // 指向的数据结构在对应锁释放前保持有效。
    struct PutCtx {
        std::unique_lock<std::mutex> slock;
        std::unique_lock<std::shared_mutex> mlock;
        Shard* shard = nullptr;
        SingleEntry* pending_entry = nullptr;
        Entry* entries_entry = nullptr;
        EntryProxy current_proxy{};
        bool found = false;
        bool current_is_tombstone = false;
        bool fold_active = false;
        std::uint64_t this_epoch = 0;
        std::uint64_t now_sec = 0;
    };
    PutResult put_probe(PutCtx& ctx, std::string_view key,
                         std::uint32_t old_file_id);
    PutResult put_insert(PutCtx& ctx, std::string_view key,
                          std::uint32_t file_id, std::uint32_t total_sz,
                          std::uint64_t offset, std::uint64_t tstamp,
                          bool newest_put, std::uint32_t old_file_id,
                          std::uint64_t ord);
    PutResult put_overwrite(PutCtx& ctx, std::string_view key,
                             std::uint32_t file_id, std::uint32_t total_sz,
                             std::uint64_t offset, std::uint64_t tstamp,
                             bool newest_put,
                             std::uint32_t old_file_id, std::uint64_t old_offset,
                             std::uint64_t ord);
    // pending_/iter 协调状态专用(仅 fold 期间触碰,冷路径)。
    mutable std::shared_mutex meta_mu_;

    // key → 分片下标(hash 低位路由;kShards 是 2^n)。
    [[nodiscard]] static std::size_t shard_for(std::string_view key) noexcept {
        return StringHash{}(key) & (kShards - 1);
    }

    // === 屏障 v2:写者闸门（替代旧"同时持全部分片锁"方案）===
    // 旧 lock_all_shards() 同时持 kShards+1=257 把锁,撞 TSan 死锁检测器
    // 64 持锁硬上限(sanitizer_deadlock_detector.h:67 CHECK,实测
    // KeyDir.DeepCopyPreservesOrd 在 detect_deadlocks=1 下崩溃),已删除。
    // BarrierGuard（keydir.cpp 内部）:置 barrier_active_ 后逐分片
    // 加锁-放锁排干在途写者（任意瞬间只持 1 把分片锁）;写者在拿到
    // 分片锁后检查闸门,active 则放分片锁到 gate_cv_ 退避;读者不受限。
    friend class BarrierGuard;
    std::atomic<bool>       barrier_active_{false};
    std::mutex              barrier_mu_;   // 屏障间互斥,跨整个屏障持有
    std::mutex              gate_mu_;      // 写者退避等待的 cv 配套锁
    std::condition_variable gate_cv_;

    // fold 期间「pending 表」：写时复制规则触发后，新 key 的写入和
    // 「fold 期间临时 key 的 tombstone」会落到这里。最后一个 release 时
    // merge 回各分片 entries。
    // 不变量(S2 起)：key ∈ 某分片 entries ⟹ key ∉ pending_——已存在
    // key 的新版本一律走分片内 sibling 链,绝不进 pending。get/put/remove
    // 的「entries 优先、miss 再查 pending」探测顺序依赖该不变量。
    // 锁要求：meta_mu_。
    std::optional<std::unordered_map<std::string, SingleEntry,
                                     StringHash, std::equal_to<>>> pending_;
    std::uint64_t pending_start_epoch_ = 0;  // 第一个 fold 启动时的 epoch(meta_mu_)
    std::uint64_t pending_start_time_  = 0;  // 第一个 fold 启动时的 wall-clock(meta_mu_)
    std::uint64_t pending_updated_     = 0;  // pending 中累积的写入次数(meta_mu_)

    // file_id 是 keydir_registry 分配的小整数单调计数,直接用 vector 按
    // 下标存(替代 unordered_map:update_fstats 在 put/remove 热路径上)。
    // present 位独立存;trim 后槽位清空但数组不收缩。
    // M6-S1:fstats 无锁热路径(设计 keydir-sharding-design-zh.md §3)。
    // deque 元素地址稳定;槽位经 fstats_grow_mu_ 串行构造后,以
    // fstats_size_ release 发布——读者 idx < size(acquire) 即可直接对
    // 字段做 relaxed 原子累加,put 热路径零锁字共享(S2 起生效;S1 仍在
    // mutex_ 内调用,顺序平凡安全)。
    //
    // S13-F8（TSan 实证,新 F1 并发测试抓出）:「deque 元素地址稳定」不等于
    // 「operator[] 并发安全」——deque::operator[] 要遍历内部块指针表(map),
    // 而 emplace_back 触发的 map 重分配会释放旧表 → 无锁读者 UAF。修复:
    // deque 仍是元素所有者(地址稳定),旁挂 RCU 指针表 fstats_ptrs_——扩容
    // 时在 grow_mu_ 下建新表、release 发布,旧表退休进 fstats_ptr_arrays_
    // 不释放(在途读者可能仍持有;总内存 < 2×终表 ≈ 16B/file_id,有界)。
    // 无锁读者一律经 fstats_slot(idx) 访问,前置条件 idx < size(acquire):
    // size 的 release 发布在指针表填充/替换之后,acquire 读者必见新表。
    // alignas(64):deque 每元素独占 cache line。否则两个 file_id 的 stats
    // 可能跨同一 64B 行,merge + active 并发写不同文件时假共享。
    struct alignas(64) AtomicFStats {
        std::atomic<std::uint64_t> live_keys{0};
        std::atomic<std::uint64_t> total_keys{0};
        std::atomic<std::uint64_t> live_bytes{0};
        std::atomic<std::uint64_t> total_bytes{0};
        std::atomic<std::uint64_t> oldest_tstamp{0};
        std::atomic<std::uint64_t> newest_tstamp{0};
        std::atomic<std::uint64_t> expiration_epoch{kMaxEpoch};
        std::atomic<std::uint8_t>  present{0};
    };
    mutable std::mutex fstats_grow_mu_;   // 仅新 file_id 槽位构造(罕见)
    mutable std::deque<AtomicFStats> fstats_;
    std::atomic<std::size_t> fstats_size_{0};
    // S13-F8：RCU 指针表（见上）。fstats_ptr_arrays_/fstats_ptr_cap_ 仅在
    // fstats_grow_mu_ 下触碰；fstats_ptrs_ 读者 acquire 无锁读。
    mutable std::atomic<AtomicFStats**> fstats_ptrs_{nullptr};
    mutable std::vector<std::unique_ptr<AtomicFStats*[]>> fstats_ptr_arrays_;
    mutable std::size_t fstats_ptr_cap_ = 0;

    // 无锁读路径的槽位访问。前置条件：idx < fstats_size_.load(acquire)。
    [[nodiscard]] AtomicFStats& fstats_slot(std::size_t idx) const {
        return *fstats_ptrs_.load(std::memory_order_acquire)[idx];
    }
    // 扩容到至少 need_size 个槽位并同步指针表 + 发布 size。
    // 锁要求：调用方持 fstats_grow_mu_。
    void grow_fstats_locked(std::size_t need_size);

    // M6-S1/S2:全局标量原子化。epoch_ 的跨线程可见性判据
    // (entry.epoch < iter_epoch)由「分片锁内 fetch_add + 屏障内读取」
    // 保证,见设计 §6.4。
    //
    // 缓存行分组（S2 实测关键）:
    //   写热行——每次 put/remove 都 RMW（epoch_/next_ord_;key_count_/
    //   key_bytes_ 在插入删除时）。没有热读者(info 是冷路径)。
    alignas(64) std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint64_t> key_count_{0};
    std::atomic<std::uint64_t> key_bytes_{0};
    // ord 分配器独立为 atomic:alloc_ord/advance_ord 不再抢全局锁
    // (put 热路径上每次写都要分配 ord)。
    std::atomic<std::uint64_t> next_ord_{0};

    // S33-4：OKI 运行态（随 KeyDir 在 registry 内共享；ctor 构造，恒非空）。
    std::shared_ptr<oki::OkiState> oki_;

    // S36-2/3：影子对拍 + 逐出安全状态。oki_hooks_in_flight_ = 写路径
    // [哈希更新可见 → OKI append 完成] 的在途括号，**点查开启即计数**
    // （S36-3 起不只服务影子）：影子对拍靠它跳过无法判定的窗口，evict
    // 靠它拒逐「哈希已有、append 在途」的行（否则冷读者会拿到旧行并回填
    // = 静默回滚，见 evict 实现注释）。点查关（Level A 默认）时写路径只
    // 多一次 acquire 读。
    std::atomic<bool> oki_shadow_{false};
    mutable std::atomic<std::uint32_t> oki_hooks_in_flight_{0};
    mutable std::atomic<std::uint64_t> shadow_checks_{0};
    mutable std::atomic<std::uint64_t> shadow_skips_{0};
    mutable std::atomic<std::uint64_t> shadow_drifts_{0};

    // S36-3：读升温频度门（4096 槽 × 32 位指纹 = 16KB；无锁 exchange）。
    static constexpr std::size_t kWarmGateSlots = 4096;
    mutable std::array<std::atomic<std::uint32_t>, kWarmGateSlots>
        warm_gate_{};
    // S36-3：累计逐出数。>0 后「哈希 miss」不再是权威 miss——影子对拍的
    // 「miss vs 组合视图活行」方向据此降级为 skip（逐出前保持全严格）。
    std::atomic<std::uint64_t> evictions_{0};

    // S36-4：缓存预算（Level B）。cache_budget_ = 总条目数（0=不限）；
    // shard_budget_ = 派生的分片预算（写路径插入后比对 entries.size()）。
    std::atomic<std::size_t> cache_budget_{0};
    std::atomic<std::size_t> shard_budget_{0};
    // 采样逐出（前置：持 sh.mu；fold 活跃/无预算时 no-op）。
    void maybe_evict_locked(Shard& sh, bool fold_active);

    //   读热行——get/put 热路径每次 relaxed 读、写入罕见。与上面的写热
    //   行隔离,否则每个 put 的 epoch_ RMW 都会把读者需要的行打飞
    //   (false sharing,Mixed 基准实测主要损耗源)。
    // keyfolders_:当前活跃 fold 数。只在写者闸门屏障(BarrierGuard +
    // meta unique)内修改;写者在持自己分片锁(且通过闸门检查)后
    // relaxed 读即足够新——屏障的排干循环无法在写者持分片锁期间越过该
    // 分片,见设计 §4 时序论证。
    alignas(64) std::atomic<std::uint64_t> keyfolders_{0};
    // biggest_file_id_:put 接受判断每次读;CAS-max 推进仅 roll 时真写。
    std::atomic<std::uint32_t> biggest_file_id_{0};
    // pending_.has_value() 的无锁镜像:热路径在持分片锁后 relaxed 读,
    // 避免每次 get/put 都摸 meta_mu_。仅在持 meta_mu_ unique 时修改。
    // (单独存在的意义:release 收尾窗口里 keyfolders_ 可能已归零但 pending_
    //  仍在应用中——此时写路径仍须分流到 pending,不能只看 keyfolders_。)
    std::atomic<bool> has_pending_{false};
    std::atomic<bool> is_ready_{false};

    // 迭代协调冷状态(meta_mu_;独立行,避免污染上面两条热行)。
    alignas(64) std::uint64_t iter_generation_ = 0;  // 单调 ++(fold release 归零时 +1)
    // 写痕迹标志。当前没有读者(纯诊断位);做成 atomic 以免 sibling 升链
    // 热分支为了它单独去拿 meta_mu_(S2 设计偏差,见 keydir.cpp 注释)。
    std::atomic<bool> iter_mutation_{false};

    // fstats 增量更新。S1 起内部即无锁(增长走 fstats_grow_mu_),caller
    // 无需持有任何锁;锁序上 fstats_grow_mu_ 排最末,持分片锁/meta 时
    // 调用均合法。
    void update_fstats(std::uint32_t file_id, std::uint64_t tstamp,
                       std::uint64_t expiration_epoch,
                       std::int32_t live_inc, std::int32_t total_inc,
                       std::int32_t live_bytes_inc,
                       std::int32_t total_bytes_inc,
                       bool should_create);

    // iter release 收尾两步（拆自旧 merge_pending_and_collapse_barrier;
    // 三阶段协议见 IterHandle::release 实现注释）。
    // 阶段二:把 pending_ 逐条应用进各分片 entries。内部拿 meta shared,
    // 持有期间逐 key 嵌套该分片锁（meta→shard,"屏障内例外",无环论证
    // 见文件头/.cpp）。前置:caller 持 BarrierGuard 且 keyfolders_==0。
    void apply_pending_to_entries_barrier();
    // 把 MultiEntry 折回 SingleEntry:逐分片「lock→折叠→unlock」。
    // 前置:caller 持 BarrierGuard 且 keyfolders_==0、pending_ 已清。
    void collapse_multi_entries_barrier();
};

}  // namespace bitcask::keydir
