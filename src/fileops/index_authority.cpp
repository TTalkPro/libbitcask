#include "bitcask/index_authority.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <unordered_map>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/detail/path_utf8.hpp"
#include "bitcask/detail/scanner.hpp"
#include "bitcask/detail/committed_fold.hpp"
#include "bitcask/oki_run.hpp"

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

void IndexAuthority::advance_fold_epoch() noexcept {
    // 新时代 = 当前时代 + 1，只落在 fold_floor_ 上。与并发写的关系：写把
    // next_ord 推过 want 时时代本就已前进，CAS 失败后的重读自然收敛。
    auto cur = fold_floor_.load(std::memory_order_relaxed);
    for (;;) {
        const auto want =
            std::max(cur, next_ord_.load(std::memory_order_acquire)) + 1;
        if (fold_floor_.compare_exchange_weak(cur, want,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
            return;
        }
    }
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
    // 幂等：同 ord 同落点的行已在视图中（运行期受控重建扫到了这条刚写入
    // 的记录，写者随后才拿到 writer_mu_）——不得重复记账。
    if (prior.status == OkiState::LocateStatus::kHit && !prior.tomb &&
        prior.has_loc && prior.ord == ord && prior.loc.file_id == file_id &&
        prior.loc.offset == offset) {
        note_applied(file_id, offset + total_size);
        return PutResult::kOk;
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
    note_applied(file_id, offset + total_size);
    return PutResult::kOk;
}

void IndexAuthority::apply_remove(std::string_view key, std::uint64_t ord,
                                  std::uint32_t file_id, std::uint64_t end) {
    std::unique_lock lock(writer_mu_);
    auto prior = state_.probe_for_write(key);
    if (prior.status == OkiState::LocateStatus::kHit && !prior.tomb &&
        prior.has_loc && prior.ord < ord) {
        retire_live(ledger_, prior, key);
    }
    state_.apply_remove(key, ord, file_id, end);
    note_applied(file_id, end);
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
    note_applied(new_file_id, new_offset + new_total_size);
    state_.apply_relocation(key, ord,
                            RowLoc{new_file_id, new_total_size, new_offset,
                                   tstamp});
    return true;
}

void IndexAuthority::account_locked(const OkiState::LocateResult& prior,
                                    std::string_view key,
                                    std::uint32_t file_id,
                                    std::uint32_t total_size,
                                    std::uint64_t tstamp, std::uint64_t ord,
                                    bool tomb) {
    const bool prior_live = prior.status == OkiState::LocateStatus::kHit &&
                            !prior.tomb && prior.has_loc;
    const OkiState::WriteProbe p{prior.status, prior.ord, prior.tomb,
                                 prior.has_loc, prior.loc};
    if (tomb) {
        if (prior_live && prior.ord < ord) retire_live(ledger_, p, key);
    } else if (prior.status == OkiState::LocateStatus::kHit &&
               prior.ord >= ord) {
        // 陈旧 tail 行不能推进账本。
    } else if (!prior_live) {
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    } else {
        retire_live(ledger_, p, key);
        install_live(ledger_, key, file_id, total_size, tstamp, true);
    }
    (void)increment_file_id_at_least(file_id);
    advance_ord(ord);
}

void IndexAuthority::account_recovery_row(std::string_view key,
                                            std::uint32_t file_id,
                                            std::uint32_t total_size,
                                            std::uint64_t tstamp,
                                            std::uint64_t ord, bool tomb) {
    std::scoped_lock lock(writer_mu_);
    account_locked(state_.locate(key), key, file_id, total_size, tstamp, ord,
                   tomb);
}

void IndexAuthority::recover_row(std::string_view key, std::uint32_t file_id,
                                 std::uint32_t total_size,
                                 std::uint64_t offset, std::uint64_t tstamp,
                                 std::uint64_t ord, bool tomb) {
    std::scoped_lock lock(writer_mu_);
    const auto prior = state_.locate(key);
    if (prior.status == OkiState::LocateStatus::kHit && prior.ord >= ord) {
        // 视图已覆盖（含 merge 搬迁副本：同 ord、索引仍指向原位置）。
        (void)increment_file_id_at_least(file_id);
        advance_ord(ord);
        return;
    }
    account_locked(prior, key, file_id, total_size, tstamp, ord, tomb);
    if (tomb) {
        state_.append_update(key, ord, true, nullptr, file_id,
                             offset + total_size);
    } else {
        const RowLoc loc{file_id, total_size, offset, tstamp};
        state_.append_update(key, ord, false, &loc);
    }
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
    // fold 时代下界不落盘：已发布为 pending 的文件说明当时时代已越过其
    // expiration_epoch，据此还原下界，保证派生布尔跨重启不回退。
    std::uint64_t floor = 0;
    for (const auto& f : manifest.fstats) {
        if (f.pending_delete && f.expiration_epoch != kMaxEpoch) {
            floor = std::max(floor, f.expiration_epoch + 1);
        }
    }
    for (auto cur = fold_floor_.load(std::memory_order_relaxed);
         cur < floor && !fold_floor_.compare_exchange_weak(
                            cur, floor, std::memory_order_release,
                            std::memory_order_relaxed);) {}
    const auto epoch = fold_epoch();
    for (auto f : manifest.fstats) {
        f.pending_delete = f.expiration_epoch != kMaxEpoch &&
                           f.expiration_epoch < epoch;
        ledger_.files.emplace(f.file_id, f);
    }
    return true;
}

// 从 data（唯一持久真相）全量重建账本与（可选）OKI 视图。
//
// 流式两遍（原实现把全部 key 放进 std::map<string, Winner>、批成员连 value
// 一起拷贝暂存——内存 O(全部 key + 批 value)，与 Level B「磁盘驻留」目标相悖）：
//   1) 顺序扫 data（fold_committed，批成员不拷 value），每条记录以
//      (key, ord, 位置) 灌进行汇：可写模式 = SpillingRunBuilder（64MiB 分段
//      外排，工作集 O(spill)），只读内存视图 = 内存向量（视图本就 O(N)）。
//      胜出规则 = max (ord, 到达序)：文件按 id 升序扫，等 ord 时后扫到者
//      （merge 副本，在更大 id 的输出文件里）胜；
//   2) 顺序读胜出行，计 per-file 活跃统计 / 活键数；total_* 在第 1 遍逐条
//      累计（每条 put 记录都计入其所在文件，与运行期 install_live 同口径）。
// publish_state：0 = 只换账本（视图保持原样，但补 data 水位之后的 tail）；
//                1 = 账本 + 新 run 一起发布；2 = 账本 + 内存视图（不写盘）。
//
// 全程持 writer_mu_：扫描期间到达的运行期写（apply_put/relocate 等）必须等
// 新视图发布后再基于它应用（原实现扫描不持锁、结束时清空 memdelta 并整体
// 替换账本——并发写的索引行与账本增量都会丢失）。
bool IndexAuthority::rebuild_from_data(std::string_view dir,
                                       int publish_state, bool may_write) {
    std::unique_lock lock(writer_mu_);
    auto entries = fileops::scan_dir(std::string(dir));
    if (!entries) return false;
    std::sort(entries->begin(), entries->end(),
              [](auto& a, auto& b) { return a.tstamp < b.tstamp; });

    // 只读句柄（含 RO 内存视图）不得在目录里落任何文件。
    const bool to_disk = may_write && publish_state != 2;
    // 账本模式需要胜出墓碑（补 tail 时抵消视图里的活行）；发布模式的全量
    // 单 run 不需要墓碑行（缺席即语义）。
    const bool keep_tombs = publish_state == 0;

    Ledger rebuilt;
    std::uint32_t data_biggest_file_id = 0;

    // ---- 第 1 遍：data → 行汇 ----
    std::uint64_t gen = 0;
    std::optional<SpillingRunBuilder> builder;
    struct MemRow {
        std::string key;
        std::uint64_t ord;
        bool tomb;
        RowLoc loc;
    };
    std::vector<MemRow> mem_rows;
    if (to_disk) {
        gen = state_.reserve_gen(dir);
        auto b = SpillingRunBuilder::create(
            std::string(dir), gen, kRunVersion3,
            SpillingRunBuilder::kDefaultSpillBytes,
            /*drop_tombstones=*/!keep_tombs);
        if (!b) {
            state_.abandon_gen(dir, gen);
            return false;
        }
        builder.emplace(std::move(*b));
    }
    const auto fail = [&] {
        if (to_disk) state_.abandon_gen(dir, gen);
        return false;
    };
    bool sink_ok = true;
    for (const auto& entry : *entries) {
        const auto fid = static_cast<std::uint32_t>(entry.tstamp);
        (void)increment_file_id_at_least(fid);
        data_biggest_file_id = std::max(data_biggest_file_id, fid);
        auto df = fileops::DataFile::open(
            entry.data_path, fileops::DataFile::Mode::kRead, false, false);
        if (!df) return fail();
        fileops::CommittedFoldOptions fopts;
        fopts.keep_batch_values = false;
        auto folded = fileops::fold_committed(
            *df,
            [&](const codec::DataRecordView& view, std::uint64_t offset,
                std::uint32_t total_size) {
                advance_ord(view.ord);
                if (!sink_ok) return;
                const bool tomb = view.type == format::RecordType::kTombstone;
                const RowLoc loc{fid, total_size, offset, view.tstamp};
                if (!tomb) {
                    adjust_file(rebuilt, fid, view.tstamp, 0, 1, 0,
                                static_cast<std::int64_t>(total_size));
                }
                if (to_disk) {
                    // 墓碑也带落点（仅账本模式的临时 run 用作持久落点）。
                    if (!builder->add(view.key, view.ord, tomb, &loc)) {
                        sink_ok = false;
                    }
                } else {
                    mem_rows.push_back(MemRow{
                        std::string(reinterpret_cast<const char*>(view.key.data()),
                                    view.key.size()),
                        view.ord, tomb, loc});
                }
            },
            [&](const codec::DataRecordView& view) { advance_ord(view.ord); },
            fopts);
        df->close();
        if (!folded || !sink_ok) return fail();
    }

    // ---- 第 2 遍：胜出行 → 账本（+ 视图行 / tail 补行）----
    //
    // 账本模式（open 收尾）：已载视图保持原样，但 manifest data 水位之后的
    // tail（崩溃丢失的 memdelta）必须补进视图——否则 data 在、索引无，账本
    // 却已计入。只检查 tail 区内的胜出者，逐个与组合视图比对（流式，不攒）。
    const bool replay_tail = publish_state == 0 && state_.loaded();
    std::unordered_map<std::uint32_t, std::uint64_t> tail_ends;
    if (replay_tail) {
        for (const auto& [f, e] : state_.manifest_data_wms()) tail_ends[f] = e;
    }
    const bool pq = state_.point_query_enabled();
    const auto replay_one = [&](std::string_view key, std::uint64_t ord,
                                bool tomb, const RowLoc& loc) {
        const std::uint64_t end = loc.offset + loc.total_sz;
        if (auto it = tail_ends.find(loc.file_id);
            it != tail_ends.end() && end <= it->second) {
            return;  // 提交水位之内：run 已覆盖
        }
        if (!pq) {
            if (tomb) {
                state_.apply_remove(key, ord, loc.file_id, end);
            } else {
                state_.append(key, ord, false, &loc);
            }
            return;
        }
        const auto prior = state_.locate(key);
        // 视图对该 key 不可判定（v1 run / 坏块）：保持不可用，由读写入口的
        // 受控重建从 data 修复；不得用 delta 行掩盖。
        if (prior.status == OkiState::LocateStatus::kUnavailable) return;
        if (prior.status == OkiState::LocateStatus::kHit && prior.ord >= ord) {
            return;
        }
        if (tomb) {
            // 墓碑只需抵消视图里的活行；视图无此 key 时无事可做。
            if (prior.status != OkiState::LocateStatus::kHit || prior.tomb) return;
            state_.append_update(key, ord, true, nullptr, loc.file_id, end);
        } else {
            state_.append_update(key, ord, false, &loc);
        }
    };
    std::vector<OkiState::DeltaRow> view_rows;  // 仅内存视图
    std::uint64_t live_rows = 0;
    const auto on_winner = [&](std::string_view key, std::uint64_t ord,
                               bool tomb, const RowLoc& loc) {
        if (!tomb) {
            adjust_file(rebuilt, loc.file_id, loc.tstamp, 1, 0,
                        static_cast<std::int64_t>(loc.total_sz), 0);
            ++rebuilt.active_keys;
            rebuilt.key_bytes += key.size();
            ++live_rows;
        }
        if (publish_state == 2 && !tomb) {
            view_rows.push_back(
                OkiState::DeltaRow{std::string(key), ord, false, true, loc});
        } else if (replay_tail) {
            replay_one(key, ord, tomb, loc);
        }
    };
    if (to_disk) {
        if (!builder->finish(/*fsync_dir=*/true)) return fail();
        builder.reset();
        auto rd = OkiRunReader::open(mk_run_filename(dir, gen));
        if (!rd) return fail();
        auto c = rd->begin();
        OkiRunReader::Entry e;
        while (true) {
            auto n = c.next(e);
            if (!n) return fail();
            if (!*n) break;
            on_winner(e.key, e.ord, e.tomb, e.loc);
        }
    } else {
        // stable：等 ord 时保持扫描序 → 取末 = 后扫到者胜。
        std::stable_sort(mem_rows.begin(), mem_rows.end(),
                         [](const MemRow& a, const MemRow& b) {
                             if (a.key != b.key) return a.key < b.key;
                             return a.ord < b.ord;
                         });
        for (std::size_t i = 0; i < mem_rows.size(); ++i) {
            if (i + 1 < mem_rows.size() && mem_rows[i + 1].key == mem_rows[i].key) {
                continue;
            }
            const auto& r = mem_rows[i];
            on_winner(r.key, r.ord, r.tomb, r.loc);
        }
        mem_rows.clear();
        mem_rows.shrink_to_fit();
    }
    for (auto& [id, f] : rebuilt.files) {
        if (f.live_keys == 0) {
            f.oldest_tstamp = 0;
            f.newest_tstamp = 0;
        }
    }

    ledger_ = std::move(rebuilt);
    // registry 关闭时会保存 biggest+1 作为进程内防碰撞水位；该值不是
    // 持久真相。从 data 全量重建时必须收回到实际最大 data file id，
    // 否则 v4 元数据会“复活”一个从未落盘的 file generation。
    biggest_file_id_.store(data_biggest_file_id, std::memory_order_relaxed);

    if (publish_state == 0) {
        // 临时 run 只是外排载体，不发布（tail 已在第 2 遍流式补入视图）。
        if (to_disk) state_.abandon_gen(dir, gen);
        return true;
    }

    CommitSnapshot snapshot;
    auto& meta = snapshot.metadata;
    meta.next_ord = next_ord_.load(std::memory_order_relaxed);
    meta.biggest_file_id = biggest_file_id_.load(std::memory_order_relaxed);
    if (meta.biggest_file_id == kMaxFileId) return fail();
    meta.next_file_id = meta.biggest_file_id + 1;
    meta.active_keys = ledger_.active_keys;
    meta.live_bytes = ledger_.key_bytes;
    const auto epoch = fold_epoch();
    for (auto& [id, f] : ledger_.files) {
        auto out = f;
        out.pending_delete = f.expiration_epoch != kMaxEpoch &&
                             f.expiration_epoch < epoch;
        if (out.live_keys != 0) {
            meta.oldest_tstamp = meta.oldest_tstamp == 0 ? out.oldest_tstamp :
                std::min(meta.oldest_tstamp, out.oldest_tstamp);
            meta.newest_tstamp = std::max(meta.newest_tstamp,
                                          out.newest_tstamp);
        }
        snapshot.fstats.push_back(out);
    }
    std::sort(snapshot.fstats.begin(), snapshot.fstats.end(),
              [](auto& a, auto& b) { return a.file_id < b.file_id; });
    if (!snapshot_valid(meta, snapshot.fstats)) return fail();
    const auto cover_ord = meta.next_ord;
    applied_end_.clear();  // 新视图的落点由 data 扫描界定
    state_.degrade_for_rebuild();
    // 新视图是全 v3、具备点查能力：必须在释放写序闸门**之前**开启点查。
    // 否则被挡在 writer_mu_ 外的写者会在「已发布、点查未开」的窗口里探测
    // 到 kUnavailable 而失败（原实现由调用方事后开启，窗口真实存在）。
    if (publish_state == 2) {
        state_.install_memory_view(std::move(view_rows), cover_ord,
                                   std::move(snapshot));
        if (state_.loaded()) state_.enable_point_query();
        return state_.loaded();
    }
    // S33-6：零活 key 不落空 run（0 run + wm=cover 已完整表达语义）。
    if (live_rows == 0) {
        state_.abandon_gen(dir, gen);
        gen = 0;
    }
    const bool ok =
        state_.install_built_run(dir, gen, cover_ord, std::move(snapshot));
    if (ok) state_.enable_point_query();
    return ok;
}

std::optional<IndexAuthority::CommitSnapshot>
IndexAuthority::capture_commit_snapshot() {
    std::scoped_lock lock(writer_mu_);
    return capture_locked();
}

std::optional<IndexAuthority::CommitSnapshot>
IndexAuthority::capture_locked() {
    CommitSnapshot snap;
    snap.cut_seq = state_.current_seq();
    auto& g = snap.metadata;
    g.next_ord = next_ord_.load(std::memory_order_acquire);
    g.biggest_file_id = biggest_file_id_.load(std::memory_order_acquire);
    if (g.biggest_file_id == kMaxFileId) return std::nullopt;
    g.next_file_id = g.biggest_file_id + 1;
    g.active_keys = ledger_.active_keys;
    g.live_bytes = ledger_.key_bytes;
    const auto epoch = fold_epoch();
    for (auto& [id, f] : ledger_.files) {
        auto out = f;
        out.pending_delete = f.expiration_epoch != kMaxEpoch &&
                             f.expiration_epoch < epoch;
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

std::optional<std::pair<IndexAuthority::CommitSnapshot,
                        IndexAuthority::DataWms>>
IndexAuthority::capture_commit_cut(
    const std::function<std::optional<DataWms>()>& cut_fn) {
    std::scoped_lock lock(writer_mu_);
    auto wms = cut_fn();
    if (!wms) return std::nullopt;
    for (auto& [fid, wm] : *wms) {
        if (auto it = applied_end_.find(fid); it != applied_end_.end()) {
            wm = std::min(wm, it->second);
        }
    }
    auto snap = capture_locked();
    if (!snap) return std::nullopt;
    return std::make_pair(std::move(*snap), std::move(*wms));
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
    const auto epoch = fold_epoch();
    for (auto& [id, f] : ledger_.files) {
        auto copy = f;
        copy.pending_delete = f.expiration_epoch != kMaxEpoch &&
                              f.expiration_epoch < epoch;
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
    // 记录当前 fold 时代；时代推进（下一次写，或 CaskIter::start 的
    // advance_fold_epoch）之后，v4 capture 将其发布为派生的 pending_delete。
    it->second.expiration_epoch =
        std::min(it->second.expiration_epoch, fold_epoch());
}

std::uint32_t IndexAuthority::trim_fstats(std::span<const std::uint32_t> ids) {
    std::scoped_lock lock(writer_mu_);
    std::uint32_t missing = 0;
    for (auto id : ids) {
        if (!ledger_.files.erase(id)) ++missing;
        applied_end_.erase(id);
    }
    return missing;
}

void IndexAuthority::with_writer_lock(const std::function<void()>& fn) {
    std::scoped_lock lock(writer_mu_);
    fn();
}

} // namespace bitcask::oki
