// SearchLayerAdapter：SearchLayer 的 CaskPlugin 适配壳（S15-3，P1 过渡形态）。
//
// P1 阶段 SearchLayer 原样保留（BM25+HNSW 仍聚合），本 adapter 把它包装成
// 「唯一插件」接入 Cask 的插件分发通路——IndexPool 写路径（prepare/on_put/
// on_delete）经本类走通用接口；恢复、merge 收尾、checkpoint 编排仍由 Cask
// 直调 SearchLayer（P3/P4 分批收进接口，见 doc/plugin-arch-split-design-zh.md §9）。
//
// 语义决定（属于本插件，非宿主约定）：
//   - doc == nullptr（纯 KV put）时 text := value——「纯 put 也入全文索引」
//     的现行为在 adapter 层保持。
//   - 多字段路径（doc.fields 非空）走 prepare（map_analyze 分词，纯函数并行）；
//     单文本路径 prepare 返回 nullptr，分析在 on_put（reducer）内进行
//     ——精确镜像 S15-2 之前 process_task 的「Add && !fields.empty()」路由。

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "bitcask/plugin_api.hpp"
#include "bitcask/search_layer.hpp"

namespace bitcask::search {

// prepare 相产物：map_analyze 的 ReduceJob（类型擦除跨线程移交）。
struct SearchPrepared final : plugin::Prepared {
    ReduceJob job;
};

class SearchLayerAdapter final : public plugin::CaskPlugin {
public:
    explicit SearchLayerAdapter(SearchLayer& search) : search_(search) {}

    std::string_view name() const override { return "search"; }

    // ---- 生命周期：P1 未接线（ckpt 载入/保存编排仍在 Cask，P3 收进来）----
    plugin::PluginStatus open(const plugin::OpenContext&) override {
        return plugin::PluginStatus::kOk;
    }
    std::uint64_t watermark() const override {
        // P1：恢复起点仍由 Cask 的成对快照协议（keydir 字节水位）决定。
        return 0;
    }
    plugin::PluginStatus close() override { return plugin::PluginStatus::kOk; }

    // ---- 两相写入 ----
    bool wants_prepare() const override { return true; }

    plugin::PreparedPtr prepare(const plugin::PutEvent& e) const override {
        if (!e.doc || e.doc->fields.empty()) return nullptr;  // 单文本：reducer 内分析
        auto p = std::make_unique<SearchPrepared>();
        p->job = search_.map_analyze(e.key, e.ord, e.doc->fields,
                                     e.loc.file_id, e.loc.offset,
                                     e.loc.total_sz, e.tstamp);
        return p;
    }

    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr prep) override {
        const std::span<const std::byte> meta = e.doc ? e.doc->meta
                                                      : std::span<const std::byte>{};
        const std::span<const float> vec = e.doc ? e.doc->vec
                                                 : std::span<const float>{};
        if (e.doc && !e.doc->fields.empty()) {
            // 多字段：消费 prepare 相 ReduceJob。prep 为空 = prepare 抛过异常
            // （宿主已计数）→ 空 job 降级，reduce_apply 的空 job 守卫兜底
            // ——与旧版「map 异常收空 ReduceEntry」语义一致。
            auto* sp = static_cast<SearchPrepared*>(prep.get());
            if (sp) {
                search_.reduce_apply(sp->job, meta, vec);
            } else {
                const ReduceJob empty{};
                search_.reduce_apply(empty, meta, vec);
            }
        } else {
            // 单文本（原 OnWriteEntry 语义）：分析在 reducer 内进行。
            const std::string_view text = e.doc ? e.doc->text : e.value;
            search_.on_write(e.key, e.ord, text, e.loc.file_id, e.loc.offset,
                             e.loc.total_sz, e.tstamp);
            if (!meta.empty()) search_.index().set_meta(e.ord, meta);
            if (!vec.empty()) search_.on_vector(e.ord, vec);
        }
    }

    void on_delete(const plugin::DeleteEvent& e) override {
        search_.on_delete(e.key, e.ord);  // 返回的旧 ord 此处无用途（旧版同）
    }

    void on_relocate(const plugin::RelocateEvent& e) override {
        search_.on_relocate(e.key, e.ord, e.loc.file_id, e.loc.offset,
                            e.loc.total_sz);
    }

    void maintain(const plugin::MaintainEvent& e) override {
        // P1 未接线（merge 收尾仍由 Cask 经 RunFn 直调 compact/rebuild），
        // 实现备用：单写者上下文契约同 RunFn。
        if (e.reason == plugin::MaintainEvent::Reason::kPostMerge) {
            search_.compact(e.dead_ratio_hint);
            search_.compact_index_chunks();
        }
    }

    // ---- 持久化：P1 未接线（save/load_search_ckpt 编排在 Cask，P3 收）----
    plugin::FlushResult flush(const plugin::FlushRequest&) override {
        return {plugin::PluginStatus::kOk, 0, 0};
    }

private:
    SearchLayer& search_;
};

}  // namespace bitcask::search
