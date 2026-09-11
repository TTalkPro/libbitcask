// docmap 组件 checkpoint 实现（S18-2）。代码自 SearchLayer 的
// save_components_base/save_components_delta/load_component/apply_delta_file
// 的 kDocmap 分支平移——文件格式逐字节不变，只移交发起权到宿主侧。

#include "bitcask/docmap_ckpt.hpp"
#include "bitcask/ckpt_chain.hpp"        // S20-2：walk_chain / remove_chain_files
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/vbyte.hpp"             // S21-2 A2：v2 行 gap+vbyte

#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::index {

namespace sc = bitcask::search;

namespace {
constexpr const char* kDocmapCkptName = "docmap.ckpt";

std::string comp_path(std::string_view dir) {
    return bitcask::detail::to_utf8(bitcask::detail::from_utf8(dir) / kDocmapCkptName);
}

}  // namespace

// kDocmapDelta 段解析 + 应用（S20-2 R3：公开供 legacy_ckpt 共用）。窗口行 +
// 删除日志按 ord 交错重放到 Index（「删后重写」场景删除必须先于同 key 新行），
// keydir 半边收集给 hook。
bool apply_docmap_delta_section(Index& docmap,
                                std::span<const std::byte> payload,
                                std::vector<DocmapDeltaRow>& rows,
                                std::vector<DocmapDeltaRemoval>& rems) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
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

// S21-2 A2：kDocmapDeltaV2/V3 解析 + 应用。与 v1 定宽版语义一致，行编码换
// gap+vbyte：ord/tomb 为窗口内单调升序 → 差分后典型 1-2B；klen/file_id/
// offset/total_sz/doc_len 走标量 vbyte；tstamp 保持定宽（时间戳 vbyte 反而
// 更大）——V2 为 4B，V3（64 位时间戳 flag-day）为 8B，由 tstamp64 区分。
// gap 用 u64 二补数回绕（prev + (v - prev) ≡ v），
// 正确性不依赖升序——乱序只损压缩率不损数据。
bool apply_docmap_delta_section_v2(Index& docmap,
                                   std::span<const std::byte> payload,
                                   std::vector<DocmapDeltaRow>& rows,
                                   std::vector<DocmapDeltaRemoval>& rems,
                                   bool tstamp64) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
    bool fail = false;
    auto vb = [&]() -> std::uint64_t {  // 边界安全 vbyte
        std::uint64_t v = 0, shift = 0;
        while (true) {
            if (p >= end || shift > 63) { fail = true; return 0; }
            const auto byte = static_cast<std::uint8_t>(*p++);
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return v;
            shift += 7;
        }
    };
    auto u32 = [&]() -> std::uint32_t {
        if (end - p < 4) { fail = true; return 0; }
        std::uint32_t v = sc::detail::get_u32(p); p += 4;
        return v;
    };
    auto u64 = [&]() -> std::uint64_t {
        if (end - p < 8) { fail = true; return 0; }
        std::uint64_t v = sc::detail::get_u64(p); p += 8;
        return v;
    };
    // 定宽时间戳：V3 8B / V2 4B（旧纪元段，零扩展）。
    auto tstamp_field = [&]() -> std::uint64_t {
        return tstamp64 ? u64() : static_cast<std::uint64_t>(u32());
    };
    const std::uint64_t rn = vb();
    if (fail || rn > (1ull << 40)) return false;
    rows.reserve(rn);
    std::uint64_t prev_ord = 0;
    for (std::uint64_t i = 0; i < rn; ++i) {
        DocmapDeltaRow r;
        r.ord = prev_ord + vb();  // gap（二补数回绕安全）
        prev_ord = r.ord;
        const std::uint64_t klen = vb();
        if (fail || klen > 0xFFFF ||
            static_cast<std::uint64_t>(end - p) < klen) {
            return false;
        }
        r.ext.assign(reinterpret_cast<const char*>(p), klen);
        p += klen;
        const std::uint64_t fid = vb(), off = vb(), tsz = vb();
        r.slot.tstamp  = tstamp_field();
        const std::uint64_t dl = vb();
        if (fail || fid > 0xFFFFFFFFull || tsz > 0xFFFFFFFFull ||
            dl > 0xFFFFFFFFull) {
            return false;
        }
        r.slot.loc.offset   = off;
        r.slot.loc.file_id  = static_cast<std::uint32_t>(fid);
        r.slot.loc.total_sz = static_cast<std::uint32_t>(tsz);
        r.slot.doc_len      = static_cast<std::uint32_t>(dl);
        rows.push_back(std::move(r));
    }
    const std::uint64_t mn = vb();
    if (fail || mn > (1ull << 40)) return false;
    rems.reserve(mn);
    std::uint64_t prev_tomb = 0;
    for (std::uint64_t i = 0; i < mn; ++i) {
        DocmapDeltaRemoval m;
        m.tomb = prev_tomb + vb();
        prev_tomb = m.tomb;
        const std::uint64_t klen = vb();
        if (fail || klen > 0xFFFF ||
            static_cast<std::uint64_t>(end - p) < klen) {
            return false;
        }
        m.key.assign(reinterpret_cast<const char*>(p), klen);
        p += klen;
        rems.push_back(std::move(m));
    }
    if (fail || p != end) return false;
    // 按 ord 交错重放（与 v1 同一不变量：删后重写场景删除先于同 key 新行）。
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

// V5 meta 段（kDocmapMeta / kDocmapMetaDelta 同一布局）：
//   count vbyte; 每条 ord-gap vbyte | len vbyte | bytes
// 只收 [from, to) 内 live 且 meta 非空的 ord。返回是否有条目（无则 caller
// 不写段——无 meta 部署零字节开销）。
bool encode_docmap_meta_section(const Index& docmap, std::uint64_t from,
                                std::uint64_t to, std::vector<std::byte>& out) {
    std::vector<std::byte> rows_buf;
    std::uint64_t rows = 0;
    std::uint64_t prev_ord = 0;
    docmap.for_each_meta_in(from, to,
        [&](std::uint64_t ord, std::span<const std::byte> blob) {
            codec::vbyte_encode(ord - prev_ord, rows_buf);
            prev_ord = ord;
            codec::vbyte_encode(blob.size(), rows_buf);
            rows_buf.insert(rows_buf.end(), blob.begin(), blob.end());
            ++rows;
        });
    if (rows == 0) return false;
    out.clear();
    out.reserve(rows_buf.size() + 16);
    codec::vbyte_encode(rows, out);
    out.insert(out.end(), rows_buf.begin(), rows_buf.end());
    return true;
}

// 解析 + 应用 meta 段：逐条 set_meta（ord 已由同文件更早的 kDocmap /
// kDocmapDelta* 段 put_doc 登记；set_meta 对未登记 ord 也只是扩容，不会
// 误亮 live）。载荷结构错误（长度不足/尾部残留）返回 false。
bool apply_docmap_meta_section(Index& docmap,
                               std::span<const std::byte> payload) {
    const auto* p = payload.data();
    const auto* end = p + payload.size();
    bool fail = false;
    auto vb = [&]() -> std::uint64_t {  // 边界安全 vbyte
        std::uint64_t v = 0, shift = 0;
        while (true) {
            if (p >= end || shift > 63) { fail = true; return 0; }
            const auto byte = static_cast<std::uint8_t>(*p++);
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return v;
            shift += 7;
        }
    };
    const std::uint64_t rn = vb();
    if (fail || rn > (1ull << 40)) return false;
    std::uint64_t prev_ord = 0;
    for (std::uint64_t i = 0; i < rn; ++i) {
        const std::uint64_t ord = prev_ord + vb();
        prev_ord = ord;
        const std::uint64_t len = vb();
        if (fail || len == 0 ||
            static_cast<std::uint64_t>(end - p) < len) {
            return false;
        }
        docmap.set_meta(ord, std::span<const std::byte>(p, len));
        p += len;
    }
    return !fail && p == end;
}

namespace {

// 单个 delta 文件的段集应用（原 apply_delta_file 的 docmap 版）。
// S21-3 B3：CRC 预检 + hook 收尾骨架收敛至 apply_delta_sections（与
// legacy_ckpt 共用），此处只留段分发。
bool apply_delta_file(Index& docmap,
                      const std::vector<sc::LoadedSection>& sections,
                      const DocmapReplayHook& hook) {
    return apply_delta_sections(
        sections, hook,
        [&](sc::CkptSectionType st, std::span<const std::byte> pl,
            std::vector<DocmapDeltaRow>& rows,
            std::vector<DocmapDeltaRemoval>& rems) {
            switch (st) {
            case sc::CkptSectionType::kDocmapDelta:
                return apply_docmap_delta_section(docmap, pl, rows, rems);
            case sc::CkptSectionType::kDocmapDeltaV2:
                return apply_docmap_delta_section_v2(docmap, pl, rows, rems,
                                                     /*tstamp64=*/false);
            case sc::CkptSectionType::kDocmapDeltaV3:
                return apply_docmap_delta_section_v2(docmap, pl, rows, rems,
                                                     /*tstamp64=*/true);
            case sc::CkptSectionType::kDocmapMetaDelta:
                // 段序：save_docmap_delta 先写行段再写 meta 段，此处按文件序
                // 应用 → 行先落、meta 后补，与活写路径 put_doc→set_meta 同序。
                return apply_docmap_meta_section(docmap, pl);
            default:
                return true;  // kDeltaInfo 由链校验消费；其余段型忽略。
            }
        });
}

}  // namespace

bool save_docmap_base(Index& docmap, std::string_view dir,
                      std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    const std::string prev = fp + ".prev";
    // 旧 base 轮转成 .prev —— 它不是普通备份，是**崩溃恢复的回退源**
    // （主 ckpt 结构损坏时读它，见 legacy_ckpt.hpp）。
    //
    // P1：经 io::atomic_rename 而非 fs::rename。理由是**纪律不是语义**——
    // 两者在 Windows 上实测行为逐条一致：覆盖已存在的目标都成功；目标仍开着
    // 句柄都以 ERROR_ACCESS_DENIED 失败，且带不带 FILE_SHARE_DELETE 都拦
    // （与 posix_file_test 的 AtomicRenameOverMappedFileKeepsViewContent 所守的
    // 是同一条规则）。收进 seam 是为了让「Windows 的 rename 语义」全库只有
    // 一个地方需要解释，不是为了修什么。
    //
    // **尽力而为，失败不算错**：轮转没成只意味着 .prev 停在更旧的一份，新
    // base 照常写。恢复端仅在主 ckpt 损坏时才回退到 .prev，且一旦回退就不吃
    // delta 链（legacy_ckpt.cpp:133），本就是退到更旧的水位再全量 fold。
    // 两种崩溃结局（改名了 / 没改名）也都落在可恢复状态上，故此处不需要
    // atomic_rename 顺带提供的 MOVEFILE_WRITE_THROUGH（实测 0.229 vs
    // 0.124 ms/次，代价可忽略，但也不构成收编理由）。
    std::error_code ec;
    if (std::filesystem::exists(bitcask::detail::from_utf8(fp), ec)) {
        (void)io::atomic_rename(fp, prev);
    }
    std::vector<std::uint8_t> buf;
    if (!docmap.serialize_docmap(buf, watermark)) return false;
    sc::SectionWriter sw;
    sw.add(sc::CkptSectionType::kDocmap,
           std::vector<std::byte>(
               reinterpret_cast<const std::byte*>(buf.data()),
               reinterpret_cast<const std::byte*>(buf.data()) + buf.size()));
    // V5 meta：全部 live ord 的 meta 随 base 落盘（此前只活在内存，重开即丢
    // → 带 filter 的检索恒空）。无 meta 部署不写段。
    {
        std::vector<std::byte> mb;
        if (encode_docmap_meta_section(docmap, 0, watermark, mb)) {
            sw.add(sc::CkptSectionType::kDocmapMeta, std::move(mb));
        }
    }
    if (!sc::SearchCheckpoint::write(fp, watermark, sw.sections())) {
        return false;
    }
    sc::remove_chain_files(fp);  // S20-2 R8
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
    sc::SectionWriter sw;  // S20-1 R4
    // kDeltaInfo：链校验三元组。
    {
        std::vector<std::byte> b;
        sc::detail::put_u64(b, base_gen);
        sc::detail::put_u64(b, from);
        sc::detail::put_u32(b, seq);
        sw.add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }
    // kDocmapDeltaV2（S21-2 A2）：窗口 live 行 + 删除日志，gap+vbyte 编码
    // （布局见 apply_docmap_delta_section_v2 注释）。行数不可先知（回调产出）
    // → 先收集行字节再前置 vbyte 计数（v1 是定宽占位回填，vbyte 变长没法
    // 占位）。
    {
        std::vector<std::byte> rows_buf;
        std::uint64_t rows = 0;
        std::uint64_t prev_ord = 0;
        bool ok = true;
        docmap.for_each_live_in(
            from, watermark,
            [&](std::uint64_t ord, const std::string& ext,
                const DocSlot& slot) {
                if (ext.size() > 0xFFFF) { ok = false; return; }
                codec::vbyte_encode(ord - prev_ord, rows_buf);
                prev_ord = ord;
                codec::vbyte_encode(ext.size(), rows_buf);
                rows_buf.insert(rows_buf.end(),
                    reinterpret_cast<const std::byte*>(ext.data()),
                    reinterpret_cast<const std::byte*>(ext.data()) +
                        ext.size());
                codec::vbyte_encode(slot.loc.file_id, rows_buf);
                codec::vbyte_encode(slot.loc.offset, rows_buf);
                codec::vbyte_encode(slot.loc.total_sz, rows_buf);
                sc::detail::put_u64(rows_buf, slot.tstamp);  // V3：定宽 8B
                codec::vbyte_encode(slot.doc_len, rows_buf);
                ++rows;
            });
        if (!ok) return false;
        std::vector<std::byte> b;
        b.reserve(rows_buf.size() + 64);
        codec::vbyte_encode(rows, b);
        b.insert(b.end(), rows_buf.begin(), rows_buf.end());
        const auto removals = docmap.removals_snapshot();
        codec::vbyte_encode(removals.size(), b);
        std::uint64_t prev_tomb = 0;
        for (const auto& [key, tomb] : removals) {
            if (key.size() > 0xFFFF) return false;
            codec::vbyte_encode(tomb - prev_tomb, b);
            prev_tomb = tomb;
            codec::vbyte_encode(key.size(), b);
            b.insert(b.end(),
                reinterpret_cast<const std::byte*>(key.data()),
                reinterpret_cast<const std::byte*>(key.data()) + key.size());
        }
        sw.add(sc::CkptSectionType::kDocmapDeltaV3, std::move(b));
    }
    // V5 meta：窗口内 live 行的 meta（必须排在行段之后——读端按文件序应用，
    // 行先落再 set_meta）。窗口内无 meta 则不写段。
    {
        std::vector<std::byte> mb;
        if (encode_docmap_meta_section(docmap, from, watermark, mb)) {
            sw.add(sc::CkptSectionType::kDocmapMetaDelta, std::move(mb));
        }
    }
    // keydir meta 仅在 docmap 组件落（S14-7 成对不变量）。
    if (!keydir_delta.empty()) {
        std::vector<std::byte> kb(keydir_delta.begin(), keydir_delta.end());
        sw.add(sc::CkptSectionType::kKeydirDelta, std::move(kb));
    }
    // 文件版本 3（含 V3 段型）——旧读端整文件拒收 → 链断退 fold（降级安全，
    // 见 kDocmapDeltaV3 注释）。
    if (!sc::SearchCheckpoint::write(dpath, watermark, sw.sections(),
                                     sc::detail::kCkptVersion3)) {
        return false;
    }
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
    std::optional<std::span<const std::byte>> meta_pl;
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
        } else if (ls.type == static_cast<std::uint16_t>(
                                  sc::CkptSectionType::kDocmapMeta)) {
            // 延后到行段之后应用（不依赖段在文件里的先后）。
            meta_pl = std::span<const std::byte>(ls.payload.data(),
                                                 ls.payload.size());
        }
        // 旧文件可能含 meta/terms 等扩展段——忽略。
    }
    if (!any) segments_ok = false;
    if (segments_ok && meta_pl &&
        !apply_docmap_meta_section(docmap, *meta_pl)) {
        segments_ok = false;  // meta 段畸形 → 整组件退 fold（fold 可重建 meta）
    }
    // 链重放（.prev 回退 = 链不可信，与 SearchLayer 版语义一致）。S20-2 R2：
    // 走读收敛至 sc::walk_chain（有界，apply = apply_delta_file 含 CRC 预检）。
    std::uint64_t coverage = lc->watermark;
    bool chain_ok = true;
    if (segments_ok && !from_prev) {
        const auto w = sc::walk_chain(
            fp, /*base_gen=*/coverage, /*base_coverage=*/coverage, chain_seq,
            /*unbounded=*/false, [&](const sc::LoadedCheckpoint& dc) {
                return apply_delta_file(docmap, dc.sections, hook);
            });
        coverage = w.coverage;
        chain_ok = w.ok;
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
