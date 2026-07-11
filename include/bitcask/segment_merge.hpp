// segment_merge.hpp — k-way 段合并(S30-P3)。设计:
// docs/design/s30-mmap-segments.md §4。
//
// 把 N 个输入段(v1 内存态 / v2 mmap 背衬混合皆可)流式合并为一个 v2 段
// 文件:词典有序 k 路归并逐 term 现产 posting;**死行物理回收**(docid
// 重编:live 行按输入序取稠密新 docid,死行跳过——posting/doc_store/
// positions 同步收敛);字段统计现算归正(df/N/sum_dl 不再含死点,
// 设计文档 §4「merge 自愈」)。
//
// === 内存上界 ===
// O(输入词典 + 输出块表 + docid 映射(4B×Σdocs) + 单 term 合并 posting)。
// posting/positions/doc_store 主体全流式——无 O(输出段) 缓冲。单 term
// 工作集 = 该词跨输入总 posting(最热词封顶),Lucene 同款接受。
//
// === 正确性要点 ===
// - 新 docid 按 (输入序, 段内序) 分配 ⇒ 各输入的重编 posting 天然升序、
//   跨输入拼接仍升序——**无需排序**,与「把 live 文档按同序 add 进单段」
//   逐位同构(金标测试依据,segment_v2_test)。
// - 统计:per-field N = 含该字段 posting 的 distinct live docid 数,
//   sum_dl = 其 per-field dl 之和(posting dls 列携带),bitset+首见即记。
// - 输入段 live 位在归并期间**不得变更**(调用契约:静止点/上层持
//   key_loc_mu_ 补拷窗口,同 P2 封口换入协议)。
//
// 线程安全:单线程调用;输入段只读(caller 持 shared_ptr pin)。

#pragma once

#include "bitcask/segment.hpp"
#include "bitcask/segment_v2.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitcask::search {

// 合并产物的 docid 重编映射(P3 换入时 key 定位重指用)。
struct MergeResult {
    static constexpr std::uint32_t kDead =
        std::numeric_limits<std::uint32_t>::max();
    // docid_map[i][old_docid] = 新段 docid;死行 = kDead。
    std::vector<std::vector<std::uint32_t>> docid_map;
    std::uint32_t out_docs = 0;
    std::uint64_t out_hi_lsn = 0;  // 输出段最高 LSN(清单 hi_lsn 用)
};

// 合并 inputs → path(v2 段文件,tmp+rename 原子)。失败返回 false(不留
// 半成品;tmp 由 writer 清理)。out 必填(换入重指依赖映射)。
[[nodiscard]] inline bool merge_segments_v2(
    const std::string& path,
    std::uint64_t seg_id,
    std::span<const std::shared_ptr<const SealedSegment>> inputs,
    MergeResult& out) {
    // ---- 阶段 0:docid 重编 + doc_store 反查表 + 全段统计 ----
    out.docid_map.clear();
    out.docid_map.resize(inputs.size());
    out.out_docs = 0;
    out.out_hi_lsn = 0;
    // 新 docid → (输入下标, 旧 docid)(doc_row 两趟流式反查)。
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rev;
    std::uint64_t total_dl = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& seg = *inputs[i];
        const auto n = static_cast<std::uint32_t>(seg.doc_count());
        out.docid_map[i].assign(n, MergeResult::kDead);
        for (std::uint32_t d = 0; d < n; ++d) {
            if (!seg.is_live(d)) continue;
            out.docid_map[i][d] = out.out_docs;
            rev.emplace_back(static_cast<std::uint32_t>(i), d);
            total_dl += seg.doc_len(d);
            out.out_hi_lsn = std::max(out.out_hi_lsn, seg.lsn_at(d));
            ++out.out_docs;
        }
    }

    // ---- 阶段 1:字段名并集(升序;恒含 kDefaultField——v2 reader 要求) ----
    std::vector<std::string> field_names;
    field_names.emplace_back(kDefaultField);
    for (const auto& in : inputs) {
        for (auto& n : in->merge_field_names()) field_names.push_back(std::move(n));
    }
    std::sort(field_names.begin(), field_names.end());
    field_names.erase(std::unique(field_names.begin(), field_names.end()),
                      field_names.end());

    // ---- 阶段 2:每字段一个 k-way 归并流 + 现算统计 ----
    struct FieldMergeState {
        std::uint64_t n_live = 0;
        std::uint64_t sum_dl = 0;
    };
    std::vector<FieldMergeState> fstates(field_names.size());
    std::vector<SegV2FieldSource> sources;
    sources.reserve(field_names.size());
    for (std::size_t fi = 0; fi < field_names.size(); ++fi) {
        const std::string& fname = field_names[fi];
        FieldMergeState* fs = &fstates[fi];
        SegV2FieldSource src;
        src.name = fname;
        src.visit = [&, fname, fs](const std::function<void(
                                       std::string_view,
                                       const bm25::PostingList&)>& emit) {
            // 游标:每输入一份词表(升序) + 当前下标。k ≤ 数十,线性求最小。
            struct Cur {
                std::vector<std::string> terms;
                std::size_t i = 0;
            };
            std::vector<Cur> curs(inputs.size());
            for (std::size_t s = 0; s < inputs.size(); ++s) {
                curs[s].terms = inputs[s]->merge_field_terms(fname);
            }
            // per-docid 首见位图(统计现算:N/sum_dl 只记 distinct docid)。
            std::vector<std::uint8_t> seen((out.out_docs + 7) / 8, 0);
            bm25::PostingList in_pl;
            bm25::PostingList out_pl;
            while (true) {
                const std::string* min_t = nullptr;
                for (const auto& c : curs) {
                    if (c.i >= c.terms.size()) continue;
                    if (min_t == nullptr || c.terms[c.i] < *min_t) {
                        min_t = &c.terms[c.i];
                    }
                }
                if (min_t == nullptr) break;
                const std::string term = *min_t;  // 拷出(游标推进使指向失效)

                out_pl.ords.clear();
                out_pl.tfs.clear();
                out_pl.dls.clear();
                out_pl.pos_data.clear();
                out_pl.pos_off.clear();
                out_pl.blocks.clear();
                out_pl.max_tf = 0;
                for (std::size_t s = 0; s < inputs.size(); ++s) {
                    auto& c = curs[s];
                    if (c.i >= c.terms.size() || c.terms[c.i] != term) continue;
                    c.i++;
                    if (!inputs[s]->merge_field_postings(fname, term, in_pl)) {
                        continue;  // 词表/取数瞬间不一致:静止段不应发生,防御跳过
                    }
                    const auto& map = out.docid_map[s];
                    for (std::size_t r = 0; r < in_pl.size(); ++r) {
                        const auto old_d =
                            static_cast<std::uint32_t>(in_pl.ords[r]);
                        if (old_d >= map.size()) continue;  // 防御
                        const auto nd = map[old_d];
                        if (nd == MergeResult::kDead) continue;  // 物理回收
                        out_pl.append(nd, in_pl.tfs[r], in_pl.dls[r],
                                      in_pl.positions(r));
                        out_pl.max_tf =
                            std::max(out_pl.max_tf, in_pl.tfs[r]);
                        // 统计:首见 docid 记 N/sum_dl(per-field dl 在
                        // posting dls 列)。
                        if (((seen[nd / 8] >> (nd % 8)) & 1u) == 0) {
                            seen[nd / 8] |= static_cast<std::uint8_t>(
                                1u << (nd % 8));
                            fs->n_live += 1;
                            fs->sum_dl += in_pl.dls[r];
                        }
                    }
                }
                if (!out_pl.empty()) emit(term, out_pl);
            }
        };
        src.stats = [fs] {
            return std::pair<std::uint64_t, std::uint64_t>{fs->n_live,
                                                           fs->sum_dl};
        };
        sources.push_back(std::move(src));
    }

    // ---- 阶段 3:流式写(doc_row 两趟经反查表取输入行) ----
    return write_segment_v2_streams(
        path, seg_id, sources, out.out_docs,
        [&](std::uint32_t nd) {
            const auto [si, od] = rev[nd];
            const auto& seg = *inputs[si];
            return SegV2DocRow{seg.key_at(od), seg.lsn_at(od),
                               seg.slot_at(od)};
        },
        total_dl);
}

}  // namespace bitcask::search
