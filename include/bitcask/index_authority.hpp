// S43-9：OKI-only 后的目录级索引权威。
//
// 该对象只承担原内存索引中仍必须存在的“账本 + 写序”职责：ord/file_id
// 分配、逻辑计数、per-file fstats、OKI 行 CAS/应用与提交快照。点查、
// range、merge 活性全部来自 OkiState；没有任何 key 哈希缓存。
//
// 线程模型：
//   * alloc_ord / file_id 为 atomic，可与写路径并发；
//   * probe+apply 的 CAS 域由 writer_mu_ 串行，跨同一目录多个 Cask 共享；
//   * capture_commit_snapshot/flush 的成对静止窗口使用 with_writer_lock；
//   * open 初始化由 registry 的 ready 协议保证单线程。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bitcask/oki_state.hpp"

namespace bitcask::oki {

enum class PutResult { kOk, kAlreadyExists, kIndexUnavailable };

struct AuthorityInfo {
    std::uint64_t key_count = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t next_ord = 0;
    std::uint32_t biggest_file_id = 0;
    bool frozen = false;
};

class IndexAuthority {
public:
    using CommitSnapshot = OkiCommitSnapshot;
    using FStats = OkiManifestFStats;

    [[nodiscard]] OkiState& state() noexcept { return state_; }
    [[nodiscard]] const OkiState& state() const noexcept { return state_; }

    void mark_ready() noexcept { ready_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_ready() const noexcept {
        return ready_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t alloc_ord() noexcept {
        return next_ord_.fetch_add(1, std::memory_order_relaxed);
    }
    void advance_ord(std::uint64_t ord) noexcept;
    [[nodiscard]] std::uint64_t peek_next_ord() const noexcept {
        return next_ord_.load(std::memory_order_acquire);
    }
    // fold 时代：set_pending_delete 记下标记当时的时代，时代越过它之后
    // 该文件才派生出 pending_delete（下一次 v4 提交发布）。时钟 =
    // max(next_ord, fold_floor_)：每次写（发 ord）自然推进；启动一次 fold
    // 只抬 fold_floor_，**绝不动 next_ord_**。
    //
    // ord 序列只属于真正的写：每个发出去的 ord 必须恰有一个任务进索引通道
    // （Add / Delete / Skip / RunFn），重排缓冲按 ord 严格连续 apply。6.9.0
    // ～ 7.0.0 这里曾用 advance_ord(peek_next_ord()) 借 ord 推进时代，每开
    // 一次迭代器凭空跳掉一个号，其后第一次写即令 flush_index 永久阻塞
    // （feedbacks/2026-11-16-flush-index-hangs-after-iter.md）。
    [[nodiscard]] std::uint64_t fold_epoch() const noexcept {
        return std::max(next_ord_.load(std::memory_order_acquire),
                        fold_floor_.load(std::memory_order_acquire));
    }
    void advance_fold_epoch() noexcept;
    [[nodiscard]] std::uint32_t biggest_file_id() const noexcept {
        return biggest_file_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint32_t increment_file_id() noexcept;
    [[nodiscard]] std::uint32_t
    increment_file_id_at_least(std::uint32_t id) noexcept;

    // 运行期数据已持久后的权威应用。所有方法在 writer_mu_ 内 probe+apply。
    [[nodiscard]] PutResult apply_put(std::string_view key,
                                      std::uint32_t file_id,
                                      std::uint32_t total_size,
                                      std::uint64_t offset,
                                      std::uint64_t tstamp,
                                      std::uint64_t ord);

    // S63：原子批的一次性应用入口。一次 writer_mu_ 内先裁决全部成员、再
    // 一次 OkiState::apply_batch 发布整批：并发读者要么看到整批，要么一行
    // 都看不到。items 内 key 必须两两不同（put_batch_atomic 的 final_ops
    // 已保证）。kAlreadyExists 时 rejected 返回被拒 put 成员的下标、零成员
    // 入账/入行；kIndexUnavailable 时同样零副作用。remove 成员沿用
    // apply_remove 的口径（不裁决、不进 rejected）。
    struct BatchApply {
        std::string_view key;
        bool remove = false;
        std::uint32_t file_id = 0; // remove：墓碑记录的落点（可为 0）
        std::uint32_t total_size = 0;
        std::uint64_t offset = 0;
        std::uint64_t tstamp = 0;
        std::uint64_t ord = 0;
    };
    [[nodiscard]] PutResult apply_batch(
        std::span<const BatchApply> items,
        std::vector<std::size_t>& rejected);

    // file_id/end：墓碑 data 记录的落点（供 flush 持久性判定与一致切面）。
    void apply_remove(std::string_view key, std::uint64_t ord,
                      std::uint32_t file_id = 0, std::uint64_t end = 0);
    [[nodiscard]] bool conditional_remove(std::string_view key,
                                           std::uint64_t tstamp,
                                           std::uint32_t file_id,
                                           std::uint64_t offset,
                                           std::uint64_t remove_time);
    [[nodiscard]] bool relocate(std::string_view key,
                                std::uint32_t old_file_id,
                                std::uint64_t old_offset,
                                std::uint32_t new_file_id,
                                std::uint32_t new_total_size,
                                std::uint64_t new_offset,
                                std::uint64_t tstamp,
                                std::uint64_t ord);

    // 恢复 tail / search delta 重放。调用方随后把行交给 OkiState。
    void account_recovery_row(std::string_view key, std::uint32_t file_id,
                              std::uint32_t total_size,
                              std::uint64_t tstamp, std::uint64_t ord,
                              bool tomb);
    void replay_row(std::string_view key, std::uint32_t file_id,
                    std::uint32_t total_size, std::uint64_t offset,
                    std::uint64_t tstamp, std::uint64_t ord);
    void replay_removal(std::string_view key, std::uint64_t tomb_ord);
    // data-only tail 重放（单线程恢复期）：与组合视图比对裁决——视图已有
    // 同 key 且 ord ≥ 本行 → 已覆盖/陈旧，跳过（不记账、不入行）；否则记账
    // 并**绕过 wm 门**入行。不再依赖 wm 门判定覆盖：wm 只是 ord 上界的近似，
    // 越过在途/持留行时会把真实 tail 挡在门外。
    void recover_row(std::string_view key, std::uint32_t file_id,
                     std::uint32_t total_size, std::uint64_t offset,
                     std::uint64_t tstamp, std::uint64_t ord, bool tomb);

    [[nodiscard]] bool adopt_manifest(const OkiManifest& manifest);
    // 全量重建：ledger 与 OkiState 一起换新。适用于 open 时 OKI 未加载、
    // 或运行期受控重建（旧视图已显式降级）。
    [[nodiscard]] bool rebuild_from_data(std::string_view dir) {
        return rebuild_from_data(dir, /*publish_state=*/1, /*may_write=*/true);
    }
    // 只重建权威账本，不发布新的 OkiState/manifest。open 收尾专用：
    // 健康已载视图（含待读路径暴露的坏行）必须保持原样；Level A 也只
    // 需要账本，不得在 open 期伪造 v4 元数据。
    // may_write=false（只读句柄）：外排改走内存，不在目录里落临时 run。
    [[nodiscard]] bool rebuild_ledger_from_data(std::string_view dir,
                                                bool may_write = true) {
        return rebuild_from_data(dir, /*publish_state=*/0, may_write);
    }
    // 最近一次 rebuild_ledger_from_data 的视图抽检结果：manifest 声明的各
    // 文件 data 水位是否属实。每个文件取水位之内的**最后一条**记录，tail
    // 补完后比对视图与 data 胜出版本——同一文件内记录按追加序应用到权威，
    // 末条在视图里 ⟹ 之前的都在。false = manifest 高估了覆盖（旧版本的
    // capture_commit_cut 缺陷会写出这种 manifest），调用方应从 data 全量重建。
    // 视图对抽检 key 不可判定（未开点查 / 坏块）时不作结论，记为 true。
    [[nodiscard]] bool last_ledger_view_consistent() const noexcept {
        return ledger_view_consistent_;
    }
    // 测试缝：apply_put 入口（取 writer_mu_ 之前）回调——用于把写者停在
    // 「data 已追加、尚未应用到权威」的窗口。须在任何写者运行前设置。
    void set_pre_apply_hook_for_test(std::function<void()> fn) {
        pre_apply_hook_ = std::move(fn);
    }
    // 测试缝：删掉某文件的已应用落点登记——模拟旧版本「新文件无登记项、
    // 一致切面不钳水位」的缺陷，用于构造高估覆盖的 manifest。
    void drop_applied_end_for_test(std::uint32_t file_id) {
        std::scoped_lock lock(writer_mu_);
        applied_end_.erase(file_id);
    }
    // 测试 / 诊断：最近一次抽检比对的 key 数。
    [[nodiscard]] std::size_t last_ledger_view_probes() const noexcept {
        return ledger_view_probes_;
    }
    // 只读目录/离线升级专用：完整账本 + 内存点查视图，不写 run/manifest。
    [[nodiscard]] bool rebuild_memory_from_data(std::string_view dir) {
        return rebuild_from_data(dir, /*publish_state=*/2, /*may_write=*/false);
    }
    [[nodiscard]] std::optional<CommitSnapshot>
    capture_commit_snapshot();
    // 一致切面提交快照：在 writer_mu_ 内调用 cut_fn 采集 data 字节水位
    // （caller 在其中做 fsync），随后按本进程已应用落点（applied_end）
    // 钳制各文件水位，再捕获账本与 cut_seq。三者同处一把写序闸门，保证
    //「水位之前的记录 ⟺ 账本已计入 ⟺ seq ≤ cut」。cut_fn 失败 → nullopt。
    using DataWms = std::vector<std::pair<std::uint32_t, std::uint64_t>>;
    [[nodiscard]] std::optional<std::pair<CommitSnapshot, DataWms>>
    capture_commit_cut(const std::function<std::optional<DataWms>()>& cut_fn);
    [[nodiscard]] AuthorityInfo info() const;
    [[nodiscard]] std::vector<FStats> fstats() const;
    void set_pending_delete(std::uint32_t file_id) noexcept;
    [[nodiscard]] std::uint32_t trim_fstats(std::span<const std::uint32_t> ids);
    void with_writer_lock(const std::function<void()>& fn);

    // S67：可能带「没写完的尾巴」的 data 文件（上一个写者崩溃时正在写的那个，或
    // 别的进程里仍在写的那个）。Cask 在 open 的恢复开始前按写锁文件的证据设置；
    // 不在其中的文件一律视为已封口（封口即 fsync）——从 data 重建时在它们中间
    // 遇到坏区会跳过去接着读，而不是把后面的记录当成尾巴丢掉。
    void set_unsealed_files(std::vector<std::uint32_t> ids);
    [[nodiscard]] bool may_have_torn_tail(std::uint32_t file_id) const;

private:
    struct Ledger {
        std::uint64_t active_keys = 0;
        std::uint64_t key_bytes = 0;
        std::unordered_map<std::uint32_t, FStats> files;
    };

    static void adjust_file(Ledger& l, std::uint32_t file_id,
                            std::uint64_t tstamp,
                            std::int64_t live_keys, std::int64_t total_keys,
                            std::int64_t live_bytes, std::int64_t total_bytes);
    static void retire_live(Ledger& l, const OkiState::WriteProbe& prior,
                            std::string_view key);
    static void install_live(Ledger& l, std::string_view key,
                             std::uint32_t file_id,
                             std::uint32_t total_size,
                             std::uint64_t tstamp, bool count_total);
    static bool snapshot_valid(const OkiManifestMetadata& m,
                               const std::vector<FStats>& files) noexcept;
    [[nodiscard]] std::optional<CommitSnapshot> capture_locked();
    // 前置：持 writer_mu_。按 prior 裁决恢复行对账本的影响。
    void account_locked(const OkiState::LocateResult& prior,
                        std::string_view key, std::uint32_t file_id,
                        std::uint32_t total_size, std::uint64_t tstamp,
                        std::uint64_t ord, bool tomb);
    void note_applied(std::uint32_t file_id, std::uint64_t end) {
        if (file_id == 0) return;
        auto& e = applied_end_[file_id];
        e = std::max(e, end);
    }
    [[nodiscard]] bool rebuild_from_data(std::string_view dir,
                                         int publish_state, bool may_write);

    OkiState state_;
    Ledger ledger_;
    mutable std::mutex writer_mu_;
    // 本进程经权威应用的各文件最大落点（记录末字节）。一致切面用它钳制
    // data 水位：已写入但尚未应用的在途记录必须落在水位之后（交给重放）。
    std::unordered_map<std::uint32_t, std::uint64_t> applied_end_;
    std::function<void()> pre_apply_hook_;  // 测试缝
    bool ledger_view_consistent_ = true;   // 见 last_ledger_view_consistent
    std::size_t ledger_view_probes_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> next_ord_{0};
    std::atomic<std::uint32_t> biggest_file_id_{0};
    // fold 时代下界（见 fold_epoch()）。只在内存：持久侧由 fstats 的
    // pending_delete 布尔 + expiration_epoch 还原（adopt_manifest）。
    // 放在末尾：不挪动既有成员的偏移。
    std::atomic<std::uint64_t> fold_floor_{0};
    // S67（同上，放在末尾）：见 set_unsealed_files。writer_mu_ 保护。
    std::vector<std::uint32_t> unsealed_files_;
};

} // namespace bitcask::oki
