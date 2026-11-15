// cask_iter.cpp — S43-6：OKI-only fold 迭代器。
// CaskIter 不再拍 authority shard 表，也不参与 pending/epoch 协调；其唯一
// 权威来源是创建时捕获的 OKI 组合视图（runs + memdelta）。值读取仍落回
// data 文件，并执行与 public get 相同的 key/ord 自证。
#include "bitcask/cask.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bitcask/codec.hpp"
#include "bitcask/detail/scanner.hpp"
#include "bitcask/format.hpp"

#include "cask_internal.hpp"

namespace bitcask {

namespace {

[[nodiscard]] CaskFault map_oki_error(const oki::OkiError& e,
                                      const char* where) {
    if (e == oki::OkiError::kIo) {
        return err(CaskError::kIo, where);
    }
    return err(CaskError::kBadCrc, where);
}

}  // namespace

CaskIter::~CaskIter() noexcept { release(); }

std::expected<void, CaskFault>
CaskIter::start(int /*maxage*/, int /*maxputs*/, std::uint64_t /*now_sec*/,
                bool see_tombstones, std::span<const std::byte> key_prefix) {
    if (parent_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    if (iterating_) {
        return std::unexpected(err(CaskError::kIo, "iter already started"));
    }

    auto view = parent_->authority_->state().make_read_view();
    if (!view) {
        // 旧 Level A / 运行期降级仍由 S43-4 的受控重建闸统一处理。
        // RO 句柄没有重建能力，显式要求重开可写；绝不退回 shard 表。
        if (!parent_->opts_.read_write || parent_->opts_.merge_only) {
            return std::unexpected(err(
                CaskError::kNoIndex,
                "oki unavailable on read-only handle; reopen read-write"));
        }
        if (!parent_->ensure_oki_for_write()) {
            if (parent_->oki_rebuild_failed_) {
                return std::unexpected(err(
                    CaskError::kIndexRebuildFailed,
                    "oki rebuild failed before fold"));
            }
            return std::unexpected(err(
                CaskError::kIndexCorrupt,
                "oki unavailable: controlled rebuild in progress"));
        }
        view = parent_->authority_->state().make_read_view();
    }
    if (!view) {
        return std::unexpected(err(CaskError::kNoIndex, "oki view unavailable"));
    }

    release();
    view_ = *std::move(view);
    see_tombstones_ = see_tombstones;
    key_prefix_.clear();
    if (!key_prefix.empty()) {
        key_prefix_.assign(reinterpret_cast<const char*>(key_prefix.data()),
                           key_prefix.size());
    }

    cursors_.reserve(view_.runs.size());
    heads_.reserve(view_.runs.size());
    const std::string lo_str = key_prefix_;
    const std::string_view lo(lo_str);
    for (const auto& run : view_.runs) {
        auto c = run->seek(key_prefix);
        if (!c) return std::unexpected(map_oki_error(c.error(), "oki seek"));
        oki::OkiRunReader::Entry head;
        auto n = c->next(head);
        if (!n) return std::unexpected(map_oki_error(n.error(), "oki read"));
        cursors_.push_back(std::move(*c));
        heads_.push_back(*n ? std::optional<oki::OkiRunReader::Entry>(
                                std::move(head))
                            : std::nullopt);
    }
    delta_pos_ = static_cast<std::size_t>(
        std::lower_bound(view_.delta->begin(), view_.delta->end(), lo,
                         [](const oki::OkiState::DeltaRow& row,
                            std::string_view bound) {
                             return row.key < bound;
                         }) -
        view_.delta->begin());
    cur_key_.clear();
    done_ = false;
    iterating_ = true;
    parent_->authority_->advance_fold_epoch();
    pin_files();
    return {};
}

// 与 Range 迭代器同一规则：key 升序，同 key 取 max(ord)，等 ord 时
// memdelta / 更高 gen 到达序胜。墓碑在归并层抵消活行。
std::expected<std::optional<oki::OkiState::DeltaRow>, CaskFault>
CaskIter::next_merged_key() {
    if (!iterating_ || done_) return std::optional<oki::OkiState::DeltaRow>{};

    while (true) {
        std::string_view min_key;
        bool any = false;
        bool have_run_winner = false;
        std::uint64_t run_ord = 0;
        std::size_t delta_pos = std::numeric_limits<std::size_t>::max();

        // 先按 key 分组找最小 key；runs 内到达序随 vector 顺序升序，等
        // ord 时后扫描者覆盖前者，天然实现“更高 gen 胜”。
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            const auto& head = heads_[i];
            if (!head) continue;
            const std::string_view key(head->key);
            if (!any || key < min_key) {
                min_key = key;
                any = true;
                have_run_winner = false;
                run_ord = 0;
            }
            if (key == min_key &&
                (!have_run_winner || head->ord >= run_ord)) {
                run_ord = head->ord;
                have_run_winner = true;
            }
        }
        if (delta_pos_ < view_.delta->size()) {
            const std::string_view key((*view_.delta)[delta_pos_].key);
            if (!any || key < min_key) {
                min_key = key;
                any = true;
                have_run_winner = false;
                run_ord = 0;
            }
            if (key == min_key) delta_pos = delta_pos_;
        }
        if (!any) {
            done_ = true;
            return std::optional<oki::OkiState::DeltaRow>{};
        }

        // 推进 run cursor 会复用/改写 head.key，因此最小 key 必须先物化，
        // 否则分组和后续行裁决可能读到下一 key。
        const std::string current(min_key);
        cur_key_ = current;
        oki::OkiState::DeltaRow winner;
        bool have_winner = false;

        for (std::size_t i = 0; i < heads_.size(); ++i) {
            auto& head = heads_[i];
            if (!head || std::string_view(head->key) != current) continue;
            if (!have_winner || head->ord >= winner.ord) {
                winner.key = head->key;
                winner.ord = head->ord;
                winner.tomb = head->tomb;
                winner.has_loc = head->has_loc;
                winner.loc = head->loc;
                have_winner = true;
            }
            head.emplace();
            auto n = cursors_[i].next(*head);
            if (!n) return std::unexpected(map_oki_error(n.error(), "oki read"));
            if (!*n) head.reset();
        }
        const bool delta_same_key =
            delta_pos != std::numeric_limits<std::size_t>::max();
        if (delta_same_key) {
            const auto& row = (*view_.delta)[delta_pos];
            if (!have_winner || row.ord >= winner.ord) winner = row;
            have_winner = true;
            delta_pos_ = delta_pos + 1;
        }
        if (!have_winner || winner.key != current) {
            return std::unexpected(err(CaskError::kIndexCorrupt,
                                       "oki merge winner mismatch"));
        }
        if (winner.tomb) continue;
        return std::optional<oki::OkiState::DeltaRow>(winner);
    }
}

std::expected<std::optional<CaskIter::Entry>, CaskFault> CaskIter::next() {
    if (!iterating_) return std::optional<Entry>{};
    const auto expiry = parent_->opts_.expiry_secs;
    const auto now = (expiry > 0) ? now_sec_default() : 0;

    while (true) {
        auto merged = next_merged_key();
        if (!merged) return std::unexpected(merged.error());
        if (!*merged) return std::optional<Entry>{};
        if (!key_prefix_.empty() &&
            (cur_key_.size() < key_prefix_.size() ||
             std::memcmp(cur_key_.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            done_ = true;
            return std::optional<Entry>{};
        }

        oki::OkiState::DeltaRow row = **merged;
        if (row.tomb && !see_tombstones_) continue;
        if (!row.tomb && !row.has_loc) {
            return std::unexpected(err(
                CaskError::kIndexCorrupt, "v1 OKI row cannot expose value"));
        }
        if (expiry > 0 &&
            static_cast<std::uint64_t>(row.loc.tstamp) + expiry <= now) {
            continue;
        }

        Entry e;
        const auto* kp = reinterpret_cast<const std::byte*>(cur_key_.data());
        e.key.assign(kp, kp + cur_key_.size());
        e.tstamp = row.loc.tstamp;
        e.file_id = row.loc.file_id;
        e.offset = row.loc.offset;
        e.total_sz = row.loc.total_sz;
        e.is_tombstone = row.tomb;
        e.ord = row.ord;
        if (row.tomb) {
            e.value.clear();
            e.value.shrink_to_fit();
            return std::optional<Entry>{std::move(e)};
        }

        // 只按视图位置读原始记录；key/ord 核对沿用 S43-1 点查契约。
        // 陈旧 loc 在 merge 窗口里可短暂缺失，重查 OKI 后重试一次。
        bool retry = false;
        fileops::DataFile* df = nullptr;
        std::shared_ptr<fileops::DataFile> shared_df;
        if (auto it = pinned_files_.find(e.file_id); it != pinned_files_.end()) {
            df = it->second.get();
        } else {
            shared_df = parent_->read_file(e.file_id);
            df = shared_df.get();
        }
        if (!df) {
            if (!parent_->is_closed()) retry = true;
            else return std::unexpected(err(
                CaskError::kIo,
                "open read file_id=" + std::to_string(e.file_id)));
        } else {
            auto rec = df->read(e.offset, e.total_sz);
            if (!rec) {
                if (!parent_->is_closed()) retry = true;
                else {
                    switch (rec.error().kind) {
                        case fileops::DataFileError::kBadCrc:
                            return std::unexpected(err(CaskError::kBadCrc));
                        case fileops::DataFileError::kIo:
                            return std::unexpected(io_fault(rec.error().errnum));
                        default:
                            return std::unexpected(err(CaskError::kIo, "read"));
                    }
                }
            } else {
                const bool key_matches =
                    rec->key.size() == cur_key_.size() &&
                    std::memcmp(rec->key.data(), cur_key_.data(),
                                cur_key_.size()) == 0;
                if (!key_matches || rec->ord != e.ord) {
                    return std::unexpected(err(
                        CaskError::kIndexCorrupt,
                        "located record key/ord mismatch during fold"));
                }
                const bool value_is_tomb =
                    rec->type == format::RecordType::kTombstone;
                if (value_is_tomb) {
                    if (!see_tombstones_) continue;
                    e.is_tombstone = true;
                    e.value = std::move(rec->value);
                    return std::optional<Entry>{std::move(e)};
                }
                auto dv = codec::decode_doc_value(
                    std::span<const std::byte>(rec->value));
                if (!dv) {
                    return std::unexpected(err(CaskError::kIo,
                                               "corrupt DocValue"));
                }
                if (dv->expiry_at != 0 &&
                    dv->expiry_at <= now_sec_default()) {
                    continue;
                }
                if (filter_ &&
                    (dv->meta.empty() || !filter_->evaluate(dv->meta))) {
                    continue;
                }
                e.value.assign(dv->text.begin(), dv->text.end());
                if (want_meta_) e.meta.assign(dv->meta.begin(), dv->meta.end());
                return std::optional<Entry>{std::move(e)};
            }
        }

        if (!retry) continue;
        auto refreshed = parent_->authority_->state().locate(cur_key_);
        if (refreshed.status != oki::OkiState::LocateStatus::kHit ||
            refreshed.tomb || !refreshed.has_loc || refreshed.ord == e.ord) {
            return std::unexpected(err(CaskError::kIo,
                                       "fold location disappeared"));
        }
    }
}

std::expected<std::vector<CaskIter::Entry>, CaskFault>
CaskIter::next_batch(std::size_t max_n) {
    std::vector<Entry> batch;
    batch.reserve(max_n);
    for (std::size_t i = 0; i < max_n; ++i) {
        auto r = next();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) break;
        batch.push_back(std::move(**r));
    }
    return batch;
}

CaskIter::FlatKeys CaskIter::drain_live_keys() {
    FlatKeys out;
    if (!iterating_) return out;
    out.offs.push_back(0);
    while (true) {
        auto row = next_merged_key();
        if (!row || !*row) break;  // parallel_scan 随后 get 会显式暴露读错误
        const auto& key = (*row)->key;
        if (!key_prefix_.empty() &&
            (key.size() < key_prefix_.size() ||
             std::memcmp(key.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            break;
        }
        const auto* p = reinterpret_cast<const std::byte*>(key.data());
        out.buf.insert(out.buf.end(), p, p + key.size());
        out.offs.push_back(out.buf.size());
    }
    done_ = true;
    return out;
}

void CaskIter::pin_files() {
    pinned_files_.clear();
    auto scan = fileops::scan_dir(parent_->dirname_);
    if (!scan) return;
    bool has_active = false;
    std::uint32_t active_fid = 0;
    {
        std::shared_lock lk(parent_->read_cache_mu_);
        has_active = static_cast<bool>(parent_->active_data_);
        active_fid = parent_->active_file_id_.load(std::memory_order_relaxed);
    }
    for (const auto& entry : *scan) {
        const auto fid = static_cast<std::uint32_t>(entry.tstamp);
        if (has_active && fid == active_fid) continue;
        auto df = fileops::DataFile::open(
            entry.data_path, fileops::DataFile::Mode::kRead,
            /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) continue;
        pinned_files_.emplace(fid, std::make_unique<fileops::DataFile>(
                                       std::move(*df)));
    }
}

void CaskIter::release() noexcept {
    iterating_ = false;
    done_ = true;
    cursors_.clear();
    heads_.clear();
    view_ = {};
    cur_key_.clear();
    delta_pos_ = 0;
    pinned_files_.clear();
}

}  // namespace bitcask
