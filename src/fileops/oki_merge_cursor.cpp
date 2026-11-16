#include "bitcask/oki_merge_cursor.hpp"

#include <algorithm>
#include <string_view>

namespace bitcask::oki {

std::expected<OkiMergeCursor, OkiError>
OkiMergeCursor::open(std::vector<std::shared_ptr<OkiRunReader>> runs,
                     std::shared_ptr<const std::vector<Row>> delta,
                     std::span<const std::byte> lo) {
    OkiMergeCursor m;
    m.runs_ = std::move(runs);
    m.delta_ = std::move(delta);
    m.cursors_.reserve(m.runs_.size());
    m.heads_.reserve(m.runs_.size());
    for (const auto& r : m.runs_) {
        auto c = lo.empty() ? std::expected<OkiRunReader::Cursor, OkiError>(
                                  r->begin())
                            : r->seek(lo);
        if (!c) return std::unexpected(c.error());
        OkiRunReader::Entry e;
        auto n = c->next(e);
        if (!n) return std::unexpected(n.error());
        m.cursors_.push_back(std::move(*c));
        m.heads_.push_back(*n ? std::optional<OkiRunReader::Entry>(std::move(e))
                              : std::nullopt);
    }
    if (m.delta_ && !lo.empty()) {
        const std::string_view lov(reinterpret_cast<const char*>(lo.data()),
                                   lo.size());
        m.delta_pos_ = static_cast<std::size_t>(
            std::lower_bound(m.delta_->begin(), m.delta_->end(), lov,
                             [](const Row& r, std::string_view v) {
                                 return r.key < v;
                             }) -
            m.delta_->begin());
    }
    return m;
}

std::expected<bool, OkiError> OkiMergeCursor::next(Row& out,
                                                    bool include_tombs) {
    const std::size_t dn = delta_ ? delta_->size() : 0;
    while (!done_) {
        std::string_view min_key;
        bool any = false;
        for (const auto& h : heads_) {
            if (h && (!any || std::string_view(h->key) < min_key)) {
                min_key = h->key;
                any = true;
            }
        }
        if (delta_pos_ < dn) {
            const std::string_view dk((*delta_)[delta_pos_].key);
            if (!any || dk < min_key) {
                min_key = dk;
                any = true;
            }
        }
        if (!any) {
            done_ = true;
            break;
        }
        // 推进 run 游标会复用 head.key 缓冲：先物化当前 key。
        const std::string key(min_key);
        bool have = false;
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            auto& h = heads_[i];
            if (!h || h->key != key) continue;
            if (!have || h->ord >= out.ord) {  // >=：更高 gen 胜
                out.ord = h->ord;
                out.tomb = h->tomb;
                out.has_loc = h->has_loc;
                out.loc = h->loc;
                have = true;
            }
            auto n = cursors_[i].next(*h);
            if (!n) return std::unexpected(n.error());
            if (!*n) h.reset();
        }
        if (delta_pos_ < dn && (*delta_)[delta_pos_].key == key) {
            const Row& d = (*delta_)[delta_pos_];
            if (!have || d.ord >= out.ord) {  // delta = 最高来源
                out.ord = d.ord;
                out.tomb = d.tomb;
                out.has_loc = d.has_loc;
                out.loc = d.loc;
                have = true;
            }
            ++delta_pos_;
        }
        if (out.tomb && !include_tombs) continue;
        out.key = key;
        out.dur_file = 0;
        out.dur_end = 0;
        out.seq = 0;
        return true;
    }
    return false;
}

}  // namespace bitcask::oki
