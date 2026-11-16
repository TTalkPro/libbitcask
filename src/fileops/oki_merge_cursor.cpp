#include "bitcask/oki_merge_cursor.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace bitcask::oki {

std::expected<OkiMergeCursor, OkiError>
OkiMergeCursor::open_impl(std::vector<std::shared_ptr<OkiRunReader>> runs,
                          std::vector<std::uint64_t> gens,
                          std::shared_ptr<OkiBlockCache> cache,
                          std::vector<MemSource> mem,
                          std::span<const std::byte> lo) {
    OkiMergeCursor m;
    m.runs_ = std::move(runs);
    m.mem_ = std::move(mem);
    m.cache_ = std::move(cache);
    const bool cached_seek = m.cache_ && gens.size() == m.runs_.size();
    m.cursors_.reserve(m.runs_.size());
    m.heads_.reserve(m.runs_.size());
    for (std::size_t i = 0; i < m.runs_.size(); ++i) {
        const auto& r = m.runs_[i];
        auto c = lo.empty()   ? std::expected<OkiRunReader::Cursor, OkiError>(
                                    r->begin())
                 : cached_seek ? r->seek_cached(lo, m.cache_.get(), gens[i])
                               : r->seek(lo);
        if (!c) return std::unexpected(c.error());
        OkiRunReader::Entry e;
        auto n = c->next(e);
        if (!n) return std::unexpected(n.error());
        m.cursors_.push_back(std::move(*c));
        m.heads_.push_back(*n ? std::optional<OkiRunReader::Entry>(std::move(e))
                              : std::nullopt);
    }
    const std::string_view lov(reinterpret_cast<const char*>(lo.data()),
                               lo.size());
    m.mem_pos_.assign(m.mem_.size(), 0);
    for (std::size_t i = 0; i < m.mem_.size(); ++i) {
        const auto& src = m.mem_[i];
        if (!src || lo.empty()) continue;
        m.mem_pos_[i] = static_cast<std::size_t>(
            std::lower_bound(src->begin(), src->end(), lov,
                             [](const Row& r, std::string_view v) {
                                 return r.key < v;
                             }) -
            src->begin());
    }
    return m;
}

// 来源编号：run i → i（gen 升序），内存来源 j → runs 数 + j（旧→新）；编号越大
// 来源序越高。堆按「当前 key 最小」出堆（O(log 来源数)），原实现每行线性扫两遍
// 全部来源——多 run 的全量迭代因此按 run 数线性变慢。
std::string_view OkiMergeCursor::head_key(std::size_t src) const {
    if (src < heads_.size()) return heads_[src]->key;
    const std::size_t j = src - heads_.size();
    return (*mem_[j])[mem_pos_[j]].key;
}

bool OkiMergeCursor::source_live(std::size_t src) const {
    if (src < heads_.size()) return heads_[src].has_value();
    const std::size_t j = src - heads_.size();
    return mem_[j] && mem_pos_[j] < mem_[j]->size();
}

namespace {

// key 字典序比较的快路径：先比前 8 字节（大端装载成整数），只有前缀相同才
// 退回 memcmp。归并的每行都要做若干次比较，profile 里 memcmp 调用本身占了
// 全量迭代约 1/4。
inline std::uint64_t load_be8(const char* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

inline int key_cmp(std::string_view a, std::string_view b) noexcept {
    if (a.size() >= 8 && b.size() >= 8) {
        const auto x = load_be8(a.data());
        const auto y = load_be8(b.data());
        if (x != y) return x < y ? -1 : 1;
    }
    return a.compare(b);
}

}  // namespace

// 小顶堆的下沉（提前退出）：被替换的根若不大于两个子节点即停。常见情况下
// 刚推进的那一路仍是最小者，每行只需 2 次比较（原 pop_heap + push_heap
// 两次完整调整）。
void OkiMergeCursor::sift_down(std::size_t i) {
    const std::size_t n = heap_.size();
    const std::size_t x = heap_[i];
    const std::string_view xk = head_key(x);
    while (true) {
        std::size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && key_cmp(head_key(heap_[c + 1]), head_key(heap_[c])) < 0) ++c;
        if (key_cmp(head_key(heap_[c]), xk) >= 0) break;
        heap_[i] = heap_[c];
        i = c;
    }
    heap_[i] = x;
}

std::expected<bool, OkiError> OkiMergeCursor::next(Row& out,
                                                    bool include_tombs) {
    if (!heap_built_) {
        heap_.clear();
        const std::size_t total = heads_.size() + mem_.size();
        for (std::size_t i = 0; i < total; ++i) {
            if (source_live(i)) heap_.push_back(i);
        }
        for (std::size_t i = heap_.size() / 2; i-- > 0;) sift_down(i);
        heap_built_ = true;
    }
    while (!done_) {
        if (heap_.empty()) {
            done_ = true;
            break;
        }
        // 取出全部「当前 key == 最小 key」的来源（同 key 的来源在堆顶附近，
        // 逐个移出堆顶）。推进 run 游标会复用其 key 缓冲，故先物化当前 key。
        key_.assign(head_key(heap_.front()));
        same_.clear();
        while (!heap_.empty() && head_key(heap_.front()) == key_) {
            same_.push_back(heap_.front());
            heap_.front() = heap_.back();
            heap_.pop_back();
            if (!heap_.empty()) sift_down(0);
        }
        // 胜出：max (ord, 来源编号)。
        std::sort(same_.begin(), same_.end());
        bool have = false;
        for (const std::size_t src : same_) {
            if (src < heads_.size()) {
                const auto& h = *heads_[src];
                if (!have || h.ord >= out.ord) {
                    out.ord = h.ord;
                    out.tomb = h.tomb;
                    out.has_loc = h.has_loc;
                    out.loc = h.loc;
                    have = true;
                }
            } else {
                const std::size_t j = src - heads_.size();
                const Row& d = (*mem_[j])[mem_pos_[j]];
                if (!have || d.ord >= out.ord) {
                    out.ord = d.ord;
                    out.tomb = d.tomb;
                    out.has_loc = d.has_loc;
                    out.loc = d.loc;
                    have = true;
                }
            }
        }
        // 推进这些来源并放回堆。
        for (const std::size_t src : same_) {
            if (src < heads_.size()) {
                auto& h = heads_[src];
                auto n = cursors_[src].next(*h);
                if (!n) return std::unexpected(n.error());
                if (!*n) h.reset();
            } else {
                ++mem_pos_[src - heads_.size()];
            }
            if (source_live(src)) {
                // 上浮插入（小顶堆）。
                heap_.push_back(src);
                std::size_t i = heap_.size() - 1;
                const std::string_view sk = head_key(src);
                while (i > 0) {
                    const std::size_t p = (i - 1) / 2;
                    if (key_cmp(head_key(heap_[p]), sk) <= 0) break;
                    heap_[i] = heap_[p];
                    i = p;
                }
                heap_[i] = src;
            }
        }
        if (out.tomb && !include_tombs) continue;
        out.key.assign(key_);
        out.dur_file = 0;
        out.dur_end = 0;
        out.seq = 0;
        return true;
    }
    return false;
}

}  // namespace bitcask::oki
