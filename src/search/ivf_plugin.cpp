// IvfPlugin 实现（S32-M3）— 仅 IVF 引擎 hook；公共骨架见
// detail/sealed_segment_vector_plugin.hpp。
#include "bitcask/ivf_plugin.hpp"

namespace bitcask::vec {

std::shared_ptr<IvfSegment>
IvfPlugin::open_sealed(std::string_view path, std::uint16_t dim,
                       std::uint64_t expected_gen, bool verify_crc) const {
    auto seg = std::make_shared<IvfSegment>();
    if (!seg->open(path, dim, expected_gen, verify_crc)) {
        return nullptr;
    }
    return seg;
}

bool IvfPlugin::build_sealed(std::string_view path, std::uint16_t dim,
                             const IvfBuildSource& src,
                             std::uint64_t gen) const {
    return IvfSegment::build(path, dim, src, config_.ivf_nlist, gen);
}

std::vector<detail::EngineHit>
IvfPlugin::search_sealed(const IvfSegment& seg, std::span<const float> q,
                         std::size_t k, std::size_t ef,
                         const std::function<bool(std::uint64_t)>* live) const {
    // ef → nprobe：0 = config 默认；非 0 = 调用方旋钮。
    const std::uint32_t nprobe =
        ef != 0 ? static_cast<std::uint32_t>(ef) : config_.ivf_nprobe;
    const auto& hits = seg.search(q, k, nprobe, live);
    std::vector<detail::EngineHit> out;
    out.reserve(hits.size());
    for (const auto& h : hits) {
        out.push_back({h.ord, h.score});
    }
    return out;
}

void IvfPlugin::write_blob_tail(std::vector<std::byte>& b,
                                const IvfSegment& seg) const {
    search::detail::put_u32(b, seg.nlist());
}

}  // namespace bitcask::vec