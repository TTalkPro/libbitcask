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

    {
        auto mc = oki::OkiMergeCursor::open(view_, key_prefix);
        if (!mc) return std::unexpected(map_oki_error(mc.error(), "oki seek"));
        merged_.emplace(std::move(*mc));
    }
    cur_key_.clear();
    done_ = false;
    iterating_ = true;
    // 只推进 fold 时代（独立下界），不占 ord：ord 空洞会卡死索引通道。
    parent_->authority_->advance_fold_epoch();
    pin_files();
    return {};
}

// 归并规则统一由 OkiMergeCursor 实现（与 Range 迭代器、merge、run 归并同一份）。
std::expected<std::optional<oki::OkiState::DeltaRow>, CaskFault>
CaskIter::next_merged_key() {
    if (!iterating_ || done_ || !merged_) {
        return std::optional<oki::OkiState::DeltaRow>{};
    }
    oki::OkiState::DeltaRow row;
    // S75 顺带修复：see_tombstones 须透传到归并层，否则墓碑在归并层就被
    // 抵消，全表迭代器的 see_tombstones=true 形同虚设（下一行的过滤只对
    // 透传出来的墓碑生效）。
    auto n = merged_->next(row, see_tombstones_);
    if (!n) return std::unexpected(map_oki_error(n.error(), "oki read"));
    if (!*n) {
        done_ = true;
        return std::optional<oki::OkiState::DeltaRow>{};
    }
    cur_key_ = row.key;
    return std::optional<oki::OkiState::DeltaRow>(std::move(row));
}

std::expected<bool, CaskFault> CaskIter::next_row(RowView& e) {
    if (!iterating_) return false;
    const auto expiry = parent_->opts_.expiry_secs;
    const auto now = (expiry > 0) ? now_sec_default() : 0;

    while (true) {
        auto merged = next_merged_key();
        if (!merged) return std::unexpected(merged.error());
        if (!*merged) return false;
        if (!key_prefix_.empty() &&
            (cur_key_.size() < key_prefix_.size() ||
             std::memcmp(cur_key_.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            done_ = true;
            return false;
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

        const auto* kp = reinterpret_cast<const std::byte*>(cur_key_.data());
        e.key = std::span<const std::byte>(kp, cur_key_.size());
        e.key_from_cursor = true;
        e.tstamp = row.loc.tstamp;
        e.file_id = row.loc.file_id;
        e.offset = row.loc.offset;
        e.total_sz = row.loc.total_sz;
        e.is_tombstone = row.tomb;
        e.ord = row.ord;
        if (row.tomb) {
            e.value = {};
            return true;
        }

        // 只按视图位置读原始记录；key/ord 核对沿用 S43-1 点查契约。
        // 陈旧 loc 在 merge 窗口里可短暂缺失，重查 OKI 后重试一次。
        bool healed = false;  // S59：本 key 已为「文件被别的进程合并删掉」重建过
        int relocations = 0;  // S59：本 key 按重查位置重读的次数（有界）
    read_at_loc:
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
            // S71：先走映射读（封口文件 P6 整文件映射、活跃文件 S65 只读映射），
            // 不发系统调用、不拷贝整条记录——与点查 read_doc 同一条路径；界外
            // （kShortRead）或没有映射时才退回 pread。此前每行一次 pread 加两份
            // 拷贝，全表扫描比 range 慢 2～3 倍（feedbacks/2026-11-23-keydir-scan-…）。
            std::optional<codec::DataRecordView> view;
            std::optional<fileops::ReadRecord> owned;
            if (df->mmapped()) {
                auto rv = df->read_mmap(e.offset, e.total_sz);
                if (rv) {
                    view = *rv;
                } else if (rv.error().kind == fileops::DataFileError::kBadCrc) {
                    return std::unexpected(err(CaskError::kBadCrc));
                }
            }
            if (!view) {
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
                    owned = std::move(*rec);
                }
            }
            if (view || owned) {
                const std::span<const std::byte> rkey =
                    view ? view->key : std::span<const std::byte>(owned->key);
                const std::span<const std::byte> rval =
                    view ? view->value : std::span<const std::byte>(owned->value);
                const format::RecordType rtype = view ? view->type : owned->type;
                const std::uint64_t rord = view ? view->ord : owned->ord;
                const bool key_matches =
                    rkey.size() == cur_key_.size() &&
                    std::memcmp(rkey.data(), cur_key_.data(),
                                cur_key_.size()) == 0;
                if (!key_matches || rord != e.ord) {
                    return std::unexpected(err(
                        CaskError::kIndexCorrupt,
                        "located record key/ord mismatch during fold"));
                }
                if (rtype == format::RecordType::kTombstone) {
                    if (!see_tombstones_) continue;
                    e.is_tombstone = true;
                    e.value = owned ? std::span<const std::byte>(owned->value)
                                    : rval;
                    e.key = rkey;
                    e.key_from_cursor = false;
                    e.file_holder = shared_df;
                    if (owned) e.owned = std::move(*owned);
                    return true;
                }
                auto dv = codec::decode_doc_value(rval);
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
                // S75：span 直达映射（mmap）或 owned 记录（pread 兜底），不拷。
                e.key = rkey;
                e.key_from_cursor = false;
                e.value = dv->text;
                if (want_meta_) e.meta = dv->meta;
                e.file_holder = shared_df;
                if (owned) e.owned = std::move(*owned);
                return true;
            }
        }

        if (!retry) continue;
        auto refreshed = parent_->authority_->state().locate(cur_key_);
        // S59：同一版本仍指向已不存在的文件 ⇒ 跨进程 merge_only 旁车合并过、
        // 本句柄索引不知道那次搬迁。从 data 重建一次后按新位置重读（搬迁不改
        // ord；迭代器视图固定在旧 run 上，所以按重查结果读，不回视图）。
        const auto same_version = [&] {
            return refreshed.status == oki::OkiState::LocateStatus::kHit &&
                   !refreshed.tomb && refreshed.has_loc && refreshed.ord == e.ord;
        };
        if (!healed && same_version() && refreshed.loc.file_id == e.file_id &&
            refreshed.loc.offset == e.offset &&
            parent_->heal_stale_location(e.file_id)) {
            healed = true;
            refreshed = parent_->authority_->state().locate(cur_key_);
        }
        // 同一版本、位置已变（本 key 刚重建过，或前面的 key 触发的重建已把它
        // 搬到合并输出）⇒ 按新位置重读。迭代器视图固定在旧 run 上，后续 key
        // 拿到的仍是旧位置，都会走这里。
        if (same_version() && relocations < 2 &&
            (refreshed.loc.file_id != e.file_id || refreshed.loc.offset != e.offset)) {
            ++relocations;
            e.file_id = refreshed.loc.file_id;
            e.offset = refreshed.loc.offset;
            e.total_sz = refreshed.loc.total_sz;
            goto read_at_loc;  // 按新位置重读同一条记录
        }
        if (refreshed.status != oki::OkiState::LocateStatus::kHit ||
            refreshed.tomb || !refreshed.has_loc || refreshed.ord == e.ord) {
            return std::unexpected(err(CaskError::kIo,
                                       "fold location disappeared"));
        }
    }
}

std::expected<std::optional<CaskIter::Entry>, CaskFault> CaskIter::next() {
    reset_view_batch();  // S75：推进调用使上一批视图失效（契约见 next_batch_view）
    RowView rv;
    auto produced = next_row(rv);
    if (!produced) return std::unexpected(produced.error());
    if (!*produced) return std::optional<Entry>{};
    Entry e;
    e.key.assign(rv.key.begin(), rv.key.end());
    e.value.assign(rv.value.begin(), rv.value.end());
    if (want_meta_) e.meta.assign(rv.meta.begin(), rv.meta.end());
    e.tstamp = rv.tstamp;
    e.file_id = rv.file_id;
    e.offset = rv.offset;
    e.total_sz = rv.total_sz;
    e.is_tombstone = rv.is_tombstone;
    e.ord = rv.ord;
    return std::optional<Entry>{std::move(e)};
}

void CaskIter::reset_view_batch() noexcept {
    view_entries_.clear();
    view_files_.clear();
    view_owned_.clear();
    view_scratch_keys_.clear();
    view_key_buf_.clear();  // 只 clear 不 shrink：容量跨批复用
}

std::expected<std::span<const CaskIter::EntryView>, CaskFault>
CaskIter::next_batch_view(std::size_t max_n) {
    reset_view_batch();
    std::size_t n = 0;
    while (n < max_n) {
        RowView rv;
        auto produced = next_row(rv);
        if (!produced) {
            reset_view_batch();  // 错误即整批作废
            return std::unexpected(produced.error());
        }
        if (!*produced) break;
        view_entries_.emplace_back();
        EntryView& ev = view_entries_.back();
        if (rv.key_from_cursor) {
            view_scratch_keys_.push_back(
                {n, view_key_buf_.size(), rv.key.size()});
            view_key_buf_.append(reinterpret_cast<const char*>(rv.key.data()),
                                 rv.key.size());
        } else {
            ev.key = rv.key;
        }
        ev.value = rv.value;
        ev.meta = rv.meta;
        ev.tstamp = rv.tstamp;
        ev.file_id = rv.file_id;
        ev.offset = rv.offset;
        ev.total_sz = rv.total_sz;
        ev.is_tombstone = rv.is_tombstone;
        ev.ord = rv.ord;
        if (rv.file_holder) view_files_.push_back(std::move(rv.file_holder));
        if (rv.owned) view_owned_.push_back(std::move(*rv.owned));
        ++n;
    }
    // 整批填完再回填墓碑行的 key span（见 hpp 注释）。
    const auto* base = reinterpret_cast<const std::byte*>(view_key_buf_.data());
    for (const auto& s : view_scratch_keys_) {
        view_entries_[s.entry].key = std::span<const std::byte>(base + s.off, s.len);
    }
    return std::span<const EntryView>(view_entries_.data(), n);
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
        if ((*row)->tomb) continue;  // 只收活 key（see_tombstones 透传后的防御）
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
        // S71：带映射打开——取值走 read_mmap，不再每行一次 pread（映射失败时
        // 照旧 pread）。恢复缺省预读：扫描大体按文件顺序取值，冷缓存靠预读。
        auto df = fileops::DataFile::open(
            entry.data_path, fileops::DataFile::Mode::kRead,
            /*sync*/ false, /*mmap_enabled*/ true);
        if (!df) continue;
        df->advise_normal();
        pinned_files_.emplace(fid, std::make_unique<fileops::DataFile>(
                                       std::move(*df)));
    }
}

void CaskIter::release() noexcept {
    iterating_ = false;
    done_ = true;
    merged_.reset();
    view_ = {};
    cur_key_.clear();
    pinned_files_.clear();
    reset_view_batch();
}

}  // namespace bitcask
