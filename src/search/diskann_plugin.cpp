// DiskannPlugin 实现（S32-M5）— 仅 DiskANN 引擎 hook；公共骨架见
// detail/sealed_segment_vector_plugin.hpp。
#include "bitcask/diskann_plugin.hpp"

namespace bitcask::vec {

std::shared_ptr<DiskannSegment>
DiskannPlugin::open_sealed(std::string_view path, std::uint16_t dim,
                           std::uint64_t expected_gen, bool verify_crc) const {
    auto seg = std::make_shared<DiskannSegment>();
    if (!seg->open(path, dim, expected_gen, verify_crc)) {
        return nullptr;
    }
    return seg;
}

bool DiskannPlugin::build_sealed(std::string_view path, std::uint16_t dim,
                                 const IvfBuildSource& src,
                                 std::uint64_t gen) const {
    return DiskannSegment::build(path, dim, src, config_.diskann_r,
                                 config_.diskann_l_build, gen);
}

std::vector<detail::EngineHit>
DiskannPlugin::search_sealed(const DiskannSegment& seg,
                             std::span<const float> q, std::size_t k,
                             std::size_t ef,
                             const std::function<bool(std::uint64_t)>* live) const {
    // DiskANN ef = beam 宽 L；段内 0 → max(2k,64) 自适应。
    const auto l = static_cast<std::uint32_t>(ef);
    const auto& hits = seg.search(q, k, l, live);
    std::vector<detail::EngineHit> out;
    out.reserve(hits.size());
    for (const auto& h : hits) {
        out.push_back({h.ord, h.score});
    }
    return out;
}

void DiskannPlugin::write_blob_tail(std::vector<std::byte>& b,
                                    const DiskannSegment& seg) const {
    search::detail::put_u32(b, seg.r());
}

}  // namespace bitcask::vec