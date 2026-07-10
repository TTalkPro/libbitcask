// TextPlugin 实现（S18-4）。函数体自 SearchLayer 文本域平移——行为与文件
// 格式逐字节不变，只换持有方：index_ 的读面 → docs_（DocTable）、doc_len
// 回填 → doc_len_writer_（S18-1 窄接口）、压实统计 → stats_（S18-4 窄接口）。

#include "bitcask/text_plugin.hpp"
#include "bitcask/ckpt_chain.hpp"       // S20-2：walk_chain / remove_chain_files
#include "bitcask/search_checkpoint.hpp"
#include "bitcask/text_utils.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

namespace bitcask::text {

namespace sc = bitcask::search;
using search::kDefaultField;
using search::CacheKey;
using search::ReduceJob;
using search::SearchError;
using search::SearchHit;
using search::SearchHitEx;
using search::HighlightOptions;
using search::Snippet;

namespace {

constexpr const char* kBm25CkptName = "bm25.ckpt";

std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kBm25CkptName).string();
}

// S12-2：自动压实的节流下限（与原 SearchLayer 常量一致）。
constexpr std::uint64_t kAutoCompactMinDeaths = 1024;

}  // namespace

TextPlugin::TextPlugin(const TextPluginConfig& config,
                       const bm25::DocTable& docs,
                       bm25::DocLenWriter& doc_len,
                       bm25::CompactionStats& stats)
    : config_(config)
    , docs_(docs)
    , doc_len_writer_(doc_len)
    , stats_(stats)
    , analyzer_(AnalyzerFactory::create(config.analyzer_config))
    , cache_(config.cache_max_entries)
    , doc_texts_(config.doc_text_cache_max)
    , synonym_map_(config.synonym_map) {
    // S27-3 步骤 3:building_ 在构造期即建——fields_ 退役后它是唯一写入
    // 目标,独立用法(shim/单测,不走 plugin open)也必须可写。
    building_.store(std::make_shared<search::SealedSegment>(),
                    std::memory_order_release);
}

TextPlugin::TextPlugin(const TextPluginConfig& config,
                       const bm25::DocTable& docs,
                       bm25::DocLenWriter& doc_len,
                       bm25::CompactionStats& stats,
                       std::unique_ptr<Analyzer> injected_analyzer)
    : TextPlugin(config, docs, doc_len, stats) {
    if (injected_analyzer) analyzer_ = std::move(injected_analyzer);
}

// S27-3 步骤 3:field_index/fields_ 退役(段集是唯一索引载体)。

std::string_view TextPlugin::intern_field_name(std::string_view name) {
    // S10-A4:双检。常态(字段名已 intern)只共享锁;首次出现升级独占 emplace。
    {
        std::shared_lock lk(field_names_intern_mu_);
        auto it = field_names_intern_.find(name);
        if (it != field_names_intern_.end()) return std::string_view(*it);
    }
    std::unique_lock lk(field_names_intern_mu_);
    auto [it, _] = field_names_intern_.emplace(name);
    return std::string_view(*it);
}

void TextPlugin::apply_text(std::string_view key, std::uint64_t ord,
                            std::string_view text) {
    (void)key;
    // docmap 脏位由 Index 自记账（宿主 put_doc/set_doc_len 内部置位）。
    // 默认域倒排仅在真有词项时标脏（向量-only 文档 text 为空，不碰 bm25）。
    auto term_data = analyzer_->analyze_with_positions(text);

    std::uint32_t doc_len = 0;
    // S21-1：借用 term_data 的 key（本函数内存活到 invalidate_terms 之后），
    // 免每词一次 owning string 深拷。
    std::vector<std::string_view> changed_terms;
    changed_terms.reserve(term_data.size());
    for (auto& [term, data] : term_data) {
        doc_len += data.first;
        changed_terms.push_back(term);
    }
    doc_len_writer_.set_doc_len(ord, doc_len);  // S18-1：经窄接口回填

    doc_texts_.put(ord, std::string(text));

    // S27-3 B2b 步骤 3:fields_ 退役——building_/段集是唯一写入目标。
    // 覆盖写先 mark_dead 旧 docid（段级 is_live 不知道宿主覆盖,B2a）。
    // 重放幂等:恢复重放 = 同 key 重加——rebuild_key_locations(步骤 4)
    // 使旧副本可定位,mark_dead 后恒一份 live,无需 ord 显式门。
    auto bld = building_.load(std::memory_order_acquire);
    if (bld && !term_data.empty()) {
        seg_dirty_.store(true, std::memory_order_relaxed);
        // S27-4 P1:LSN 守卫 upsert(任意到达序安全,设计 §2)。
        KeyLocation prior{};
        bool have_prior = false;
        {
            std::shared_lock lk(key_loc_mu_);
            if (auto it = key_to_location_.find(std::string(key));
                it != key_to_location_.end()) {
                prior = it->second;
                have_prior = true;
            }
        }
        if (have_prior && prior.ord > ord) {
            // 更新版本已在(乱序到达的旧 put)→ 本版本不索引。
            cache_.invalidate_terms(changed_terms);
            return;
        }
        if (have_prior && !prior.tomb && prior.seg) {
            (void)prior.seg->mark_dead(prior.docid);
        }
        const TermPositionsMap* borrowed = &term_data;
        const search::SealedSegment::FieldInput fin{search::kDefaultField,
                                                     borrowed};
        const DocId docid = bld->add(
            std::string(key), ord,
            std::span<const search::SealedSegment::FieldInput>(&fin, 1),
            doc_len);
        {
            std::unique_lock lk(key_loc_mu_);
            auto& e = key_to_location_[std::string(key)];
            if (e.ord <= ord) e = KeyLocation{bld, docid, ord, false};
        }
        if (bld->doc_count() >= kBuildingFlushDocThreshold) {
            flush_building();
        }
    }

    // S9.2：只失效查询词与本文档词集有交集的缓存条目。
    cache_.invalidate_terms(changed_terms);
    maybe_auto_compact();  // S12-2
}

// S6 索引流水线的 **Map 阶段**（设计稿 §3）。**纯 const 函数**：只读
// analyzer_（const 配置态，cppjieba Cut 亦 const 线程安全），对每个字段跑
// NFKC + 分词 + 位置，**不碰任何共享可变态** → 可在 N 个 map worker 上对不同
// 文档**并发**调用（F7 不变量，TSan 已验证）。产出 owning `ReduceJob`，由
// reducer 线程按 ord 序串行 apply。catch-all 合并见下方注释。
ReduceJob TextPlugin::map_analyze(
    std::string_view key, std::uint64_t ord,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::uint32_t file_id, std::uint64_t offset,
    std::uint32_t total_sz, std::uint32_t tstamp) const {
    ReduceJob job;
    job.key      = std::string(key);
    job.ord      = ord;
    job.file_id  = file_id;
    job.offset   = offset;
    job.total_sz = total_sz;
    job.tstamp   = tstamp;

    // catch-all（S8.6 + O5）：把非默认字段词项合并进默认字段，使
    // search_text/phrase/near（只查默认字段）也能命中多字段文档。O5：直接
    // 合并各字段分词结果（免整体重新分词），position 按字段顺序平移
    // ca_pos_base。字段内相对位置不变（phrase/near 字段内语义不变）；跨字段
    // 间隔取「字段最大 position + 1」。
    TermPositionsMap ca_data;
    std::uint32_t ca_pos_base = 0;
    job.fields.reserve(fields.size());  // S26-3b：免逐字段扩容

    for (auto& [fname, ftext] : fields) {
        const std::string_view field = fname.empty() ? kDefaultField : fname;
        auto term_data = analyzer_->analyze_with_positions(ftext);
        std::uint32_t flen = 0;
        for (auto& [_, data] : term_data) flen += data.first;

        const bool is_default = (field == kDefaultField);

        if (is_default && !config_.index_catch_all) {
            // catch-all 关闭:kDefaultField 走直接写入(保持 S26-2 语义)
            job.wrote_default = true;
        } else if (config_.index_catch_all && !term_data.empty()) {
            // S28-1 + S8.6:catch-all 合并(kDefaultField + 命名字段统一进 ca_data)。
            // S28-1 修复:catch-all 开启时 kDefaultField 词项(doc.text)也合并进
            // ca_data → apply_job_impl 单次 add_doc(kDefaultField, ca_data) 写入
            // 全部词项(doc.text + 命名字段),避免水位幂等保护冲突。
            std::uint32_t field_max_pos = 0;
            for (auto& [term, data] : term_data) {
                auto& [tf, positions] = data;
                auto& [ca_tf, ca_positions] = ca_data[term];
                ca_tf += tf;
                for (auto p : positions) {
                    ca_positions.push_back(p + ca_pos_base);
                    if (p > field_max_pos) field_max_pos = p;
                }
            }
            job.ca_len += flen;
            ca_pos_base += field_max_pos + 1;
        }

        // S28-1:catch-all 开启时 kDefaultField 不 push job.fields(catch-all
        // 统一写 ca_data);否则 push(命名字段直接写入 + catch-all 关闭时
        // kDefaultField 直接写入)。
        if (!(is_default && config_.index_catch_all)) {
            job.fields.push_back(ReduceJob::FieldResult{
                std::string(field), std::move(term_data), flen});
        }
        job.total_doc_len += flen;
    }

    job.ca_data = std::move(ca_data);
    // 高亮：默认字段原文（多字段高亮的精细化留待后续）。
    // S26-3a：高亮 LRU 关闭（doc_text_cache_max==0，apply_job_impl put 恒丢弃）
    // 时跳过整段正文深拷（O(V)/doc）——config 语义本就是「0 → 无高亮片段」。
    job.doc_text = (config_.doc_text_cache_max == 0 || fields.empty())
                       ? std::string{}
                       : std::string(fields.front().second);
    return job;
}

// Reduce 阶段的 BM25 半边（原 reduce_apply 去掉 on_vector；向量半边归
// VectorPlugin，由 SearchLayer shim / S18-5 起的宿主扇出编排）。reducer
// 单写者、严格 ord 序（reorder buffer 保证）。
// 步骤：① 侧表 ord_field_lens_ 记字段长 ② 各字段 add_doc 进倒排 ③ catch-all
// 合并默认字段 ④ doc_len 回填（docmap 行本体与 meta 由宿主先落）⑤ 高亮原文
// ⑥ 失效查询缓存。
// S23-M4：双入口共享 impl——生产流水线（on_put 持有可变 TextPrepared::job）
// 走非 const 版把 doc_text move 进原文 LRU（每文档省一次全文深拷）；shim/
// 空 job 降级走 const 版，仅 doc_text 付一次拷贝，其余行为零差异。
void TextPlugin::apply_job(ReduceJob& job) {
    apply_job_impl(job, std::move(job.doc_text));
}

void TextPlugin::apply_job(const ReduceJob& job) {
    apply_job_impl(job, std::string(job.doc_text));
}

void TextPlugin::apply_job_impl(const ReduceJob& job, std::string&& doc_text) {
    // S6-P2: 空 job 守卫（prepare 抛异常时 adapter 送来空 job）。
    // key+fields 都空 = map_analyze 未产出，跳过 apply；reducer 仍推进 ord。
    if (job.key.empty() && job.fields.empty()) return;
    // S27-3 B2b 步骤 3:fields_/ord_field_lens_ 退役——building_/段集是唯一
    // 写入目标(倒排 + 字段统计都在段内 InvertedIndex 自含)。

    // S16-2：docmap 行与 meta 由宿主（流水线）/caller（standalone・recover）
    // 先落；分析产物 doc_len 在此回填（宿主不做分析拿不到 token 数）。
    doc_len_writer_.set_doc_len(job.ord, job.total_doc_len);
    if (!doc_text.empty()) {
        doc_texts_.put(job.ord, std::move(doc_text));
    }
    // S13-P1：S9.2 选择性失效——job 已物化本文档全部词集（各字段 terms +
    // catch-all）。BM25 全局统计漂移是 S9.2 已接受的 near-real-time 近似。
    // S21-1：借用 job 的 term key（job 存活覆盖本调用），免 owning 深拷。
    std::vector<std::string_view> changed_terms;
    {
        std::size_t est = job.ca_data.size();
        for (const auto& f : job.fields) est += f.terms.size();
        changed_terms.reserve(est);
    }
    for (const auto& f : job.fields) {
        for (const auto& [term, data] : f.terms) changed_terms.push_back(term);
    }
    for (const auto& [term, data] : job.ca_data) changed_terms.push_back(term);

    // S27-3 步骤 3:唯一写入路径(原 B1「镜像」转正)+ B2a:覆盖写先 mark_dead。
    auto bld = building_.load(std::memory_order_acquire);
    if (bld) {
        seg_dirty_.store(true, std::memory_order_relaxed);
        // S27-4 P1:LSN 守卫 upsert(同 apply_text,设计 §2)。
        KeyLocation prior{};
        bool have_prior = false;
        {
            std::shared_lock lk(key_loc_mu_);
            if (auto it = key_to_location_.find(std::string(job.key));
                it != key_to_location_.end()) {
                prior = it->second;
                have_prior = true;
            }
        }
        if (have_prior && prior.ord > job.ord) {
            cache_.invalidate_terms(changed_terms);
            return;  // 乱序到达的旧版本,不索引
        }
        if (have_prior && !prior.tomb && prior.seg) {
            (void)prior.seg->mark_dead(prior.docid);
        }
        std::vector<search::SealedSegment::FieldInput> fin;
        fin.reserve(job.fields.size() + 1);
        for (const auto& f : job.fields) {
            if (!f.terms.empty()) {
                fin.push_back({intern_field_name(f.field_name), &f.terms});
            }
        }
        if (!job.wrote_default && !job.ca_data.empty()) {
            fin.push_back({search::kDefaultField, &job.ca_data});
        }
        if (!fin.empty()) {
            const DocId docid = bld->add(std::string(job.key), job.ord,
                                          fin, job.total_doc_len);
            {
                std::unique_lock lk(key_loc_mu_);
                auto& e = key_to_location_[std::string(job.key)];
                if (e.ord <= job.ord) e = KeyLocation{bld, docid, job.ord, false};
            }
            if (bld->doc_count() >= kBuildingFlushDocThreshold) {
                flush_building();
            }
        }
    }

    cache_.invalidate_terms(changed_terms);
    maybe_auto_compact();  // S12-2：本 reducer 线程内，达阈值则压实死 posting
}

void TextPlugin::on_delete(std::string_view key, std::uint64_t tomb_ord,
                           std::uint64_t prior_ord) {
    (void)key;
    (void)tomb_ord;
    // 前置条件：docmap 行已删（宿主或 SearchLayer 二参版）。doc_len 经
    // prior_ord 从 SoA 读取——Index::remove 只翻 live/ext2ord，不清 SoA，
    // 删除后仍可读（S16-2 侦查坐实）。
    (void)prior_ord;
    // S27-3 步骤 3:fields_ 退役——删除只走段级 mark_dead(下方);段内
    // N/sum_dl 统计经 LiveChecker 排除死文档,df 高估由 merge 自愈(§3.4)。
    seg_dirty_.store(true, std::memory_order_relaxed);

    // S9.2：取被删文档词集做选择性失效。原文 LRU 命中则精确 analyze；
    // miss（冷文档被挤出）则降级为整缓存失效（安全但粗粒度）。
    // S13-P8.5：缓存本就为空时跳过整个「LRU 拷原文 + NFKC + 分词」。
    // S21-1：changed_terms 借用 tf 的 key——tf 须提出块外，存活覆盖下方
    // invalidate_terms 调用（块内声明会让 view 悬垂）。
    text::TermFreqMap tf;
    std::vector<std::string_view> changed_terms;
    bool have_terms = false;
    if (cache_.size() > 0) {
        auto text = doc_texts_.get(prior_ord);  // 拷贝(C1:并发安全)
        if (text) {
            tf = analyzer_->analyze(*text);
            changed_terms.reserve(tf.size());
            for (auto& [term, _] : tf) changed_terms.push_back(term);
            have_terms = true;
        }
    } else {
        have_terms = true;  // 空缓存：invalidate_terms(空) 即 no-op，语义等价
    }

    doc_texts_.erase(prior_ord);
    if (have_terms) {
        cache_.invalidate_terms(changed_terms);  // 空缓存时为 no-op
    } else {
        cache_.invalidate();  // 原文 LRU miss：降级整缓存失效（S9.2）
    }

    // S27-3 Slice B1：段级删除镜像（fields_ remove_doc 上面已做）。
    // S27-4 P1:墓碑保留 + LSN 守卫(erase 会让乱序到达的旧 put 复活;
    // 墓碑条目重启随 rebuild_key_locations 消失,设计 §2)。
    KeyLocation prior{};
    bool have_prior = false;
    {
        std::unique_lock lk(key_loc_mu_);
        auto& e = key_to_location_[std::string(key)];
        if (e.ord > tomb_ord) return;  // 已有更新版本(put/墓碑),本删除过期
        prior = e;
        have_prior = (e.seg != nullptr || e.tomb);
        e = KeyLocation{nullptr, 0, tomb_ord, true};
    }
    if (have_prior && !prior.tomb && prior.seg) {
        (void)prior.seg->mark_dead(prior.docid);
    }

    maybe_auto_compact();          // S12-2(内联模式)
    maybe_auto_compact_reducer();  // S27-4 P2(builder 模式;on_delete 恒在 reducer)
}

void TextPlugin::rebuild_index(
    const std::function<void(
        const std::function<void(std::uint64_t, const std::string&)>&)>&
        for_each_doc) {
    // S27-3 步骤 3:重建目标从 fields_ 换为段世界——丢弃全部旧段(文件在
    // 下一次 checkpoint commit 时删)+ 全新 building_,回调喂入的文档进
    // building_(key 经 docmap ord→ext 反查——回调只给 (ord, text))。
    rebase_needed_.store(true, std::memory_order_relaxed);  // S18-6/S14-4
    doc_texts_.clear();
    {
        std::unique_lock lk(key_loc_mu_);
        key_to_location_.clear();
    }
    if (segment_set_) {
        std::vector<std::uint64_t> ids;
        for (const auto& e : segment_set_->entries_view()) ids.push_back(e.seg_id);
        for (auto id : ids) (void)segment_set_->drop_pending(id);
    }
    building_.store(std::make_shared<search::SealedSegment>(),
                    std::memory_order_release);
    seg_dirty_.store(true, std::memory_order_relaxed);

    for_each_doc([&](std::uint64_t ord, const std::string& text) {
        auto term_data = analyzer_->analyze_with_positions(text);
        if (term_data.empty()) return;
        auto key = docs_.ord_to_ext(ord);
        if (!key) return;  // docmap 无行(理论不可达:caller 遍历 docmap)
        std::uint32_t doc_len = 0;
        for (const auto& [t, d] : term_data) doc_len += d.first;
        const TermPositionsMap* borrowed = &term_data;
        const search::SealedSegment::FieldInput fin{search::kDefaultField,
                                                     borrowed};
        auto bld = building_.load(std::memory_order_acquire);
        const DocId docid = bld->add(
            *key, ord,
            std::span<const search::SealedSegment::FieldInput>(&fin, 1),
            doc_len);
        {
            std::unique_lock lk(key_loc_mu_);
            key_to_location_[*key] = KeyLocation{bld, docid, ord, false};
        }
        doc_texts_.put(ord, text);
        if (bld->doc_count() >= kBuildingFlushDocThreshold) {
            flush_building();
        }
    });

    cache_.invalidate();
}

// S27-3 步骤 5:段世界压实——① building 原地删死 posting;② 全死段整段
// drop(posting 全量回收;文件在下一次 checkpoint commit 删);③ 带死段原地
// 压实(dead_dirty → 下次 checkpoint 重存)。跨段合并(Lucene consolidation,
// 段数收敛)与 legacy 段化迁移留待 S27-4 era(需 InvertedIndex 词表遍历原语)。
// 调用契约:reducer 静止点(compact 遍历 tbb map 与并发 add_doc 不兼容——
// on_merge_commit 经 run_serialized、auto 路径在 apply 内,均满足)。
std::size_t TextPlugin::compact(double dead_ratio_threshold) {
    std::size_t total = 0;
    if (auto bld = building_.load(std::memory_order_acquire)) {
        total += bld->compact_postings(dead_ratio_threshold);
    }
    if (segment_set_) {
        std::vector<std::uint64_t> dead_ids;
        const auto entries = segment_set_->entries_view();
        const auto segs = segment_set_->segments_view();  // reducer 上下文
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (segs[i]->live_doc_count() == 0) {
                total += segs[i]->inverted().total_postings();
                dead_ids.push_back(entries[i].seg_id);
            } else {
                total += segs[i]->compact_postings(dead_ratio_threshold);
            }
        }
        for (auto id : dead_ids) {
            (void)segment_set_->drop_pending(id);  // 清单/文件随下次 ckpt commit
        }
    }
    if (total > 0) {
        rebase_needed_.store(true, std::memory_order_relaxed);
        seg_dirty_.store(true, std::memory_order_relaxed);
        cache_.invalidate();
    }
    return total;
}

std::size_t TextPlugin::total_postings() const {
    // 步骤 3:统计源换段集 + building(默认字段口径,与旧 fields_ 相当)。
    std::size_t n = 0;
    if (segment_set_) {
        for (const auto& s : segment_set_->segments_view()) {
            n += s->inverted().total_postings();
        }
    }
    if (auto bld = building_.load(std::memory_order_acquire)) {
        n += bld->inverted().total_postings();
    }
    return n;
}

void TextPlugin::maybe_auto_compact() {
    // S27-3 步骤 5:复活(S12-2 原节流逻辑,目标换段世界 compact)。
    // S27-4 P2:builder 模式下本函数在 builder 线程被调 → 必须 no-op
    // (compact 遍历他人 building 的 tbb map 与并发 add 不兼容);压实
    // 触发改由 reducer 侧 maybe_auto_compact_reducer 承担。
    if (!builders_.empty()) return;
    const double thr = config_.auto_compact_dead_ratio;
    if (thr <= 0.0) return;  // opt-in 关（默认）→ 零开销

    const std::uint64_t retired = stats_.retired_since_compact();
    if (retired < kAutoCompactMinDeaths) return;      // 常态早退
    if (retired < stats_.live_docs() / 2) return;     // 随 live 规模缩放

    compact(thr);  // reducer 线程内串行 → 与 add_doc/put_doc 无并发窗口
    stats_.reset_retired_since_compact();
}

// ---- 查询面（自 SearchLayer 平移，S18-4）----

// D2：bm25 结果集 → SearchHit 物化骨架。filter 非空时按 meta_blob 后过滤；
// k>0 时截断。S27-3 Slice B2a：加全局 is_live 兜底（段级 is_live 不知道宿主墓碑）。
std::vector<SearchHit> TextPlugin::materialize_hits(
    const std::vector<bm25::SearchResult>& results,
    const bm25::DocTable& doc_table,
    const meta::MetaFilter* filter, std::size_t k) const {
    std::vector<SearchHit> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        if (!doc_table.is_live(r.ord)) continue;  // B2a：全局兜底（S18-8 段级盲区）
        if (filter) {
            if (!doc_table.eval_meta(r.ord, *filter)) continue;
        }
        auto ext_id = doc_table.ord_to_ext(r.ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), r.ord, r.score});
    }
    if (k > 0 && hits.size() > k) hits.resize(k);
    return hits;
}

// ---- S27-3 Slice B2a：[SegmentSet + Building] 多段视图收集 ----
std::vector<search::SegmentView>
TextPlugin::collect_default_segment_views() const {
    // S27-3 步骤 5:查询侧走 snapshot(shared 锁下拷 shared_ptr)+ pin——
    // 段封口(building 切换)/段压实 drop 与查询并发,对象由 pin 续命。
    std::vector<search::SegmentView> views;
    if (segment_set_) {
        auto snap = segment_set_->snapshot();
        views.reserve(snap.size() + 1);
        for (auto& sp : snap) {
            const search::SealedSegment* seg = sp.get();
            views.push_back(search::SegmentView{
                &seg->inverted(), seg,
                [seg](DocId d) -> const std::string& { return seg->key_at(d); },
                [seg](DocId d) -> Lsn { return seg->lsn_at(d); },
                sp});
        }
    }
    if (auto bld = building_.load(std::memory_order_acquire);
        bld && bld->doc_count() > 0) {
        const search::SealedSegment* seg = bld.get();
        views.push_back(search::SegmentView{
            &seg->inverted(), seg,
            [seg](DocId d) -> const std::string& { return seg->key_at(d); },
            [seg](DocId d) -> Lsn { return seg->lsn_at(d); },
            std::move(bld)});
    }
    return views;
}

std::vector<search::MultiFieldSegmentView>
TextPlugin::collect_multi_field_segment_views() const {
    // S27-3 步骤 5:同 collect_default_segment_views——snapshot + pin。
    std::vector<search::MultiFieldSegmentView> views;
    if (segment_set_) {
        for (auto& sp : segment_set_->snapshot()) {
            auto v = sp->multi_view();
            v.pin = sp;
            views.push_back(std::move(v));
        }
    }
    if (auto bld = building_.load(std::memory_order_acquire);
        bld && bld->doc_count() > 0) {
        auto v = bld->multi_view();
        v.pin = std::move(bld);
        views.push_back(std::move(v));
    }
    return views;
}

// D2：phrase/near 共用——analyze_with_positions 还原 query 词序。
std::vector<std::string> TextPlugin::ordered_query_terms(
    std::string_view query) const {
    auto tpm = analyzer_->analyze_with_positions(query);
    std::vector<std::pair<std::uint32_t, std::string>> ordered;  // (position, term)
    for (auto& [term, data] : tpm) {
        for (auto pos : data.second) ordered.push_back({pos, term});
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> terms;
    terms.reserve(ordered.size());
    for (auto& [_, term] : ordered) terms.push_back(std::move(term));
    return terms;
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_text(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override,
                         const meta::MetaFilter* filter) const {
    // S10-A1:缓存检查前置以跳过 ~20µs NLP analyze（详见 TASK.md）。
    // 安全前提:CacheKey 仅依赖 (query_type, query, k_req),不依赖 analyze 结果。
    if (query.empty()) return std::vector<SearchHit>{};

    // V5:filter 非空时 overfetch K'=max(k×4, 64)——BM25 评分排序在
    // filter 之前,过严 filter 命中数 < k 时需更多候选弥补损耗。无 filter
    // 仍按 k 请求(避免无谓放大,保持兼容)。
    const std::size_t k_req = filter ? std::max<std::size_t>(k * 4, 64) : k;

    auto cache_key = CacheKey::make("text", query, k_req);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        auto term_freqs = analyzer_->analyze(query);
        if (term_freqs.empty()) return std::vector<SearchHit>{};

        std::vector<std::string> terms;
        terms.reserve(term_freqs.size());
        for (auto& [term, _] : term_freqs) {
            terms.push_back(term);
        }
        if (synonym_map_) {
            terms = synonym_map_->expand_terms(terms);
        }

        // S27-3 Slice B2a：走 [SegmentSet + Building] 多段 G-on-the-fly 归并。
        auto views = collect_default_segment_views();
        if (!views.empty()) {
            const auto hits = search::multi_segment_search(
                views, terms, k_req, params_override);
            results.reserve(hits.size());
            for (const auto& h : hits) {
                results.push_back({h.ord, static_cast<float>(h.score)});
            }
            // S29-5 评估后保留：非冗余排序——multi_segment_search 返回并列
            // 以 key 升序，此处重排为 ord 降序，对齐单索引路径（score_bow_topk
            // 堆序 → 并列 ord 降序）与缓存语义；≤k_req 个元素，成本可忽略。
            std::sort(results.begin(), results.end(),
                      [](const bm25::SearchResult& a,
                         const bm25::SearchResult& b) {
                          if (a.score != b.score) return a.score > b.score;
                          return a.ord > b.ord;
                      });
        }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    // D2：filter 后过滤（空 meta 不通过）+ overfetch 后截断到 k。
    return materialize_hits(results, docs_, filter, k);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_phrase(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    // S10-A1:缓存检查前置（同 search_text）。
    if (query.empty()) return std::vector<SearchHit>{};

    auto cache_key = CacheKey::make("phrase", query, k);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        // S9.28：短语匹配依赖查询词序——用 analyze_with_positions 还原（D2 helper）。
        auto terms = ordered_query_terms(query);
        if (terms.empty()) return std::vector<SearchHit>{};

        // S27-3 Slice B2a：走 [SegmentSet + Building] 逐段并集。
        auto views = collect_default_segment_views();
        if (!views.empty()) {
            for (const auto& s : views) {
                auto seg_hits = s.inv->search_phrase(terms, k, *s.live, params_override);
                for (auto& h : seg_hits) {
                    results.push_back({s.lsn_of(h.ord), h.score});
                }
            }
            std::sort(results.begin(), results.end(),
                      [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                          if (a.score != b.score) return a.score > b.score;
                          return a.ord > b.ord;
                      });
            if (results.size() > k) results.resize(k);
        }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)
        if (!params_override) cache_.put(cache_key, results, terms);
    }

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    // 近邻依赖查询词序——用 analyze_with_positions 还原（D2 helper，同 phrase）。
    auto terms = ordered_query_terms(query);
    if (terms.empty()) return std::vector<SearchHit>{};

    std::vector<bm25::SearchResult> results;
    // S27-3 Slice B2a：走 [SegmentSet + Building] 逐段并集。
    auto views = collect_default_segment_views();
    if (!views.empty()) {
        for (const auto& s : views) {
            auto seg_hits = s.inv->search_near(terms, k, slop, *s.live, params_override);
            for (auto& h : seg_hits) {
                results.push_back({s.lsn_of(h.ord), h.score});
            }
        }
        std::sort(results.begin(), results.end(),
                  [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.ord > b.ord;
                  });
        if (results.size() > k) results.resize(k);
    }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance,
                          const bm25::Bm25Params* params_override) const {
    auto term_freqs = analyzer_->analyze(query);
    if (term_freqs.empty()) return std::vector<SearchHit>{};

    std::vector<std::string> terms;
    terms.reserve(term_freqs.size());
    for (auto& [term, _] : term_freqs) {
        terms.push_back(term);
    }

    std::vector<bm25::SearchResult> results;
    // S27-3 Slice B2a：走 [SegmentSet + Building] 逐段并集。
    auto views = collect_default_segment_views();
    if (!views.empty()) {
        for (const auto& s : views) {
            auto seg_hits = s.inv->search_fuzzy(terms, k, max_edit_distance, *s.live, params_override);
            for (auto& h : seg_hits) {
                results.push_back({s.lsn_of(h.ord), h.score});
            }
        }
        std::sort(results.begin(), results.end(),
                  [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.ord > b.ord;
                  });
        if (results.size() > k) results.resize(k);
    }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::bool_search(std::string_view query, std::size_t k,
                         const bm25::Bm25Params* params_override) const {
    // S10-A1:缓存检查前置（同 search_text）。
    if (query.empty()) return std::vector<SearchHit>{};

    auto cache_key = CacheKey::make("bool", query, k);
    auto cached = params_override
                      ? std::optional<std::vector<bm25::SearchResult>>{}
                      : cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        // S13-D9：含 '(' 或 '"' 的查询走树路径（递归下降 + 集合求值）；
        // 其余仍走扁平路径——既有查询行为位级不变。
        const bool tree_syntax =
            query.find('(') != std::string_view::npos ||
            query.find('"') != std::string_view::npos;
        auto query_node = tree_syntax ? bitcask::bm25::parse_query_tree(query)
                                      : bitcask::bm25::parse_query(query);
        if (query_node.term.empty() && query_node.children.empty()) {
            return std::vector<SearchHit>{};
        }
        if (tree_syntax) {
            // 短语叶子：analyzer 切词填 phrase_terms（有序）。
            std::function<void(bm25::QueryNode&)> fill =
                [&](bm25::QueryNode& node) {
                if (node.is_phrase) {
                    node.phrase_terms = ordered_query_terms(node.term);
                    return;
                }
                for (auto& c : node.children) fill(c);
            };
            fill(query_node);
        }

        // S27-3 Slice B2a：走 [SegmentSet + Building] 逐段并集。
        auto views = collect_default_segment_views();
        if (!views.empty()) {
            for (const auto& s : views) {
                auto seg_hits = tree_syntax
                                    ? s.inv->bool_search_tree(query_node, k, *s.live, params_override)
                                    : s.inv->bool_search(query_node, k, *s.live, params_override);
                for (auto& h : seg_hits) {
                    results.push_back({s.lsn_of(h.ord), h.score});
                }
            }
            std::sort(results.begin(), results.end(),
                      [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                          if (a.score != b.score) return a.score > b.score;
                          return a.ord > b.ord;
                      });
            if (results.size() > k) results.resize(k);
        }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)
        if (!params_override && !results.empty()) {
            // 收集 MUST/SHOULD/MUST_NOT 全部叶子词，作为该缓存条目的词集。
            std::vector<std::string> must, should, must_not;
            bm25::collect_terms(query_node, must, should, must_not);
            std::vector<std::string> terms = std::move(must);
            terms.insert(terms.end(), should.begin(), should.end());
            terms.insert(terms.end(), must_not.begin(), must_not.end());
            cache_.put(cache_key, results, std::move(terms));
        }
    }

    return materialize_hits(results, docs_);
}

std::optional<bm25::ScoreExplanation>
TextPlugin::explain(std::string_view query, std::string_view key,
                     const bm25::Bm25Params* params_override) const {
    auto ord = docs_.ord_of(key);
    if (!ord) return std::nullopt;

    auto term_freqs = analyzer_->analyze(query);
    std::vector<std::string> terms;
    terms.reserve(term_freqs.size());
    for (auto& [term, _] : term_freqs) terms.push_back(term);

    // S27-3 Slice B2a：段级 explain（用 key_to_location_ 找段，段内 LiveChecker）。
    // drain_plugins 提供 HB（reducer 写 key_to_location_ 先于查询读）。
    // S27-3 步骤 5:key_loc_mu_ shared 下取定位快照 + shared_ptr 钉住段
    // 对象(查询线程 vs reducer 写表/封口/drop 全并发安全)。
    KeyLocation loc{};
    bool have_loc = false;
    {
        std::shared_lock lk(key_loc_mu_);
        if (auto it = key_to_location_.find(std::string(key));
            it != key_to_location_.end()) {
            loc = it->second;
            have_loc = true;
        }
    }
    if (have_loc && !loc.tomb && loc.seg) {
        // S27-4 P1:loc.seg 即段对象 shared_ptr(天然 pin)。
        const auto& seg = *loc.seg;
        return seg.inverted().explain(terms, loc.docid, seg, params_override);
    }
    // S27-3 步骤 3:fields_ fallback 删除——key_to_location_ 由 recovery
    // 重建(步骤 4),未命中即该 key 无文本索引。
    return bm25::ScoreExplanation{};
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_wildcard(std::string_view pattern, std::size_t k,
                             const bm25::Bm25Params* params_override) const {
    std::vector<bm25::SearchResult> results;
    // S27-3 Slice B2a：走 [SegmentSet + Building] 逐段并集。
    auto views = collect_default_segment_views();
    if (!views.empty()) {
        const std::string pat(pattern);
        for (const auto& s : views) {
            auto seg_hits = s.inv->search_wildcard(pat, k, *s.live, params_override);
            for (auto& h : seg_hits) {
                results.push_back({s.lsn_of(h.ord), h.score});
            }
        }
        std::sort(results.begin(), results.end(),
                  [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.ord > b.ord;
                  });
        if (results.size() > k) results.resize(k);
    }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)

    return materialize_hits(results, docs_);
}

std::expected<std::vector<SearchHit>, SearchError>
TextPlugin::search_fields(std::string_view query, std::size_t k,
                           const bm25::Bm25Params* params_override) const {
    auto qnode = bitcask::bm25::parse_query(query);

    std::vector<const bm25::QueryNode*> leaves;
    std::function<void(const bm25::QueryNode&)> walk = [&](const bm25::QueryNode& n) {
        if (!n.term.empty()) { leaves.push_back(&n); return; }
        for (auto& c : n.children) walk(c);
    };
    walk(qnode);
    if (leaves.empty()) return std::vector<SearchHit>{};

    struct FieldQuery { std::vector<std::string> terms; float boost; };
    std::unordered_map<std::string, std::vector<std::pair<std::string,float>>> by_field;
    for (auto* leaf : leaves) {
        std::string field = leaf->field.empty() ? std::string(kDefaultField) : leaf->field;
        auto tf = analyzer_->analyze(leaf->term);
        for (auto& [norm_term, _] : tf) {
            by_field[field].push_back({norm_term, leaf->boost});
        }
    }

    // S27-3 Slice B2a：逐段逐字段 + boost 累加（同 fields_ 逻辑，换段集源）。
    std::unordered_map<std::uint64_t, double> acc;
    auto seg_views = collect_multi_field_segment_views();
    if (!seg_views.empty()) {
        for (const auto& sv : seg_views) {
            for (const auto& fv : sv.fields) {
                auto fbi = by_field.find(std::string(fv.field_name));
                if (fbi == by_field.end()) continue;
                std::map<float, std::vector<std::string>> boost_groups;
                for (auto& [t, boost] : fbi->second) {
                    auto expanded = synonym_map_ ? synonym_map_->expand(t)
                                                 : std::span<const std::string>{};
                    if (expanded.empty()) expanded = {&t, 1};
                    auto& g = boost_groups[boost];
                    g.insert(g.end(), expanded.begin(), expanded.end());
                }
                for (auto& [boost, gterms] : boost_groups) {
                    auto res = fv.inv->search(gterms, k, *sv.seg, params_override);
                    for (auto& r : res) {
                        acc[sv.seg->lsn_at(r.ord)] += static_cast<double>(r.score) * boost;
                    }
                }
            }
        }
    }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)

    std::vector<std::pair<std::uint64_t,double>> ranked(acc.begin(), acc.end());
    std::partial_sort(ranked.begin(),
                      ranked.begin() +
                          static_cast<std::ptrdiff_t>(std::min(k, ranked.size())),
                      ranked.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    if (ranked.size() > k) ranked.resize(k);

    std::vector<SearchHit> hits;
    hits.reserve(ranked.size());
    for (auto& [ord, score] : ranked) {
        if (!docs_.is_live(ord)) continue;  // B2a：全局兜底（S18-8 段级盲区）
        auto ext_id = docs_.ord_to_ext(ord);
        if (!ext_id) continue;
        hits.push_back(SearchHit{std::move(*ext_id), ord, score});
    }
    return hits;
}

std::expected<std::vector<SearchHitEx>, SearchError>
TextPlugin::search_text_highlight(std::string_view query, std::size_t k,
                                   const HighlightOptions& opts) const {
    auto term_freqs = analyzer_->analyze(query);
    if (term_freqs.empty()) return std::vector<SearchHitEx>{};

    auto cache_key = CacheKey::make("highlight", query, k);
    auto cached = cache_.get(cache_key);

    std::vector<bm25::SearchResult> results;
    if (cached) {
        results = std::move(*cached);
    } else {
        std::vector<std::string> terms;
        terms.reserve(term_freqs.size());
        for (auto& [term, _] : term_freqs) {
            terms.push_back(term);
        }

        // S27-3 Slice B2a：走 [SegmentSet + Building] 多段归并（同 search_text）。
        auto views = collect_default_segment_views();
        if (!views.empty()) {
            const auto hh = search::multi_segment_search(views, terms, k);
            results.reserve(hh.size());
            for (const auto& h : hh) {
                results.push_back({h.ord, static_cast<float>(h.score)});
            }
            std::sort(results.begin(), results.end(),
                      [](const bm25::SearchResult& a, const bm25::SearchResult& b) {
                          if (a.score != b.score) return a.score > b.score;
                          return a.ord > b.ord;
                      });
        }  // S27-3 步骤 3:fields_ 回退删除(段集唯一源)
        cache_.put(cache_key, results, terms);
    }

    std::vector<SearchHitEx> hits;
    hits.reserve(results.size());
    for (auto& r : results) {
        auto ext_id = docs_.ord_to_ext(r.ord);
        if (!ext_id) continue;

        // S9.3：原文 LRU 命中才生成高亮片段；冷文档被挤出（miss）时降级为
        // 无片段的 hit，而非整条丢弃——保证结果集不因 LRU 容量而缩水。
        auto doc_text = doc_texts_.get(r.ord);  // 拷贝(C1)
        std::vector<Snippet> snippets;
        if (doc_text) {
            // S9.19：analyze_with_offsets 产出的 byte offset 相对「归一化文本」，
            // 故 highlight 也必须在归一化文本上切片，否则非规范文本（全角/组合
            // 字符等）会因坐标系不一致切出乱码。NFKC 幂等，传 norm 再归一化无害。
            std::string norm = text::detail::nfkc_fold(*doc_text);
            auto token_offsets = analyzer_->analyze_with_offsets(norm);
            std::unordered_map<std::string, std::vector<text::TokenInfo>> query_token_offsets;
            for (auto& [term, _] : term_freqs) {
                auto it_token = token_offsets.find(term);
                if (it_token != token_offsets.end()) {
                    query_token_offsets[term] = it_token->second;
                }
            }
            auto hl_result = highlight(norm, query_token_offsets, opts);
            snippets = std::move(hl_result.snippets);
        }

        hits.push_back(SearchHitEx{
            std::move(*ext_id),
            r.ord,
            r.score,
            std::move(snippets)
        });
    }
    return hits;
}


// ---- ckpt 原语（legacy 统一容器与 bm25 组件共用；S18-4）----
// 小端字节编码统一用 sc::detail::put_u*（S20-1 R7：删除本文件的转发层）。

// S27-3 步骤 3:fields_ 序列化/反序列化/delta 家族退役——bm25.ckpt 只剩
// kSegManifest(段自含 kBm25Default/kSegFields/kSegDocStore,见 segment.hpp)。

// ---- bm25 组件 checkpoint（bm25.ckpt 文件族；S17 格式不变）----

bool TextPlugin::save_component_base(std::string_view dir,
                                     std::uint64_t watermark) {
    const std::string fp = comp_path(dir);
    const std::string prev = fp + ".prev";
    std::error_code ec;
    if (std::filesystem::exists(fp, ec)) {
        std::filesystem::rename(fp, prev, ec);
    }
    sc::SectionWriter sw;  // S20-1 R4
    // S27-3 步骤 3:kBm25Default/kBm25Fields 退役——倒排数据在各段文件内
    // 自含,bm25.ckpt 只承载段清单(kSegManifest)。
    // S27-3 B2b 步骤 1:段清单进 bm25.ckpt(kSegManifest,recovery 重写
    // 步骤 4 的读取源;先于 index.manifest 提交,单一 commit point 不变量
    // 见设计 §4.1)。过渡期仍提交 segments.manifest(SegmentSet::open 兼容,
    // 步骤 4 后退役)。commit 失败 → 本次 base 保存失败(盘错误,flush 上报)。
    if (segment_set_) {
        // 步骤 4:先重存有新 mark_dead 的段(live_ 位持久化——否则 ckpt 前
        // 的删除在 recovery 后复活为幽灵),再提交清单。
        if (!segment_set_->resave_dead_dirty()) return false;
        if (!segment_set_->commit()) return false;
        sw.add(sc::CkptSectionType::kSegManifest,
               segment_set_->manifest_payload());
    }
    const bool ok = !sw.empty() &&
        sc::SearchCheckpoint::write(fp, watermark, sw.sections());
    if (!ok) return false;
    sc::remove_chain_files(fp);  // 链坍缩（S20-2 R8）
    chain_ = ChainState{watermark, watermark, 1};
    clear_dirty();
    return true;
}

// S27-3 步骤 3:save_component_delta 退役(Slice C 已停用 delta 链,
// fields_ 删除后 delta 序列化源不复存在)。

TextPlugin::LoadResult
TextPlugin::load_component(std::string_view dir,
                           std::uint64_t expected_base_wm,
                           std::uint32_t chain_seq) {
    LoadResult result;
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
        chain_ = ChainState{};
        return result;
    };
    if (!lc) return fail();
    // S27-3 步骤 3:只认 kSegManifest(倒排在段文件内自含)。老格式 ckpt
    // (仅 kBm25Default/kBm25Fields,无 kSegManifest)→ 不认 → 退全量 fold
    // 重建段集(安全慢;legacy 迁移见步骤 5)。
    bool segments_ok = true;
    bool any = false;
    for (const auto& ls : lc->sections) {
        if (!ls.crc_ok) { segments_ok = false; continue; }
        auto st = static_cast<sc::CkptSectionType>(ls.type);
        if (st == sc::CkptSectionType::kSegManifest) {
            // 捕获内嵌段清单,open() 从它开段集(步骤 4 recovery 主路径)。
            pending_seg_manifest_.assign(ls.payload.begin(), ls.payload.end());
            any = true;
        }
    }
    if (!any) segments_ok = false;
    // S27-3 Slice C：delta 链重放退役。只读 base，chain_seq 恒 0。
    const std::uint64_t coverage = lc->watermark;
    const bool chain_ok = segments_ok && !from_prev;
    result.loaded = segments_ok && chain_ok;
    result.watermark = coverage;
    result.from_prev = from_prev;
    result.all_segments_ok = segments_ok && chain_ok;
    if (result.loaded) {
        chain_ = ChainState{expected_base_wm, coverage, /*next_seq=*/1};
        clear_dirty();
    } else {
        return fail();
    }
    // S27-3 步骤 3:段集装载下沉到此(原在 open())——shim/独立调用
    // load_component 的路径同样恢复段集。dir 为空(纯内存用法)跳过。
    if (!dir.empty()) {
        init_segment_set(dir, /*loaded=*/true);
    }
    return result;
}

// S27-3 步骤 3/4:初始化段集——优先 bm25.ckpt 内嵌 kSegManifest(单一
// commit point 主路径),回退过渡期 segments.manifest,再回退空集。装载后
// 重建 key→(seg_id, docid) 定位。
void TextPlugin::init_segment_set(std::string_view dir, bool loaded) {
    const std::string segs_dir = std::string(dir) + "/bm25_segments/";
    std::error_code ec;
    std::filesystem::create_directories(segs_dir, ec);
    std::unique_ptr<search::SegmentSet> opened;
    if (loaded && !pending_seg_manifest_.empty()) {
        opened = search::SegmentSet::open_from_payload(segs_dir,
                                                       pending_seg_manifest_);
    }
    if (!opened) opened = search::SegmentSet::open(segs_dir);  // 过渡回退
    if (opened) {
        segment_set_ = std::move(opened);
    } else {
        segment_set_ = std::make_unique<search::SegmentSet>();
    }
    pending_seg_manifest_.clear();
    pending_seg_manifest_.shrink_to_fit();
    rebuild_key_locations();
}


// ---- S18-6：CaskPlugin flush/open 实装 ----

plugin::PluginStatus TextPlugin::open(const plugin::OpenContext& ctx) {
    dir_.assign(ctx.dir);
    host_ = ctx.host;
    auto r = load_component(dir_, ctx.committed_base_watermark,
                            ctx.committed_chain_seq);
    // 损坏/缺失 → 自行降级：watermark 0（宿主全量重放），rebase 置位
    //（下次 flush 全量 base）。loaded → 续链。
    watermark_ = r.loaded ? r.watermark : 0;
    rebase_needed_.store(!r.loaded, std::memory_order_relaxed);

    // S27-3 步骤 3/4:段集初始化。loaded 情形已由 load_component 完成
    // (内嵌 kSegManifest 主路径);此处兜底未 loaded(ckpt 缺失/损坏 →
    // 空段集 + 全量重放重建)。building_ 已在构造期建好。
    if (!dir_.empty() && !r.loaded) {
        init_segment_set(dir_, /*loaded=*/false);
    }
    building_.store(std::make_shared<search::SealedSegment>(),
                    std::memory_order_release);

    start_builders();  // S27-4 P2:B>0 才起线程(默认 0 = 内联)

    return plugin::PluginStatus::kOk;
}

// S27-3 B2b 步骤 4:从段集重建 key_to_location_(open 尾部调,单线程)。
// 按段集序(seg_id 升序 == 清单序)遍历,段内 docid 升序 == LSN 升序 →
// 同 key 后写天然覆盖前写;只登记 live 文档(dead 槽位无删除/覆盖价值)。
void TextPlugin::rebuild_key_locations() {
    std::unique_lock lk(key_loc_mu_);
    key_to_location_.clear();
    if (!segment_set_) return;
    const auto segs = segment_set_->segments_view();  // open 单线程上下文
    for (const auto& sp : segs) {
        const auto& seg = *sp;
        const auto n = seg.doc_count();
        for (std::size_t d = 0; d < n; ++d) {
            const auto docid = static_cast<DocId>(d);
            if (!seg.is_live(docid)) continue;
            // 段序=seg_id 升序、段内 docid 升序=LSN 升序 → 直接覆盖即
            // 后写胜;S27-4 P1 起条目带 ord(LSN 守卫)与段对象指针。
            key_to_location_[seg.key_at(docid)] =
                KeyLocation{sp, docid, seg.lsn_at(docid), false};
        }
    }
}

plugin::FlushResult TextPlugin::flush(const plugin::FlushRequest& req) {
    // S27-3 Slice C：delta 链退役，只走 base（全量序列化 fields_）。
    // S27-3 Slice B2b 步骤 1：同时封口 building_ 到段集（段集持久化源）。
    plugin::FlushResult res;
    drain_builders();  // S27-4 P2:ckpt 覆盖到 watermark 需在途 apply 完成
    if (!dirty() && !req.force_rebase) {
        res.covered_ord = chain_.chain_wm;
    } else if ((flush_building(),
                save_component_base(dir_, req.watermark))) {
        // S27-3 B2b 步骤 4:保存前先封口 building_ → kSegManifest 覆盖到本次
        // watermark 的全部已 apply 文档(否则 recovery 后段集落后 fields_,
        // 查询永久走退化路径)。flush 经 RunFn 在 reducer 线程执行(S13-F6),
        // 与 apply/flush_building 同一单写者上下文,直接调用安全。
        res.covered_ord = req.watermark;
    } else {
        res.status = plugin::PluginStatus::kFailed;
        res.covered_ord = chain_.chain_wm;
    }
    res.generation = chain_.base_gen;
    res.chain_seq  = 0;
    res.chain_wm   = chain_.chain_wm;
    return res;
}


void TextPlugin::on_merge_commit(const plugin::MergeCommitEvent&) {
    // S13-F6：compact 遍历 concurrent_hash_map 必须在 reducer 静止点执行
    // （与 add_doc 并发不安全）——经 run_serialized 投递（原 Cask 硬编码
    // RunFn 的插件自治版）。merge 后压实恒 rebase（compact 内自置标志）。
    constexpr double kMergeCompactDeadRatio = 0.2;
    if (host_) {
        host_->run_serialized([this] {
            drain_builders();  // S27-4 P2:压实需 builder 静止
            compact(kMergeCompactDeadRatio);
        });
    } else {
        drain_builders();
        compact(kMergeCompactDeadRatio);  // 无宿主（standalone 测试）：直跑
    }
}

// S27-3 Slice B1：Building 段阈值封口。
// 契约：reducer 单写者（apply_text / apply_job_impl 内），故内部无须锁。
// 空 building_ / 无 segment_set_ 直接 no-op。失败兜底：add 失败（盘错误等）
// → building_ 物归原主，下次再试；不抛异常（reducer 不能挂）。
void TextPlugin::flush_building() {
    auto sealed = building_.load(std::memory_order_acquire);
    if (!sealed || sealed->doc_count() == 0) return;
    if (!segment_set_) return;

    const std::uint64_t hi_lsn =
        sealed->lsn_at(static_cast<DocId>(sealed->doc_count() - 1));

    // S27-3 步骤 5 并发序:先切空 building(查询对 sealed 内文档出现微秒级
    // 不可见窗口——NRT 语义可接受;反序会出现「building+段集同现」的双份
    // 命中,更糟)。add_pending 失败回滚恢复可见(reducer 单写者,无并发
    // add 丢失)。步骤 1 缺陷修正保留:失败时段不丢失。
    building_.store(std::make_shared<search::SealedSegment>(),
                    std::memory_order_release);
    auto pending = sealed;  // add_pending 成功时取走;失败时本地引用仍在
    if (!segment_set_->add_pending(pending, hi_lsn)) {
        building_.store(std::move(sealed), std::memory_order_release);  // 回滚
        return;
    }

    // S27-4 P1:封口零清扫——KeyLocation 持段**对象**指针,building 封口后
    // 对象身份不变(shared_ptr 移入段集),定位天然继续有效。
    seg_dirty_.store(true, std::memory_order_relaxed);
}


// ===========================================================================
// S27-4 P2:BuilderPool(设计 docs/design/s27-4-dwpt-design.md §1/§3)
// ===========================================================================

void TextPlugin::start_builders() {
    // P2 钳制:B 上限 1(串行等价——所有 builder 共享单一 building_,
    // SealedSegment::add 是单写者契约)。P3 落地每 builder 一段后开放 B>1
    // (设计 §1)。
    const std::size_t n = std::min<std::size_t>(config_.builder_threads, 1);
    if (n == 0 || !builders_.empty()) return;
    builders_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto b = std::make_unique<Builder>();
        Builder* raw = b.get();
        b->th = std::thread([this, raw] { builder_loop(*raw); });
        builders_.push_back(std::move(b));
    }
}

void TextPlugin::stop_builders() {
    for (auto& b : builders_) {
        {
            std::lock_guard<std::mutex> lk(b->mu);
            b->stop = true;
        }
        b->cv_idle.notify_all();
        b->cv_push.notify_all();
        if (b->th.joinable()) b->th.join();
    }
    builders_.clear();
}

// 生产者 = reducer 单线程;round-robin + 容量背压。
void TextPlugin::dispatch_job(BuilderJob&& j) {
    Builder& b = *builders_[rr_next_];
    rr_next_ = (rr_next_ + 1) % builders_.size();
    std::unique_lock<std::mutex> lk(b.mu);
    b.cv_push.wait(lk, [&] { return b.q.size() < kBuilderQueueCap || b.stop; });
    if (b.stop) return;  // 关停中:丢弃(close 前必有 drain,此处防御)
    b.q.push_back(std::move(j));
    lk.unlock();
    b.cv_idle.notify_all();
}

void TextPlugin::builder_loop(Builder& b) {
    for (;;) {
        BuilderJob j;
        {
            std::unique_lock<std::mutex> lk(b.mu);
            b.cv_idle.wait(lk, [&] { return !b.q.empty() || b.stop; });
            if (b.q.empty() && b.stop) return;
            j = std::move(b.q.front());
            b.q.pop_front();
            b.busy = true;
        }
        // 执行(builder 不可抛——吞异常,语义同 reducer 对 apply 异常的
        // 空 job 降级;宿主错误计数不可达,接受)。
        try {
            if (j.is_raw) {
                apply_text(j.raw_key, j.raw_ord, j.raw_text);
            } else {
                apply_job(j.job);
            }
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(b.mu);
            b.busy = false;
        }
        b.cv_idle.notify_all();  // drain 等待者 + 潜在下一任务(自唤醒无害)
        b.cv_push.notify_all();  // 背压等待的生产者
    }
}

// 读屏障:全部 builder 队列空 + 非 busy。可从 reducer(flush/compact)与
// 查询线程(plugin drain 钩子)并发调用。
void TextPlugin::drain_builders() {
    for (auto& b : builders_) {
        std::unique_lock<std::mutex> lk(b->mu);
        b->cv_idle.wait(lk, [&] { return b->q.empty() && !b->busy; });
    }
}

// reducer 侧压实触发:阈值判定(便宜,常态早退)→ drain(builder 静止,
// compact 的 tbb 遍历契约)→ compact。B=0 时 apply 路径的 maybe_auto_compact
// 已覆盖,这里直接返回避免双触发。
void TextPlugin::maybe_auto_compact_reducer() {
    if (builders_.empty()) return;  // 内联模式走 apply 路径既有触发
    const double thr = config_.auto_compact_dead_ratio;
    if (thr <= 0.0) return;
    const std::uint64_t retired = stats_.retired_since_compact();
    if (retired < kAutoCompactMinDeaths) return;
    if (retired < stats_.live_docs() / 2) return;
    drain_builders();
    compact(thr);
    stats_.reset_retired_since_compact();
}

}  // namespace bitcask::text
