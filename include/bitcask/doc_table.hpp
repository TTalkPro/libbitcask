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

}  // namespace bitcask::bm25
