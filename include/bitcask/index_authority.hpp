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
    // fold 时代复用 next_ord 高水位：启动一次 fold 必须推进一个时代，
    // 让 set_pending_delete 标记的文件在下一次 v4 提交变成可安全删除。
    void advance_fold_epoch() noexcept {
        advance_ord(peek_next_ord());
    }
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
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> next_ord_{0};
    std::atomic<std::uint32_t> biggest_file_id_{0};
};

} // namespace bitcask::oki
