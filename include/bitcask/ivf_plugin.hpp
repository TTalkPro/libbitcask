// IvfPlugin — IVF 磁盘档向量引擎插件（S32-M3；设计
// doc/vector-dual-engine-selection-zh.md §4/§5.1）。
//
// 结构/持久化/线程模型等全部语义下沉到 SealedSegmentVectorPlugin（基类
// 模板 SealedSegmentVectorPlugin<IvfSegment>）— 本类仅按 IVF 引擎语义
// 实现 6 个纯虚 hook（ckpt_name / segment_path_ext / section_type /
// blob_size / open_sealed / build_sealed / search_sealed / write_blob_tail
// / plugin_name）。窗口恒为 HnswIndex（设计 §5.1）。

#pragma once

#include "bitcask/detail/sealed_segment_vector_plugin.hpp"
#include "bitcask/ivf_rq.hpp"
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::vec {

class IvfPlugin final
    : public detail::SealedSegmentVectorPlugin<IvfSegment> {
public:
    using detail::SealedSegmentVectorPlugin<IvfSegment>::
        SealedSegmentVectorPlugin;

    IvfPlugin(const IvfPlugin&) = delete;
    IvfPlugin& operator=(const IvfPlugin&) = delete;

    [[nodiscard]] std::string_view name() const override {
        return "ivfrq";
    }

protected:
    [[nodiscard]] const char* ckpt_name() const noexcept override {
        return "ivf.ckpt";
    }
    [[nodiscard]] std::string
    segment_path_ext(std::string_view base) const override {
        return bitcask::detail::to_utf8(bitcask::detail::from_utf8(base).replace_extension(".biv"));
    }
    [[nodiscard]] search::CkptSectionType
    section_type() const noexcept override {
        return search::CkptSectionType::kIvf;
    }
    [[nodiscard]] std::size_t blob_size() const noexcept override {
        return 8 + 8 + 8 + 2 + 4;  // gen | count | max_ord | dim | nlist
    }
    [[nodiscard]] std::shared_ptr<IvfSegment>
    open_sealed(std::string_view path, std::uint16_t dim,
                std::uint64_t expected_gen, bool verify_crc) const override;
    [[nodiscard]] bool
    build_sealed(std::string_view path, std::uint16_t dim,
                 const IvfBuildSource& src, std::uint64_t gen) const override;
    [[nodiscard]] std::vector<detail::EngineHit>
    search_sealed(const IvfSegment& seg, std::span<const float> q,
                  std::size_t k, std::size_t ef,
                  const std::function<bool(std::uint64_t)>* live) const override;
    void write_blob_tail(std::vector<std::byte>& b,
                         const IvfSegment& seg) const override;
};

}  // namespace bitcask::vec