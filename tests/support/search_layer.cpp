#include "bitcask/search_layer.hpp"
#include "bitcask/ckpt_chain.hpp"        // S20-2：walk_chain / remove_chain_files
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/text_utils.hpp"
#include "bitcask/codec.hpp"
#include "bitcask/highlighter.hpp"

#include <oneapi/tbb/parallel_for.h>      // S3:恢复期批量并行 analyze
#include <oneapi/tbb/task_arena.h>        // S7:共享有界 Search 池（inter-query）
#include <thread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace bitcask::search {

// S19-1：search_arena/parallel_for_queries 已迁 search_arena.{hpp,cpp}。

SearchLayer::SearchLayer(const SearchLayerConfig& config,
                         std::shared_ptr<index::Index> docmap)
    : config_(config)
    , index_holder_(docmap ? std::move(docmap)
                           : std::make_shared<index::Index>())
    , index_(*index_holder_)
    // S18-4：文本域整体抽出为 TextPlugin（analyzer/倒排/缓存在其构造）。
    // 三个窄接口引用都绑同一 Index 实例（DocTable/DocLenWriter/CompactionStats）。
    , text_(config.text_config(), *index_holder_, *index_holder_,
            *index_holder_)
    // S18-3：向量域整体抽出为 VectorPlugin（HNSW 装配/metric 映射在其构造）。
    , vec_(config.vector_config(), *index_holder_)
{
}

SearchLayer::SearchLayer(const SearchLayerConfig& config,
                         std::unique_ptr<text::Analyzer> injected_analyzer)
    : SearchLayer(config) {
    text_.replace_analyzer(std::move(injected_analyzer));  // S18-4
}

void SearchLayer::on_vector(std::uint64_t ord, std::span<const float> vec) {
    vec_.insert(ord, vec);  // S18-3：向量写整体委托 VectorPlugin
}

std::size_t SearchLayer::hnsw_size() const {
    return vec_.size();
}

void SearchLayer::rebuild_hnsw() {
    if (!vec_.enabled()) return;
    // S14-4：图重建（物理清死节点）后旧链的插入日志语义不再成立 → rebase
    // （legacy 全局标志；per-component rebase 语义 S18-6 收进插件 flush）。
    ckpt_rebase_needed_.store(true, std::memory_order_relaxed);
    vec_.rebuild();
}

// S18-3：查询/写入归一化 SIMD 内核已迁 vector_plugin.cpp。

// S18-4：materialize_hits / ordered_query_terms 已迁 TextPlugin。

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_vector(std::span<const float> query, std::size_t k,
                           std::size_t ef,
                           const meta::MetaFilter* filter) const {
    // S18-3：向量查询整体委托 VectorPlugin（归一化/live 过滤/ord 翻译）。
    return vec_.search(query, k, ef, filter);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_hybrid(std::string_view text_query,
                           std::span<const float> vec_query,
                           std::size_t k,
                           const meta::MetaFilter* filter) const {
    // S18-9：RRF 融合整体上移 HybridSearcher（纯算法平移）。
    return hybrid_.search(text_query, vec_query, k, filter);
}

// S18-4：field_index / intern_field_name 已迁 TextPlugin。

void SearchLayer::on_write(std::string_view key, std::uint64_t ord,
                           std::string_view text,
                           std::uint32_t file_id, std::uint64_t offset,
                           std::uint32_t total_sz, std::uint32_t tstamp) {
    // S16-2：legacy/standalone 入口——自落 docmap 行（doc_len 由 apply_text
    // 分析后经 set_doc_len 回填），随后跑单文本核心。流水线路径不走本方法。
    index_.put_doc(key, ord,
                   index::DocSlot{
                       index::DocLoc{.offset   = offset,
                                     .file_id  = file_id,
                                     .total_sz = total_sz},
                       tstamp,
                       /*doc_len=*/0});
    apply_text(key, ord, text);
}

void SearchLayer::apply_text(std::string_view key, std::uint64_t ord,
                             std::string_view text) {
    text_.apply_text(key, ord, text);  // S18-4：单文本核心归 TextPlugin
}

ReduceJob SearchLayer::map_analyze(
    std::string_view key, std::uint64_t ord,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) const {
    // S18-4：Map 阶段（纯函数并发）归 TextPlugin。
    return text_.map_analyze(key, ord, fields, file_id, offset, total_sz,
                             tstamp);
}

// S18-4：Reduce 阶段三域融合解体——BM25 半边归 TextPlugin::apply_job，
// 向量半边归 VectorPlugin::insert（S18-5 起由宿主直接双插件扇出，本 shim
// 供 adapter/standalone/recover 路径过渡）。
void SearchLayer::reduce_apply(const ReduceJob& job, std::span<const float> vec) {
    text_.apply_job(job);
    if (!vec.empty()) {
        vec_.insert(job.ord, vec);
    }
}

void SearchLayer::on_write_fields(
    std::string_view key, std::uint64_t ord,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) {
    // S10-A5:同步路径—fields 借 caller 的 string 构造 views（无堆分配）。
    std::vector<std::pair<std::string_view, std::string_view>> fvs;
    fvs.reserve(fields.size());
    for (const auto& [name, text] : fields) {
        fvs.emplace_back(name, text);
    }
    auto job = map_analyze(key, ord, fvs, file_id, offset, total_sz, tstamp);
    // S16-2：standalone 同步路径自落 docmap 行（doc_len 直接用分析产物）。
    index_.put_doc(key, ord,
                   index::DocSlot{index::DocLoc{.offset   = offset,
                                                .file_id  = file_id,
                                                .total_sz = total_sz},
                                  tstamp, job.total_doc_len});
    reduce_apply(job, {});
}

std::optional<std::uint64_t> SearchLayer::on_delete(std::string_view key, std::uint64_t tomb_ord) {
    // S16-2：legacy/standalone 入口——自查旧行 + 自删 docmap，再跑共享核心。
    // 流水线路径不走本方法（宿主捕获 prior_ord 并 remove，adapter 调三参版）。
    auto slot = index_.get(key);
    if (!slot) return std::nullopt;
    index_.remove(key, tomb_ord);
    on_delete(key, tomb_ord, slot->ord);
    return tomb_ord;
}

void SearchLayer::on_delete(std::string_view key, std::uint64_t tomb_ord,
                            std::uint64_t prior_ord) {
    // S18-4：BM25 统计扣减半边归 TextPlugin（docmap 删除/日志由 Index 自记账）。
    text_.on_delete(key, tomb_ord, prior_ord);
}

void SearchLayer::on_relocate(std::string_view key, std::uint64_t ord,
                              std::uint32_t new_file_id, std::uint64_t new_offset,
                              std::uint32_t new_total_sz) {
    auto slot = index_.get(key);
    if (!slot) return;

    // S18-2：docmap 脏位由 Index::put_doc 自记账（loc 列刷新）。
    index_.put_doc(key, ord,
                   index::DocSlot{
                       index::DocLoc{.offset   = new_offset,
                                     .file_id  = new_file_id,
                                     .total_sz = new_total_sz},
                       slot->tstamp,
                       slot->doc_len});
}

// ---- S18-4：查询全家委托 TextPlugin ----

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_text(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override,
                         const meta::MetaFilter* filter) const {
    return text_.search_text(query, k, params_override, filter);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_phrase(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    return text_.search_phrase(query, k, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_near(std::string_view query, std::uint32_t slop,
                         std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    return text_.search_near(query, slop, k, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_fuzzy(std::string_view query, std::size_t k,
                          std::uint32_t max_edit_distance,
                          const bm25::Bm25Params* params_override) const {
    return text_.search_fuzzy(query, k, max_edit_distance, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::bool_search(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    return text_.bool_search(query, k, params_override);
}

std::optional<bm25::ScoreExplanation>
SearchLayer::explain(std::string_view query, std::string_view key,
                     const bm25::Bm25Params* params_override) const {
    return text_.explain(query, key, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_wildcard(std::string_view pattern, std::size_t k,
                             const bm25::Bm25Params* params_override) const {
    return text_.search_wildcard(pattern, k, params_override);
}

std::expected<std::vector<SearchHit>, SearchError>
SearchLayer::search_fields(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    return text_.search_fields(query, k, params_override);
}

void SearchLayer::recover_doc(std::string_view key, std::uint64_t ord,
                              std::string_view text,
                              std::uint32_t file_id, std::uint64_t offset,
                              std::uint32_t total_sz, std::uint32_t tstamp,
                              std::span<const float> vector) {
    // S6-P0:单字段(kDefaultField) 恢复——map_analyze + reduce_apply 复用。
    // map_analyze 在 default_field 上写出 → wrote_default=true,触发不到
    // catch-all 路径,语义与原版逐条 recover_doc 完全一致。
    std::vector<std::pair<std::string_view, std::string_view>> fields;
    fields.emplace_back(kDefaultField, text);
    auto job = map_analyze(key, ord, fields, file_id, offset, total_sz, tstamp);
    // S16-2：恢复路径自落 docmap 行（SearchLayer 借用宿主实例；持久化载入
    // 仍归本层直到 P3）。
    index_.put_doc(key, ord,
                   index::DocSlot{index::DocLoc{.offset   = offset,
                                                .file_id  = file_id,
                                                .total_sz = total_sz},
                                  tstamp, job.total_doc_len});
    reduce_apply(job, vector);
}

// S3:批量恢复——并行 analyze + 串行有序插入（见头文件注释的正确性论证）。
// 与逐条 recover_doc 字节等价：同一 fold 序插入、同一单字段路径、HNSW 串行。
void SearchLayer::recover_doc_batch(std::vector<RecoverDoc>& batch) {
    if (batch.empty()) return;
    const std::size_t n = batch.size();

    // 阶段一：并行 map_analyze（analyzer_ const 无可变态；写 jobs[i] 互不相交，
    // 无共享可变状态）。map_analyze 自身 const、纯函数，并行调用安全。TBB
    // 全局线程池，无 per-batch 线程创建。
    std::vector<ReduceJob> jobs(n);
    tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
        const auto& d = batch[i];
        std::vector<std::pair<std::string_view, std::string_view>> fields;
        if (d.fields.empty()) {
            fields.emplace_back(kDefaultField, d.text);
        } else {
            // S14-6：命名字段文档镜像活写路径（map 只喂 fields，text 不参与
            // 索引——与 put_doc 的 task.fields 装配一致）；per-field 词表与
            // catch-all 合并进默认域都在 map_analyze 内自然复原。
            fields.reserve(d.fields.size());
            for (const auto& [fname, fval] : d.fields) {
                fields.emplace_back(fname, fval);
            }
        }
        jobs[i] = map_analyze(d.key, d.ord, fields,
                              d.file_id, d.offset, d.total_sz, d.tstamp);
    });

    // 阶段二：按 batch 序串行 reduce_apply（= fold 序）。HNSW 单写者 = 本线程。
    // reduce_apply 内部逐条按词失效缓存（S13-P1）：恢复期无查询，缓存本就空，
    // 性能影响可忽略；最终状态与旧版完全一致。
    for (std::size_t i = 0; i < n; ++i) {
        const auto& d = batch[i];
        // S16-2：恢复路径自落 docmap 行（同 recover_doc；行先于 postings，
        // 与流水线的宿主先落序一致）。
        index_.put_doc(d.key, d.ord,
                       index::DocSlot{
                           index::DocLoc{.offset   = d.offset,
                                         .file_id  = d.file_id,
                                         .total_sz = d.total_sz},
                           d.tstamp, jobs[i].total_doc_len});
        reduce_apply(jobs[i], d.vector);
    }
}

void SearchLayer::recover_tomb(std::string_view key, std::uint64_t ord) {
    // S18-2：脏位 + 窗口删除日志（S14-4 门限：重叠区旧墓碑不入）由
    // Index::remove 自记账，门限 = Index 的 delta 窗口水位。
    index_.remove(key, ord);
}


// S18-2：docmap sidecar 序列化已下沉 index::Index（宿主服务自持其持久化
// 编解码）。此处保留转发壳供 legacy save/load 路径与既有测试使用。
bool SearchLayer::serialize_docmap(std::vector<std::uint8_t>& buf,
                                   std::uint64_t covers_next_ord) const {
    return index_.serialize_docmap(buf, covers_next_ord);
}

std::optional<std::uint64_t>
SearchLayer::deserialize_docmap(std::span<const std::uint8_t> buf) {
    return index_.deserialize_docmap(buf);
}

void SearchLayer::rebuild_index(DocReader doc_reader) {
    ckpt_rebase_needed_.store(true, std::memory_order_relaxed);  // S14-4
    // S18-4：docmap 遍历 + 磁盘读回（宿主侧素材）留 shim，重建本体归
    // TextPlugin（emit 回调喂 (ord, text)）。
    text_.rebuild_index([&](const std::function<void(
                                std::uint64_t, const std::string&)>& emit) {
        index_.for_each_live([&](std::uint64_t ord,
                                 const std::string& /*ext_id*/,
                                 const index::DocSlot& slot) {
            auto text = doc_reader(slot.loc.file_id, slot.loc.offset,
                                   slot.loc.total_sz);
            if (!text) return;
            emit(ord, *text);
        });
    });
}

std::size_t SearchLayer::compact(double dead_ratio_threshold) {
    // S14-4：压实破坏 base+delta 可重构性（posting 物理重排）→ 下次 save
    // 必须全量 base、链坍缩。merge 收尾恒 compact ⇒ merge 即 rebase 点。
    // S18-4：压实本体归 TextPlugin，legacy 全局 rebase 标志留 shim。
    ckpt_rebase_needed_.store(true, std::memory_order_relaxed);
    return text_.compact(dead_ratio_threshold);
}

std::size_t SearchLayer::total_postings() const {
    return text_.total_postings();  // S18-4
}

std::expected<std::vector<SearchHitEx>, SearchError>
SearchLayer::search_text_highlight(std::string_view query, std::size_t k,
                                   const HighlightOptions& opts) const {
    return text_.search_text_highlight(query, k, opts);  // S18-4
}

// ---- P14e:统一分段 search.ckpt 持久化 ----

// S20-1 R7：本文件已在 namespace bitcask::search 内，小端编解码直接用
// detail::put_u*/get_u*（search_checkpoint.hpp），删除原 *_byte 重实现。

// S14-4：delta 保存——只序列化窗口 [ckpt_chain_wm_, watermark) 的增量，
// 写独立文件 search.ckpt.d<seq>（BCSC 容器，tmp+rename），base 不动。
// 与 base 同在 reducer / 静止点上下文执行。失败返回 false（caller 落回
// 全量 base）；成功推进链状态并清窗口日志/脏位。
bool SearchLayer::save_delta_ckpt(const std::string& base_path,
                                  std::uint64_t watermark,
                                  std::span<const std::byte> keydir_delta) {
    namespace sc = bitcask::search;
    const std::uint64_t from = ckpt_chain_wm_;

    sc::SectionWriter sw;  // S20-1 R4

    // 段 kDeltaInfo：链校验三元组。
    {
        std::vector<std::byte> b;
        detail::put_u64(b, ckpt_base_gen_);
        detail::put_u64(b, from);
        detail::put_u32(b, ckpt_next_seq_);
        sw.add(sc::CkptSectionType::kDeltaInfo, std::move(b));
    }

    // bm25 delta（default / fields）——脏才有内容；干净直接省段（S18-4：
    // 经 TextPlugin 原语）。
    if (text_.dirty_default()) {
        std::vector<std::byte> b;
        if (text_.serialize_default_delta(b, from)) {
            sw.add(sc::CkptSectionType::kBm25DefaultDelta, std::move(b));
        }
    }
    if (text_.dirty_fields()) {
        std::vector<std::byte> fb;
        if (text_.serialize_fields_delta(fb, from)) {
            sw.add(sc::CkptSectionType::kBm25FieldsDelta, std::move(fb));
        }
    }

    // docmap delta：窗口 live 行 + 删除日志。恒写（可能为空，几十字节）。
    {
        std::vector<std::byte> b;
        const std::uint64_t cnt_pos = b.size();
        detail::put_u64(b, 0);  // 行数占位
        std::uint64_t rows = 0;
        bool ok = true;
        index_.for_each_live_in(
            from, watermark,
            [&](std::uint64_t ord, const std::string& ext,
                const index::DocSlot& slot) {
                if (ext.size() > 0xFFFF) { ok = false; return; }
                detail::put_u64(b, ord);
                detail::put_u16(b, static_cast<std::uint16_t>(ext.size()));
                b.insert(b.end(),
                    reinterpret_cast<const std::byte*>(ext.data()),
                    reinterpret_cast<const std::byte*>(ext.data()) +
                        ext.size());
                detail::put_u32(b, slot.loc.file_id);
                detail::put_u64(b, slot.loc.offset);
                detail::put_u32(b, slot.loc.total_sz);
                detail::put_u32(b, slot.tstamp);
                detail::put_u32(b, slot.doc_len);
                ++rows;
            });
        if (!ok) return false;  // 超长 key（理论不可达，写端已限）
        std::memcpy(b.data() + cnt_pos, &rows, 8);
        // S18-2：删除日志由 Index 自记账，此处读快照序列化。
        const auto legacy_removals = index_.removals_snapshot();
        detail::put_u64(b, static_cast<std::uint64_t>(legacy_removals.size()));
        for (const auto& [key, tomb] : legacy_removals) {
            if (key.size() > 0xFFFF) return false;
            detail::put_u64(b, tomb);
            detail::put_u16(b, static_cast<std::uint16_t>(key.size()));
            b.insert(b.end(),
                reinterpret_cast<const std::byte*>(key.data()),
                reinterpret_cast<const std::byte*>(key.data()) + key.size());
        }
        sw.add(sc::CkptSectionType::kDocmapDelta, std::move(b));
    }

    // S14-7：keydir 元数据段（水位/标量/fstats，caller 于提交时刻构建）。
    // 与搜索增量同文件原子成对——delta 路径不再单写 kv.keydir.ckpt，
    // 成对写序的崩溃窗口在增量路径上彻底消失。
    if (!keydir_delta.empty()) {
        std::vector<std::byte> b(keydir_delta.begin(), keydir_delta.end());
        sw.add(sc::CkptSectionType::kKeydirDelta, std::move(b));
    }

    // hnsw delta：窗口插入日志（S18-3：日志归 VectorPlugin）。
    if (!vec_.delta_log_empty()) {
        std::vector<std::byte> b;
        vec_.serialize_delta_log(b);
        sw.add(sc::CkptSectionType::kHnswDelta, std::move(b));
    }

    const std::string dpath =
        base_path + ".d" + std::to_string(ckpt_next_seq_);
    if (!sc::SearchCheckpoint::write(dpath, watermark, sw.sections())) {
        return false;
    }

    ckpt_chain_wm_ = watermark;
    ++ckpt_next_seq_;
    // S18-1/4：入账门归各持有方——legacy 单链路径联动置同值经下方
    // text_/vec_ set_chain_state（Index 窗口经 begin_delta_window）。
    // S18-2/S18-3：docmap 记账收尾经 Index，vec 记账收尾经 VectorPlugin。
    index_.begin_delta_window(watermark);
    index_.clear_removals();
    index_.clear_dirty();
    vec_.set_chain_state({ckpt_base_gen_, ckpt_chain_wm_, ckpt_next_seq_});
    vec_.clear_delta_log();
    vec_.clear_dirty();
    // 脏位语义 =「自上次 save（任一种）以来」——delta 落成同样清零（S18-4：
    // bm25 归 TextPlugin，链状态联动同值）。
    text_.set_chain_state({ckpt_base_gen_, ckpt_chain_wm_, ckpt_next_seq_});
    text_.clear_dirty();
    return true;
}

// S14-4：应用单个 delta 文件的段集。先整体 CRC 预检（原子性：任何坏段 →
// 整个 delta 拒绝，不部分应用）；应用中途解析失败返回 false，caller 视链
// 断裂、退全量 fold（自门保证多放安全）。
bool SearchLayer::apply_delta_file(
    const std::vector<bitcask::search::LoadedSection>& sections,
    const DeltaReplayHook& hook) {
    namespace sc = bitcask::search;
    for (const auto& ls : sections) {
        if (!ls.crc_ok) return false;
    }
    // S14-7：钩子素材——docmap 行/删除（解析一次共用）+ keydir 元数据段。
    std::vector<DeltaDocRow> hook_rows;
    std::vector<DeltaRemoval> hook_rems;
    std::span<const std::byte> hook_meta;
    for (const auto& ls : sections) {
        switch (static_cast<sc::CkptSectionType>(ls.type)) {
        case sc::CkptSectionType::kBm25DefaultDelta: {
            // S18-4：bm25 delta 重放归 TextPlugin 原语。
            if (!text_.apply_default_delta(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                return false;
            }
            break;
        }
        case sc::CkptSectionType::kBm25FieldsDelta: {
            if (!text_.apply_fields_delta(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                return false;
            }
            break;
        }
        case sc::CkptSectionType::kDocmapDelta: {
            const auto* p = ls.payload.data();
            const auto* end = p + ls.payload.size();
            std::vector<DeltaDocRow>& rows = hook_rows;
            std::vector<DeltaRemoval>& rems = hook_rems;
            if (end - p < 8) return false;
            std::uint64_t rn = detail::get_u64(p); p += 8;
            rows.reserve(rn);
            for (std::uint64_t i = 0; i < rn; ++i) {
                if (end - p < 10) return false;
                DeltaDocRow r;
                r.ord = detail::get_u64(p); p += 8;
                std::uint16_t klen = detail::get_u16(p); p += 2;
                if (end - p < klen + 24) return false;
                r.ext.assign(reinterpret_cast<const char*>(p), klen);
                p += klen;
                r.slot.loc.file_id = detail::get_u32(p); p += 4;
                r.slot.loc.offset = detail::get_u64(p); p += 8;
                r.slot.loc.total_sz = detail::get_u32(p); p += 4;
                r.slot.tstamp = detail::get_u32(p); p += 4;
                r.slot.doc_len = detail::get_u32(p); p += 4;
                rows.push_back(std::move(r));
            }
            if (end - p < 8) return false;
            std::uint64_t mn = detail::get_u64(p); p += 8;
            rems.reserve(mn);
            for (std::uint64_t i = 0; i < mn; ++i) {
                if (end - p < 10) return false;
                DeltaRemoval m;
                m.tomb = detail::get_u64(p); p += 8;
                std::uint16_t klen = detail::get_u16(p); p += 2;
                if (end - p < klen) return false;
                m.key.assign(reinterpret_cast<const char*>(p), klen);
                p += klen;
                rems.push_back(std::move(m));
            }
            if (p != end) return false;
            // 按 ord 交错重放（行与删除都按 ord 升序产出）——「删后重写」
            // 场景下删除必须先于同 key 的新行应用，否则误杀。
            std::size_t ri = 0, mi = 0;
            while (ri < rows.size() || mi < rems.size()) {
                const bool take_row =
                    mi >= rems.size() ||
                    (ri < rows.size() && rows[ri].ord < rems[mi].tomb);
                if (take_row) {
                    index_.put_doc(rows[ri].ext, rows[ri].ord, rows[ri].slot);
                    ++ri;
                } else {
                    index_.remove(rems[mi].key, rems[mi].tomb);
                    ++mi;
                }
            }
            break;
        }
        case sc::CkptSectionType::kHnswDelta: {
            // S18-3：直插重放归 VectorPlugin（不入日志、不标脏，见其注释）。
            if (!vec_.apply_delta_log(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                return false;
            }
            break;
        }
        case sc::CkptSectionType::kKeydirDelta:
            hook_meta = std::span<const std::byte>(ls.payload.data(),
                                                    ls.payload.size());
            break;
        case sc::CkptSectionType::kDeltaInfo:
        default:
            break;  // info 由 caller 校验；未知段忽略（前向兼容）。
        }
    }
    // S14-7：全部段应用成功后回调（行/删除已按本 delta 解析；顺序契约：
    // caller 先应用行/删除、后应用 meta——meta 的水位声明覆盖 ≤ 行集）。
    if (hook) hook(hook_rows, hook_rems, hook_meta);
    return true;
}

bool SearchLayer::save_search_ckpt(std::string_view path,
                                   std::uint64_t watermark,
                                   std::span<const std::byte> keydir_delta,
                                   bool* wrote_base) {
    namespace sc = bitcask::search;
    const std::string fp(path);
    if (wrote_base) *wrote_base = true;  // delta 路径成功时改写

    // S14-4：增量路径——无 rebase 事件（compact/rebuild）且链状态有效时只
    // 写 delta 文件（search.ckpt.d<seq>），base 不重写：写 I/O 从 ∝ 索引
    // 总量降到 ∝ 窗口增量。失败落回全量 base（重建链，安全兜底）。
    // S14-5：链长达上限 → 强制 base（坍缩回收）——不 merge 的纯追加负载
    // 否则链无界堆积（merge 的 rebase 只覆盖有删除的库）。
    const bool chain_full =
        config_.max_delta_chain != 0 &&
        ckpt_next_seq_ > config_.max_delta_chain;
    if (!chain_full &&
        !ckpt_rebase_needed_.load(std::memory_order_relaxed) &&
        watermark >= ckpt_chain_wm_) {
        if (save_delta_ckpt(fp, watermark, keydir_delta)) {
            if (wrote_base) *wrote_base = false;
            return true;
        }
    }

    std::vector<sc::CkptSection> secs;
    // 段 payload 缓冲区须活到 write() 完成——span 是非 owning 视图。
    std::vector<std::vector<std::byte>> byte_bufs;
    std::vector<std::vector<std::uint8_t>> u8_bufs;
    auto add_byte_sec = [&](std::uint16_t type,
                            std::vector<std::byte> buf) {
        byte_bufs.push_back(std::move(buf));
        secs.push_back(sc::CkptSection{
            type, 0,
            std::span<const std::byte>(byte_bufs.back().data(),
                                        byte_bufs.back().size())});
    };
    auto add_u8_sec = [&](std::uint16_t type,
                          std::vector<std::uint8_t> buf) {
        u8_bufs.push_back(std::move(buf));
        secs.push_back(sc::CkptSection{
            type, 0,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(u8_bufs.back().data()),
                u8_bufs.back().size())});
    };

    // S14-3（路线 A §5）：段级 dirty-bit——干净段从现有 search.ckpt 原字节
    // 前移（免重序列化：无向量写周期 hnsw 段零 CPU，纯向量负载 bm25 段零
    // CPU）。读取发生在下方 rename→.prev 之前（同一 inode）。脏位只捕获
    // 一次：save 全程在 reducer / 静止点内执行，无并发写者推进的窗口。
    // 干净但旧文件缺段/坏段（首存、结构损坏）→ 正常回退重序列化。
    // S14-4：链激活期间 base 文件字节落后于内存（差 delta 内容）——一律
    // 视作脏，禁止 S14-3 原字节前移（否则新 base 把链内容丢了）。
    const bool chain_active = ckpt_chain_wm_ > ckpt_base_gen_;
    const bool d_doc =
        chain_active || index_.dirty();  // S18-2：docmap 脏位经 Index
    const bool d_bd =
        chain_active || text_.dirty_default();  // S18-4：脏位经 TextPlugin
    const bool d_bf =
        chain_active || text_.dirty_fields();
    const bool d_h =
        chain_active || vec_.dirty();  // S18-3：vec 脏位经 VectorPlugin
    std::vector<sc::LoadedSection> carried;  // 须活到 write() 完成
    if (!(d_doc && d_bd && d_bf && d_h)) {
        auto sel = sc::SearchCheckpoint::read_selected(
            fp, [&](std::uint16_t t) {
                switch (static_cast<sc::CkptSectionType>(t)) {
                case sc::CkptSectionType::kDocmap:      return !d_doc;
                case sc::CkptSectionType::kBm25Default: return !d_bd;
                case sc::CkptSectionType::kBm25Fields:  return !d_bf;
                case sc::CkptSectionType::kHnsw:        return !d_h;
                default:                                return false;
                }
            });
        if (sel) carried = std::move(*sel);
    }
    auto carried_sec =
        [&](sc::CkptSectionType t) -> const sc::LoadedSection* {
        for (const auto& ls : carried) {
            if (ls.type == static_cast<std::uint16_t>(t)) return &ls;
        }
        return nullptr;
    };
    auto add_carried = [&](const sc::LoadedSection& ls) {
        secs.push_back(sc::CkptSection{
            ls.type, ls.flags,
            std::span<const std::byte>(ls.payload.data(),
                                        ls.payload.size())});
    };

    // 段 1: docmap (type 1)。S14-3：干净则前移（其内嵌的旧 covers_next_ord
    // 较小——自门方向安全，fold 多放的重叠区被幂等丢弃）。
    if (const auto* ls =
            d_doc ? nullptr : carried_sec(sc::CkptSectionType::kDocmap)) {
        add_carried(*ls);
    } else {
        std::vector<std::uint8_t> buf;
        if (serialize_docmap(buf, watermark)) {
            add_u8_sec(static_cast<std::uint16_t>(sc::CkptSectionType::kDocmap),
                       std::move(buf));
        }
    }

    // 段 2 + 3: bm25.default + bm25.fields。S14-3：各自独立前移（S18-4：
    // 序列化经 TextPlugin 原语）。
    if (const auto* ls = d_bd
            ? nullptr
            : carried_sec(sc::CkptSectionType::kBm25Default)) {
        add_carried(*ls);
    } else {
        std::vector<std::byte> buf;
        if (text_.serialize_default(buf)) {
            add_byte_sec(
                static_cast<std::uint16_t>(sc::CkptSectionType::kBm25Default),
                std::move(buf));
        }
    }
    if (const auto* ls = d_bf
            ? nullptr
            : carried_sec(sc::CkptSectionType::kBm25Fields)) {
        add_carried(*ls);
    } else {
        std::vector<std::byte> fbuf;
        if (text_.serialize_fields(fbuf)) {
            add_byte_sec(
                static_cast<std::uint16_t>(sc::CkptSectionType::kBm25Fields),
                std::move(fbuf));
        }
    }

    // 段 4: hnsw (type 4)。V7:BCVS v2 双文件——vecs_ 先落 search.vec
    // (save_vec_payload, S14-2 起常规为追加),再 serialize header 入 ckpt 段。
    // S14-3：干净 ⇒ 无向量写 ⇒ .vec 也未动——BVH2 段前移且跳过
    // save_vec_payload（本周期图序列化零成本）。
    if (const auto* ls = (!vec_.enabled() || d_h)
            ? nullptr
            : carried_sec(sc::CkptSectionType::kHnsw)) {
        add_carried(*ls);
    } else if (vec_.enabled()) {
        auto hnsw = vec_.graph();  // S18-3：图句柄经 VectorPlugin
        if (hnsw) {
            const std::string vec_path =
                std::filesystem::path(fp).replace_extension(".vec").string();
            const std::string qc_path =
                std::filesystem::path(fp).replace_extension(".qc8").string();
            if (hnsw->save_vec_payload(vec_path) &&
                hnsw->save_qc_payload(qc_path)) {
                std::vector<std::uint8_t> buf;
                if (hnsw->serialize(buf)) {
                    add_u8_sec(
                        static_cast<std::uint16_t>(sc::CkptSectionType::kHnsw),
                        std::move(buf));
                }
            }
        }
    }

    // 代际回退:把现有 search.ckpt 重命名为 search.ckpt.prev。
    {
        const std::string prev = fp + ".prev";
        std::error_code ec;
        if (std::filesystem::exists(fp, ec)) {
            std::filesystem::rename(fp, prev, ec);
        }
    }

    if (!sc::SearchCheckpoint::write(fp, watermark, secs)) return false;

    // S14-3：文件此刻反映内存态（fresh 序列化或 carried 原字节）→ 清脏位。
    // save 在 reducer / 静止点内执行，序列化之后无并发写者置位的窗口。
    // S18-2/3/4：docmap 脏位经 Index、bm25 经 TextPlugin、vec 经 VectorPlugin。
    text_.clear_dirty();
    vec_.clear_dirty();

    // S14-4：base 落成 → 链坍缩（S20-2 R8：清 .d 链，连续序号 + 8 空洞
    // orphan 扫尾），重置链状态与窗口日志。
    sc::remove_chain_files(fp);
    ckpt_base_gen_ = watermark;
    ckpt_chain_wm_ = watermark;
    ckpt_next_seq_ = 1;
    // S18-1/4：legacy base 落成同样联动各持有方链状态（下方三连）。
    ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
    // S18-2/3：docmap 记账收尾经 Index，vec 收尾经 VectorPlugin。
    index_.begin_delta_window(watermark);
    index_.clear_removals();
    text_.set_chain_state({watermark, watermark, 1});
    vec_.set_chain_state({watermark, watermark, 1});
    vec_.clear_delta_log();

    // S22-B2：WAL 已退役（S14-4 delta 链取代；enable_wal 生产零调用）——
    // 旧「保存成功后截断 WAL」步骤删除。
    return true;
}

SearchLayer::CkptLoadResult
SearchLayer::load_search_ckpt(std::string_view path,
                              const DeltaReplayHook& hook) {
    namespace sc = bitcask::search;
    const std::string fp(path);
    const std::string prev = fp + ".prev";
    // V7:BCVS v2 vecs_ payload 路径(与 ckpt 同目录,.vec 扩展名)。
    const std::string vec_path =
        std::filesystem::path(fp).replace_extension(".vec").string();

    auto try_load = [&](std::string_view p) -> std::optional<sc::LoadedCheckpoint> {
        return sc::SearchCheckpoint::read(p);
    };

    auto lc = try_load(fp);
    bool from_prev = false;
    if (!lc) {
        lc = try_load(prev);
        if (!lc) return {};
        from_prev = true;
    }

    CkptLoadResult result;
    result.loaded = true;
    result.watermark = lc->watermark;
    result.all_segments_ok = true;

    // 逐段分发到反序列化器。
    bool bm25_loaded = false;
    bool docmap_loaded = false;
    bool hnsw_loaded = false;
    // S14-3: per-type 载入成功标记（清脏位用；bm25_loaded 是「至少一个域」
    // 的聚合语义，粒度不够）。
    bool default_sec_ok = false;
    bool fields_sec_ok = false;

    for (auto& ls : lc->sections) {
        auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (!ls.crc_ok) {
            result.all_segments_ok = false;
            continue;
        }
        switch (st) {
        case sc::CkptSectionType::kBm25Default: {
            // S18-4：反序列化归 TextPlugin 原语（成功即自清对应脏位）。
            if (text_.deserialize_default(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                bm25_loaded = true;
                default_sec_ok = true;  // S14-3
            } else {
                result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kBm25Fields: {
            if (text_.deserialize_fields(std::span<const std::byte>(
                    ls.payload.data(), ls.payload.size()))) {
                bm25_loaded = true;
                fields_sec_ok = true;  // S14-3
            } else {
                result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kDocmap: {
            auto covers = deserialize_docmap(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(ls.payload.data()),
                ls.payload.size()));
            if (covers) {
                docmap_loaded = true;
            } else {
                result.all_segments_ok = false;
            }
            break;
        }
        case sc::CkptSectionType::kHnsw: {
            // S18-3：图 + .vec/.qc8 payload 载入归 VectorPlugin。
            if (vec_.enabled()) {
                const std::string qc_path =
                    std::filesystem::path(vec_path)
                        .replace_extension(".qc8")
                        .string();
                if (vec_.load_graph_section(
                        std::span<const std::byte>(ls.payload.data(),
                                                   ls.payload.size()),
                        vec_path, qc_path)) {
                    hnsw_loaded = true;
                } else {
                    result.all_segments_ok = false;
                }
            }
            break;
        }
        default:
            break;  // 未知段类型（meta/terms 等）忽略。
        }
    }

    // 如果 docmap 未载入,标记 all_segments_ok=false（需要 fold 补全 Index 侧表）。
    if (!docmap_loaded) result.all_segments_ok = false;
    // bm25 至少要有一个字段载入才算成功。
    if (!bm25_loaded) result.all_segments_ok = false;
    // 有向量配置但 hnsw 未载入 → 需要重建。
    if (vec_.enabled() && !hnsw_loaded) result.all_segments_ok = false;

    // S14-4：.prev 回退显式化——段级健康与「可走字节水位快路径」分离：
    // 退到旧代时磁盘 keydir 快照可能与坏掉的新代成对（水位超前于 prev
    // 覆盖），caller 必须放弃快路径退全量 fold（自门跳过已载入区）。
    result.from_prev = from_prev;

    // S14-4：delta 链重放。base 健康且非 .prev 回退才吃链（回退时
    // base_gen 天然不匹配，此处显式跳过是双保险）。链在「文件缺失」处
    // 正常终止；「文件存在但无效」（结构坏/info 链断/apply 失败）→ 保守
    // 判整体不健康、退全量 fold——与 base 坏段同级的健康语义，快路径的
    // 字节水位可能超前于链覆盖，必须放弃。
    // S20-2 R2：无界走读（chain_seq=0）收敛至 sc::walk_chain。
    std::uint64_t chain_coverage = result.watermark;
    const std::uint64_t chain_base_gen = result.watermark;
    std::uint32_t chain_next_seq = 1;
    if (result.all_segments_ok && !from_prev) {
        const auto w = sc::walk_chain(
            fp, chain_base_gen, /*base_coverage=*/chain_coverage,
            /*chain_seq=*/0, /*unbounded=*/true,
            [&](const sc::LoadedCheckpoint& dc) {
                return apply_delta_file(dc.sections, hook);
            });
        chain_coverage = w.coverage;
        chain_next_seq = w.next_seq;
        if (!w.ok) result.all_segments_ok = false;
    }
    result.watermark = chain_coverage;
    // 链状态初始化：后续 save 由此续链；不健康 ⇒ rebase（下次全量 base）。
    ckpt_base_gen_ = chain_base_gen;
    ckpt_chain_wm_ = chain_coverage;
    ckpt_next_seq_ = chain_next_seq;
    // S18-1/4：legacy 载入同样联动各持有方链状态（下方三连）。
    ckpt_rebase_needed_.store(!result.all_segments_ok,
                              std::memory_order_relaxed);
    // S18-2/3：docmap 记账收尾经 Index（重放污染清空 + 窗口对齐链覆盖），
    // vec 收尾经 VectorPlugin（链状态同步顺带推进入账窗口）。
    index_.begin_delta_window(chain_coverage);
    index_.clear_removals();
    text_.set_chain_state({chain_base_gen, chain_coverage, chain_next_seq});
    vec_.set_chain_state({chain_base_gen, chain_coverage, chain_next_seq});
    vec_.clear_delta_log();

    // S14-3：载入成功的段清脏位——此刻该段内存态 == 文件字节，下次 save
    // 可原字节前移。未载入/坏段保持脏（首次 save 重序列化）；fold 尾部
    // 重放随后把真正变过的段重新标脏。bm25 两段由 TextPlugin 反序列化
    // 原语自清（S18-4），此处无需重复。
    if (docmap_loaded) index_.clear_dirty();  // S18-2
    (void)default_sec_ok;
    (void)fields_sec_ok;
    if (hnsw_loaded) vec_.clear_dirty();  // S18-3

    return result;
}

// ============================================================================
// S17-2/S17-3:per-component 持久化（docmap.ckpt / bm25.ckpt / vec.ckpt）
// ============================================================================
// 旧的 save_search_ckpt / load_search_ckpt 仍保留（标 legacy）——S17-5 一
// 次性迁移路径依赖它读旧 search.ckpt。S17-4 之后 Cask 路径只调新接口。
//
// S18-2/3/4：per-component 持久化全部归各持有方——docmap = 宿主
// （index::save_docmap_*/load_docmap）、bm25 = TextPlugin、vec = VectorPlugin。
// 本层只留调度壳（Cask 既有调用面零改动；S18-6 起改为逐插件 flush/open，
// 本壳随 P5 删除）。

SearchLayer::ComponentCkptState
SearchLayer::get_component_state(bitcask::ComponentId comp) const {
    ComponentCkptState s;
    s.rebase_needed = ckpt_rebase_needed_.load(std::memory_order_relaxed);
    switch (comp) {
    case bitcask::ComponentId::kBm25: {
        const auto cs = text_.chain_state();
        s.base_gen = cs.base_gen;
        s.chain_wm = cs.chain_wm;
        s.next_seq = cs.next_seq;
        break;
    }
    case bitcask::ComponentId::kVec: {
        const auto cs = vec_.chain_state();
        s.base_gen = cs.base_gen;
        s.chain_wm = cs.chain_wm;
        s.next_seq = cs.next_seq;
        break;
    }
    case bitcask::ComponentId::kDocmap:
        break;  // S18-2：docmap 链状态归宿主（Cask docmap_chain_），此处零值
    }
    return s;
}

void SearchLayer::set_component_state(bitcask::ComponentId comp,
                                      const ComponentCkptState& st) {
    switch (comp) {
    case bitcask::ComponentId::kBm25:
        text_.set_chain_state({st.base_gen, st.chain_wm, st.next_seq});
        break;
    case bitcask::ComponentId::kVec:
        // S18-3：vec 链状态归 VectorPlugin（顺带同步入账窗口）。
        vec_.set_chain_state({st.base_gen, st.chain_wm, st.next_seq});
        break;
    case bitcask::ComponentId::kDocmap:
        break;  // S18-2：docmap 归宿主
    }
    ckpt_rebase_needed_.store(st.rebase_needed, std::memory_order_relaxed);
}

SearchLayer::ComponentSaveResult SearchLayer::save_components_base(
    std::string_view dir, std::uint64_t watermark,
    std::array<bool, kComponentCount> /*dirty_mask*/) {
    ComponentSaveResult result;
    // docmap（[0]）归宿主 save_docmap_base，恒 false。
    result.wrote_base[1] = text_.save_component_base(dir, watermark);
    result.wrote_base[2] = vec_.save_component_base(dir, watermark);
    // base 落成 → 链坍缩、rebase 标志清零（legacy 全局语义）。
    ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
    return result;
}

SearchLayer::ComponentDeltaResult SearchLayer::save_components_delta(
    std::string_view dir, std::uint64_t watermark,
    std::array<bool, kComponentCount> dirty_mask,
    std::span<const std::byte> keydir_delta) {
    (void)keydir_delta;  // S18-2 起 keydir 段随宿主 docmap delta 落
    ComponentDeltaResult result{};
    if (dirty_mask[1]) {
        auto t = text_.save_component_delta(dir, watermark);
        result.wrote[1] = t.wrote;
        result.new_seqs[1] = t.new_seq;
    }
    if (dirty_mask[2]) {
        auto v = vec_.save_component_delta(dir, watermark);
        result.wrote[2] = v.wrote;
        result.new_seqs[2] = v.new_seq;
    }
    return result;
}

SearchLayer::ComponentLoadResult SearchLayer::load_component(
    bitcask::ComponentId comp, std::string_view dir,
    std::uint64_t expected_base_wm, std::uint32_t chain_seq,
    const DeltaReplayHook& hook) {
    (void)hook;  // S18-2 起 keydir 链重放归宿主 docmap 载入
    ComponentLoadResult result;
    switch (comp) {
    case bitcask::ComponentId::kBm25: {
        auto r = text_.load_component(dir, expected_base_wm, chain_seq);
        result.loaded = r.loaded;
        result.watermark = r.watermark;
        result.from_prev = r.from_prev;
        result.all_segments_ok = r.all_segments_ok;
        break;
    }
    case bitcask::ComponentId::kVec: {
        auto r = vec_.load_component(dir, expected_base_wm, chain_seq);
        result.loaded = r.loaded;
        result.watermark = r.watermark;
        result.from_prev = r.from_prev;
        result.all_segments_ok = r.all_segments_ok;
        break;
    }
    case bitcask::ComponentId::kDocmap:
        break;  // S18-2：归宿主 index::load_docmap（调用方不应走到这里）
    }
    return result;
}

}  // namespace bitcask::search