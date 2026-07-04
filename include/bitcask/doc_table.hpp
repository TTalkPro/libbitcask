// DocTable — 查询面只读文档身份表接口（S16-3，设计 doc/plugin-arch-split-design-zh.md §4）。
//
// LiveChecker 只覆盖评分热路径需要的存活/doc_len 判定；DocTable 在其上扩展
// 查询面身份翻译（ord↔ext）与 meta 过滤——SearchLayer 的查询代码、HNSW 的
// live-callback、materialize_hits 经本接口消费 docmap，不再直摸 index::Index
// 具体类型。这是 P4 双插件（Text/Vector）拆分的前置：双方借同一 DocTable
// 只读视图，不各持身份表。
//
// index::Index 实现本接口（已有全部方法，仅补 override + ord_of 薄包装）。
// BM25 单测的 FakeLiveChecker 仅实现 LiveChecker，不受影响（本接口不添评分侧
// 方法，不动 LiveChecker 契约）。

#pragma once

#include "bitcask/live_checker.hpp"
#include "bitcask/meta_filter.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bitcask::bm25 {

class DocTable : public LiveChecker {
public:
    // ord → ext_id（命中翻译）。越界 / 已删返回 nullopt。
    [[nodiscard]] virtual std::optional<std::string>
    ord_to_ext(std::uint64_t ord) const = 0;

    // meta 过滤锁内求值（S13-P8：省 meta_blob 锁内拷贝）。
    [[nodiscard]] virtual bool
    eval_meta(std::uint64_t ord, const meta::MetaFilter& filter) const = 0;

    // ext_id → ord（explain 等 key→ord 反查）。
    [[nodiscard]] virtual std::optional<std::uint64_t>
    ord_of(std::string_view ext_id) const = 0;
};

// CompactionStats — 文档退休统计窄接口（S18-4，S12-2 自动压实的节流输入）。
//
// TextPlugin 的 maybe_auto_compact 需要「自上次压实起退休的文档版本数」与
// live 规模做节流决策——这是宿主 DocMap 的写路径统计，经本接口暴露，
// 不给插件完整 Index&（保查询面只读纪律）。
class CompactionStats {
public:
    virtual ~CompactionStats() = default;
    [[nodiscard]] virtual std::uint64_t retired_since_compact() const = 0;
    virtual void reset_retired_since_compact() = 0;
    [[nodiscard]] virtual std::uint64_t live_docs() const = 0;
};

// DocLenWriter — doc_len 回填的窄写接口（S18-1，设计 §4「doc_len 缓行」）。
//
// doc_len 存储在 DocMap（DocSlot 行 + 平坦 SoA 支撑 SIMD gather），语义归
// BM25：宿主 put_doc 时以 doc_len=0 占位，BM25 侧分析出 token 数后经本接口
// 回填（S16-2 通道的接口化）。P4 拆分后 TextPlugin 构造注入本接口——不给
// Index&（保 S16-3 查询面只读纪律），不进 PluginHost（text 域与宿主的构造期
// 专属契约，不污染通用插件接口）。
//
// 契约：仅 reducer 单写者上下文可调（与 set_doc_len 原语义一致）。
class DocLenWriter {
public:
    virtual ~DocLenWriter() = default;

    // 回填 ord 的 doc_len。ord 未登记则 no-op（防御）。
    virtual void set_doc_len(std::uint64_t ord, std::uint32_t len) = 0;
};

}  // namespace bitcask::bm25
