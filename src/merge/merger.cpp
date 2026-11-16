#include "bitcask/merger.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/format.hpp"
#include "bitcask/oki_merge_cursor.hpp"
#include "bitcask/oki_state.hpp"
#include "bitcask/plugin_api.hpp"  // S18-7：merge 参与协议（设计 §3.9）
#include "bitcask/detail/path_utf8.hpp"
#include "bitcask/detail/scanner.hpp"

namespace bitcask::merge {

namespace {

MergeFault io_fault(MergeError kind, int errnum, std::string detail = {}) {
    return MergeFault{kind, errnum, std::move(detail)};
}

// 延迟应用项：选中 OKI 行只写新文件 + 记录变更，不立即提交 relocation。
// 输出 fsync 成功后才统一 apply——保证 merge 失败时索引权威完全未动。
struct PendingUpdate {
    std::string    key;
    std::uint32_t  old_file_id;
    std::uint64_t  old_offset;
    std::uint32_t  new_total_size;
    std::uint64_t  new_offset;
    std::uint64_t  tstamp;
    std::uint64_t  ord;
};

// S43-7：OKI-driven merge。先钉住 runs + memdelta 的一致 ReadView，按
// key 归并出权威行；只把位置仍落在候选输入的行读回并复制。输出是纯
// data 文件；提交仍走 authority 条件 CAS（内部在分片锁内调 OKI relocation
// 挂钩），避免旧 ReadView 与并发写者之间的 TOCTOU。
class MergeRunner {
public:
    MergeRunner(oki::IndexAuthority& authority, bool sync_output,
                std::span<plugin::CaskPlugin* const> plugins,
                std::uint64_t now_sec)
        : authority_(authority), oki_(authority.state()), sync_output_(sync_output),
          plugins_(plugins), now_sec_(now_sec) {}

    std::expected<MergeStats, MergeFault>
    run(std::span<const std::string> input_data_paths,
        std::string_view output_dir);

private:
    // S13-P8：pending 分批 apply。每个 OKI key 批边界兜住巨型合并，
    // 批 apply 前先 flush + fsync（fsync-before-index-commit 契约）。
    static constexpr std::size_t kApplyBatch = 1u << 18;  // 256K 条/批

    [[nodiscard]] std::expected<std::optional<oki::OkiState::DeltaRow>,
                                MergeFault>
    next_oki_row();

    [[nodiscard]] std::expected<void, MergeFault> apply_pending();

    [[nodiscard]] std::expected<void, MergeFault>
    copy_oki_record(const oki::OkiState::DeltaRow& row,
                    fileops::DataFile& source);

    // 输入文件里的墓碑保留到输出（见 run() 中的说明）。
    [[nodiscard]] std::expected<void, MergeFault>
    copy_tombstones(std::unordered_map<std::uint32_t, fileops::DataFile>& sources);

    void cleanup_partial_outputs() {
        std::error_code ec;
        std::filesystem::remove(
            bitcask::detail::from_utf8(stats_.output_data_path), ec);
    }

    // 失败清理（分批版）：任何批 apply 过之后，输出已被索引引用——
    // 绝不能删；只在零 apply 时清残件。
    void fail_cleanup() {
        if (!any_applied_) cleanup_partial_outputs();
    }

    oki::IndexAuthority&            authority_;
    oki::OkiState&             oki_;
    bool                       sync_output_;
    std::span<plugin::CaskPlugin* const> plugins_;
    std::uint64_t              now_sec_;

    MergeStats                                 stats_;
    std::optional<fileops::DataFile>           out_data_;
    std::vector<PendingUpdate>                 pending_;
    oki::OkiState::ReadView                    oki_view_;

    std::optional<oki::OkiMergeCursor>         merged_;
    bool                                       any_applied_ = false;
    // 目录内存在输入集之外的 data 文件 ⟹ 其中可能有同 key 的更旧记录，
    // 删除墓碑/过期记录后它们会在 data 重建时复活。此时墓碑必须随输出保留。
    bool                                       keep_tombs_ = false;
};

std::expected<void, MergeFault> MergeRunner::apply_pending() {
    if (pending_.empty()) return {};
    if (auto f = out_data_->flush_batch(); !f) {
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                        f.error().errnum,
                                        stats_.output_data_path));
    }
    if (auto sy = out_data_->sync(); !sy) {
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                        sy.error().errnum,
                                        stats_.output_data_path));
    }
    for (const auto& u : pending_) {
        auto pr = authority_.relocate(
            u.key, u.old_file_id, u.old_offset, stats_.output_file_id,
            u.new_total_size, u.new_offset, u.tstamp, u.ord);
        if (pr) {
            // S18-7：搬迁事件广播。分批 apply 后源值不在手，事件里的
            // value 仍按设计保持可选便利字段语义。
            const plugin::RelocateEvent ev{
                u.ord, u.key,
                plugin::RecordLoc{stats_.output_file_id, u.new_offset,
                                  u.new_total_size},
                std::string_view{}};
            for (auto* p : plugins_) p->on_relocate(ev);
            continue;
        }

        // CAS 被拒 = pinned ReadView 之后该 key 已被并发写者更新，输入
        // 中的旧 record 自然死亡。stuck 复查仍走 locate；组合视图如果
        // 还指旧位置，caller 必须保留输入文件。
        stats_.records_stale += 1;
        auto cur = oki_.locate(u.key);
        if (cur.status == oki::OkiState::LocateStatus::kHit && cur.has_loc &&
            cur.loc.file_id == u.old_file_id &&
            cur.loc.offset == u.old_offset) {
            stats_.relocations_stuck += 1;
            stats_.stuck_file_ids.push_back(u.old_file_id);
        }
    }
    pending_.clear();
    any_applied_ = true;
    return {};
}

// runs 与 memdelta 的 key 归并（统一规则见 OkiMergeCursor）：同 key 只
// 返回一条权威活行，墓碑在归并层抵消。
std::expected<std::optional<oki::OkiState::DeltaRow>, MergeFault>
MergeRunner::next_oki_row() {
    oki::OkiState::DeltaRow row;
    auto n = merged_->next(row);
    if (!n) {
        return std::unexpected(io_fault(MergeError::kInputReadFailed, 0,
                                        "oki run cursor read failed"));
    }
    if (!*n) return std::optional<oki::OkiState::DeltaRow>{};
    return std::optional<oki::OkiState::DeltaRow>(std::move(row));
}

std::expected<void, MergeFault>
MergeRunner::copy_oki_record(const oki::OkiState::DeltaRow& row,
                             fileops::DataFile& source) {
    auto rec = source.read(row.loc.offset, row.loc.total_sz);
    if (!rec) {
        return std::unexpected(io_fault(MergeError::kInputReadFailed,
                                        rec.error().errnum,
                                        std::string(source.path())));
    }

    // S43-1 契约：索引定位的 record 必须自证 key 与 ord。这里再加
    // tstamp/total_size，杜绝把陈旧或坏块记录搬运成新权威。
    const std::string_view rec_key(
        reinterpret_cast<const char*>(rec->key.data()), rec->key.size());
    if (rec_key != row.key || rec->ord != row.ord ||
        rec->tstamp != row.loc.tstamp ||
        rec->total_size != row.loc.total_sz ||
        rec->type != format::RecordType::kDoc) {
        return std::unexpected(io_fault(
            MergeError::kInputReadFailed, 0,
            "merge source record does not match OKI row: " +
            std::string(source.path())));
    }

    stats_.records_seen += 1;
    if (now_sec_ != 0) {
        auto dv = codec::decode_doc_value(rec->value);
        if (dv && dv->expiry_at != 0 && dv->expiry_at <= now_sec_) {
            stats_.records_expired += 1;
            const bool removed = authority_.conditional_remove(
                row.key, rec->tstamp, row.loc.file_id, row.loc.offset,
                now_sec_);
            // 过期记录随输入删除；若别处还有同 key 的更旧版本，必须写一条
            // 墓碑（受害者 ord）背书本次删除，否则 data 重建会让旧版本复活。
            if (removed && keep_tombs_) {
                auto tw = out_data_->write_buffered(
                    format::RecordType::kTombstone, now_sec_, rec->ord,
                    rec->key, std::span<const std::byte>{});
                if (!tw) {
                    return std::unexpected(io_fault(
                        MergeError::kOutputWriteFailed, tw.error().errnum,
                        stats_.output_data_path));
                }
                stats_.bytes_written += tw->total_size;
            }
            return {};
        }
    }

    auto w = out_data_->write_buffered(format::RecordType::kDoc,
                                       rec->tstamp, rec->ord,
                                       rec->key, rec->value);
    if (!w) {
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                        w.error().errnum,
                                        stats_.output_data_path));
    }
    pending_.push_back(PendingUpdate{
        row.key,
        row.loc.file_id, row.loc.offset,
        w->total_size, w->offset,
        rec->tstamp, rec->ord,
    });
    stats_.records_kept += 1;
    stats_.bytes_written += rec->total_size;

    if (pending_.size() >= kApplyBatch) {
        if (auto ap = apply_pending(); !ap) return std::unexpected(ap.error());
    }
    return {};
}

std::expected<void, MergeFault> MergeRunner::copy_tombstones(
    std::unordered_map<std::uint32_t, fileops::DataFile>& sources) {
    struct Tomb {
        std::uint64_t ord = 0;
        std::uint64_t tstamp = 0;
        std::vector<std::byte> value;
    };
    // 同 key 只保留输入里 ord 最大的一条墓碑。
    std::unordered_map<std::string, Tomb> tombs;
    for (auto& [fid, df] : sources) {
        auto folded = df.fold(
            [&](const codec::DataRecordView& v, std::uint64_t,
                std::uint32_t) {
                if (v.type != format::RecordType::kTombstone) return;
                std::string k(reinterpret_cast<const char*>(v.key.data()),
                              v.key.size());
                auto [it, ins] = tombs.try_emplace(std::move(k));
                if (ins || v.ord > it->second.ord) {
                    it->second.ord = v.ord;
                    it->second.tstamp = v.tstamp;
                    it->second.value.assign(v.value.begin(), v.value.end());
                }
            },
            /*tolerate_crc_errors=*/true);
        if (!folded) {
            return std::unexpected(io_fault(MergeError::kInputReadFailed,
                                            folded.error().errnum,
                                            std::string(df.path())));
        }
    }
    for (const auto& [key, t] : tombs) {
        // 组合视图里已有更新的行（新 put 或更新的墓碑）⟹ 本墓碑在 data
        // 重建中必然被压过，可以安全丢弃；其余（含视图里墓碑已被 run 归并
        // 丢弃的 miss）一律保留。
        const auto cur = oki_.locate(key);
        if (cur.status == oki::OkiState::LocateStatus::kHit &&
            cur.ord > t.ord) {
            stats_.records_tombs += 1;
            continue;
        }
        const std::span<const std::byte> kspan(
            reinterpret_cast<const std::byte*>(key.data()), key.size());
        auto w = out_data_->write_buffered(format::RecordType::kTombstone,
                                           t.tstamp, t.ord, kspan, t.value);
        if (!w) {
            return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                            w.error().errnum,
                                            stats_.output_data_path));
        }
        stats_.bytes_written += w->total_size;
    }
    return {};
}

std::expected<MergeStats, MergeFault>
MergeRunner::run(std::span<const std::string> input_data_paths,
                 std::string_view output_dir) {
    // S43-7：合并语义只在健康 OKI 可用时启动。kUnavailable/未加载不能
    // 被当作“没有活 key”；受控重建或重开是 caller 的恢复闸。
    auto view = oki_.make_read_view();
    if (!view || !oki_.point_query_enabled()) {
        return std::unexpected(io_fault(
            MergeError::kInputOpenFailed, 0,
            "oki unavailable for merge; refuse to classify as empty"));
    }
    oki_view_ = std::move(*view);

    std::unordered_set<std::uint32_t> input_ids;
    std::unordered_map<std::uint32_t, fileops::DataFile> sources;
    input_ids.reserve(input_data_paths.size());
    sources.reserve(input_data_paths.size());
    for (const auto& path : input_data_paths) {
        const auto in_tstamp = fileops::parse_data_tstamp(path);
        if (!in_tstamp) {
            return std::unexpected(io_fault(
                MergeError::kInputOpenFailed, 0,
                "merge input is not a data file: " + path));
        }
        const auto fid = static_cast<std::uint32_t>(*in_tstamp);
        if (!input_ids.insert(fid).second) {
            return std::unexpected(io_fault(
                MergeError::kInputOpenFailed, 0,
                "duplicate merge input file_id " + std::to_string(fid)));
        }
        auto df = fileops::DataFile::open(path,
                                           fileops::DataFile::Mode::kRead,
                                           /*sync*/ false,
                                           /*mmap_enabled*/ false);
        if (!df) {
            return std::unexpected(io_fault(MergeError::kInputOpenFailed,
                                            df.error().errnum, path));
        }
        sources.emplace(fid, std::move(*df));
    }

    // 先把 OKI 游标全部就位，再创建输出。任何视图/输入读取失败都不会
    // 消耗 output file_id 或留下 data 残件。
    {
        auto mc = oki::OkiMergeCursor::open(oki_view_);
        if (!mc) {
            return std::unexpected(io_fault(
                MergeError::kInputReadFailed, 0,
                "oki run cursor read failed"));
        }
        merged_.emplace(std::move(*mc));
    }

    {
        auto all = fileops::scan_dir(std::string(output_dir));
        if (!all) {
            return std::unexpected(io_fault(MergeError::kInputOpenFailed,
                                            all.error().errnum,
                                            std::string(output_dir)));
        }
        for (const auto& e : *all) {
            if (!input_ids.contains(static_cast<std::uint32_t>(e.tstamp))) {
                keep_tombs_ = true;
                break;
            }
        }
    }

    stats_.output_file_id = authority_.increment_file_id();
    stats_.output_data_path =
        fileops::mk_data_filename(output_dir, stats_.output_file_id);
    stats_.output_hint_path.clear();  // S43-7：merge 输出 data-only。

    {
        std::uint64_t est_rows = 0;
        for (const auto& run : oki_view_.runs) {
            est_rows += run->entry_count();
        }
        est_rows += oki_view_.delta->size();
        if (est_rows > 0) {
            pending_.reserve(static_cast<std::size_t>(
                std::min<std::uint64_t>(est_rows, kApplyBatch)));
        }
    }

    auto od = fileops::DataFile::open(stats_.output_data_path,
                                       fileops::DataFile::Mode::kCreate,
                                       sync_output_);
    if (!od) {
        cleanup_partial_outputs();
        return std::unexpected(io_fault(MergeError::kOutputOpenFailed,
                                        od.error().errnum,
                                        stats_.output_data_path));
    }
    out_data_.emplace(std::move(*od));

    std::expected<std::optional<oki::OkiState::DeltaRow>, MergeFault> next;
    for (;;) {
        next = next_oki_row();
        if (!next.has_value()) break;
        if (!*next) break;
        const auto& row = **next;
        if (!row.has_loc) {
            return std::unexpected(io_fault(
                MergeError::kInputReadFailed, 0,
                "live OKI row has no data location"));
        }
        if (!input_ids.contains(row.loc.file_id)) continue;
        auto it = sources.find(row.loc.file_id);
        if (it == sources.end()) {
            return std::unexpected(io_fault(
                MergeError::kInputOpenFailed, 0,
                "merge source handle missing for file_id " +
                std::to_string(row.loc.file_id)));
        }
        if (auto copied = copy_oki_record(row, it->second); !copied) {
            fail_cleanup();
            return std::unexpected(copied.error());
        }
    }
    if (!next.has_value()) {
        fail_cleanup();
        return std::unexpected(next.error());
    }
    // 墓碑保留：原实现只复制活行，输入里的墓碑随输入删除——同 key 的旧
    // put 若在未参与合并的文件里，任何一次 data 重建（manifest 损坏、
    // Level A 重开、运行期受控重建）都会让已删除的 key 复活。
    if (keep_tombs_) {
        if (auto t = copy_tombstones(sources); !t) {
            fail_cleanup();
            return std::unexpected(t.error());
        }
    }

    if (auto ap = apply_pending(); !ap) {
        fail_cleanup();
        return std::unexpected(ap.error());
    }
    if (auto f = out_data_->flush_batch(); !f) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                        f.error().errnum,
                                        stats_.output_data_path));
    }
    if (auto s = out_data_->sync(); !s) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                        s.error().errnum,
                                        stats_.output_data_path));
    }
    if (auto ap = apply_pending(); !ap) {
        fail_cleanup();
        return std::unexpected(ap.error());
    }

    std::sort(stats_.stuck_file_ids.begin(), stats_.stuck_file_ids.end());
    stats_.stuck_file_ids.erase(
        std::unique(stats_.stuck_file_ids.begin(),
                    stats_.stuck_file_ids.end()),
        stats_.stuck_file_ids.end());
    return stats_;
}

}  // namespace

std::expected<MergeStats, MergeFault>
run_merge(std::span<const std::string> input_data_paths,
          std::string_view output_dir,
          oki::IndexAuthority& authority,
          bool sync_output,
          std::span<plugin::CaskPlugin* const> plugins,
          std::uint64_t now_sec) {
    MergeRunner runner(authority, sync_output, plugins, now_sec);
    {
        const plugin::MergeBeginEvent ev{{}, authority.peek_next_ord()};
        for (auto* p : plugins) p->on_merge_begin(ev);
    }
    auto result = runner.run(input_data_paths, output_dir);
    if (result) {
        const std::uint32_t out_id = result->output_file_id;
        const plugin::MergeCommitEvent ev{{&out_id, 1}, 0.0};
        for (auto* p : plugins) p->on_merge_commit(ev);
    } else {
        for (auto* p : plugins) p->on_merge_abort();
    }
    return result;
}

}  // namespace bitcask::merge
