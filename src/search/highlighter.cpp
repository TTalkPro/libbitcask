#include "bitcask/highlighter.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace bitcask::search {

namespace {

struct OffsetRange {
    std::uint32_t start;
    std::uint32_t end;
};

std::vector<OffsetRange> collect_query_ranges(
    const std::unordered_map<std::string, std::vector<text::TokenInfo>>& query_token_offsets) {
    std::vector<OffsetRange> ranges;
    for (auto& [_, infos] : query_token_offsets) {
        for (auto& info : infos) {
            if (info.end_byte > info.start_byte) {
                ranges.push_back({info.start_byte, info.end_byte});
            }
        }
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const auto& a, const auto& b) { return a.start < b.start; });
    return ranges;
}

std::vector<OffsetRange> select_best_fragments(
    const std::vector<OffsetRange>& sorted_ranges,
    std::size_t fragment_size,
    std::size_t max_fragments) {
    if (sorted_ranges.empty()) return {};

    // S9.20：每轮在「尚未被已选片段覆盖」的 range 里贪心选最佳窗口，选定后
    // 移除窗口内的 range，再选下一个——避免反复选中同一窗口产出重复片段。
    std::vector<OffsetRange> remaining_ranges(sorted_ranges);
    std::vector<OffsetRange> selected;
    std::size_t remaining = max_fragments;

    while (remaining > 0 && !remaining_ranges.empty()) {
        std::size_t best_idx = 0;
        std::size_t best_count = 0;

        // C1:remaining_ranges 已按 start 排序 → 用 lower/upper_bound 计数窗口内 range
        // 数（O(log R)），替代旧 O(R) 线性扫描 → 每轮 O(R log R) 替代 O(R²)。
        const auto cmp_start = [](const OffsetRange& r, std::size_t val) {
            return r.start < val;
        };
        for (std::size_t ai = 0; ai < remaining_ranges.size(); ++ai) {
            const std::size_t window_start = remaining_ranges[ai].start;
            const std::size_t window_end = window_start + fragment_size;

            auto lo = std::lower_bound(remaining_ranges.begin(),
                                       remaining_ranges.end(),
                                       window_start, cmp_start);
            auto hi = std::lower_bound(remaining_ranges.begin(),
                                       remaining_ranges.end(),
                                       window_end, cmp_start);
            const std::size_t count = static_cast<std::size_t>(hi - lo);

            if (count > best_count) {
                best_count = count;
                best_idx = ai;
            }
        }

        std::size_t best_start = remaining_ranges[best_idx].start;
        std::size_t best_end = std::min<std::size_t>(
            best_start + fragment_size, remaining_ranges.back().end);

        if (best_count == 0) {
            best_start = remaining_ranges[0].start;
            best_end = std::min<std::size_t>(best_start + fragment_size,
                                             remaining_ranges.back().end);
        }

        selected.push_back({static_cast<std::uint32_t>(best_start),
                            static_cast<std::uint32_t>(best_end)});

        // 消费掉落在本片段窗口内的 range，下一轮只在剩余 range 中选。
        remaining_ranges.erase(
            std::remove_if(remaining_ranges.begin(), remaining_ranges.end(),
                           [&](const OffsetRange& r) {
                               return r.start >= best_start && r.start < best_end;
                           }),
            remaining_ranges.end());
        --remaining;
    }

    return selected;
}

}  // anonymous namespace

HighlightResult highlight(
    std::string_view text,
    const std::unordered_map<std::string, std::vector<text::TokenInfo>>& query_token_offsets,
    const HighlightOptions& opts) {
    HighlightResult result;

    auto ranges = collect_query_ranges(query_token_offsets);
    if (ranges.empty()) return result;

    auto fragments = select_best_fragments(ranges, opts.fragment_size, opts.max_fragments);
    if (fragments.empty()) return result;

    for (auto& frag : fragments) {
        std::uint32_t frag_start = frag.start;
        std::uint32_t frag_end = frag.end;

        if (frag_end > text.size()) {
            frag_end = static_cast<std::uint32_t>(text.size());
        }
        if (frag_start >= frag_end) {
            continue;
        }

        // S55：片段内的命中区间先按位置排序、合并重叠 / 相接的，再插标签。原实现按
        // unordered_map 的遍历序逐词处理、只收 start >= pos 的出现 ⇒ 多个查询词时位置在前、
        // 遍历在后的词标不出；ngram 的 token 互相重叠（「着色」「色器」「着色器」）只标出一段残片。
        std::vector<OffsetRange> marks;
        for (auto& [term, infos] : query_token_offsets) {
            (void)term;
            for (auto& info : infos) {
                if (info.end_byte > info.start_byte && info.start_byte >= frag_start &&
                    info.end_byte <= frag_end) {
                    marks.push_back({info.start_byte, info.end_byte});
                }
            }
        }
        std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) {
            return a.start != b.start ? a.start < b.start : a.end > b.end;
        });
        std::vector<OffsetRange> merged;
        for (const auto& m : marks) {
            if (!merged.empty() && m.start <= merged.back().end) {
                merged.back().end = std::max(merged.back().end, m.end);
            } else {
                merged.push_back(m);
            }
        }

        std::string highlighted;
        highlighted.reserve(frag_end - frag_start + 64);

        std::size_t pos = frag_start;
        for (const auto& m : merged) {
            highlighted.append(text.data() + pos, m.start - pos);
            highlighted.append(opts.pre_tag);
            highlighted.append(text.data() + m.start, m.end - m.start);
            highlighted.append(opts.post_tag);
            pos = m.end;
        }

        if (pos < frag_end) {
            highlighted.append(text.data() + pos, frag_end - pos);
        }

        std::size_t match_count = 0;
        for (auto& [term, infos] : query_token_offsets) {
            (void)term;
            for (auto& info : infos) {
                if (info.start_byte >= frag_start && info.end_byte <= frag_end) {
                    ++match_count;
                }
            }
        }

        result.snippets.push_back({std::move(highlighted), static_cast<double>(match_count)});
    }

    return result;
}

}  // namespace bitcask::search