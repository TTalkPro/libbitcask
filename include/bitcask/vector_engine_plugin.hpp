// VectorEnginePlugin — 向量引擎插件的宿主契约（S32-M3;设计
// doc/vector-dual-engine-selection-zh.md §4.1）。
//
// Cask / HybridSearcher 对向量子系统的**全部**依赖收敛于此:引擎实现
// （VectorPlugin=HNSW、IvfPlugin=IVF 段,S32-M5 预留 DiskANN）在建库时经
// `meta::VectorEngine` 一次性选定、由 Cask 工厂实例化。两实现的
// delta/base 落盘语义真实分叉（插入日志重放建图 vs posting 段重写）,
// 故持久化面**不**进本接口——那是各插件经 CaskPlugin::flush/open 的
// 自治领域;本接口只收宿主的运行期调用面。
//
// 例外:legacy 统一 ckpt 迁移路径（cask_recovery）是 HNSW 纪元的产物,
// 由调用方以 engine==kHnsw 为门 static_cast 到具体类型——IVF 库不存在
// legacy 形态,该路径按构造不可达。

#pragma once

#include "bitcask/meta_filter.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/search_types.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace bitcask::vec {

class VectorEnginePlugin : public plugin::CaskPlugin {
public:
    // 是否配置了向量域（dim > 0）。
    [[nodiscard]] virtual bool enabled() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t dim() const noexcept = 0;

    // 写入端归一化（Cask put 路径同步调用;错误为静态消息文本）。
    [[nodiscard]] virtual std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const = 0;

    // 写（reducer 单写者;防御:无引擎/dim 不符直接忽略）。
    virtual void insert(std::uint64_t ord, std::span<const float> v) = 0;

    // 查询（线程安全,多读者）。ef 是「搜索宽度」旋钮:HNSW = ef,IVF =
    // nprobe（各实现自行映射;0 = 引擎默认）。
    [[nodiscard]] virtual std::expected<std::vector<search::SearchHit>,
                                        search::SearchError>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter) const = 0;

    // merge 收尾清死（单写者上下文;实现可 no-op 若清死延迟到下次 base）。
    virtual void rebuild() = 0;

    // 索引规模（观测用;含软删）。
    [[nodiscard]] virtual std::size_t size() const = 0;

    // 记账面（宿主 flush 决策/统计用）。
    [[nodiscard]] virtual bool dirty() const noexcept = 0;
    virtual void force_rebase() noexcept = 0;
};

}  // namespace bitcask::vec
