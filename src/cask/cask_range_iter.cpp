// cask_range_iter.cpp — S33-5：OKI 有序 Range 迭代器。
// 语义与架构见 cask.hpp CaskRangeIter 类注释与
// doc/ordered-key-index-design-zh.md §4。

#include "bitcask/cask.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "cask_internal.hpp"  // err / io_fault / bytes_to_view

namespace bitcask {

namespace {

std::string_view sv(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

std::expected<std::unique_ptr<CaskRangeIter>, CaskFault>
Cask::make_range_iter(const RangeOptions& opts) {
    if (!keydir_) return std::unexpected(err(CaskError::kClosed));
    auto view = keydir_->oki().make_read_view();
    if (!view) {
        return std::unexpected(err(
            CaskError::kNoIndex,
            "oki unavailable (read-only open of a dir without oki, or "
            "rebuild failed; reopen read-write to rebuild)"));
    }

    // unique_ptr + 私有构造：make_unique 不可用，直接 new。
    std::unique_ptr<CaskRangeIter> it(new CaskRangeIter());
    it->cask_ = this;
    it->keydir_pin_ = keydir_;
    it->view_ = *std::move(view);
    it->hi_.assign(sv(opts.hi));
    it->has_hi_ = !opts.hi.empty();
    it->prefetch_ = opts.prefetch;
    it->prefetch_threads_ = opts.prefetch_threads;

    // 各 run seek(lo)；memdelta lower_bound(lo)。
    it->cursors_.reserve(it->view_.runs.size());
    it->heads_.reserve(it->view_.runs.size());
    for (const auto& r : it->view_.runs) {
        auto c = r->seek(opts.lo);
        if (!c) {
            return std::unexpected(err(CaskError::kBadCrc,
                                       "oki run seek failed (corrupt run)"));
        }
        // 预取头元素。
        oki::OkiRunReader::Entry e;
        auto n = c->next(e);
        if (!n) {
            return std::unexpected(err(CaskError::kBadCrc,
                                       "oki run read failed (corrupt run)"));
        }
        it->cursors_.push_back(*std::move(c));
        if (*n) {
            it->heads_.push_back(std::move(e));
        } else {
            it->heads_.push_back(std::nullopt);
        }
    }
    const std::string_view lo = sv(opts.lo);
    it->delta_pos_ = static_cast<std::size_t>(
        std::lower_bound(it->view_.delta.begin(), it->view_.delta.end(), lo,
                         [](const oki::OkiState::DeltaRow& r,
                            std::string_view v) { return r.key < v; }) -
        it->view_.delta.begin());
    return it;
}

std::expected<std::optional<std::string>, CaskFault>
CaskRangeIter::next_merged_key() {
    if (done_) return std::optional<std::string>{};

    while (true) {
        // 1) 找当前最小 key（跨全部 run 头 + memdelta 头）。
        std::string_view min_key;
        bool any = false;
        for (const auto& h : heads_) {
            if (h && (!any || std::string_view(h->key) < min_key)) {
                min_key = h->key;
                any = true;
            }
        }
        if (delta_pos_ < view_.delta.size()) {
            const auto& d = view_.delta[delta_pos_];
            if (!any || std::string_view(d.key) < min_key) {
                min_key = d.key;
                any = true;
            }
        }
        if (!any || (has_hi_ && min_key >= std::string_view(hi_))) {
            done_ = true;
            return std::optional<std::string>{};
        }

        // 2) 归并同 key 各源：max-ord 胜（memdelta 行恒新于 runs 的同 key
        //    行——但不依赖此性质，统一 max-ord）；并推进这些源。
        std::uint64_t win_ord = 0;
        bool win_tomb = false;
        bool first = true;
        const std::string key(min_key);  // 推进源会失效 view，先物化
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            auto& h = heads_[i];
            if (!h || h->key != key) continue;
            if (first || h->ord > win_ord) {
                win_ord = h->ord;
                win_tomb = h->tomb;
                first = false;
            }
            // 推进该 run 游标。
            oki::OkiRunReader::Entry e;
            auto n = cursors_[i].next(e);
            if (!n) {
                return std::unexpected(err(CaskError::kBadCrc,
                                           "oki run read failed mid-scan"));
            }
            if (*n) {
                h = std::move(e);
            } else {
                h.reset();
            }
        }
        if (delta_pos_ < view_.delta.size() &&
            view_.delta[delta_pos_].key == key) {
            const auto& d = view_.delta[delta_pos_];
            if (first || d.ord > win_ord) {
                win_ord = d.ord;
                win_tomb = d.tomb;
                first = false;
            }
            ++delta_pos_;
        }

        // 3) 墓碑抵消 → 下一个 key。
        if (win_tomb) continue;
        return std::optional<std::string>(key);
    }
}

// 惰性单条：归并出 key → 立刻回查 keydir 取权威值（key 可能已死 / 已被
// 更新——OKI 行陈旧无害）。kNotFound → 跳过；其余错误上抛。
std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::next() {
    if (prefetch_ > 1) {
        // 预取路径：缓冲空则再取一批（整批 key 全死时 buf_ 为空而迭代
        // 未结束——循环直到有货或到尾）。
        while (buf_pos_ >= buf_.size()) {
            if (done_) return std::optional<Entry>{};
            auto f = fill_prefetch();
            if (!f) return std::unexpected(f.error());
        }
        return std::optional<Entry>(std::move(buf_[buf_pos_++]));
    }

    while (true) {
        auto k = next_merged_key();
        if (!k) return std::unexpected(k.error());
        if (!*k) return std::optional<Entry>{};
        const std::string& key = **k;

        auto g = cask_->get_owned(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(key.data()), key.size()));
        if (!g) {
            if (g.error().kind == CaskError::kNotFound) continue;
            return std::unexpected(g.error());
        }
        Entry out;
        const auto* kp = reinterpret_cast<const std::byte*>(key.data());
        out.key.assign(kp, kp + key.size());
        out.value = std::move(g->value);
        out.tstamp = g->tstamp;
        out.ord = g->ord;
        return std::optional<Entry>(std::move(out));
    }
}

// S33-6：预取一批。归并 ≤prefetch_ 个 key（串行、廉价，只碰 run 块与
// memdelta）→ 分段并发 get 取值（被并行化的是 value 的 pread+decode，
// 与 parallel_scan 同款 JoiningPool）→ 按 key 序压实进 buf_。
// 死 key（并发删除 / OKI 行陈旧）与 parallel_scan 同样静默跳过。
std::expected<void, CaskFault> CaskRangeIter::fill_prefetch() {
    buf_.clear();
    buf_pos_ = 0;

    std::vector<std::string> keys;
    keys.reserve(prefetch_);
    while (keys.size() < prefetch_) {
        auto k = next_merged_key();
        if (!k) return std::unexpected(k.error());
        if (!*k) break;  // done_ 已由 next_merged_key 置位
        keys.push_back(*std::move(*k));
    }
    if (keys.empty()) return {};

    const std::size_t total = keys.size();
    std::size_t nthr = prefetch_threads_;
    if (nthr == 0) {
        nthr = std::min<std::size_t>(
            std::max<std::size_t>(std::thread::hardware_concurrency(), 1), 4);
    }
    nthr = std::min(nthr, total);
    // 线程是**每批**创建的（无常驻池）——单次创建 ~20µs 级，批太小时这笔
    // 成本会盖过并行取值的收益（实测：批 64 × 4 线程比惰性还慢 50%，批
    // 256 × 4 线程则快 1.6×）。故按「每线程至少 kMinKeysPerThread 个 key」
    // 收窄线程数：小批自动退化为串行（= 惰性路径的成本，不亏）。
    constexpr std::size_t kMinKeysPerThread = 64;
    nthr = std::min(nthr, (total + kMinKeysPerThread - 1) / kMinKeysPerThread);

    // 槽位与 keys 一一对应（保序）；死 key 留 nullopt，最后压实。
    std::vector<std::optional<Entry>> slots(total);
    std::atomic<bool> ok{true};
    std::mutex err_mu;
    CaskFault first_err{};
    auto worker = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            if (!ok.load(std::memory_order_relaxed)) return;
            auto g = cask_->get_owned(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(keys[i].data()),
                keys[i].size()));
            if (!g) {
                if (g.error().kind == CaskError::kNotFound) continue;
                std::lock_guard<std::mutex> lk(err_mu);
                if (ok.exchange(false)) first_err = g.error();
                return;
            }
            Entry e;
            const auto* kp =
                reinterpret_cast<const std::byte*>(keys[i].data());
            e.key.assign(kp, kp + keys[i].size());
            e.value = std::move(g->value);
            e.tstamp = g->tstamp;
            e.ord = g->ord;
            slots[i] = std::move(e);
        }
    };

    if (nthr <= 1) {
        worker(0, total);
    } else {
        // 与 Cask::parallel_scan 同款 RAII join guard：emplace_back 抛异常
        // （资源耗尽）时已创建的 joinable 线程被析构自动 join。
        struct JoiningPool {
            std::vector<std::thread> threads;
            ~JoiningPool() {
                for (auto& t : threads) if (t.joinable()) t.join();
            }
        } ws;
        ws.threads.reserve(nthr);
        const std::size_t chunk = (total + nthr - 1) / nthr;
        for (std::size_t t = 0; t < nthr; ++t) {
            const std::size_t b = t * chunk;
            if (b >= total) break;
            ws.threads.emplace_back(worker, b, std::min(b + chunk, total));
        }
        for (auto& w : ws.threads) w.join();
    }
    if (!ok.load()) return std::unexpected(first_err);

    buf_.reserve(total);
    for (auto& s : slots) {
        if (s) buf_.push_back(*std::move(s));
    }
    return {};
}

}  // namespace bitcask
