// legacy_ckpt 实现（S19-2）。自 SearchLayer::load_search_ckpt +
// apply_delta_file 平移——段分发与链重放语义逐字节一致，成员调用改为
// Index/TextPlugin/VectorPlugin 公开原语。

#include "legacy_ckpt.hpp"

#include "bitcask/ckpt_chain.hpp"        // S20-2：walk_chain
#include "bitcask/search_checkpoint.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace bitcask::legacy_ckpt {

namespace sc = bitcask::search;

namespace {

// 单个 delta 文件的段集应用（原 SearchLayer::apply_delta_file）。
// S21-3 B3：CRC 预检 + hook 收尾骨架收敛至 index::apply_delta_sections
// （与 docmap_ckpt 共用），此处只留 legacy 段分发（bm25/hnsw/docmap v1；
// legacy 链在 v2 段型引入前落笔，永不含 kDocmapDeltaV2）。
bool apply_delta_file(const std::vector<sc::LoadedSection>& sections,
                      index::Index& docmap, text::TextPlugin& text,
                      vec::VectorPlugin& vec,
                      const index::DocmapReplayHook& hook) {
    return index::apply_delta_sections(
        sections, hook,
        [&](sc::CkptSectionType st, std::span<const std::byte> pl,
            std::vector<index::DocmapDeltaRow>& rows,
            std::vector<index::DocmapDeltaRemoval>& rems) {
            switch (st) {
            case sc::CkptSectionType::kBm25DefaultDelta:
                return text.apply_default_delta(pl);
            case sc::CkptSectionType::kBm25FieldsDelta:
                return text.apply_fields_delta(pl);
            case sc::CkptSectionType::kDocmapDelta:
                // S20-2 R3：解析 + 交错重放收敛至 apply_docmap_delta_section。
                return index::apply_docmap_delta_section(docmap, pl, rows,
                                                         rems);
            case sc::CkptSectionType::kHnswDelta:
                return vec.apply_delta_log(pl);
            case sc::CkptSectionType::kDeltaInfo:
            default:
                return true;  // info 由 caller 校验；未知段忽略（前向兼容）。
            }
        });
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
    // S20-2 R2：无界走读（chain_seq=0）收敛至 sc::walk_chain。
    std::uint64_t chain_coverage = result.watermark;
    const std::uint64_t chain_base_gen = result.watermark;
    std::uint32_t chain_next_seq = 1;
    if (result.all_segments_ok && !from_prev) {
        const auto w = sc::walk_chain(
            fp, chain_base_gen, /*base_coverage=*/chain_coverage,
            /*chain_seq=*/0, /*unbounded=*/true,
            [&](const sc::LoadedCheckpoint& dc) {
                return apply_delta_file(dc.sections, docmap, text, vec, hook);
            });
        chain_coverage = w.coverage;
        chain_next_seq = w.next_seq;
        if (!w.ok) result.all_segments_ok = false;
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
