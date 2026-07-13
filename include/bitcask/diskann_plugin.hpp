// DiskannPlugin — DiskANN 磁盘档向量引擎插件（S32-M5；设计
// doc/vector-dual-engine-selection-zh.md §4/§5.2）。
//
// === 结构 ===
//   sealed_：不可变 DiskannSegment（Vamana 图 beam search；diskann.bda 侧车）
//   window_：增量窗口 = HnswIndex(inmem_int8, kDot) 有界实例——设计 §5.1
//            「FreshDiskANN 最难的 RAM 侧临时索引本库已有」的兑现。
//   查询 = sealed ∪ window 双路归并（同 int8 重建内积分数，直接可比；
//   base 换代瞬间可能双侧同 ord → 归并去重）。
//
// === 持久化（组件链，与 VectorPlugin 同构）===
//   base  = diskann.ckpt（kDiskann 段：gen/count/dim/R 交叉校验）+
//           diskann.bda 侧车（DiskannSegment::build 全量重建图：sealed
//           活集 + window 活集；死清理内建）。
//   delta = diskann.ckpt.d<seq>（kDeltaInfo + kHnswDelta 通用插入日志），
//           恢复重放进 window（重放 = 窗口重建图，代价由 rebase_min_docs
//           双门槛围住——S32-M1 同款）。
//   崩溃窗口：新 diskann.bda 已 rename 而新 ckpt 未落 → gen 守卫拒载 → fold
//           全量重建（与 HNSW payload 同语义）。
//
// === 与 HNSW 引擎的语义差 ===
//   rebuild()（merge 收尾）不做 eager 重建——盘上段死清理推迟到下次
//   base（查询侧 live 已滤，无正确性差）。search 的 ef 参数按 beam 宽 L
//   解释（0 = 自动 max(2k,64)）。
//
// === 线程模型 ===
//   单写者（reducer：insert/flush/open）+ 多读者（search）。sealed_/
//   window_ 均 atomic<shared_ptr>，读端快照、写端换指针，旧代引用计数
//   续命（与 VectorPlugin::hnsw_ 同协议）。

#pragma once

#include "bitcask/component_ckpt.hpp"
#include "bitcask/doc_table.hpp"
#include "bitcask/hnsw.hpp"
#include "bitcask/diskann.hpp"
#include "bitcask/meta_file.hpp"
#include "bitcask/meta_filter.hpp"
#include "bitcask/vector_delta_log.hpp"  // S32-M0b：插入日志单一真源
#include "bitcask/vector_engine_plugin.hpp"
#include "bitcask/vector_plugin_config.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::vec {

class DiskannPlugin final : public VectorEnginePlugin {
public:
    DiskannPlugin(const VectorPluginConfig& config, const bm25::DocTable& docs);

    DiskannPlugin(const DiskannPlugin&) = delete;
    DiskannPlugin& operator=(const DiskannPlugin&) = delete;

    // ---- VectorEnginePlugin ----
    [[nodiscard]] bool enabled() const noexcept override {
        return config_.dim > 0;
    }
    [[nodiscard]] std::uint16_t dim() const noexcept override {
        return config_.dim;
    }
    [[nodiscard]] std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const override;
    void insert(std::uint64_t ord, std::span<const float> v) override;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>,
                                search::SearchError>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter) const override;
    // merge 收尾：置 rebase（下次 base 重建时物理清死），无 eager 工作。
    void rebuild() override;
    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] bool dirty() const noexcept override {
        return dirty_.load(std::memory_order_relaxed);
    }
    void force_rebase() noexcept override {
        rebase_needed_.store(true, std::memory_order_relaxed);
    }

    // ---- plugin::CaskPlugin ----
    [[nodiscard]] std::string_view name() const override { return "diskann"; }
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
    plugin::FlushResult flush(const plugin::FlushRequest& req) override;
    void on_merge_commit(const plugin::MergeCommitEvent&) override;

    // ---- 组件链（测试可直调；语义同 VectorPlugin 同名方法）----
    using DeltaSaveResult = ckpt::DeltaSaveResult;
    using LoadResult = ckpt::LoadResult;
    using ChainState = ckpt::ChainState;
    [[nodiscard]] bool save_component_base(std::string_view dir,
                                           std::uint64_t watermark);
    [[nodiscard]] DeltaSaveResult save_component_delta(
        std::string_view dir, std::uint64_t watermark);
    [[nodiscard]] LoadResult load_component(std::string_view dir,
                                            std::uint64_t expected_base_wm,
                                            std::uint32_t chain_seq);
    [[nodiscard]] ChainState chain_state() const { return chain_; }

    // 观测（测试用）：sealed 段规模 / 窗口规模。
    [[nodiscard]] std::size_t sealed_size() const;
    [[nodiscard]] std::size_t window_size() const;

private:
    // kHnswDelta 通用插入日志（格式同 VectorPlugin：count u64 | dim u16 |
    // 每条 ord u64 + f32[dim]）。
    void serialize_delta_log(std::vector<std::byte>& out) const;
    [[nodiscard]] bool apply_delta_log(std::span<const std::byte> payload);
    void clear_delta_log() { delta_.clear(); }
    [[nodiscard]] std::shared_ptr<HnswIndex> make_window() const;

    VectorPluginConfig    config_;
    const bm25::DocTable& docs_;
    std::atomic<std::shared_ptr<const DiskannSegment>> sealed_;
    std::atomic<std::shared_ptr<HnswIndex>>        window_;

    std::atomic<bool> dirty_{true};
    std::atomic<bool> rebase_needed_{true};
    // 重放幂等门：open 载入覆盖水位之下的事件已在 sealed/链里（宿主从
    // min(全插件水位) 起 fold，本插件跳过 ord ≤ 此值；uint64(-1) = 无）。
    std::uint64_t replay_gate_ = static_cast<std::uint64_t>(-1);
    // S32-M0b：插入日志（与 VectorPlugin 共用 vec::DeltaLog 单一真源）。
    DeltaLog delta_;
    // S32-M1 同款：自 base 以来入窗向量数（恢复链重放代价的直接度量）。
    std::uint64_t vec_docs_since_base_ = 0;
    ChainState chain_{};
    std::string dir_;
    plugin::PluginHost* host_ = nullptr;
    std::uint64_t watermark_ = 0;
};

}  // namespace bitcask::vec
