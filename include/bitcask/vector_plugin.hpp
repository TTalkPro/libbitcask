// VectorPlugin — HNSW 向量域插件（S18-3，设计 doc/plugin-arch-split-design-zh.md §6）。
//
// 自 SearchLayer 抽出的向量子系统：HNSW 图 + 写入端归一化 + 向量查询 +
// merge 重建 + vec 组件 checkpoint（vec.ckpt 文件族含 .vec/.qc8 侧车 +
// delta 插入日志）。S18-5 起实现 plugin::CaskPlugin（on_put/flush/open）；
// S19 起由 Cask 直持（S18 期经 SearchLayer 委托，shim 已降级为测试夹具）
// ——外部 API 面零变化。
//
// === 线程模型（与原 SearchLayer 向量域一致）===
//   单写者（reducer：insert/rebuild/组件 save/load）+ 多读者（search）。
//   hnsw_ 为 atomic<shared_ptr>：读写两端每次操作开头 load 快照，rebuild
//   以「旁路建新图 + 原子换指针」发布，旧图由引用计数续命（V3.5）。
//
// === 记账（S14-3/S14-4/S18-1 语义平移）===
//   dirty：insert/rebuild 置位，组件 save/load 成功清零。
//   delta 插入日志：insert 在 ord ≥ delta 窗口水位时入账（fold 重叠区
//   重放不入——已在链里）；窗口由组件 save/load 或 legacy 路径推进。

#pragma once

#include "bitcask/component_ckpt.hpp"       // S20-1 R6：共用链状态/载入结果类型
#include "bitcask/doc_table.hpp"
#include "bitcask/hnsw.hpp"
#include "bitcask/vector_plugin_config.hpp"  // S20-4：VectorPluginConfig（轻量头）
#include "bitcask/meta_file.hpp"     // meta::VectorMetric
#include "bitcask/meta_filter.hpp"
#include "bitcask/plugin_api.hpp"    // S18-5：实现 CaskPlugin
#include "bitcask/search_types.hpp"  // search::SearchHit / SearchError

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bitcask::vec {

// VectorPluginConfig 定义已迁 vector_plugin_config.hpp（S20-4）。

class VectorPlugin final : public plugin::CaskPlugin {
public:
    // dim>0 时构造 HNSW 图。metric 映射：kCosineNormalized/kDot →
    // HnswMetric::kDot（cosine 已在写入端归一化），kL2 → kL2。
    // docs：只读身份表（live 过滤 / ord→ext 翻译），生命周期由持有方保证。
    VectorPlugin(const VectorPluginConfig& config, const bm25::DocTable& docs);

    VectorPlugin(const VectorPlugin&) = delete;
    VectorPlugin& operator=(const VectorPlugin&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return config_.dim > 0; }
    [[nodiscard]] std::uint16_t dim() const noexcept { return config_.dim; }

    // ---- plugin::CaskPlugin（S18-5：写路径直连；flush/open 实装在 S18-6）----
    // 写入端归一化已在宿主 put 路径同步完成（normalize_for_write），事件里
    // 的 doc->vec 即最终存储值——on_put 直插。删除无动作（HNSW 软删经
    // DocTable::is_live 过滤，rebuild 物理清理）。
    [[nodiscard]] std::string_view name() const override { return "hnsw"; }
    // S18-6：载入 vec 组件；损坏/缺失自行降级（watermark 0 → 全量重放）。
    plugin::PluginStatus open(const plugin::OpenContext& ctx) override;
    [[nodiscard]] std::uint64_t watermark() const override {
        return watermark_;
    }
    plugin::PluginStatus close() override { return plugin::PluginStatus::kOk; }
    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr) override {
        if (e.doc && !e.doc->vec.empty()) {
            insert(e.ord, e.doc->vec);
        }
    }
    void on_delete(const plugin::DeleteEvent&) override {}
    // S18-6：落盘决策同 TextPlugin（force ‖ 自身 rebase（rebuild 置位）‖
    // 链长上限）；无向量配置时 base 清残留返回 no-op 成功（覆盖水位随
    // 请求推进——组件恒空，宿主 manifest 不为其记账）。
    plugin::FlushResult flush(const plugin::FlushRequest& req) override;
    // S18-7（设计 §3.9）：merge 收尾——HNSW 重建（物理清死节点）经
    // run_serialized 投递 reducer 静止点（单写者约束保持；rebuild 内部
    // 自置 rebase 标志）。无向量配置 no-op。
    void on_merge_commit(const plugin::MergeCommitEvent&) override;
    void force_rebase() noexcept {
        rebase_needed_.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] bool rebase_needed() const noexcept {
        return rebase_needed_.load(std::memory_order_relaxed);
    }

    // ---- 写入端归一化（原 Cask::prepare_vector 的领域核心，S18-3 下沉）----
    // Cask put 路径**同步**调用（归一化结果编码进 data file——V3.1「存储即
    // 归一化」；校验错误同步返回 put 调用方）。错误为静态消息文本，Cask
    // 边界翻译成 CaskFault（消息逐字保留）。空输入 → 空 span（合法：无向量）。
    [[nodiscard]] std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const;

    // ---- 写（reducer 单写者）----
    // 防御：无图 / dim 不符直接忽略。ord ≥ delta 窗口水位才入插入日志。
    void insert(std::uint64_t ord, std::span<const float> v);

    // ---- 查询（线程安全）----
    // cosine 配置时内部归一化查询向量（零向量返回空）；ef=0 → max(k,64)。
    // filter 与 is_live 组合为 HNSW live callback（V5）。
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter) const;

    // merge 重建（物理清死节点；单写者上下文）。S13-P8：clone_live 结构化
    // 拷贝 + 原子换指针。调用方负责 ckpt rebase 标志（legacy 全局语义）。
    void rebuild();

    // 图节点数（含软删死节点；观测用）。无图 = 0。
    [[nodiscard]] std::size_t size() const;

    // 图句柄（legacy 统一 ckpt 容器路径用；P5 随 legacy 收编后删除）。
    [[nodiscard]] std::shared_ptr<HnswIndex> graph() const {
        return hnsw_.load(std::memory_order_acquire);
    }
    void set_graph(std::shared_ptr<HnswIndex> g) {
        hnsw_.store(std::move(g), std::memory_order_release);
    }

    // ---- 记账 ----
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_.load(std::memory_order_relaxed);
    }
    void clear_dirty() noexcept {
        dirty_.store(false, std::memory_order_relaxed);
    }
    void begin_delta_window(std::uint64_t wm) { delta_window_wm_ = wm; }
    [[nodiscard]] bool delta_log_empty() const { return delta_vecs_.empty(); }
    void clear_delta_log() { delta_vecs_.clear(); }
    // kHnswDelta 段序列化：count u64 | dim u16 | 每条 ord u64 + f32[dim]。
    void serialize_delta_log(std::vector<std::byte>& out) const;
    // kHnswDelta 段重放：直插（不入日志、不标脏——链内容本就已持久化），
    // insert 自带 ord 水位幂等门。dim 不符 / 结构损坏返回 false。
    [[nodiscard]] bool apply_delta_log(std::span<const std::byte> payload);

    // ---- vec 组件 checkpoint（vec.ckpt 文件族；S18-6 收进 flush/open）----
    // base：rename→.prev + .vec/.qc8 侧车 + kHnsw 段 + 清 .d 链 + 记账收尾。
    // 无向量配置时清残留文件并返回 false（与旧 save_components_base 一致）。
    [[nodiscard]] bool save_component_base(std::string_view dir,
                                           std::uint64_t watermark);
    // 三组件同构，收敛至 ckpt:: 共用类型（S20-1 R6）。
    using DeltaSaveResult = ckpt::DeltaSaveResult;
    // delta：插入日志非空才写 .d<seq>；成功推进自身链状态并清日志。
    [[nodiscard]] DeltaSaveResult save_component_delta(std::string_view dir,
                                                       std::uint64_t watermark);
    using LoadResult = ckpt::LoadResult;
    // 载入：base（wm 校验，失败退 .prev）→ 链 .d1..d{chain_seq} 重放。
    [[nodiscard]] LoadResult load_component(std::string_view dir,
                                            std::uint64_t expected_base_wm,
                                            std::uint32_t chain_seq);

    // 链状态（Cask 转发同步；与 manifest entry 对齐）。
    using ChainState = ckpt::ChainState;
    [[nodiscard]] ChainState chain_state() const { return chain_; }
    void set_chain_state(const ChainState& st) {
        chain_ = st;
        delta_window_wm_ = st.chain_wm;  // 入账门与链水位同步
    }

    // legacy 统一 ckpt 的 kHnsw 段载入（图 + .vec/.qc8 payload）。
    [[nodiscard]] bool load_graph_section(std::span<const std::byte> payload,
                                          const std::string& vec_path,
                                          const std::string& qc_path);

private:
    VectorPluginConfig    config_;
    const bm25::DocTable& docs_;
    // V3.5：atomic<shared_ptr>——rebuild「旁路建新图 + 原子换指针」，读者
    // 每次操作开头 load 快照，旧图引用计数续命。
    std::atomic<std::shared_ptr<HnswIndex>> hnsw_;
    std::atomic<bool> dirty_{true};
    // S14-4/S18-1：delta 插入日志 + 入账窗口（单写者上下文访问）。
    std::uint64_t delta_window_wm_ = 0;
    std::vector<std::pair<std::uint64_t, std::vector<float>>> delta_vecs_;
    ChainState chain_{};
    // S18-6：flush/open 自治状态。
    std::string dir_;
    plugin::PluginHost* host_ = nullptr;  // open 时注入（S18-7 merge 收尾用）
    std::uint64_t watermark_ = 0;
    std::atomic<bool> rebase_needed_{true};
};

}  // namespace bitcask::vec
