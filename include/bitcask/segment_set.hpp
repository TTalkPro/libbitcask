// 段集 / 段管理器（SegmentSet，S27-2 Slice 4）。见 doc/segment-index-design-zh.md §3.1/§3.2/§8。
//
// 管理一个目录下的一批活跃 SealedSegment + 活跃段清单（segments.manifest）。
// 清单是唯一原子提交点（复用 SearchCheckpoint 的 tmp+rename + 段级 CRC）。
// 段生命周期（Building→Flushed→Merging→Dropped）中，本类负责 Flushed 段的
// 登记/查询/drop；Building 与 merge 编排留待 S27-3/4 接线到 live 路径。
//
// 本阶段为**隔离基础设施**（header-only，独立测试），未接入 Cask/IndexPool/plugin。

#pragma once

#include "bitcask/search_checkpoint.hpp"
#include "bitcask/segment.hpp"
#include "bitcask/segment_query.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bitcask::search {

class SegmentSet {
public:
    struct Entry {
        std::string   filename;   // basename（join(dir_) 得全路径）
        std::uint64_t seg_id;
        std::uint64_t hi_lsn;     // 段覆盖的最高 LSN
        std::uint64_t doc_count;
    };

    SegmentSet() = default;
    SegmentSet(const SegmentSet&) = delete;
    SegmentSet& operator=(const SegmentSet&) = delete;

    // 打开目录：读 manifest → 载入各活跃段。无 manifest → 空集。
    // manifest CRC 坏 / 任一段载入失败 → nullptr（保守：退全量重建）。
    [[nodiscard]] static std::unique_ptr<SegmentSet> open(const std::string& dir) {
        auto set = std::make_unique<SegmentSet>();
        set->dir_ = dir;
        auto lc = SearchCheckpoint::read(manifest_path(dir));
        if (!lc) return set;  // 无清单 = 空段集（首次 open）
        set->next_seg_id_ = lc->watermark;
        bool decoded = false;
        for (const auto& s : lc->sections) {
            if (!s.crc_ok) return nullptr;
            if (static_cast<CkptSectionType>(s.type) != CkptSectionType::kSegManifest)
                continue;
            if (!set->decode_manifest(
                    std::span<const std::byte>(s.payload.data(), s.payload.size())))
                return nullptr;
            decoded = true;
        }
        if (!decoded) return nullptr;
        for (const auto& e : set->entries_) {
            auto seg = SealedSegment::load(join(dir, e.filename));
            if (!seg) return nullptr;  // 段损坏 → 整体拒收
            set->segments_.push_back(std::move(seg));
        }
        return set;
    }

    // ---- S27-3 B2b 步骤 1:持久化解耦(设计 s27-3-b2b-recovery-design §3) ----
    // add/drop 拆为 pending(段文件落盘 + 内存登记,**不提交清单**)与
    // commit(清单提交)。过渡期 TextPlugin 在 checkpoint(save_component_base)
    // 统一 commit + 把清单写入 bm25.ckpt(kSegManifest);recovery 重写(步骤 4)
    // 后 segments.manifest 退役,index.manifest 成为唯一 commit point。

    // 落盘段文件 + 内存登记。**仅成功时取走所有权**——失败时 seg 留在 caller
    // 手里(修正原 add 按值取走的缺陷:失败路径 caller 拿回的是空指针,
    // Building 段丢失且后续 apply 解引用空 building_)。
    [[nodiscard]] bool add_pending(std::unique_ptr<SealedSegment>& seg,
                                   std::uint64_t hi_lsn) {
        const std::uint64_t id = next_seg_id_;
        const std::string fname = "seg-" + std::to_string(id) + ".seg";
        if (!seg->save(join(dir_, fname), hi_lsn)) return false;
        entries_.push_back(Entry{fname, id, hi_lsn, seg->doc_count()});
        segments_.push_back(std::move(seg));
        ++next_seg_id_;
        return true;
    }

    // 内存态移除;段文件 unlink 延后到 commit **之后**(先清单后删文件——
    // 崩溃窗口只留孤儿段文件,open 忽略;原 drop 反序有「清单仍列已删文件
    // → open 整体拒收 → 退全量重建」窗口,顺带修正)。
    [[nodiscard]] bool drop_pending(std::uint64_t seg_id) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].seg_id != seg_id) continue;
            pending_unlink_.push_back(entries_[i].filename);
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            segments_.erase(segments_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }

    // 提交清单(segments.manifest,过渡期)+ 清理已 drop 的段文件。
    [[nodiscard]] bool commit() {
        if (!commit_manifest()) return false;
        for (const auto& f : pending_unlink_) {
            std::error_code ec;
            std::filesystem::remove(join(dir_, f), ec);
        }
        pending_unlink_.clear();
        return true;
    }

    // 段清单编码(bm25.ckpt 的 kSegManifest section 载荷;与
    // segments.manifest 内容同格式——步骤 4 recovery 从这里读)。
    [[nodiscard]] std::vector<std::byte> manifest_payload() const {
        return encode_manifest();
    }

    // 兼容包装(既有测试/独立使用):pending + 立即 commit。
    [[nodiscard]] bool add(std::unique_ptr<SealedSegment> seg,
                           std::uint64_t hi_lsn) {
        if (!add_pending(seg, hi_lsn)) return false;
        if (!commit()) {  // 提交失败 → 回滚内存态（段文件残留由下次 open 忽略）
            entries_.pop_back();
            segments_.pop_back();
            return false;
        }
        return true;
    }

    // 查询：跨所有活跃段 G-on-the-fly 归并（§3.5）。
    [[nodiscard]] std::vector<SearchHit> search(
        const std::vector<std::string>& terms, std::size_t k,
        const bm25::Bm25Params* params = nullptr) const {
        std::vector<SegmentView> views;
        views.reserve(segments_.size());
        for (const auto& s : segments_) views.push_back(s->view());
        return multi_segment_search(views, terms, k, params);
    }

    // 移除一个段（兼容包装）：pending + 立即 commit（清单先行,再删文件）。
    [[nodiscard]] bool drop(std::uint64_t seg_id) {
        if (!drop_pending(seg_id)) return false;
        return commit();
    }

    [[nodiscard]] std::size_t segment_count() const { return segments_.size(); }
    [[nodiscard]] std::uint64_t next_seg_id() const { return next_seg_id_; }
    [[nodiscard]] std::size_t total_docs() const {
        std::size_t n = 0;
        for (const auto& s : segments_) n += s->doc_count();
        return n;
    }

    // ---- S27-3 Slice B1 接入 ----
    // 按 seg_id 查段（in-memory 访问，O(N) 扫——段集通常 <一二十）；不存在
    // 返回 nullptr。**返回非 const**——caller 可能调 mark_dead（设计 §3.4
    // 允许的封口后 mutation）。
    [[nodiscard]] SealedSegment* segment(std::uint64_t seg_id) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].seg_id == seg_id) return segments_[i].get();
        }
        return nullptr;
    }
    // const 重载（只读访问）。
    [[nodiscard]] const SealedSegment* segment(std::uint64_t seg_id) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].seg_id == seg_id) return segments_[i].get();
        }
        return nullptr;
    }
    // 当前活跃段列表（只读视图，查询归并 / 测试用）。
    [[nodiscard]] std::span<const std::unique_ptr<SealedSegment>>
    segments_view() const { return segments_; }
    // 清单条目视图(与 segments_view 平行;key_to_location_ 重建需 seg_id)。
    [[nodiscard]] std::span<const Entry> entries_view() const { return entries_; }

    // ---- S27-3 B2b 步骤 4:recovery 主路径 + 死亡位重存 ----

    // 从 bm25.ckpt 内嵌的 kSegManifest 载荷打开(单一 commit point 语义:
    // 清单随 index.manifest 原子提交)。任一段载入失败 → nullptr(caller
    // 回退:过渡期 segments.manifest → 空集 + fields_ 退化路径)。
    // next_seg_id 取清单内最大 seg_id+1——崩溃残留的孤儿段文件(add_pending
    // 后未 commit)可能与新 id 同名,save 的 tmp+rename 原子覆盖,无害。
    [[nodiscard]] static std::unique_ptr<SegmentSet> open_from_payload(
        const std::string& dir, std::span<const std::byte> payload) {
        auto set = std::make_unique<SegmentSet>();
        set->dir_ = dir;
        if (!set->decode_manifest(payload)) return nullptr;
        for (const auto& e : set->entries_) {
            auto seg = SealedSegment::load(join(dir, e.filename));
            if (!seg) return nullptr;
            set->segments_.push_back(std::move(seg));
        }
        std::uint64_t nid = 0;
        for (const auto& e : set->entries_) nid = std::max(nid, e.seg_id + 1);
        set->next_seg_id_ = nid;
        return set;
    }

    // 重存有新 mark_dead 的段(live_ 位随 kSegDocStore 持久化,但封口段不
    // 自动重存——不重存则 ckpt 之前的删除在 recovery 后复活为幽灵)。
    // tmp+rename 原子替换同名文件;watermark 沿用该段 hi_lsn。
    [[nodiscard]] bool resave_dead_dirty() {
        for (std::size_t i = 0; i < segments_.size(); ++i) {
            auto& seg = segments_[i];
            if (!seg->dead_dirty()) continue;
            if (!seg->save(join(dir_, entries_[i].filename),
                           entries_[i].hi_lsn)) {
                return false;
            }
            seg->clear_dead_dirty();
        }
        return true;
    }

private:
    static constexpr std::uint32_t kManifestMagic = 0x464D4753;  // 'SGMF'
    static constexpr std::uint32_t kManifestVersion = 1;

    static std::string join(const std::string& dir, const std::string& f) {
        return (std::filesystem::path(dir) / f).string();
    }
    static std::string manifest_path(const std::string& dir) {
        return join(dir, "segments.manifest");
    }

    [[nodiscard]] bool commit_manifest() const {
        SectionWriter sw;
        sw.add(CkptSectionType::kSegManifest, encode_manifest());
        return SearchCheckpoint::write(manifest_path(dir_), next_seg_id_,
                                       sw.sections());
    }

    [[nodiscard]] std::vector<std::byte> encode_manifest() const {
        using namespace detail;
        std::vector<std::byte> b;
        put_u32(b, kManifestMagic);
        put_u32(b, kManifestVersion);
        put_u32(b, static_cast<std::uint32_t>(entries_.size()));
        for (const auto& e : entries_) {
            put_u32(b, static_cast<std::uint32_t>(e.filename.size()));
            b.insert(b.end(),
                     reinterpret_cast<const std::byte*>(e.filename.data()),
                     reinterpret_cast<const std::byte*>(e.filename.data()) +
                         e.filename.size());
            put_u64(b, e.seg_id);
            put_u64(b, e.hi_lsn);
            put_u64(b, e.doc_count);
        }
        return b;
    }

    [[nodiscard]] bool decode_manifest(std::span<const std::byte> in) {
        const std::byte* p = in.data();
        const std::byte* end = p + in.size();
        auto rd_u32 = [&](std::uint32_t& v) {
            if (end - p < 4) return false;
            std::memcpy(&v, p, 4);
            p += 4;
            return true;
        };
        auto rd_u64 = [&](std::uint64_t& v) {
            if (end - p < 8) return false;
            std::memcpy(&v, p, 8);
            p += 8;
            return true;
        };
        std::uint32_t magic = 0, ver = 0, n = 0;
        if (!rd_u32(magic) || magic != kManifestMagic) return false;
        if (!rd_u32(ver) || ver != kManifestVersion) return false;
        if (!rd_u32(n)) return false;
        entries_.clear();
        entries_.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t flen = 0;
            if (!rd_u32(flen)) return false;
            if (static_cast<std::size_t>(end - p) < flen) return false;
            Entry e;
            e.filename.assign(reinterpret_cast<const char*>(p), flen);
            p += flen;
            if (!rd_u64(e.seg_id) || !rd_u64(e.hi_lsn) || !rd_u64(e.doc_count))
                return false;
            entries_.push_back(std::move(e));
        }
        return true;
    }

    std::string   dir_;
    std::uint64_t next_seg_id_ = 0;
    std::vector<Entry> entries_;                              // 与 segments_ 平行
    std::vector<std::unique_ptr<SealedSegment>> segments_;
    std::vector<std::string> pending_unlink_;  // drop_pending → commit 后删
};

}  // namespace bitcask::search
