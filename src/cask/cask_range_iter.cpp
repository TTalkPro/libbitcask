// cask_range_iter.cpp — S33-5：OKI 有序 Range 迭代器。
// 语义与架构见 cask.hpp CaskRangeIter 类注释与
// doc/ordered-key-index-design-zh.md §4。

#include "bitcask/cask.hpp"

#include <algorithm>

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

std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::next() {
    if (done_) return std::optional<Entry>{};

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
            return std::optional<Entry>{};
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

        // 4) 回查 keydir 取权威值（key 可能已死 / 已被更新——OKI 行陈旧
        //    无害）。kNotFound → 跳过；其余错误上抛。
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

}  // namespace bitcask
