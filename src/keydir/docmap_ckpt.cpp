// docmap 组件 checkpoint 实现（S18-2）。代码自 SearchLayer 的
// save_components_base/save_components_delta/load_component/apply_delta_file
// 的 kDocmap 分支平移——文件格式逐字节不变，只移交发起权到宿主侧。

#include "bitcask/docmap_ckpt.hpp"
#include "bitcask/search_checkpoint.hpp"

#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>

namespace bitcask::index {

namespace sc = bitcask::search;

namespace {
constexpr const char* kDocmapCkptName = "docmap.ckpt";

std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kDocmapCkptName).string();
}

// .d 链清理（base 落成后链坍缩）。连续 miss 8 个序号即停（链连续 1..N）。
void remove_chain_files(const std::string& fp) {
    std::uint32_t misses = 0;
    for (std::uint32_t i = 1; misses < 8; ++i) {
        std::error_code ec;
        if (std::filesystem::remove(fp + ".d" + std::to_string(i), ec)) {
            misses = 0;
        } else {
            ++misses;
        }
    }
}

// kDocmapDelta 段解析 + 应用：窗口行 + 删除日志按 ord 交错重放到 Index
// （「删后重写」场景删除必须先于同 key 新行），keydir 半边收集给 hook。
bool apply_docmap_delta_section(Index& docmap, const sc::LoadedSection& ls,
                                std::vector<DocmapDeltaRow>& rows,
                                std::vector<DocmapDeltaRemoval>& rems) {
    const auto* p = ls.payload.data();
    const auto* end = p + ls.payload.size();
    if (end - p < 8) return false;
    std::uint64_t rn = sc::detail::get_u64(p); p += 8;
    rows.reserve(rn);
    for (std::uint64_t i = 0; i < rn; ++i) {
        if (end - p < 10) return false;
        DocmapDeltaRow r;
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
        rows.push_back(std::move(r));
    }
    if (end - p < 8) return false;
    std::uint64_t mn = sc::detail::get_u64(p); p += 8;
    rems.reserve(mn);
    for (std::uint64_t i = 0; i < mn; ++i) {
        if (end - p < 10) return false;
        DocmapDeltaRemoval m;
        m.tomb = sc::detail::get_u64(p); p += 8;
        std::uint16_t klen = sc::detail::get_u16(p); p += 2;
        if (end - p < klen) return false;
        m.key.assign(reinterpret_cast<const char*>(p), klen);
        p += klen;
        rems.push_back(std::move(m));
    }
    if (p != end) return false;
    // 按 ord 交错重放（行与删除都按 ord 升序产出）。
    std::size_t ri = 0, mi = 0;
    while (ri < rows.size() || mi < rems.size()) {
        const bool take_row =
            mi >= rems.size() ||
            (ri < rows.size() && rows[ri].ord < rems[mi].tomb);
        if (take_row) {
            docmap.put_doc(rows[ri].ext, rows[ri].ord, rows[ri].slot);
            ++ri;
        } else {
            docmap.remove(rems[mi].key, rems[mi].tomb);
            ++mi;
        }
    }
    return true;
}

// 单个 delta 文件的段集应用（原 apply_delta_file 的 docmap 版）：先整体
// CRC 预检（任何坏段 → 整个 delta 拒绝，不部分应用），再逐段应用 +
// keydir 半边经 hook 一次性透传。
bool apply_delta_file(Index& docmap,
                      const std::vector<sc::LoadedSection>& sections,
                      const DocmapReplayHook& hook) {
    for (const auto& ls : sections) {
        if (!ls.crc_ok) return false;
    }
    std::vector<DocmapDeltaRow> rows;
    std::vector<DocmapDeltaRemoval> rems;
    std::span<const std::byte> keydir_meta;
    for (const auto& ls : sections) {
        const auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (st == sc::CkptSectionType::kDocmapDelta) {
            if (!apply_docmap_delta_section(docmap, ls, rows, rems)) {
                return false;
            }
        } else if (st == sc::CkptSectionType::kKeydirDelta) {
            keydir_meta = std::span<const std::byte>(ls.payload.data(),
                                                     ls.payload.size());
        }
        // kDeltaInfo 由链校验消费；其余段型忽略。
    }
    if (hook) hook(rows, rems, keydir_meta);
    return true;
}

}  // namespace

bool save_docmap_base(Index& docmap, std::string_view dir,
                      std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    const std::string prev = fp + ".prev";
    std::error_code ec;
    if (std::filesystem::exists(fp, ec)) {
        std::filesystem::rename(fp, prev, ec);
    }
    std::vector<std::uint8_t> buf;
    if (!docmap.serialize_docmap(buf, watermark)) return false;
    sc::CkptSection sec{
        static_cast<std::uint16_t>(sc::CkptSectionType::kDocmap), 0,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(buf.data()), buf.size())};
    if (!sc::SearchCheckpoint::write(fp, watermark, {&sec, 1})) return false;
    remove_chain_files(fp);
    // 记账收尾：base 落成 = 链坍缩（静止点所有已入账 ord < watermark）。
    docmap.begin_delta_window(watermark);
    docmap.clear_removals();
    docmap.clear_dirty();
    return true;
}

bool save_docmap_delta(Index& docmap, std::string_view dir,
                       std::uint64_t watermark, std::uint64_t base_gen,
                       std::uint64_t from, std::uint32_t seq,
                       std::span<const std::byte> keydir_delta) {
    const std::string fp = comp_path(dir);
    const std::string dpath = fp + ".d" + std::to_string(seq);
    std::vector<sc::CkptSection> secs;
    std::vector<std::vector<std::byte>> bufs;
    auto add = [&](sc::CkptSectionType t, std::vector<std::byte> b) {
        bufs.push_back(std::move(b));
        secs.push_back(sc::CkptSection{
            static_cast<std::uint16_t>(t), 0,
            std::span<const std::byte>(bufs.back().data(),
                                       bufs.back().size())});
    };
    // kDeltaInfo：链校验三元组。
    {
        std::vector<std::byte> b;
        sc::detail::put_u64(b, base_gen);
        sc::detail::put_u64(b, from);
        sc::detail::put_u32(b, seq);
        add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }
    // kDocmapDelta：窗口 live 行 + 删除日志。
    {
        std::vector<std::byte> b;
        const std::size_t cnt_pos = b.size();
        sc::detail::put_u64(b, 0);
        std::uint64_t rows = 0;
        bool ok = true;
        docmap.for_each_live_in(
            from, watermark,
            [&](std::uint64_t ord, const std::string& ext,
                const DocSlot& slot) {
                if (ext.size() > 0xFFFF) { ok = false; return; }
                sc::detail::put_u64(b, ord);
                sc::detail::put_u16(b, static_cast<std::uint16_t>(ext.size()));
                b.insert(b.end(),
                    reinterpret_cast<const std::byte*>(ext.data()),
                    reinterpret_cast<const std::byte*>(ext.data()) +
                        ext.size());
                sc::detail::put_u32(b, slot.loc.file_id);
                sc::detail::put_u64(b, slot.loc.offset);
                sc::detail::put_u32(b, slot.loc.total_sz);
                sc::detail::put_u32(b, slot.tstamp);
                sc::detail::put_u32(b, slot.doc_len);
                ++rows;
            });
        if (!ok) return false;
        std::memcpy(b.data() + cnt_pos, &rows, 8);
        const auto removals = docmap.removals_snapshot();
        sc::detail::put_u64(b, static_cast<std::uint64_t>(removals.size()));
        for (const auto& [key, tomb] : removals) {
            if (key.size() > 0xFFFF) return false;
            sc::detail::put_u64(b, tomb);
            sc::detail::put_u16(b, static_cast<std::uint16_t>(key.size()));
            b.insert(b.end(),
                reinterpret_cast<const std::byte*>(key.data()),
                reinterpret_cast<const std::byte*>(key.data()) + key.size());
        }
        add(sc::CkptSectionType::kDocmapDelta, std::move(b));
    }
    // keydir meta 仅在 docmap 组件落（S14-7 成对不变量）。
    if (!keydir_delta.empty()) {
        std::vector<std::byte> kb(keydir_delta.begin(), keydir_delta.end());
        add(sc::CkptSectionType::kKeydirDelta, std::move(kb));
    }
    if (!sc::SearchCheckpoint::write(dpath, watermark, secs)) return false;
    // 记账收尾：窗口推进 + 已序列化的删除日志清空。
    docmap.begin_delta_window(watermark);
    docmap.clear_removals();
    docmap.clear_dirty();
    return true;
}

DocmapLoadResult load_docmap(Index& docmap, std::string_view dir,
                             std::uint64_t expected_base_wm,
                             std::uint32_t chain_seq,
                             const DocmapReplayHook& hook) {
    DocmapLoadResult result;
    const std::string fp = comp_path(dir);
    const std::string prev_path = fp + ".prev";
    auto lc = sc::SearchCheckpoint::read(fp);
    bool from_prev = false;
    if (lc && lc->watermark != expected_base_wm) lc.reset();
    if (!lc) {
        lc = sc::SearchCheckpoint::read(prev_path);
        if (lc && lc->watermark != expected_base_wm) lc.reset();
        if (lc) from_prev = true;
    }
    auto fail = [&]() {
        result.loaded = false;
        result.watermark = 0;
        result.all_segments_ok = false;
        // 窗口归零：fold 全量重建期一切墓碑照常入账（与旧 comp_*[0]=0 一致）。
        // 链重放中途失败可能已污染删除日志——一并清空（fold 会重新入账）。
        docmap.begin_delta_window(0);
        docmap.clear_removals();
        return result;
    };
    if (!lc) return fail();
    // kDocmap 段应用。
    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        if (ls.type ==
            static_cast<std::uint16_t>(sc::CkptSectionType::kDocmap)) {
            auto covers = docmap.deserialize_docmap(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(ls.payload.data()),
                    ls.payload.size()));
            if (!covers) segments_ok = false;
            else any = true;
        }
        // 旧文件可能含 meta/terms 等扩展段——忽略。
    }
    if (!any) segments_ok = false;
    // 链重放（.prev 回退 = 链不可信，与 SearchLayer 版语义一致）。
    std::uint64_t coverage = lc->watermark;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const std::uint64_t base_gen_for_chain = coverage;
        for (std::uint32_t s = 1; s <= chain_seq; ++s) {
            const std::string dpath = fp + ".d" + std::to_string(s);
            std::error_code ec;
            if (!std::filesystem::exists(dpath, ec)) { chain_ok = false; break; }
            auto dc = sc::SearchCheckpoint::read(dpath);
            if (!dc) { chain_ok = false; break; }
            const sc::LoadedSection* info = nullptr;
            for (const auto& dls : dc->sections) {
                if (dls.type ==
                    static_cast<std::uint16_t>(
                        sc::CkptSectionType::kDeltaInfo)) {
                    info = &dls; break;
                }
            }
            if (!info || !info->crc_ok || info->payload.size() != 20) {
                chain_ok = false; break;
            }
            const auto* q = info->payload.data();
            const std::uint64_t gen = sc::detail::get_u64(q); q += 8;
            const std::uint64_t prev_wm = sc::detail::get_u64(q); q += 8;
            const std::uint32_t seq = sc::detail::get_u32(q);
            if (gen != base_gen_for_chain || prev_wm != coverage ||
                seq != s) {
                chain_ok = false; break;
            }
            if (!apply_delta_file(docmap, dc->sections, hook)) {
                chain_ok = false; break;
            }
            coverage = dc->watermark;
        }
    } else {
        chain_ok = false;
    }
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (result.loaded) {
        // 记账收尾：窗口 = 链覆盖水位；重放污染的删除日志清空；内存 == 文件。
        docmap.begin_delta_window(coverage);
        docmap.clear_removals();
        docmap.clear_dirty();
    } else {
        return fail();
    }
    return result;
}

}  // namespace bitcask::index
