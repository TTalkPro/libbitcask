// legacy_ckpt 实现（S19-2）。自 SearchLayer::load_search_ckpt +
// apply_delta_file 平移——段分发与链重放语义逐字节一致，成员调用改为
// Index/TextPlugin/VectorPlugin 公开原语。

#include "legacy_ckpt.hpp"

#include "bitcask/search_checkpoint.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace bitcask::legacy_ckpt {

namespace sc = bitcask::search;

namespace {

// 单个 delta 文件的段集应用（原 SearchLayer::apply_delta_file）：先整体
// CRC 预检（任何坏段 → 整个 delta 拒绝，不部分应用），再逐段应用；
// docmap 行/删除按 ord 交错重放（「删后重写」场景删除先于同 key 新行），
// keydir 半边一次性透传 hook。
bool apply_delta_file(const std::vector<sc::LoadedSection>& sections,
                      index::Index& docmap, text::TextPlugin& text,
                      vec::VectorPlugin& vec,
                      const index::DocmapReplayHook& hook) {
    for (const auto& ls : sections) {
        if (!ls.crc_ok) return false;
    }
    std::vector<index::DocmapDeltaRow> hook_rows;
    std::vector<index::DocmapDeltaRemoval> hook_rems;
    std::span<const std::byte> hook_meta;
    for (const auto& ls : sections) {
        std::span<const std::byte> pl(ls.payload.data(), ls.payload.size());
        switch (static_cast<sc::CkptSectionType>(ls.type)) {
        case sc::CkptSectionType::kBm25DefaultDelta:
            if (!text.apply_default_delta(pl)) return false;
            break;
        case sc::CkptSectionType::kBm25FieldsDelta:
            if (!text.apply_fields_delta(pl)) return false;
            break;
        case sc::CkptSectionType::kDocmapDelta: {
            const auto* p = ls.payload.data();
            const auto* end = p + ls.payload.size();
            if (end - p < 8) return false;
            std::uint64_t rn = sc::detail::get_u64(p); p += 8;
            hook_rows.reserve(rn);
            for (std::uint64_t i = 0; i < rn; ++i) {
                if (end - p < 10) return false;
                index::DocmapDeltaRow r;
                r.ord = sc::detail::get_u64(p); p += 8;
                std::uint16_t klen = sc::detail::get_u16(p); p += 2;
                if (end - p < klen + 24) return false;
                r.ext.assign(reinterpret_cast<const char*>(p), klen);
                p += klen;
                r.slot.loc.file_id  = sc::detail::get_u32(p); p += 4;
                r.slot.loc.offset   = sc::detail::get_u64(p); p += 8;
                r.slot.loc.total_sz = sc::detail::get_u32(p); p += 4;
                r.slot.tstamp       = sc::detail::get_u32(p); p += 4;
                r.slot.doc_len      = sc::detail::get_u32(p); p += 4;
                hook_rows.push_back(std::move(r));
            }
            if (end - p < 8) return false;
            std::uint64_t mn = sc::detail::get_u64(p); p += 8;
            hook_rems.reserve(mn);
            for (std::uint64_t i = 0; i < mn; ++i) {
                if (end - p < 10) return false;
                index::DocmapDeltaRemoval m;
                m.tomb = sc::detail::get_u64(p); p += 8;
                std::uint16_t klen = sc::detail::get_u16(p); p += 2;
                if (end - p < klen) return false;
                m.key.assign(reinterpret_cast<const char*>(p), klen);
                p += klen;
                hook_rems.push_back(std::move(m));
            }
            if (p != end) return false;
            std::size_t ri = 0, mi = 0;
            while (ri < hook_rows.size() || mi < hook_rems.size()) {
                const bool take_row =
                    mi >= hook_rems.size() ||
                    (ri < hook_rows.size() &&
                     hook_rows[ri].ord < hook_rems[mi].tomb);
                if (take_row) {
                    docmap.put_doc(hook_rows[ri].ext, hook_rows[ri].ord,
                                   hook_rows[ri].slot);
                    ++ri;
                } else {
                    docmap.remove(hook_rems[mi].key, hook_rems[mi].tomb);
                    ++mi;
                }
            }
            break;
        }
        case sc::CkptSectionType::kHnswDelta:
            if (!vec.apply_delta_log(pl)) return false;
            break;
        case sc::CkptSectionType::kKeydirDelta:
            hook_meta = pl;
            break;
        case sc::CkptSectionType::kDeltaInfo:
        default:
            break;  // info 由 caller 校验；未知段忽略（前向兼容）。
        }
    }
    if (hook) hook(hook_rows, hook_rems, hook_meta);
    return true;
}

}  // namespace

LoadResult load(std::string_view path, index::Index& docmap,
                text::TextPlugin& text, vec::VectorPlugin& vec,
                const index::DocmapReplayHook& hook) {
    const std::string fp(path);
    const std::string prev = fp + ".prev";
    // V7:BCVS v2 vecs_ payload 路径(与 ckpt 同目录,.vec 扩展名)。
    const std::string vec_path =
        std::filesystem::path(fp).replace_extension(".vec").string();

    auto lc = sc::SearchCheckpoint::read(fp);
    bool from_prev = false;
    if (!lc) {
        lc = sc::SearchCheckpoint::read(prev);
        if (!lc) return {};
        from_prev = true;
    }

    LoadResult result;
    result.loaded = true;
    result.watermark = lc->watermark;
    result.all_segments_ok = true;

    bool bm25_loaded = false;
    bool docmap_loaded = false;
    bool hnsw_loaded = false;

    for (auto& ls : lc->sections) {
        auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (!ls.crc_ok) {
            result.all_segments_ok = false;
            continue;
        }
        std::span<const std::byte> pl(ls.payload.data(), ls.payload.size());
        switch (st) {
        case sc::CkptSectionType::kBm25Default:
            if (text.deserialize_default(pl)) {
                bm25_loaded = true;
            } else {
                result.all_segments_ok = false;
            }
            break;
        case sc::CkptSectionType::kBm25Fields:
            if (text.deserialize_fields(pl)) {
                bm25_loaded = true;
            } else {
                result.all_segments_ok = false;
            }
            break;
        case sc::CkptSectionType::kDocmap: {
            auto covers = docmap.deserialize_docmap(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(ls.payload.data()),
                    ls.payload.size()));
            if (covers) {
                docmap_loaded = true;
            } else {
                result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kHnsw:
            if (vec.enabled()) {
                const std::string qc_path =
                    std::filesystem::path(vec_path)
                        .replace_extension(".qc8")
                        .string();
                if (vec.load_graph_section(pl, vec_path, qc_path)) {
                    hnsw_loaded = true;
                } else {
                    result.all_segments_ok = false;
                }
            }
            break;
        default:
            break;  // 未知段类型（meta/terms 等）忽略。
        }
    }

    if (!docmap_loaded) result.all_segments_ok = false;
    if (!bm25_loaded) result.all_segments_ok = false;
    if (vec.enabled() && !hnsw_loaded) result.all_segments_ok = false;
    result.from_prev = from_prev;

    // S14-4：delta 链重放。base 健康且非 .prev 回退才吃链；链在「文件缺失」
    // 处正常终止；「文件存在但无效」→ 保守判整体不健康（caller 退全量 fold）。
    std::uint64_t chain_coverage = result.watermark;
    const std::uint64_t chain_base_gen = result.watermark;
    std::uint32_t chain_next_seq = 1;
    if (result.all_segments_ok && !from_prev) {
        for (;; ++chain_next_seq) {
            const std::string dpath =
                fp + ".d" + std::to_string(chain_next_seq);
            std::error_code ec;
            if (!std::filesystem::exists(dpath, ec)) break;  // 正常链尾
            auto dc = sc::SearchCheckpoint::read(dpath);
            bool applied = false;
            if (dc) {
                const sc::LoadedSection* info = nullptr;
                for (const auto& dls : dc->sections) {
                    if (dls.type == static_cast<std::uint16_t>(
                                        sc::CkptSectionType::kDeltaInfo)) {
                        info = &dls;
                        break;
                    }
                }
                if (info && info->crc_ok && info->payload.size() == 20) {
                    const auto* q = info->payload.data();
                    const std::uint64_t gen = sc::detail::get_u64(q); q += 8;
                    const std::uint64_t pw = sc::detail::get_u64(q); q += 8;
                    const std::uint32_t seq = sc::detail::get_u32(q);
                    if (gen == chain_base_gen && pw == chain_coverage &&
                        seq == chain_next_seq) {
                        applied = apply_delta_file(dc->sections, docmap,
                                                   text, vec, hook);
                    }
                }
            }
            if (!applied) {
                result.all_segments_ok = false;
                break;
            }
            chain_coverage = dc->watermark;
        }
    }
    result.watermark = chain_coverage;

    // 记账收尾（与原 SearchLayer 版一致）：docmap 窗口对齐链覆盖 + 重放
    // 污染清空；插件链状态同步（顺带推进各自入账窗口）；载入成功清脏。
    docmap.begin_delta_window(chain_coverage);
    docmap.clear_removals();
    text.set_chain_state({chain_base_gen, chain_coverage, chain_next_seq});
    vec.set_chain_state({chain_base_gen, chain_coverage, chain_next_seq});
    vec.clear_delta_log();
    if (docmap_loaded) docmap.clear_dirty();
    if (hnsw_loaded) vec.clear_dirty();

    return result;
}

}  // namespace bitcask::legacy_ckpt
