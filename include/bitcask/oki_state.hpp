// OKI 运行态（S33-4）：memdelta + run 集合 + manifest 水位。
// 设计：doc/ordered-key-index-design-zh.md §2/§5。
//
// 归属：挂索引权威（进程内随 IndexRegistry 共享，生命周期随 authority——
// 迭代器 pin 语义自动顺延）；目录路径由 Cask 注入（OkiState 不知道自己的
// 目录，与 kv.oki.manifest 同模式）。
//
// === 挂钩与水位模型 ===
// 写挂钩收敛在 IndexAuthority 的条件 put/remove 咽喉点（本类的 append
// 由 Cask 写路径调用——设计文档 §8 难点 1 的对策）。
// append 以 `ord ≥ wm` 为门：
//   - 运行期写（ord 恒新）→ 全收；
    //   - 恢复 tail 重放（fold 携真实 ord）→ 自动只收 wm 之后的行；
//   - merge 搬迁 put（old_file_id≠0）与 TTL conditional_remove（S36-2 起
//     反转 Level A 的「零交互」）→ 走 append_update **绕过水位门**
//     （旧 ord、新信息）。**仅点查开启时收**（影子对拍 / Level B）：
//     Level A 不消费这些行（range 回查 keydir），关门即零改动零开销；
//     Level B 开启必须以全量重建起步（关门期间的 run loc 本就陈旧），
//     开启后挂钩持续在线保 run 新鲜——见 keydir.cpp 挂钩点注释；
//   - 链重放 remove（ord=0）→ 不收。
// wm = **排他上界**：尚未被 runs 覆盖的最小 ord（首个合法 LSN 是 0——
// alloc_ord 从 0 起——含上界语义会漏掉 ord 0，故全链路统一排他）。
// 同 key 胜出全链路统一 **max (ord, 到达序)**（S36-2）：flush/read_view
// 的 stable_sort 取末、locate 辅助哈希的 ord≥ 顶替、SpillingRunBuilder
// 的 (ord, seq)、run 间的 (ord, gen)——同构一条规则。运行期同 key 到达
// 序即写序（write_mu_ / 分片锁串行）；恢复期并行 fold 乱序到达由 ord
// 主键消解（恢复期无同 ord 平局）。
//
// 崩溃窗口：fold 用 data 字节水位跳过已恢复行——这些行若未进 runs 即是
// 洞。约定 OKI flush 恒在 data 提交面稳定之后；崩溃丢 flush 时由 open
// 端检查 run 覆盖/metadata 活键数 → 整体重建兜底（cask_recovery）。
//
// === 线程模型 ===
// append/delta 统计：内部 mu_（运行期单写者无争用；恢复期并行 fold 短锁）。
// flush/load/rebuild：flush_mu_ 串行（close/checkpoint/merge 收尾间互斥）；
// flush 期间 memdelta 换出，append 不被 IO 阻塞。

#pragma once

#include <atomic>
#include <optional>
#include <thread>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bitcask/detail/seq_gate.hpp"
#include "bitcask/detail/snapshot_cell.hpp"
#include "bitcask/oki_run.hpp"
#include "bitcask/string_hash.hpp"

namespace bitcask::oki {

class OkiState {
public:
    OkiState() = default;
    ~OkiState();  // 等后台归并线程收尾
    OkiState(const OkiState&) = delete;
    OkiState& operator=(const OkiState&) = delete;

    // S36-2：行加宽为全字段（doc/keydir-disk-resident-design-zh.md §6）——
    // 组合视图点查（locate）的权威载体。活行恒 has_loc=true（写挂钩把
    // (file_id, total_sz, offset, tstamp) 一并送入）；墓碑行免位置。
    struct DeltaRow {
        std::string key;
        std::uint64_t ord = 0;
        bool tomb = false;
        bool has_loc = false;
        RowLoc loc{};
        // 墓碑行的持久性落点（不进 run，只供 flush 的持留判定）：墓碑没有
        // loc，但它的 data 记录同样可能尚未持久。0 = 无落点（条件删除等
        // 无数据记录背书的行）——视为已持久。
        std::uint32_t dur_file = 0;
        std::uint64_t dur_end = 0;
        // 到达序号（mu_ 下单调分配；0 = 恢复/重建装载的行）。与
        // OkiCommitSnapshot::cut_seq 配合界定「快照已计入」的行。
        std::uint64_t seq = 0;
    };

    // 写挂钩（keydir.cpp 的 put/remove 咽喉点调用）。ord < wm 的行直接
    // 丢弃（见文件头水位模型）。loc：活行的位置字段（S36-2；墓碑传空）。
    void append(std::string_view key, std::uint64_t ord, bool tomb,
                const RowLoc* loc = nullptr);

    // S36-2：**绕过 wm 水位门**的更新行——「旧 ord、新信息」的两个来源：
    //   - merge 搬迁（keydir CAS 成功后）：同 ord、新位置。胜出靠
    //     (ord, 来源序) 格——搬迁行必然落在更高 gen 的 run（或 delta），
    //     等 ord 时新来源胜（设计 §D1/§D2）；
    //   - TTL conditional_remove（受害者 ord 的墓碑）：keydir 侧删除没有
    //     数据记录背书，组合视图靠这行抵消 run 里的陈旧活行。
    // 恢复期不走本入口（两者皆运行期路径），水位门语义不受影响。
    // dur_file/dur_end：墓碑行的 data 落点（见 DeltaRow），其余行传 0。
    void append_update(std::string_view key, std::uint64_t ord, bool tomb,
                       const RowLoc* loc, std::uint32_t dur_file = 0,
                       std::uint64_t dur_end = 0);

    // 当前到达序号上界（一致切面捕获用；调用方须在写序闸门内调用）。
    [[nodiscard]] std::uint64_t current_seq() const {
        std::lock_guard<std::mutex> lk(mu_);
        return append_seq_;
    }

    // 写路径探询：memdelta 达阈值（行数/字节）？无锁近似读。
    [[nodiscard]] bool should_flush() const noexcept {
        return flush_hint_.load(std::memory_order_relaxed);
    }

    // open 早期载入 manifest。缺失/损坏 → 未加载态（caller 依恢复形态决定
    // 重建）。幂等；与 flush/rebuild 互斥。
    void load(std::string_view dir);
    [[nodiscard]] bool loaded() const noexcept {
        return loaded_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t wm() const noexcept {
        return wm_.load(std::memory_order_acquire);
    }

    // flush：memdelta 前缀拷贝 → 按 key 排序 + 同 key 取 (ord,到达序) 去重
    // （墓碑保留为 tomb 行）→ 写新 run → manifest 提交（唯一 commit
    // point）→ 推进 wm → 删已固化前缀。空 delta 且已加载 → no-op true。
    // IO 失败 → delta 原状（无恢复动作）、返回 false（下次重试）。未加载
    // 态拒绝（先 load/rebuild）。
    // S36-5 B1：durable_wms 非空 = 各文件的**已持久字节水位**——loc 越界
    // 的行**持留**在 delta（run 自身 fsync 落盘，引用可能随掉电蒸发的字节
    // = 冷点查的悬空 loc），待下次 flush（持久水位推进后）再固化。持留行
    // 恒是 active 尾巴（sealed 由封口 fsync 全量持久），ord 全局最高——
    // cover/wm 不含它们，tail 重放的水位门语义自洽。空 span = 不过滤
    // （调用方已保证全部持久，如写路径阈值 flush 的先 sync 后 flush）。
    // async_compaction：提交后若 run 数越阈值，归并交后台线程（写线程上的
    // 阈值 flush 用，免写者陪等全量归并）；否则在释放 flush_mu_ 后同步归并。
    [[nodiscard]] bool flush(
        std::string_view dir,
        std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms =
            {},
        std::optional<OkiCommitSnapshot> commit_snapshot = std::nullopt,
        bool async_compaction = false);

    // 全量重建：rows 由 caller 从 keydir 收集（活 key + ord；墓碑不需要
    // ——单一全量 run 的缺席即语义）。排序写单 run、manifest 只含它、
    // wm=cover_ord，删除全部旧 run 文件，清空 memdelta（其内容已被快照
    // 覆盖）。与 flush 互斥。
    // S33-6：rows 为空（空库 / 全删）时**不落空 run**——manifest 记 0 个
    // run + wm=cover_ord，语义等价且不留空文件与常驻 fd。
    [[nodiscard]] bool rebuild(std::string_view dir,
                               std::vector<DeltaRow>&& rows,
                               std::uint64_t cover_ord,
                               std::optional<OkiCommitSnapshot> commit_snapshot =
                                   std::nullopt);

    // 流式重建的两段接口（IndexAuthority::rebuild_from_data 用）：
    //   reserve_gen → caller 在锁外把行直接灌进 SpillingRunBuilder(dir, gen)
    //   → install_built_run 提交（语义同 rebuild：单 run、wm=cover、清 delta、
    //   sweep 旧 run）。gen=0 = 无 run（零活 key）。构建失败 → abandon_gen。
    // 预留的 gen 在提交/放弃前登记为在途，并发 sweep 不会删其 run/spill。
    [[nodiscard]] std::uint64_t reserve_gen(std::string_view dir);
    [[nodiscard]] bool install_built_run(
        std::string_view dir, std::uint64_t gen, std::uint64_t cover_ord,
        std::optional<OkiCommitSnapshot> commit_snapshot = std::nullopt);
    void abandon_gen(std::string_view dir, std::uint64_t gen);

    // 等待在途后台归并完成（测试 / 需要确定性 run 集合的调用方）。
    void wait_compaction();

    // 只读恢复/升级的内存视图：语义与 rebuild 相同，但不落 run、不改盘上
    // manifest。data 仍是持久真相；可写句柄后续会做持久受控重建。
    void install_memory_view(std::vector<DeltaRow>&& rows,
                             std::uint64_t cover_ord,
                             std::optional<OkiCommitSnapshot> commit_snapshot =
                                 std::nullopt);

    // S33-5：range 读者视图——runs 的共享 Reader（不可变，多线程各持
    // Cursor 并发读）+ memdelta 快照（已按 key 排序、同 key max-ord 去重，
    // 墓碑保留）。**弱一致**：视图是创建时刻的近似（per-key 语义，非 fold
    // 快照）。OKI 未加载 → nullopt（RO 打开无 OKI 的目录等）。
    // shared_ptr 使在途视图安全跨越 rebuild 的旧 run 删除（POSIX unlink
    // 后已开 fd 仍可读到 close）。
    // delta 走 shared_ptr<const>：排序视图缓存（见 sorted_view_）命中时
    // 与缓存**零拷贝共享**——memdelta 未 flush 的服务型负载里连续 range
    // 查询不再各自支付全量拷贝 + 排序（下游实测 5 万行 ~120ms/次 → 近零）。
    // 行内容不可变；在途视图不受后续写影响（写 bump 版本，缓存自动失效）。
    struct ReadView {
        std::vector<std::shared_ptr<OkiRunReader>> runs;
        std::shared_ptr<const std::vector<DeltaRow>> delta;
    };
    [[nodiscard]] std::optional<ReadView> make_read_view() const;

    // ---- S36-2：组合视图点查（Level B 权威的冷侧；设计 §5.1）----
    //
    // 查询序：memdelta 辅助哈希（最新到达行；(ord, 到达序) 胜出格的 delta
    // 段）→ 逐 run gen 降序（bloom 试探 → 稀疏索引二分 → 块内扫）。
    // 「gen 降序首命中即权威」依赖不变量：**行只随时间进入更高 gen**
    //（flush 按时序、compact 输出 gen = max+1、等 ord 高 gen 胜）。
    //
    // kUnavailable 的来源（caller 一律按「无法判定」降级，不得当 miss）：
    //   - 点查未启用（enable_point_query）或 OKI 未加载；
    //   - 命中路径上遇到 v1 run（无位置字段/bloom——组合视图无点查能力，
    //     设计 §D3：Level B 需全 v2，混布目录待重建自愈）；
    //   - run IO/损坏（影子/点查路径不担责报错，交由既有重建兜底）。
    enum class LocateStatus { kUnavailable, kMiss, kHit };
    struct LocateResult {
        LocateStatus status = LocateStatus::kUnavailable;
        std::uint64_t ord = 0;
        bool tomb = false;       // kHit 且 tomb ⟹ 权威「已删除」
        bool has_loc = false;
        RowLoc loc{};
    };
    [[nodiscard]] LocateResult locate(std::string_view key) const;

    // ---- S43-5：写权威服务（第一批只收敛探测/应用入口）----
    //
    // probe_for_write 是 locate 的显式写前语义包装：kUnavailable 必须由
    // caller 触发受控重建或显式失败，禁止折叠成“不存在”。返回的 loc/ord
    // 只能用于 CAS 与账务裁决；数据记录仍必须先持久化，OKI 行随后才可见。
    struct WriteProbe {
        LocateStatus status = LocateStatus::kUnavailable;
        std::uint64_t ord = 0;
        bool tomb = false;
        bool has_loc = false;
        RowLoc loc{};
    };
    [[nodiscard]] WriteProbe probe_for_write(std::string_view key) const;

    // 运行期行应用入口。put/remove 使用真实新 ord；relocation 与
    // conditional_remove 使用受害者/旧 ord 的新信息（绕过 wm 门）。
    // 这些方法不检查 CAS——CAS 判定必须先 probe_for_write，再由调用方在
    // 同一把写序闸门内应用，避免探测/应用之间出现 TOCTOU。
    void apply_put(std::string_view key, std::uint64_t ord,
                   const RowLoc& loc);
    // dur_file/dur_end：墓碑 data 记录的落点（文件 + 记录末字节），供 flush
    // 判定其是否已持久；0 = 未知/无记录。
    void apply_remove(std::string_view key, std::uint64_t ord,
                      std::uint32_t dur_file = 0, std::uint64_t dur_end = 0);
    void apply_conditional_remove(std::string_view key,
                                  std::uint64_t victim_ord);
    void apply_relocation(std::string_view key, std::uint64_t ord,
                          const RowLoc& loc);

    // 点查开关：建 memdelta 辅助哈希（key → 最新行下标，(ord, 到达序)
    // 胜出）。默认关——索引在 append 热路径上多一次哈希插入 + key 拷贝，
    // 只在影子对拍（S36-2）/ Level B（S36-3+）需要时开启。幂等。
    void enable_point_query();
    // S43-1：运行期发现坏块/组合视图不可用时立刻关闭点查并卸载视图。
    // 后续冷查询不得把该状态当 miss；受控 rebuild 成功后恢复 loaded/v3。
    void degrade_for_rebuild() noexcept;
    [[nodiscard]] bool point_query_enabled() const noexcept {
        return point_query_.load(std::memory_order_acquire);
    }
    // S43-5：点查开关只代表读端允许查，不代表组合视图具备写权威能力。
    // v1 run 没有 loc/bloom，任何 newest put 都无法安全裁决，必须先做
    // 受控重建；读取仍可对 v2/v3 子集工作。
    [[nodiscard]] bool write_probe_ready() const noexcept;

    // ---- S36-4：Level B 模式戳（BCOM v3）----
    // set_level_b：声明本写者的模式（Cask::open 在 load 之前调）。此后
    // flush/compact/rebuild 提交的 manifest 都带（或不带）level_b 戳。
    // manifest_level_b：**盘上** manifest 的戳（load/commit 时更新）——
    // Level B 开启对未带戳的目录必须全量重建起步（关门期间的 merge 会让
    // run loc 陈旧）；Level A 写者 open 时经 stamp_mode 清戳。
    void set_level_b(bool on) noexcept {
        level_b_.store(on, std::memory_order_release);
    }
    [[nodiscard]] bool manifest_level_b() const noexcept {
        return manifest_level_b_.load(std::memory_order_acquire);
    }
    // S43-2：v4 元数据迁移入口；nullopt = 盘上仍是 v1-v3 或未加载。
    // 访问器按值返回并持 flush_mu_：manifest_ 会被 flush/compact/rebuild
    // 整体替换，返回内部引用是数据竞争（且调用方可能持有悬空引用）。
    [[nodiscard]] std::optional<OkiManifestMetadata>
    manifest_metadata() const {
        std::lock_guard<std::mutex> flk(flush_mu_);
        return manifest_.metadata;
    }
    [[nodiscard]] std::vector<OkiManifestFStats>
    manifest_fstats() const {
        std::lock_guard<std::mutex> flk(flush_mu_);
        return manifest_.fstats;
    }
    // S43-3：streaming recovery 检查 run 版本/覆盖时需要的只读 manifest 面。
    [[nodiscard]] std::vector<OkiManifestEntry>
    manifest_runs() const {
        std::lock_guard<std::mutex> flk(flush_mu_);
        return manifest_.runs;
    }
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>>
    manifest_data_wms() const {
        std::lock_guard<std::mutex> flk(flush_mu_);
        return manifest_.data_wms;
    }
    // 降级戳：level_b 关而盘上戳开 → 立刻改写 manifest 清戳（Level A 写者
    // 将做无挂钩 merge，戳必须先失效——open 早期、任何 merge 之前调用）。
    // 升级（关→开）不走此路（必须经 rebuild，戳随重建的 manifest 落盘）。
    void stamp_mode(std::string_view dir);

    // S36-3：块 LRU 诊断/调参（测试与 bench；生产选项透出排 S36-6）。
    [[nodiscard]] OkiBlockCache::Stats block_cache_stats() const {
        return block_cache_.stats();
    }
    void reset_block_cache_capacity(std::size_t bytes) {
        block_cache_.reset_capacity(bytes);
    }

    // 诊断。
    [[nodiscard]] std::size_t delta_rows() const;
    [[nodiscard]] std::size_t delta_bytes() const;
    [[nodiscard]] std::size_t run_count() const;

    // flush 阈值（写路径 should_flush 探询；超限由 Cask 在写路径同步
    // flush——罕见且有界）。
    static constexpr std::size_t kFlushRowLimit  = 1u << 20;       // 1M 行
    static constexpr std::size_t kFlushByteLimit = 64u << 20;      // 64 MiB

    // S33-6：run 归并阈值（设计 §5.2「极简两层」）。flush 提交后 run 数超此
    // 值即把**全部** run 归并成一个。不做 leveled compaction——OKI 条目不含
    // value，全归并 1 亿 key 也就 ~1-2GB 顺序 IO。
    // 不归并的后果（实测）：run 数 = flush 次数（close/merge 收尾/checkpoint
    // 各一次）线性增长 ⟹ 每 run 一个常驻 fd + open 期全文件 CRC + range 多
    // 一路归并，且墓碑行永远回收不掉。
    static constexpr std::size_t kCompactRunLimit = 8;

private:
    // S33-6：把**全部** run 归并成单个新 run（持 flush_mu_ 调用）。
    // run 数 ≤1 时 no-op。best-effort：失败原状不变（下次 flush 再试）。
    //
    // **墓碑真正丢弃**——仅在「全归并」下成立：同 key 的 put 行与 tomb 行必
    // 定同在本次归并里，max-ord 胜出者若是 tomb 则两行一起丢，绝不会留下被
    // 抵消掉的陈旧 put 行。若将来改成部分归并，这条**必须**收回（否则旧 run
    // 里的 put 行会因抵消它的 tomb 消失而"复活"）。
    // 完整性不变量（OKI key 集 ⊇ keydir 活 key 集）不受影响：被丢弃的 key
    // 若之后重新 put，新行 ord > wm 走 memdelta（读视图含之），崩溃时也由
    // tail 重放补回——归并不动 wm。
    // 全归并（三段式，IO 不持 flush_mu_；见实现注释）。
    [[nodiscard]] bool compact_runs(std::string_view dir);
    void schedule_compaction(std::string_view dir);
    [[nodiscard]] bool flush_locked_impl(
        std::string_view dir,
        std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
        std::optional<OkiCommitSnapshot> commit_snapshot, bool& need_compact);
    void release_gen_locked(std::uint64_t gen);  // 前置：持 flush_mu_

    void update_flush_hint_locked() noexcept {
        flush_hint_.store(
            delta_.size() >= kFlushRowLimit || delta_bytes_ >= kFlushByteLimit,
            std::memory_order_relaxed);
    }
    // gen 必须单调永不重用：取 manifest 与目录内现存 run 文件（含孤儿、
    // sweep 滞留件）的最大 gen + 1。只看 manifest 会在「0 run 重建 /
    // manifest 丢失」后从 1 重来，与滞留旧文件同名、并污染以 gen 为键的块缓存。
    [[nodiscard]] std::uint64_t next_gen_locked(std::string_view dir);
    std::uint64_t gen_floor_ = 0;  // 曾发布过的最大 gen（flush_mu_ 保护）
    std::vector<std::uint64_t> inflight_gens_;  // 在途构建的 gen（flush_mu_）
    // 读视图的成对一致闸：凡同时改动 runs 与 delta 的提交（flush 的「发布
    // run + 裁前缀」、rebuild、install_memory_view）都在其写窗口内进行；
    // make_read_view 不持 flush_mu_，经它取到成对的 (delta, runs)。写者由
    // flush_mu_ 串行。只换 runs 的归并不进窗口（新旧内容等价）。
    ::bitcask::detail::SeqGate view_gate_;
    std::mutex compact_mu_;         // compact_runs 串行
    std::mutex compact_thread_mu_;  // compact_thread_ 的启动/join
    std::thread compact_thread_;
    std::atomic<bool> compacting_{false};

    // S36-2：memdelta 辅助哈希的维护（前置：持 mu_ 且 point_query_ 开）。
    // 胜出规则 = max (ord, 到达序)：新行 ord ≥ 旧行才顶替（等 ord 后到者
    // 胜——merge 搬迁行；ord 更小的乱序到达行——恢复期并行 fold——不顶替）。
    void index_row_locked(std::size_t idx);
    void rebuild_index_locked();

    // S36-2：locate 的 runs 快照（免抢 flush_mu_——flush/compact/rebuild
    // 持锁横跨长 IO，点查不能陪等）。发布点 = readers_ 每次变更后。
    using RunsVec =
        std::vector<std::pair<std::uint64_t, std::shared_ptr<OkiRunReader>>>;
    void publish_runs_locked();  // 前置：持 flush_mu_

    mutable std::mutex mu_;        // delta_ / delta_bytes_ / delta_idx_ / delta_version_
    std::vector<DeltaRow> delta_;
    std::size_t delta_bytes_ = 0;
    // 排序视图缓存（make_read_view 的复用层）。memdelta 本是「append + 惰性
    // 排序」——惰性排序的成本原落在每一次 range 的视图构建上，与查询次数
    // 相乘（下游实测：5 万行未 flush 时 ~120ms/次，flush 后 ~0.03ms，4200×；
    // 见 feedbacks/2026-09-18-oki-unflushed-memdelta-range-query-4000x-slower.md）。
    // 缓存 = 排序去重后的快照；失效判据 = delta_version_ 失配（任何 append /
    // flush 前缀裁剪 / rebuild 清空都 bump 版本）——「写后首查重建一次」语义
    // 不变，无写时的连续查询降为 shared_ptr 拷贝。
    // 锁序恒为 flush_mu_ → mu_ → view_mu_（构建在锁外，持锁段只碰指针）。
    mutable std::mutex view_mu_;
    mutable std::shared_ptr<const std::vector<DeltaRow>> sorted_view_;
    mutable std::uint64_t sorted_view_version_ = 0;  // 与 delta_version_ 相等即有效
    std::uint64_t delta_version_ = 0;
    std::uint64_t append_seq_ = 0;  // DeltaRow::seq 分配器（mu_ 保护）
    // S36-2：key → delta_ 内最新行下标（透明哈希，点查开启时维护）。
    std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>>
        delta_idx_;
    std::atomic<bool> point_query_{false};

    mutable std::mutex flush_mu_;  // flush/load/rebuild 串行；manifest_/readers_ 归其保护
    // B4：sweep 删除失败的滞留路径（flush_mu_ 下访问；下次 sweep 重试）。
    std::vector<std::string> sweep_backlog_;
    OkiManifest manifest_;
    // manifest_.runs 一一对应的常驻 Reader（load 全量 CRC 校验后开；
    // flush/rebuild 产出新 run 时随 manifest 提交同步维护）。
    RunsVec readers_;
    // readers_ 的不可变快照（atomic shared_ptr；locate 无锁取用）。
    ::bitcask::detail::SnapshotCell<RunsVec> runs_snap_;

    std::atomic<bool> loaded_{false};
    std::atomic<std::uint64_t> wm_{0};
    std::atomic<bool> flush_hint_{false};

    // S36-4：本写者模式 / 盘上 manifest 戳（见 set_level_b 注释）。
    std::atomic<bool> level_b_{false};
    std::atomic<bool> manifest_level_b_{false};

    // S36-3：块 LRU（(gen, 块下标) → 块字节；独立锁域，不进任何既有锁序）。
    // mutable：locate 是 const 语义的读，缓存升温/装载是它的内部副作用。
    // 跨 close/reopen 复用安全：gen 永不重用，run 文件按 gen 不可变。
    mutable OkiBlockCache block_cache_{kDefaultBlockCacheBytes};
};

}  // namespace bitcask::oki
