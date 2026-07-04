// HybridSearcher 实现（S18-9）。RRF 融合自 SearchLayer::search_hybrid 逐行
// 平移——算法与确定性平局序不变。

#include "bitcask/hybrid_searcher.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace bitcask::search {

std::expected<std::vector<SearchHit>, SearchError>
HybridSearcher::search(std::string_view text_query,
                       std::span<const float> vec_query, std::size_t k,
                       const meta::MetaFilter* filter) const {
    // 两路都空才报错;单路空 = 退化为另一路的 RRF 重打分(hnsw-design §4)。
    if (text_query.empty() && vec_query.empty()) {
        return std::unexpected(SearchError::kEmptyHybridQuery);
    }
    const std::size_t kp = std::max<std::size_t>(k * 4, 64);  // K'

    // V5:filter 独立走两条路(text 后过滤 + vec 折 live callback)——只有
    // 同时通过两路 filter 的文档才进 RRF 融合,符合「filter 收紧 live」语义。
    // S7：单查询两路串行（见头文件注释）。
    std::vector<SearchHit> text_hits;
    if (!text_query.empty()) {
        auto t = text_.search_text(text_query, kp, nullptr, filter);
        if (!t) return std::unexpected(std::move(t.error()));
        text_hits = std::move(*t);
    }
    std::vector<SearchHit> vec_hits;
    if (!vec_query.empty()) {
        // ef=0 + filter → vec 路本身已在 live callback 里过滤;无需 overfetch。
        auto v = vec_.search(vec_query, kp, 0, filter);
        if (!v) return std::unexpected(std::move(v.error()));  // 维度不符等
        vec_hits = std::move(*v);
    }

    // Reciprocal Rank Fusion（Cormack, Clarke, Buettcher 2009）：按 ord 并桶，
    // score = Σ 1/(k + rank_i)，rank 从 1 起，k=60 为论文经验常数。
    struct Fused {
        SearchHit hit;
        double score = 0.0;
    };
    std::unordered_map<std::uint64_t, Fused> acc;
    acc.reserve(text_hits.size() + vec_hits.size());
    auto fold_leg = [&acc](std::vector<SearchHit>& leg) {
        for (std::size_t i = 0; i < leg.size(); ++i) {
            auto [it, fresh] = acc.try_emplace(leg[i].ord);
            if (fresh) it->second.hit = std::move(leg[i]);
            it->second.score += 1.0 / (60.0 + static_cast<double>(i + 1));
        }
    };
    fold_leg(text_hits);
    fold_leg(vec_hits);

    std::vector<SearchHit> fused;
    fused.reserve(acc.size());
    for (auto& [ord, f] : acc) {
        f.hit.score = f.score;  // score = RRF 分(替换掉单路原始分)
        fused.push_back(std::move(f.hit));
    }
    // 确定性平局序:RRF 分相等 → ord 小者在前(测试锁此行为)。
    std::sort(fused.begin(), fused.end(),
              [](const SearchHit& a, const SearchHit& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.ord < b.ord;
              });
    if (fused.size() > k) fused.resize(k);
    return fused;
}

}  // namespace bitcask::search
