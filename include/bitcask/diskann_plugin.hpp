// DiskannPlugin — DiskANN 磁盘档向量引擎插件（S32-M5；设计
// doc/vector-dual-engine-selection-zh.md §4/§5.2）。
//
// 结构/持久化/线程模型等全部语义下沉到 SealedSegmentVectorPlugin（基类
// 模板 SealedSegmentVectorPlugin<DiskannSegment>）— 本类仅按 DiskANN
// 引擎语义实现纯虚 hook。窗口恒为 HnswIndex。

#pragma once

#include "bitcask/detail/sealed_segment_vector_plugin.hpp"
#include "bitcask/diskann.hpp"
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::vec {

class DiskannPlugin final
    : public detail::SealedSegmentVectorPlugin<DiskannSegment> {
public:
    using detail::SealedSegmentVectorPlugin<DiskannSegment>::
        SealedSegmentVectorPlugin;

    DiskannPlugin(const DiskannPlugin&) = delete;
    DiskannPlugin& operator=(const DiskannPlugin&) = delete;

    [[nodiscard]] std::string_view name() const override {
        return "diskann";
    }

protected:
    [[nodiscard]] const char* ckpt_name() const noexcept override {
        return "diskann.ckpt";
    }
    [[nodiscard]] std::string
    segment_path_ext(std::string_view base) const override {
        return bitcask::detail::to_utf8(bitcask::detail::from_utf8(base).replace_extension(".bda"));
    }
    [[nodiscard]] search::CkptSectionType
    section_type() const noexcept override {
        return search::CkptSectionType::kDiskann;
    }
    [[nodiscard]] std::size_t blob_size() const noexcept override {
        return 8 + 8 + 8 + 2 + 4;  // gen | count | max_ord | dim | R
    }
    [[nodiscard]] std::shared_ptr<DiskannSegment>
    open_sealed(std::string_view path, std::uint16_t dim,
                std::uint64_t expected_gen, bool verify_crc) const override;
    [[nodiscard]] bool
    build_sealed(std::string_view path, std::uint16_t dim,
                 const IvfBuildSource& src, std::uint64_t gen) const override;
    [[nodiscard]] std::vector<detail::EngineHit>
    search_sealed(const DiskannSegment& seg, std::span<const float> q,
                  std::size_t k, std::size_t ef,
                  const std::function<bool(std::uint64_t)>* live) const override;
    void write_blob_tail(std::vector<std::byte>& b,
                         const DiskannSegment& seg) const override;
};

}  // namespace bitcask::vec