#include "bitcask/oki_state.hpp"

#include <algorithm>
#include <filesystem>

namespace bitcask::oki {

void OkiState::append(std::string_view key, std::uint64_t ord, bool tomb) {
    // 水位门（wm = 排他上界 = 尚未覆盖的最小 ord；首个合法 LSN 是 0，
    // 故不能有 ord==0 特判）：tail 重放只收 wm 起的行。
    if (ord < wm_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(mu_);
    delta_bytes_ += key.size() + sizeof(DeltaRow);
    delta_.push_back(DeltaRow{std::string(key), ord, tomb});
    update_flush_hint_locked();
}

void OkiState::load(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    auto m = read_manifest(dir);
    if (!m) return;  // 缺失/损坏 → 未加载态（caller 决定重建）
    manifest_ = *std::move(m);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
}

std::uint64_t OkiState::next_gen_locked() const noexcept {
    std::uint64_t g = 0;
    for (const auto& r : manifest_.runs) g = std::max(g, r.gen);
    return g + 1;
}

bool OkiState::flush(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return false;

    // 换出 memdelta：IO 期间 append 不被阻塞（新行 ord 恒更高）。
    std::vector<DeltaRow> rows;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rows.swap(delta_);
        delta_bytes_ = 0;
        update_flush_hint_locked();
    }
    if (rows.empty()) return true;

    // 失败时把换出的行放回队头（保持 ord 升序不变量——放回的行恒早于
    // IO 期间新 append 的行）。
    auto restore = [&] {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& r : rows) delta_bytes_ += r.key.size() + sizeof(DeltaRow);
        rows.insert(rows.end(), std::make_move_iterator(delta_.begin()),
                    std::make_move_iterator(delta_.end()));
        delta_ = std::move(rows);
        update_flush_hint_locked();
    };

    // 排序 + 同 key 取 max-ord（stable 不必要：显式 (key, ord) 双键）。
    std::sort(rows.begin(), rows.end(), [](const DeltaRow& a, const DeltaRow& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.ord < b.ord;
    });
    std::uint64_t cover = 0;  // 排他上界 = 本批最大 ord + 1
    for (const auto& r : rows) cover = std::max(cover, r.ord + 1);

    const std::uint64_t gen = next_gen_locked();
    auto w = OkiRunWriter::create(mk_run_filename(dir, gen));
    if (!w) { restore(); return false; }
    bool io_ok = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        // 同 key 连续段取最后一条（max ord）。
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
        const auto& r = rows[i];
        auto a = w->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb);
        if (!a) { io_ok = false; break; }
    }
    if (io_ok && !w->finish(/*fsync_dir=*/true)) io_ok = false;
    if (!io_ok) { restore(); return false; }

    // manifest 提交（唯一 commit point）。失败则删刚写的 run 并放回。
    OkiManifest next = manifest_;
    next.runs.push_back({gen, cover});
    next.wm = std::max(next.wm, cover);
    if (!write_manifest(dir, next)) {
        std::error_code ec;
        std::filesystem::remove(mk_run_filename(dir, gen), ec);
        restore();
        return false;
    }
    manifest_ = std::move(next);
    wm_.store(manifest_.wm, std::memory_order_release);
    return true;
}

bool OkiState::rebuild(std::string_view dir, std::vector<DeltaRow>&& rows,
                       std::uint64_t cover_ord) {
    std::lock_guard<std::mutex> flk(flush_mu_);

    std::sort(rows.begin(), rows.end(), [](const DeltaRow& a, const DeltaRow& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.ord < b.ord;
    });

    const std::uint64_t gen = next_gen_locked();
    auto w = OkiRunWriter::create(mk_run_filename(dir, gen));
    if (!w) return false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
        const auto& r = rows[i];
        auto a = w->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb);
        if (!a) return false;
    }
    if (!w->finish(/*fsync_dir=*/true)) return false;

    OkiManifest next;
    next.runs.push_back({gen, cover_ord});
    next.wm = cover_ord;
    if (!write_manifest(dir, next)) {
        std::error_code ec;
        std::filesystem::remove(mk_run_filename(dir, gen), ec);
        return false;
    }

    // 提交后清理：旧 run 文件 + memdelta（内容已被全量快照覆盖）。
    for (const auto& r : manifest_.runs) {
        std::error_code ec;
        std::filesystem::remove(mk_run_filename(dir, r.gen), ec);
    }
    manifest_ = std::move(next);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mu_);
        delta_.clear();
        delta_bytes_ = 0;
        update_flush_hint_locked();
    }
    return true;
}

std::size_t OkiState::delta_rows() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_.size();
}

std::size_t OkiState::delta_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_bytes_;
}

std::size_t OkiState::run_count() const {
    std::lock_guard<std::mutex> lk(flush_mu_);
    return manifest_.runs.size();
}

}  // namespace bitcask::oki
