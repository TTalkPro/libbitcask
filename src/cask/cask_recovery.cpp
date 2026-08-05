// cask_recovery.cpp — Cask 恢复域：离线升级（upgrade）、open 时 keydir
// 重建（load_keydir_from_disk）、恢复快照装载（load_recovery_snapshots）、
// legacy search.ckpt 一次性迁移与 delta 链重放。S21-3 B1：从 cask.cpp 纯
// 物理平移拆出（函数体不变），先例同 meta_file.cpp / legacy_ckpt.cpp。
#include "bitcask/cask.hpp"

#include "legacy_ckpt.hpp"  // S19-2：pre-S17 统一 ckpt 迁移读取器

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>

#include "bitcask/codec.hpp"           // decode_doc_value / doc_vector_f32
#include "bitcask/format.hpp"          // RecordType（fold 墓碑判定）
#include "bitcask/oki_state.hpp"       // S33-4：OKI load/缺口检查/重建
#include "bitcask/detail/scanner.hpp"  // scan_dir（fold 数据文件枚举）

#include "cask_internal.hpp"  // err / io_fault / bytes_to_view / ckpt 常量 / component_of_plugin

namespace bitcask {

namespace {
namespace fs = std::filesystem;
}  // namespace

// 离线升级：将 KV 模式目录转为索引模式。
// 不获取任何锁——要求目录处于离线状态（无活跃 writer/merger）。
// 步骤：验证 meta 是 KV → 覆写 meta 为 kIndex → 创建搜索插件（Text/Vector）→
//       新建 KeyDir + load_keydir_from_disk → mark_ready
// 返回的 Cask 是只读的（无 active writer），调用方可以 close 后
// 再用 open(dirname, {enable_search=true, read_write=true}) 正常使用。
std::expected<std::unique_ptr<Cask>, CaskFault>
Cask::upgrade(std::string_view dirname,
              const search::SearchLayerConfig& search_config) {
    if (!fs::exists(dirname)) {
        return std::unexpected(err(CaskError::kIo, "directory does not exist"));
    }

    if (!meta::meta_exists(std::string(dirname))) {
        return std::unexpected(err(CaskError::kModeMismatch,
                                    "no bitcask.meta found — not a valid bitcask directory"));
    }
    auto mc = meta::read_meta(std::string(dirname));
    if (!mc) {
        // S33：透传 MetaError 详情——纪元门禁的迁移提示（如 v4 → hintord）
        // 必须到达用户，不能吞成笼统的 "read meta failed"。
        return std::unexpected(err(CaskError::kIo,
                                   "read meta failed: " + mc.error().message));
    }
    if (mc->mode == meta::Mode::kIndex) {
        return std::unexpected(err(CaskError::kModeMismatch,
                                    "directory is already in index mode"));
    }

    meta::MetaConfig new_mc;
    new_mc.mode = meta::Mode::kIndex;
    new_mc.version = mc->version;  // S35：保留纪元（v6 目录不得降回 v5）
    auto wr = meta::write_meta(std::string(dirname), new_mc);
    if (!wr) {
        return std::unexpected(err(CaskError::kIo, "write meta failed"));
    }

    auto cask = std::make_unique<Cask>();
    cask->dirname_ = std::string(dirname);
    cask->meta_config_ = new_mc;
    if (!cask->field_schema_.open((fs::path(dirname) / "field.schema").string())) {  // #1
        return std::unexpected(err(CaskError::kIo,
            "field.schema corrupt or incompatible version"));
    }

    cask->docmap_ = std::make_shared<index::Index>();  // S16-1：宿主服务
    // S19-2：直构插件（shim 退役）。三个窄接口引用绑同一 Index 实例。
    cask->text_ = std::make_unique<text::TextPlugin>(
        search_config.text_config(), *cask->docmap_, *cask->docmap_,
        *cask->docmap_);
    cask->vec_plugin_ = cask->create_vector_plugin(search_config);
    cask->hybrid_.emplace(*cask->text_, *cask->vec_plugin_);
    // S18-8：恢复重放经 plugins_ 广播——upgrade 手工装配路径同样要注册。
    cask->plugins_ = {cask->text_.get(), cask->vec_plugin_.get()};

    cask->keydir_ = std::make_shared<keydir::KeyDir>();
    if (auto r = cask->load_keydir_from_disk(); !r) {
        return std::unexpected(r.error());
    }
    cask->keydir_->mark_ready();

    return cask;
}

void Cask::replay_delta_to_keydir(
    const std::vector<index::DocmapDeltaRow>& rows,
    const std::vector<index::DocmapDeltaRemoval>& rems,
    std::span<const std::byte> keydir_meta,
    RecoverySnapshots& recovery) {
    // 链重放：行 → LWW put；删除 → remove_if_older，ord 守卫顺序无关；
    // kKeydirDelta 段推进标量/fstats/字节水位。行/删除先于 meta 应用
    // （meta 的水位声明覆盖 ≤ 行集）。
    for (const auto& r : rows) {
        keydir_->put(r.ext, r.slot.loc.file_id,
                     r.slot.loc.total_sz, r.slot.loc.offset,
                     r.slot.tstamp, /*now*/ 0,
                     /*newest*/ false, 0, 0, r.ord);
        keydir_->advance_ord(r.ord);
    }
    for (const auto& m : rems) {
        (void)keydir_->remove_if_older(m.key, m.tomb);
        keydir_->advance_ord(m.tomb);
    }
    if (!keydir_meta.empty()) {
        if (auto wms = keydir_->apply_meta_delta(keydir_meta)) {
            // 链尾水位驱动 fold_start（快照对里最新的一份）。
            recovery.snap_wms = std::move(*wms);
        }
    }
}

// S17-5:legacy search.ckpt → per-component 一次性迁移。流程：
//   1) 用旧 load_search_ckpt 把段全载回（带 hook 推进 keydir 字节水位）。
//   2) 从各插件读 watermark 与组件链状态，构造 manifest。
//   3) 调 save_components_base 把当前内存态写到 3 个新组件文件。
//   4) 写 manifest。
//   5) 删旧 search.ckpt + search.ckpt.prev + .d* + search.vec / .qc8
//      （这些已被 save_components_base 重新写到 docmap.ckpt / bm25.ckpt /
//       vec.ckpt 对应 .vec / .qc8 sidecar）。
// 失败返回 false（caller 退全量 fold）。
bool Cask::migrate_legacy_search_ckpt() {
    const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
    // 1) 读旧 ckpt → 内存态（不写 keydir，已由 caller 在 recovery 阶段
    // 后续的 load_recovery_snapshots 接管；这里只关心段载入与写新文件）。
    // S19-2：legacy 读取器收编（load-only；shim 已降级测试夹具）。
    // legacy 统一 ckpt 是 HNSW 纪元产物（IVF 库按构造无 legacy 形态）；
    // 非 HNSW 引擎直接判失败退全量 fold（防御，不可达）。
    if (meta_config_.vector_engine != meta::VectorEngine::kHnsw) {
        log_warn("migrate_legacy: non-hnsw engine cannot own legacy ckpt");
        return false;
    }
    auto& hnsw_vp = static_cast<vec::VectorPlugin&>(*vec_plugin_);
    auto result = legacy_ckpt::load(old_ckpt, *docmap_, *text_, hnsw_vp);
    if (!result.loaded) {
        log_warn("migrate_legacy: failed to load legacy search.ckpt");
        return false;
    }
    // 2) 构造 manifest：每组件 base_watermark = result.watermark,
    // chain_seq = 0, chain_watermark = result.watermark（旧 ckpt 不区分
    // 组件 base/链——所有组件的水位统一对齐到 result.watermark）。
    bitcask::Manifest m;
    for (auto& e : m.entries) {
        e.base_watermark = result.watermark;
        e.chain_seq = 0;
        e.chain_watermark = result.watermark;
    }
    // 3) 写 per-component 文件。S18-2：docmap 组件由宿主直写。
    const bool docmap_ok = index::save_docmap_base(
        *docmap_, dirname_, result.watermark);
    if (docmap_ok) {
        docmap_chain_ = bitcask::ManifestEntry{
            result.watermark, 0, result.watermark};
    }
    std::array<bool, bitcask::kComponentCount> all_dirty{};
    for (auto& b : all_dirty) b = true;
    // S19-2：组件 base 直调插件（原 shim save_components_base 调度壳）。
    (void)all_dirty;
    const bool bm25_ok = text_->save_component_base(dirname_,
                                                    result.watermark);
    const bool vec_ok = hnsw_vp.save_component_base(dirname_,
                                                    result.watermark);
    if (!docmap_ok || !bm25_ok) {
        log_warn("migrate_legacy: failed to write per-component base "
                 "(docmap=" + std::to_string(docmap_ok) +
                 " bm25=" + std::to_string(bm25_ok) +
                 " vec=" + std::to_string(vec_ok) + ")");
        return false;
    }
    // 4) 写 manifest。
    const std::string mpath = dirname_ + "/" +
        std::string(bitcask::kManifestName);
    if (!bitcask::write_manifest(mpath, m)) {
        log_warn("migrate_legacy: write_manifest failed");
        return false;
    }
    current_manifest_ = m;
    // 5) 删旧 search.ckpt + .prev + .d<seq> + .vec + .qc8。
    std::error_code ec;
    std::filesystem::remove(old_ckpt, ec);
    std::filesystem::remove(old_ckpt + ".prev", ec);
    for (std::uint32_t i = 1; i < 1024; ++i) {
        if (!std::filesystem::remove(
                old_ckpt + ".d" + std::to_string(i), ec)) {
            // 链中段缺失即停（链是连续 1..N）。
            if (ec) break;
        }
    }
    std::filesystem::remove(
        std::filesystem::path(old_ckpt).replace_extension(".vec"), ec);
    std::filesystem::remove(
        std::filesystem::path(old_ckpt).replace_extension(".qc8"), ec);
    return true;
}

// ---- open 时重建 keydir ----------------------------------------------------
// 优先 fold(hint_file)，hint 缺失或 trailer CRC 校验不过时回退到 fold(data_file)
// 重建。fold 顺序按 tstamp 升序——保证后写入的 entry 覆盖前面的。
// search_layer 为空时跳过 SearchLayer 的恢复。
std::expected<void, CaskFault> Cask::load_keydir_from_disk() {
    // S19-2：搜索模式判定改成员（text_ 非空 = 索引模式）。
    const bool search_on = text_ != nullptr;
    auto entries = fileops::scan_dir(dirname_);
    if (!entries) return std::unexpected(io_fault(entries.error().errnum, dirname_));

    // S33-4：OKI manifest 先载——wm 就位后，后续 fold/链重放经 keydir
    // put/remove 咽喉点的挂钩自动只收 ord ≥ wm 的 tail 行（零逐点改动）。
    // 缺失/损坏 → 未加载态，收尾 finish_oki_recovery 整体重建。
    keydir_->oki().load(dirname_);

    // P14e:search.ckpt 分段快照快路径。search.ckpt 健康且全段 CRC 通过
    // 时，fold 从 keydir 水位起跳过已覆盖字节；否则全量 fold（各索引自门）。
    auto recovery = load_recovery_snapshots();
    if (!recovery) return std::unexpected(recovery.error());
    bool snap_loaded = recovery->snap_loaded;
    const auto& snap_wms = recovery->snap_wms;
    auto wm_of = [&](std::uint32_t fid) -> std::uint64_t {
        for (auto& [id, off] : snap_wms) {
            if (id == fid) return off;
        }
        return 0;  // 快照不认识的文件(快照后新建/merge 产物)→ 全量 fold
    };

    // S3/S18-8:search 恢复期攒批重放——批内**并行 prepare**（纯函数契约
    // 允许任意线程；PutEvent.replay 让 TextPlugin 对单文本也走 prepare
    // 并行分析）+ **fold 序串行 on_put 广播**（宿主 docmap 行先落，
    // doc_len=0 占位——与活写路径 S16-2 同构）。幂等由各结构 ord 水位
    // 自门兜底（InvertedIndex::add_doc / HnswIndex::insert / docmap LWW），
    // 重叠区重放安全。插入序 == fold 序 → 与逐条 recover 结果一致。
    // 墓碑前必 flush 以保相对序。
    constexpr std::size_t kRecoverBatch = 1024;
    struct ReplayDoc {
        std::string key;
        std::uint64_t ord = 0;
        std::string text;
        std::uint32_t file_id = 0;
        std::uint64_t offset = 0;
        std::uint32_t total_sz = 0;
        std::uint64_t tstamp = 0;
        std::vector<float> vector;
        std::vector<std::pair<std::string, std::string>> fields;
    };
    std::vector<ReplayDoc> recover_batch;
    // ①（s13-review §P1 后续）：统计本次恢复重分析的文档数——它度量的是
    // 「若现在不回存 checkpoint，下次崩溃要白付多少重放」。计所有喂进
    // recover 的文档（含被索引 ord 自门丢弃的重叠区：分析成本已经付了，
    // 回存快照能让下次 fold 起点前移、免掉这部分）。
    std::size_t recovered_docs = 0;
    // S14-6：恢复期遇到 field.schema 无法解析的悬空 FieldId 的计数（掉电
    // 窗口：intern 只 fflush 未 fsync，schema 尾条映射可能晚于数据丢失）。
    // 跳过该字段（与"丢弃"同级的降级）+ 循环后聚合告警，可观测不刷屏。
    std::size_t dangling_field_ids = 0;
    auto flush_recover = [&] {
        if (!search_on || recover_batch.empty()) return;
        recovered_docs += recover_batch.size();
        const std::size_t n = recover_batch.size();
        const std::size_t np = plugins_.size();
        // 视图物化（owning → FieldKV/DocView/PutEvent；容器预 size，批内
        // 地址稳定——ev 持 &dv 指针）。
        struct ItemViews {
            std::vector<plugin::FieldKV> fkv;
            plugin::DocView dv;
            plugin::PutEvent ev;
        };
        std::vector<ItemViews> views(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto& d = recover_batch[i];
            auto& v = views[i];
            v.fkv.reserve(d.fields.size());
            for (auto& [fn, fv] : d.fields) v.fkv.emplace_back(fn, fv);
            v.dv.text = d.text;
            v.dv.fields = v.fkv;
            v.dv.vec = d.vector;
            v.ev.ord = d.ord;
            v.ev.key = d.key;
            v.ev.doc = &v.dv;
            v.ev.loc = plugin::RecordLoc{d.file_id, d.offset, d.total_sz};
            v.ev.tstamp = d.tstamp;
            v.ev.replay = true;  // S18-8
        }
        // S3：批内并行 prepare（TBB 全局线程池，无 per-batch 线程创建）。
        std::vector<std::vector<plugin::PreparedPtr>> preps(n);
        tbb::parallel_for(std::size_t{0}, n, [&](std::size_t i) {
            preps[i].resize(np);
            for (std::size_t pi = 0; pi < np; ++pi) {
                if (plugins_[pi]->wants_prepare()) {
                    preps[i][pi] = plugins_[pi]->prepare(views[i].ev);
                }
            }
        });
        // fold 序串行 apply：宿主 docmap 行先落（doc_len=0，BM25 侧回填），
        // 再按注册序广播 on_put——与活写路径 reduce_index_entry 同构。
        for (std::size_t i = 0; i < n; ++i) {
            auto& d = recover_batch[i];
            docmap_->put_doc(d.key, d.ord,
                             index::DocSlot{
                                 index::DocLoc{.offset   = d.offset,
                                               .file_id  = d.file_id,
                                               .total_sz = d.total_sz},
                                 d.tstamp, /*doc_len=*/0});
            for (std::size_t pi = 0; pi < np; ++pi) {
                plugins_[pi]->on_put(views[i].ev, std::move(preps[i][pi]));
            }
        }
        recover_batch.clear();
    };

    // R3:每个 data file 的 fold 抽成独立单元 fold_one(e)。纯 KV 恢复
    // （search_layer==null）可并行——见函数尾部的并行调度。串行语义下
    // 「按 tstamp 升序后写覆盖前写」仍成立；并行下 keydir 冲突解析按
    // (file_id, tstamp, offset) LWW 与到达序无关（put_overwrite），fstats
    // 全程无锁原子累加，cold-start 期 keyfolders_==0 故新 key 直入分片
    // entries（不触 meta_mu_），256 分片提供真并发。
    auto fold_one =
        [&](const fileops::DataFileEntry& e) -> std::expected<void, CaskFault> {
        // 把 keydir 的 biggest_file_id 推到至少这个文件的 id——保证后续
        // 分配新 file_id 时不会跟磁盘上已有的文件冲突。
        keydir_->increment_file_id_at_least(static_cast<std::uint32_t>(e.tstamp));

        // 优先走 hint 文件加速路径（不读 value，省掉绝大部分 I/O）。
        // hint 缺失或 trailer CRC 不通过则 fallback 到 fold(data) 全量重建。
        // SearchLayer 恢复需要读 value（text 段），有 search_layer 时跳过 hint。
        const std::uint64_t fold_start =
            snap_loaded ? wm_of(static_cast<std::uint32_t>(e.tstamp)) : 0;

        bool used_hint = false;
        if (e.has_hint && !search_on && !snap_loaded) {
            auto hf = fileops::HintFile::open(e.hint_path,
                                                fileops::HintFile::Mode::kRead);
            if (hf) {
                // S13-P8：单遍校验+fold（原 validate_trailer 全文件读一遍、
                // fold 再读一遍）。CRC 不过时回调零次，keydir 零污染。
                // S33（hint v5）：记录带 ord——put 用真实 ord 并 advance，
                // 与 fold(data) 路径恢复结果完全等价（v4 时代 ord 恒 0 的
                // 怪癖随 flag-day 消除）。
                auto fr = hf->fold_validated([&](const auto& rec) {
                        if (rec.tombstone) {
                            // 墓碑 hint 必须执行——否则前一个 file 里的同 key
                            // 活 entry 会被错误保留。S33：携带 ord + 缺席即
                            // 插 sentinel——并行恢复下 remove/put 到达序无关
                            // （复活门见 keydir put_insert）。
                            keydir_->remove(bytes_to_view(rec.key), rec.tstamp,
                                            rec.ord,
                                            /*insert_tombstone_if_absent=*/true);
                            keydir_->advance_ord(rec.ord);
                            return;
                        }
                        keydir_->put(bytes_to_view(rec.key),
                                     static_cast<std::uint32_t>(e.tstamp), rec.total_sz, rec.offset,
                                     rec.tstamp, /*now*/ 0,
                                     /*newest*/ false, 0, 0, rec.ord);
                        keydir_->advance_ord(rec.ord);
                });
                if (fr && *fr) used_hint = true;
            }
        }
        if (used_hint) return {};

        // Fallback：fold 整个 data file。tolerate_crc_errors=true 让单条
        // 损坏的 record 跳过而不是中断整个文件加载——legacy 也是这语义。
        // out_last_valid_end 用于后续 torn-write 修复。
        // P6:恢复纯 fold,不 mmap(避免对大库逐文件全映射)。
        auto df = fileops::DataFile::open(e.data_path,
                                           fileops::DataFile::Mode::kRead,
                                           /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) {
            return std::unexpected(io_fault(df.error().errnum, e.data_path));
        }
        std::uint64_t last_valid_end = 0;
        // S35 原子批 staging（doc/atomic-batch-design-zh.md §1.3）：批头
        // 声明区间内的成员**拷贝暂存**（fold 缓冲跨记录复用，view 不可
        // 留存），推进到区间末端才依序 apply——fold 层保证区间不完整时
        // break（lve 停批头起点，随后截断），故暂存残留 = 未提交批，
        // 随作用域丢弃即可。
        struct StagedRec {
            format::RecordType type;
            std::uint64_t tstamp;
            std::uint64_t ord;
            std::vector<std::byte> key;
            std::vector<std::byte> value;
            std::uint64_t offset;
            std::uint32_t total_size;
        };
        std::vector<StagedRec> staged;
        std::uint64_t batch_end = 0;  // 0 = 未处于批区间
        auto apply_rec = [&](const codec::DataRecordView& view,
                             std::uint64_t offset, std::uint32_t total_size) {
                if (view.type == format::RecordType::kTombstone) {
                    // S33：携带 ord + 缺席即插 sentinel（并行恢复到达序无关，
                    // 同 hint 路径）。
                    keydir_->remove(bytes_to_view(view.key), view.tstamp,
                                    view.ord,
                                    /*insert_tombstone_if_absent=*/true);
                    // S33：墓碑 ord 也推进水位——否则文件末尾是墓碑时
                    // next_ord 落后于盘上已用 ord，重启后 alloc_ord 复用
                    // （与 hint v5 路径对齐；OKI max-ord-wins 依赖 ord 不重）。
                    keydir_->advance_ord(view.ord);
                    if (search_on) {
                        // S3:墓碑前 flush 攒批，保「文档↔墓碑」相对序（否则墓碑
                        // 可能先于其要删的 batch 内文档插入而被无效化）。
                        // S18-8：墓碑重放 = 宿主 docmap remove（Index 自记账：
                        // 脏位 + S14-4 门限删除日志）。**不广播 on_delete**——
                        // 历史语义（原 recover_tomb）：恢复期不扣减倒排统计
                        //（统计基线随 ckpt 快照恢复，重放墓碑只翻 live）。
                        flush_recover();
                        docmap_->remove(bytes_to_view(view.key), view.ord);
                    }
                    return;
                }
                keydir_->put(bytes_to_view(view.key), static_cast<std::uint32_t>(e.tstamp),
                             total_size, offset, view.tstamp, /*now*/ 0,
                             /*newest*/ false, 0, 0, view.ord);
                keydir_->advance_ord(view.ord);
                if (search_on) {
                    auto dv = codec::decode_doc_value(std::span<const std::byte>(view.value));
                    // V3.3:带向量的文档即使 text 为空也要恢复(否则
                    // Index 无该 ord,live 过滤会把它当死文档)。
                    // P3b:量化落盘(vec_quantized)也算带向量。
                    const bool dv_has_vec = dv && (dv->has_vector || dv->vec_quantized);
                    // S14-6：纯命名字段文档（text 空、无向量）也必须恢复——
                    // 否则连 docmap 都缺该 ord，live 过滤把它当死文档，
                    // bm25.fields 重建无从谈起。
                    if (dv && (!dv->text.empty() || dv_has_vec ||
                               dv->has_fields)) {
                        // S3:攒进批，满 kRecoverBatch 即并行处理。ReplayDoc 持
                        // owning 拷贝（fold 缓冲会复用，view 不可跨记录留存）。
                        ReplayDoc rd;
                        rd.key.assign(reinterpret_cast<const char*>(view.key.data()),
                                      view.key.size());
                        rd.ord      = view.ord;
                        rd.text.assign(reinterpret_cast<const char*>(dv->text.data()),
                                       dv->text.size());
                        rd.file_id  = static_cast<std::uint32_t>(e.tstamp);
                        rd.offset   = offset;
                        rd.total_sz = total_size;
                        rd.tstamp   = view.tstamp;
                        // P3b:doc_vector_f32 统一处理 f32 与 int8 量化两种落盘
                        // （内部 memcpy 未对齐安全 / dequant）。
                        rd.vector   = codec::doc_vector_f32(*dv);
                        // S14-6：命名字段还原（FieldId → 名字经 field.schema），
                        // 使 fold 重放与活写路径同构（per-field + catch-all）。
                        // 此前 dv->fields 被整段丢弃——增量窗口（ckpt 不健康
                        // 时全库）的字段索引在恢复后不存在且被下次 ckpt 固化。
                        if (dv->has_fields) {
                            rd.fields.reserve(dv->fields.size());
                            for (const auto& f : dv->fields) {
                                auto fname = field_schema_.name_of(f.id);
                                if (!fname) {
                                    ++dangling_field_ids;
                                    continue;
                                }
                                rd.fields.emplace_back(
                                    std::move(*fname),
                                    std::string(
                                        reinterpret_cast<const char*>(
                                            f.value.data()),
                                        f.value.size()));
                            }
                        }
                        recover_batch.push_back(std::move(rd));
                        if (recover_batch.size() >= kRecoverBatch) flush_recover();
                    }
                }
        };
        auto fr = df->fold(
            [&](const codec::DataRecordView& view, std::uint64_t offset,
                std::uint32_t total_size) {
                if (view.type == format::RecordType::kBatchHeader) {
                    // 批头：开启 staging（fold 层已验证区间在 EOF 内；
                    // value 畸形时 fold 直接 break，此处防御性忽略）。
                    auto bh = codec::decode_batch_header_value(view.value);
                    if (!bh) return;
                    batch_end = offset + total_size + bh->span_bytes;
                    staged.clear();
                    // 批头 ord 推进水位（区间即使被弃，跳号无害、复用有害）。
                    keydir_->advance_ord(view.ord);
                    return;
                }
                if (batch_end != 0) {
                    StagedRec sr;
                    sr.type = view.type;
                    sr.tstamp = view.tstamp;
                    sr.ord = view.ord;
                    sr.key.assign(view.key.begin(), view.key.end());
                    sr.value.assign(view.value.begin(), view.value.end());
                    sr.offset = offset;
                    sr.total_size = total_size;
                    staged.push_back(std::move(sr));
                    if (offset + total_size >= batch_end) {
                        // 区间收口 = 批已提交：依序放行。
                        for (const auto& s : staged) {
                            codec::DataRecordView v{
                                .crc = 0,
                                .type = s.type,
                                .tstamp = s.tstamp,
                                .ord = s.ord,
                                .key = s.key,
                                .value = s.value,
                                .total_size = s.total_size,
                            };
                            apply_rec(v, s.offset, s.total_size);
                        }
                        staged.clear();
                        batch_end = 0;
                    }
                    return;
                }
                apply_rec(view, offset, total_size);
            }, /*tolerate_crc_errors*/ true,
            /*out_last_valid_end*/ &last_valid_end,
            /*start_offset*/ fold_start);
        if (!fr) {
            return std::unexpected(err(CaskError::kBadCrc, e.data_path));
        }
        const std::uint64_t actual_size = df->size();
        df->close();

        // Torn-write 恢复：fold 已经跳过了文件尾部的损坏字节（可能是
        // 前一次 writer 写到一半 crash 留下的），如果我们是正经的 writer
        // 就把这些字节 truncate 掉——既释放磁盘，也避免后续 fstats 计算
        // 把坏字节当成「合法死 record」算到 total_bytes 里。
        // merge_only 不能这么干：它没有 write.lock，万一别的 writer 还在
        // 同一个文件后面追写，这里 truncate 会切掉别人的数据。
        if (opts_.read_write && !opts_.merge_only &&
            last_valid_end < actual_size) {
            auto wdf = fileops::DataFile::open(
                e.data_path, fileops::DataFile::Mode::kAppend);
            if (wdf) {
                (void)wdf->truncate_to(last_valid_end);  // best-effort
            }
        }
        return {};
    };

    // 调度：search_layer 存在时 HNSW 单写者 + BM25 插入须串行 → 走串行 fold，
    // 但 S3 在串行 fold 内把 recover_doc 攒批、analyze 并行化（见 flush_recover）。
    // 纯 KV 恢复且文件数 > 1 时并行 fold——worker 各取一文件，原子计数器分发，
    // 结果数组收集错误后统一传播。
    const std::size_t nfiles = entries->size();
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t nworkers =
        std::min<std::size_t>(nfiles, hw);

    if (search_on || nfiles <= 1 || nworkers <= 1) {
        for (const auto& e : *entries) {
            if (auto r = fold_one(e); !r) return std::unexpected(r.error());
        }
        flush_recover();  // S3:落最后一个不满批
        if (dangling_field_ids > 0) {
            log_warn("recovery: skipped " +
                     std::to_string(dangling_field_ids) +
                     " field value(s) with dangling field id "
                     "(field.schema tail lost; affected fields stay "
                     "unindexed until rewritten)");
        }
        // ①（s13-review §P1 后续）：恢复期重分析量超阈值时立即回存
        // checkpoint——否则重建成果只在内存，下次干净 close/merge 前再崩一次
        // 就全价重付。触发条件用**重分析文档数**而非「是否全量 fold」：ckpt
        // 健康但陈旧时 fold 起点旧、尾部重放大，同样值得回存；空库/小尾部
        // 增量不值得付大库整体序列化（回存成本 ∝ 索引总量，省下的 ∝ 重放量）。
        // 精细节奏走 checkpoint() API（②）。此刻 index lane 未注册、无并发
        // 写者，调用线程直接序列化即安全。best-effort：失败仅降级下次启动
        // 速度，不阻断 open。只读 / merge_only 不写（不持 write.lock）。
        constexpr std::size_t kPostRecoveryCkptMinDocs = 1000;
        if (search_on && recovered_docs >= kPostRecoveryCkptMinDocs &&
            opts_.read_write && !opts_.merge_only) {
            // S14-7：经成对入口（fold 后链可能有效 → delta 回存更省）。
            std::vector<std::byte> kd;
            auto wms0 = collect_snapshot_watermarks();
            if (wms0) keydir_->serialize_meta_delta(kd, *wms0);
            if (!save_search_ckpt_paired(dirname_ + "/" + kSearchCkptName,
                                         keydir_->peek_next_ord(), wms0,
                                         kd)) {
                log_warn("post-recovery search checkpoint save failed "
                         "(next open will re-fold)");
            }
        }
        finish_oki_recovery(snap_loaded, recovery->snap_next_ord);  // S33-4
        return {};
    }

    std::vector<std::expected<void, CaskFault>> results(nfiles);
    std::atomic<std::size_t> next{0};
    // RAII join guard：emplace_back 抛异常时已创建的 worker 会被析构自动
    // join——裸 vector<std::thread> 在此场景会让 joinable 线程触发
    // std::terminate，把可恢复的资源耗尽升级为崩溃。join 幂等（joinable
    // 检查），与下方显式 join 共存无重复 join。
    struct JoiningPool {
        std::vector<std::thread> threads;
        ~JoiningPool() {
            for (auto& t : threads) if (t.joinable()) t.join();
        }
    } pool;
    pool.threads.reserve(nworkers);
    for (std::size_t t = 0; t < nworkers; ++t) {
        pool.threads.emplace_back([&] {
            for (;;) {
                std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= nfiles) break;
                results[i] = fold_one((*entries)[i]);
            }
        });
    }
    for (auto& t : pool.threads) t.join();

    for (auto& r : results) {
        if (!r) return std::unexpected(r.error());
    }
    finish_oki_recovery(snap_loaded, recovery->snap_next_ord);  // S33-4
    return {};
}

// S33-4：恢复收尾——OKI 缺口检查与全量重建。
//
// 缺口来源：keydir 快照使 fold 跳过字节水位前的行，这些行 ord 全部 <
// snap_next_ord（快照自身的 next_ord，链重放前捕获）。manifest 缺失/损坏，
// 或 wm < snap_next_ord（快照写后、OKI flush 前崩溃的窗口）→ 无法靠 tail
// 重放补齐 → 整体重建：迭代 keydir 活 key（快照语义）排序写单一 run，
// cover = 当前 peek_next_ord()（open 单线程，无并发写者）。
//
// 只在可写句柄做（RO/merge_only 不持 write.lock，不产文件；OKI 保持未
// 加载态，S33-5 的 range 查询届时按不可用降级）。best-effort：失败仅降级
// OKI 可用性（下次 open 再试），不阻断 open。
void Cask::finish_oki_recovery(bool snap_loaded,
                               std::uint64_t snap_next_ord) noexcept {
    if (!opts_.read_write || opts_.merge_only) return;
    auto& oki = keydir_->oki();
    const bool gap = snap_loaded && oki.wm() < snap_next_ord;
    if (oki.loaded() && !gap) return;

    std::vector<oki::OkiState::DeltaRow> rows;
    rows.reserve(static_cast<std::size_t>(keydir_->info().key_count));
    auto it = keydir_->make_iter();
    if (it->start(/*now_sec*/ 0, /*maxage*/ -1, /*maxputs*/ -1) !=
        keydir::StartIterResult::kOk) {
        log_warn("oki rebuild: keydir iteration unavailable "
                 "(oki disabled until next open)");
        return;
    }
    while (auto p = it->next()) {
        rows.push_back(oki::OkiState::DeltaRow{
            std::string(p->key), p->ord, /*tomb=*/false});
    }
    it->release();
    if (!keydir_->oki().rebuild(dirname_, std::move(rows),
                                keydir_->peek_next_ord())) {
        log_warn("oki rebuild failed (oki disabled until next open)");
    }
}

// P14e/P14b:加载 keydir 快照 + search.ckpt 分段快照。用 watermark 单趟
// 自门模型取代旧 4-way 成对门：search.ckpt 健康且全段 CRC 通过 → fold_start
// = keydir 水位（快路径）；否则 fold_start = 0（全量 fold，各索引按自身
// ord 水位自门丢弃重叠区，方向安全）。
// S17-4:per-component 协议——读 index.manifest 作为 commit point，按
// 组件 file 路径分别载入 docmap.ckpt / bm25.ckpt / vec.ckpt。S17-5
// 兼容：manifest 缺失但 search.ckpt 存在时触发一次性迁移。
std::expected<Cask::RecoverySnapshots, CaskFault>
Cask::load_recovery_snapshots() {
    RecoverySnapshots recovery;

    // S14-7：keydir base 快照**先**载（链的行/删除要应用在 base 之上）。
    if (auto w = keydir_->load_snapshot(dirname_ + "/" + kKeydirSnapName)) {
        recovery.snap_wms = std::move(*w);
        recovery.snap_loaded = true;
        // S33-4：链重放前捕获快照自身的 next_ord（OKI 缺口检查基准）。
        recovery.snap_next_ord = keydir_->peek_next_ord();
    }

    bool search_ok = false;
    if (text_) {
        // S18-6：插件 open 注入器——manifest 提示 + dir/host。**所有路径**
        // （含 manifest 缺失/迁移失败的全量 fold 早退）都必须调用：插件的
        // dir_/host_ 在 open 时注入，flush 依赖之；零提示 = 插件降级自建
        // （watermark 0，rebase 置位 → 首次 flush 全量 base）。
        auto open_plugins = [&](const bitcask::Manifest& m) {
            for (auto* p : plugins_) {
                const auto comp = component_of_plugin(p->name());
                if (!comp) continue;
                const auto& entry =
                    m.entries[static_cast<std::size_t>(*comp)];
                plugin::OpenContext ctx;
                ctx.dir = dirname_;
                ctx.host = &plugin_host_;
                ctx.committed_base_watermark  = entry.base_watermark;
                ctx.committed_chain_watermark = entry.chain_watermark;
                ctx.committed_chain_seq       = entry.chain_seq;
                (void)p->open(ctx);
            }
        };
        // S17-5 兼容：manifest 缺失 + search.ckpt 存在 → 一次性迁移。
        const std::string mpath = dirname_ + "/" +
            std::string(bitcask::kManifestName);
        const std::string old_ckpt = dirname_ + "/" + kSearchCkptName;
        std::error_code ec;
        const bool has_manifest = std::filesystem::exists(mpath, ec);
        const bool has_old_ckpt = std::filesystem::exists(old_ckpt, ec);
        if (!has_manifest && has_old_ckpt) {
            // 触发迁移：把旧 search.ckpt 用旧路径 load 回来，再分
            // 写到新组件文件 + 写 manifest + 删旧文件。失败 → 全量 fold。
            if (!migrate_legacy_search_ckpt()) {
                open_plugins(bitcask::Manifest{});  // S18-6：零提示注入
                recovery.snap_loaded = false;
                return recovery;
            }
        }
        auto manifest = bitcask::read_manifest(mpath);
        if (!manifest) {
            // manifest 仍不可读（迁移失败/被破坏/新库）→ 全量 fold。
            open_plugins(bitcask::Manifest{});  // S18-6：零提示注入
            recovery.snap_loaded = false;
            return recovery;
        }
        current_manifest_ = *manifest;
        // S14-7：链重放钩子（S18-2：index:: 类型，仅 docmap 消费）。
        index::DocmapReplayHook hook =
            [this, &recovery](
                const std::vector<index::DocmapDeltaRow>& rows,
                const std::vector<index::DocmapDeltaRemoval>& rems,
                std::span<const std::byte> keydir_meta) {
                replay_delta_to_keydir(rows, rems, keydir_meta, recovery);
            };
        const auto hook_arg = recovery.snap_loaded ? hook :
            index::DocmapReplayHook{};
        bool all_components_ok = true;
        std::uint64_t min_chain_wm = UINT64_MAX;
        // S18-2：docmap 组件由宿主直载（keydir 链重放钩子只在这里生效）。
        {
            const auto& entry = current_manifest_.entries[0];
            auto dr = index::load_docmap(*docmap_, dirname_,
                                         entry.base_watermark,
                                         entry.chain_seq, hook_arg);
            if (dr.loaded) {
                min_chain_wm = std::min(min_chain_wm, dr.watermark);
                docmap_chain_ = bitcask::ManifestEntry{
                    entry.base_watermark, entry.chain_seq, dr.watermark};
            } else {
                all_components_ok = false;
                docmap_chain_ = bitcask::ManifestEntry{};
            }
        }
        // S18-6：bm25/vec 经插件 open()（组件载入 + 链续接 + 自身 rebase
        // 自管）。健康判据：watermark() == manifest 的 chain_watermark
        // （损坏/缺失 → 插件自降级报 0 ≠ 非零 entry → 判不健康；新库两侧
        // 皆 0 → 健康）。
        open_plugins(current_manifest_);
        for (auto* p : plugins_) {
            const auto comp = component_of_plugin(p->name());
            if (!comp) continue;
            const auto& entry =
                current_manifest_.entries[static_cast<std::size_t>(*comp)];
            const std::uint64_t pw = p->watermark();
            if (pw == entry.chain_watermark) {
                min_chain_wm = std::min(min_chain_wm, pw);
            } else {
                all_components_ok = false;
            }
        }
        // 全组件健康 → 清 legacy 全局 rebase（细粒度标志各插件 open 自管）。
        if (all_components_ok) {
            ckpt_rebase_needed_.store(false, std::memory_order_relaxed);
        }
        if (min_chain_wm == UINT64_MAX) {
            min_chain_wm = 0;  // 没有任何组件成功
        }
        // 「全组件健康」才走字节水位快路径；任一组件 .prev 回退 → 字节水位
        // 不可信，退全量 fold。保守以 all_components_ok 近似「非 .prev」
        // （load 内部已用 manifest base_wm 校验），再叠加 snap_loaded 双重门。
        bool any_from_prev = false;
        search_ok = all_components_ok && recovery.snap_loaded;
        (void)any_from_prev;
        // S17-4:fold_start = min(chain_watermarks)，各索引自门按其自身 ord
        // 水位丢重叠区；snap_wms 保持原形态，由上层 fold 阶段自门。
        if (search_ok) {
            // min_chain_wm 不回写 snap_wms（保持契约形态，自门已足够）。
        }
    }
    // search 不健康 → 退回全量 fold（snap_loaded=false 让 fold_start=0）。
    if (text_ && !search_ok) {
        recovery.snap_loaded = false;
    }
    return recovery;
}

}  // namespace bitcask
