#include "bitcask/index_authority.hpp"

#include <algorithm>
#include <map>
#include <limits>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/detail/path_utf8.hpp"
#include "bitcask/detail/scanner.hpp"

namespace bitcask::oki {
namespace {
constexpr auto kMaxEpoch = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t kMaxFileId = std::numeric_limits<std::uint32_t>::max();
}

void IndexAuthority::advance_ord(std::uint64_t ord) noexcept {
    auto cur = next_ord_.load(std::memory_order_relaxed);
    while (cur < ord + 1 &&
           !next_ord_.compare_exchange_weak(cur, ord + 1,
                                            std::memory_order_relaxed)) {}
}

std::uint32_t IndexAuthority::increment_file_id() noexcept {
    return biggest_file_id_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t IndexAuthority::increment_file_id_at_least(std::uint32_t id) noexcept {
    auto cur = biggest_file_id_.load(std::memory_order_relaxed);
    while (id > cur &&
           !biggest_file_id_.compare_exchange_weak(cur, id,
                                                    std::memory_order_relaxed)) {}
    return std::max(cur, id);
}

void IndexAuthority::adjust_file(Ledger& l, std::uint32_t file_id,
                                 std::uint64_t tstamp,
                                 std::int64_t live_keys, std::int64_t total_keys,
                                 std::int64_t live_bytes,
                                 std::int64_t total_bytes) {
    auto& f = l.files[file_id];
    f.file_id = file_id;
    f.live_keys = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(f.live_keys) + live_keys);
    f.total_keys = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(f.total_keys) + total_keys);
    f.live_bytes = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(f.live_bytes) + live_bytes);
    f.total_bytes = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(f.total_bytes) + total_bytes);
    if (tstamp != 0) {
        f.oldest_tstamp = f.oldest_tstamp == 0 ? tstamp :
            std::min(f.oldest_tstamp, tstamp);
        f.newest_tstamp = std::max(f.newest_tstamp, tstamp);
    }
    if (f.live_keys == 0) {
        f.oldest_tstamp = 0;
        f.newest_tstamp = 0;
    }
}

void IndexAuthority::retire_live(
    Ledger& l, const OkiState::WriteProbe& prior, std::string_view key) {
    if (prior.status != OkiState::LocateStatus::kHit || prior.tomb ||
        !prior.has_loc) return;
    adjust_file(l, prior.loc.file_id, 0, -1, 0,
                -static_cast<std::int64_t>(prior.loc.total_sz), 0);
    --l.active_keys;
    l.key_bytes -= key.size();
}

void IndexAuthority::install_live(
    Ledger& l, std::string_view key, std::uint32_t file_id,
    std::uint32_t total_size, std::uint64_t tstamp, bool count_total) {
    adjust_file(l, file_id, tstamp, 1, count_total ? 1 : 0,
                static_cast<std::int64_t>(total_size),
                count_total ? static_cast<std::int64_t>(total_size) : 0);
    ++l.active_keys;
    l.key_bytes += key.size();
}

PutResult IndexAuthority::apply_put(
    std::string_view key, std::uint32_t file_id, std::uint32_t total_size,
    std::uint64_t offset, std::uint64_t tstamp, std::uint64_t ord) {
    std::unique_lock lock(writer_mu_);
    auto prior = state_.probe_for_write(key);
    if (prior.status == OkiState::LocateStatus::kUnavailable) {
        return PutResult::kIndexUnavailable;
    }
    if (file_id < biggest_file_id_.load(std::memory_order_relaxed) ||
        (prior.status == OkiState::LocateStatus::kHit && prior.ord > ord)) {
        return PutResult::kAlreadyExists;
    }
    const bool prior_live = prior.status == OkiState::LocateStatus::kHit &&
                            !prior.tomb && prior.has_loc;
    if (!prior_live) {
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    } else if (prior.loc.file_id != file_id) {
        retire_live(ledger_, prior, key);
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    } else {
        auto old = static_cast<std::int64_t>(prior.loc.total_sz);
        auto now_sz = static_cast<std::int64_t>(total_size);
        adjust_file(ledger_, file_id, tstamp, 0, 1, now_sz - old, now_sz);
    }
    (void)increment_file_id_at_least(file_id);
    const RowLoc loc{file_id, total_size, offset, tstamp};
    state_.apply_put(key, ord, loc);
    return PutResult::kOk;
}

void IndexAuthority::apply_remove(std::string_view key, std::uint64_t ord) {
    std::unique_lock lock(writer_mu_);
    auto prior = state_.probe_for_write(key);
    if (prior.status == OkiState::LocateStatus::kHit && !prior.tomb &&
        prior.has_loc && prior.ord < ord) {
        retire_live(ledger_, prior, key);
    }
    state_.apply_remove(key, ord);
}

bool IndexAuthority::conditional_remove(std::string_view key,
                                         std::uint64_t tstamp,
                                         std::uint32_t file_id,
                                         std::uint64_t offset,
                                         std::uint64_t remove_time) {
    (void)remove_time;
    std::scoped_lock lock(writer_mu_);
    auto prior = state_.probe_for_write(key);
    if (prior.status != OkiState::LocateStatus::kHit || prior.tomb ||
        !prior.has_loc) return true;
    if (prior.loc.tstamp != tstamp || prior.loc.file_id != file_id ||
        prior.loc.offset != offset) return false;
    retire_live(ledger_, prior, key);
    state_.apply_conditional_remove(key, prior.ord);
    return true;
}

bool IndexAuthority::relocate(std::string_view key,
                               std::uint32_t old_file_id,
                               std::uint64_t old_offset,
                               std::uint32_t new_file_id,
                               std::uint32_t new_total_size,
                               std::uint64_t new_offset,
                               std::uint64_t tstamp, std::uint64_t ord) {
    std::scoped_lock lock(writer_mu_);
    auto prior = state_.probe_for_write(key);
    if (prior.status != OkiState::LocateStatus::kHit || prior.tomb ||
        !prior.has_loc || prior.ord != ord ||
        prior.loc.file_id != old_file_id ||
        prior.loc.offset != old_offset) return false;
    auto old = static_cast<std::int64_t>(prior.loc.total_sz);
    auto now_sz = static_cast<std::int64_t>(new_total_size);
    adjust_file(ledger_, prior.loc.file_id, 0, -1, 0, -old, 0);
    adjust_file(ledger_, new_file_id, tstamp, 1, 1, now_sz, now_sz);
    (void)increment_file_id_at_least(new_file_id);
    state_.apply_relocation(key, ord,
                            RowLoc{new_file_id, new_total_size, new_offset,
                                   tstamp});
    return true;
}

void IndexAuthority::account_recovery_row(std::string_view key,
                                            std::uint32_t file_id,
                                            std::uint32_t total_size,
                                            std::uint64_t tstamp,
                                            std::uint64_t ord, bool tomb) {
    std::scoped_lock lock(writer_mu_);
    auto prior = state_.locate(key);
    const bool prior_live = prior.status == OkiState::LocateStatus::kHit &&
                            !prior.tomb && prior.has_loc;
    if (tomb) {
        if (prior_live && prior.ord < ord) {
            retire_live(ledger_, {prior.status, prior.ord, prior.tomb,
                                  prior.has_loc, prior.loc}, key);
        }
    } else if (prior.status == OkiState::LocateStatus::kHit &&
               prior.ord >= ord) {
        // 陈旧 tail 行不能推进账本。
    } else if (!prior_live) {
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    } else {
        retire_live(ledger_, {prior.status, prior.ord, prior.tomb,
                              prior.has_loc, prior.loc}, key);
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    }
    (void)increment_file_id_at_least(file_id);
    advance_ord(ord);
}

void IndexAuthority::replay_row(std::string_view key, std::uint32_t file_id,
                                 std::uint32_t total_size,
                                 std::uint64_t offset, std::uint64_t tstamp,
                                 std::uint64_t ord) {
    account_recovery_row(key, file_id, total_size, tstamp, ord, false);
    const RowLoc loc{file_id, total_size, offset, tstamp};
    state_.append(key, ord, false, &loc);
}

void IndexAuthority::replay_removal(std::string_view key,
                                     std::uint64_t tomb_ord) {
    account_recovery_row(key, 0, 0, 0, tomb_ord, true);
    state_.append_update(key, tomb_ord, true, nullptr);
}

bool IndexAuthority::snapshot_valid(
    const OkiManifestMetadata& m, const std::vector<FStats>& files) noexcept {
    if (m.biggest_file_id == kMaxFileId ||
        m.next_file_id != m.biggest_file_id + 1 || m.reserved != 0 ||
        (m.active_keys == 0) != (m.live_bytes == 0) ||
        (m.active_keys == 0 && (m.oldest_tstamp != 0 || m.newest_tstamp != 0)) ||
        (m.active_keys != 0 && (m.oldest_tstamp == 0 || m.newest_tstamp == 0)) ||
        files.size() > (1u << 24)) return false;
    std::map<std::uint32_t, const FStats*> seen;
    for (auto& f : files) {
        if (f.file_id > (1u << 24) || f.file_id == kMaxFileId ||
            !seen.emplace(f.file_id, &f).second) return false;
    }
    return true;
}

bool IndexAuthority::adopt_manifest(const OkiManifest& manifest) {
    if (!manifest.metadata) return true;
    if (!snapshot_valid(*manifest.metadata, manifest.fstats)) return false;
    std::scoped_lock lock(writer_mu_);
    next_ord_.store(manifest.metadata->next_ord, std::memory_order_relaxed);
    biggest_file_id_.store(manifest.metadata->biggest_file_id,
                           std::memory_order_relaxed);
    ledger_.active_keys = manifest.metadata->active_keys;
    ledger_.key_bytes = manifest.metadata->live_bytes;
    ledger_.files.clear();
    for (auto f : manifest.fstats) {
        f.pending_delete = f.expiration_epoch != kMaxEpoch &&
                           f.expiration_epoch < next_ord_.load();
        ledger_.files.emplace(f.file_id, f);
    }
    return true;
}

bool IndexAuthority::rebuild_from_data(std::string_view dir,
                                       int publish_state) {
    struct Winner {
        RowLoc loc;
        std::uint64_t ord = 0;
        bool tomb = false;
    };
    Ledger rebuilt;
    std::map<std::string, Winner, std::less<>> winners;
    std::uint32_t data_biggest_file_id = 0;
    auto entries = fileops::scan_dir(std::string(dir));
    if (!entries) return false;
    std::sort(entries->begin(), entries->end(),
              [](auto& a, auto& b) { return a.tstamp < b.tstamp; });

    for (const auto& entry : *entries) {
        const auto fid = static_cast<std::uint32_t>(entry.tstamp);
        (void)increment_file_id_at_least(fid);
        data_biggest_file_id = std::max(data_biggest_file_id, fid);
        auto df = fileops::DataFile::open(
            entry.data_path, fileops::DataFile::Mode::kRead, false, false);
        if (!df) return false;
        struct Staged {
            format::RecordType type;
            std::uint64_t tstamp;
            std::uint64_t ord;
            std::vector<std::byte> key;
            std::vector<std::byte> value;
            std::uint64_t offset;
            std::uint32_t total_size;
        };
        std::vector<Staged> staged;
        std::uint64_t batch_end = 0;
        auto apply = [&](const codec::DataRecordView& view,
                         std::uint64_t offset, std::uint32_t total_size) {
            advance_ord(view.ord);
            std::string key(reinterpret_cast<const char*>(view.key.data()),
                            view.key.size());
            auto it = winners.find(key);
            if (it != winners.end() && it->second.ord >= view.ord) return;
            if (it != winners.end() && !it->second.tomb) {
                auto prior = it->second;
                adjust_file(rebuilt, prior.loc.file_id, 0, -1, 0,
                            -static_cast<std::int64_t>(prior.loc.total_sz), 0);
                --rebuilt.active_keys;
                rebuilt.key_bytes -= key.size();
            }
            if (view.type == format::RecordType::kTombstone) {
                winners.insert_or_assign(key, Winner{{}, view.ord, true});
                return;
            }
            adjust_file(rebuilt, fid, view.tstamp, 1, 1,
                        static_cast<std::int64_t>(total_size),
                        static_cast<std::int64_t>(total_size));
            ++rebuilt.active_keys;
            rebuilt.key_bytes += key.size();
            winners.insert_or_assign(
                key, Winner{RowLoc{fid, total_size, offset, view.tstamp},
                            view.ord, false});
        };
        auto folded = df->fold(
            [&](const codec::DataRecordView& view, std::uint64_t offset,
                std::uint32_t total_size) {
                if (view.type == format::RecordType::kBatchHeader) {
                    auto bh = codec::decode_batch_header_value(view.value);
                    if (!bh) return;
                    batch_end = offset + total_size + bh->span_bytes;
                    staged.clear();
                    advance_ord(view.ord);
                    return;
                }
                if (batch_end == 0) {
                    apply(view, offset, total_size);
                    return;
                }
                staged.push_back({view.type, view.tstamp, view.ord,
                                  {view.key.begin(), view.key.end()},
                                  {view.value.begin(), view.value.end()},
                                  offset, total_size});
                if (offset + total_size >= batch_end) {
                    for (auto& row : staged) {
                        codec::DataRecordView v{0, row.type, row.tstamp,
                                                row.ord, row.key, row.value,
                                                row.total_size};
                        apply(v, row.offset, row.total_size);
                    }
                    staged.clear();
                    batch_end = 0;
                }
            }, true);
        df->close();
        if (!folded) return false;
    }

    std::unique_lock lock(writer_mu_);
    ledger_ = std::move(rebuilt);
    // registry 关闭时会保存 biggest+1 作为进程内防碰撞水位；该值不是
    // 持久真相。从 data 全量重建时必须收回到实际最大 data file id，
    // 否则 v4 元数据会“复活”一个从未落盘的 file generation。
    biggest_file_id_.store(data_biggest_file_id, std::memory_order_relaxed);
    if (publish_state == 0) return true;

    std::vector<OkiState::DeltaRow> rows;
    rows.reserve(winners.size());
    for (auto& [key, winner] : winners) {
        if (winner.tomb) continue;
        rows.push_back(OkiState::DeltaRow{key, winner.ord, false, true,
                                          winner.loc});
    }
    CommitSnapshot snapshot;
    auto& meta = snapshot.metadata;
    meta.next_ord = next_ord_.load(std::memory_order_relaxed);
    meta.biggest_file_id = biggest_file_id_.load(std::memory_order_relaxed);
    if (meta.biggest_file_id == kMaxFileId) return false;
    meta.next_file_id = meta.biggest_file_id + 1;
    meta.active_keys = ledger_.active_keys;
    meta.live_bytes = ledger_.key_bytes;
    for (auto& [id, f] : ledger_.files) {
        auto out = f;
        out.pending_delete = f.expiration_epoch != kMaxEpoch &&
                             f.expiration_epoch < meta.next_ord;
        if (out.live_keys == 0) {
            out.oldest_tstamp = 0;
            out.newest_tstamp = 0;
        } else {
            meta.oldest_tstamp = meta.oldest_tstamp == 0 ? out.oldest_tstamp :
                std::min(meta.oldest_tstamp, out.oldest_tstamp);
            meta.newest_tstamp = std::max(meta.newest_tstamp,
                                          out.newest_tstamp);
        }
        snapshot.fstats.push_back(out);
    }
    std::sort(snapshot.fstats.begin(), snapshot.fstats.end(),
              [](auto& a, auto& b) { return a.file_id < b.file_id; });
    if (!snapshot_valid(meta, snapshot.fstats)) return false;
    const auto cover_ord = meta.next_ord;
    lock.unlock();
    state_.degrade_for_rebuild();
    if (publish_state == 2) {
        state_.install_memory_view(std::move(rows), cover_ord,
                                   std::move(snapshot));
        return state_.loaded();
    }
    return state_.rebuild(std::string(dir), std::move(rows), cover_ord,
                          std::move(snapshot));
}

std::optional<IndexAuthority::CommitSnapshot>
IndexAuthority::capture_commit_snapshot() {
    std::scoped_lock lock(writer_mu_);
    CommitSnapshot snap;
    auto& g = snap.metadata;
    g.next_ord = next_ord_.load(std::memory_order_acquire);
    g.biggest_file_id = biggest_file_id_.load(std::memory_order_acquire);
    if (g.biggest_file_id == kMaxFileId) return std::nullopt;
    g.next_file_id = g.biggest_file_id + 1;
    g.active_keys = ledger_.active_keys;
    g.live_bytes = ledger_.key_bytes;
    for (auto& [id, f] : ledger_.files) {
        auto out = f;
        out.pending_delete = f.expiration_epoch != kMaxEpoch &&
                             f.expiration_epoch < g.next_ord;
        if (out.live_keys == 0) {
            out.oldest_tstamp = 0;
            out.newest_tstamp = 0;
        } else {
            g.oldest_tstamp = g.oldest_tstamp == 0 ? out.oldest_tstamp :
                std::min(g.oldest_tstamp, out.oldest_tstamp);
            g.newest_tstamp = std::max(g.newest_tstamp, out.newest_tstamp);
        }
        snap.fstats.push_back(out);
    }
    std::sort(snap.fstats.begin(), snap.fstats.end(),
              [](auto& a, auto& b) { return a.file_id < b.file_id; });
    if (!snapshot_valid(g, snap.fstats)) return std::nullopt;
    return snap;
}

auto IndexAuthority::info() const -> AuthorityInfo {
    std::scoped_lock lock(writer_mu_);
    return {ledger_.active_keys, ledger_.key_bytes,
            next_ord_.load(std::memory_order_relaxed),
            biggest_file_id_.load(std::memory_order_relaxed), false};
}

std::vector<IndexAuthority::FStats> IndexAuthority::fstats() const {
    std::scoped_lock lock(writer_mu_);
    std::vector<FStats> out;
    out.reserve(ledger_.files.size());
    for (auto& [id, f] : ledger_.files) {
        auto copy = f;
        copy.pending_delete = f.expiration_epoch != kMaxEpoch &&
                              f.expiration_epoch < next_ord_.load();
        if (copy.live_keys == 0) {
            copy.oldest_tstamp = 0;
            copy.newest_tstamp = 0;
        }
        out.push_back(copy);
    }
    std::sort(out.begin(), out.end(),
              [](auto& a, auto& b) { return a.file_id < b.file_id; });
    return out;
}

void IndexAuthority::set_pending_delete(std::uint32_t file_id) noexcept {
    std::scoped_lock lock(writer_mu_);
    auto it = ledger_.files.find(file_id);
    if (it == ledger_.files.end()) return;
    // 记录当前 fold 时代；CaskIter::start 会推进到下一个时代，随后
    // v4 capture 将其发布为派生的 pending_delete。
    it->second.expiration_epoch = std::min(
        it->second.expiration_epoch, next_ord_.load(std::memory_order_relaxed));
}

std::uint32_t IndexAuthority::trim_fstats(std::span<const std::uint32_t> ids) {
    std::scoped_lock lock(writer_mu_);
    std::uint32_t missing = 0;
    for (auto id : ids) if (!ledger_.files.erase(id)) ++missing;
    return missing;
}

void IndexAuthority::with_writer_lock(const std::function<void()>& fn) {
    std::scoped_lock lock(writer_mu_);
    fn();
}

} // namespace bitcask::oki
