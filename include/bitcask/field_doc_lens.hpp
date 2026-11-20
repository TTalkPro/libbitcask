// field_doc_lens.hpp — 每(段, 字段)一张「该字段自身词数」表 + 按字段取 dl 的
// LiveChecker（S56，下游反馈 2026-11-19 search-fields-local-stats 第 ② 条）。
//
// 背景：段级 LiveChecker::doc_len 返回**整篇总词数**（Σ 各字段 dl），而每字段
// 倒排的 avgdl 是**该字段**的 sum_dl / N。命名字段打分若取整篇总长，dl/avgdl
// 被放大到「整篇长度 ÷ 字段平均长度」——字段越短、文档越长罚得越重，title /
// labels 这类最想加权的字段反而压不过正文词频。BM25 的 dl 必须与 avgdl 同口径：
// 取该字段自己的词数（= 该字段倒排 posting 的 dls 列，索引时已经有）。
//
// 默认字段在 index_catch_all（缺省开）下 = 正文 + 全部命名字段合并，其 dl 恰等于
// 整篇总长，换成按字段取值后分数逐位不变；catch-all 关时默认字段只含正文，同样
// 由本表归正。
//
// 形态：
//   - 内存段（building / v1 载入）：add 时单写者 append（须先于该文档的 posting
//     写入——读者只经 posting 拿到 docid，见 RowChunks 并发契约）；v1 段载入后
//     由 posting dls 列一次性重建。
//   - mmap 段（v2）：盘上格式不变，首次查询该字段时从 posting dls 列惰性推出
//     （call_once，O(该字段 posting 数) 一次）；字段 sum_dl 与段总长相等时
//     （catch-all 默认字段的常态）每篇必相等，直接回退整篇总长、不建表。
//
// 0 = 未知（旧快照 dls 列缺失 / 越界）——FieldLenChecker 回退段级整篇总长：
// 总长 ≥ 字段长，块级 min_dl 上界仍 admissible（v5 不变量，见 live_checker.hpp）。

#pragma once

#include "bitcask/live_checker.hpp"
#include "bitcask/row_chunks.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace bitcask::search {

class FieldDocLens {
public:
    // 惰性推导：成功填 out（下标 = 段内 docid）返回 true；返回 false = 不建表
    // （回退整篇总长）。
    using Deriver = std::function<bool(std::vector<std::uint32_t>&)>;

    FieldDocLens() = default;
    explicit FieldDocLens(Deriver d) : deriver_(std::move(d)) {}
    FieldDocLens(const FieldDocLens&) = delete;
    FieldDocLens& operator=(const FieldDocLens&) = delete;

    // 单写者追加（内存段 add 路径）：d 之前缺的行补 0（该字段在那些文档里
    // 没有 token，posting 里不会出现它们）。d 已在表内 ⇒ 忽略（docid 不复用）。
    void append(std::uint64_t d, std::uint32_t dl) {
        std::size_t n = rows_.size();
        if (d < n) return;
        while (n < d) {
            rows_.push_back(std::uint32_t{0});
            ++n;
        }
        rows_.push_back(dl);
        pub_.store(n + 1, std::memory_order_release);
    }

    // 单线程整表装入（v1 载入；惰性推导在 call_once 内调用）。
    void assign(const std::vector<std::uint32_t>& v) {
        for (std::size_t i = rows_.size(); i < v.size(); ++i) rows_.push_back(v[i]);
        pub_.store(rows_.size(), std::memory_order_release);
    }

    [[nodiscard]] std::uint32_t get(std::uint64_t d) const {
        ensure();
        const std::size_t n = pub_.load(std::memory_order_acquire);
        return d < n ? rows_[d] : 0;
    }

    // 批量取：未知（越界 / 表里 0）填 0，由调用方回退。
    void fill(std::span<const std::uint64_t> ords,
              std::span<std::uint32_t> out) const {
        ensure();
        const std::size_t n = pub_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < ords.size(); ++i) {
            out[i] = ords[i] < n ? rows_[ords[i]] : 0;
        }
    }

    // 是否有表（惰性推导会被触发）。false ⇒ 全部回退整篇总长。
    [[nodiscard]] bool has_table() const {
        ensure();
        return pub_.load(std::memory_order_acquire) != 0;
    }

private:
    void ensure() const {
        if (!deriver_) return;
        std::call_once(once_, [this] {
            std::vector<std::uint32_t> v;
            if (deriver_(v)) const_cast<FieldDocLens*>(this)->assign(v);
        });
    }

    Deriver deriver_;
    mutable std::once_flag once_;
    RowChunks<std::uint32_t> rows_;
    std::atomic<std::size_t> pub_{0};
};

// 按字段取 dl 的 LiveChecker：存活走段（base），dl 走该字段的表，未知回退段级
// 整篇总长。查询线程并发安全（表只读 + 段 checker 自身并发安全）。
class FieldLenChecker final : public bm25::LiveChecker {
public:
    FieldLenChecker(const bm25::LiveChecker* base, const FieldDocLens* lens)
        : base_(base), lens_(lens) {}

    [[nodiscard]] bool is_live(std::uint64_t ord) const override {
        return base_->is_live(ord);
    }
    [[nodiscard]] std::uint32_t doc_len(std::uint64_t ord) const override {
        if (lens_ != nullptr) {
            if (const std::uint32_t v = lens_->get(ord); v != 0) return v;
        }
        return base_->doc_len(ord);
    }
    void fill_is_live(std::span<const std::uint64_t> ords,
                      std::span<char> out) const override {
        base_->fill_is_live(ords, out);
    }
    void fill_doc_lens(std::span<const std::uint64_t> ords,
                       std::span<std::uint32_t> out) const override {
        // 无表（catch-all 默认字段的 mmap 段 / 旧快照）⇒ 整批走段，免一遍填 0。
        if (lens_ == nullptr || !lens_->has_table()) {
            base_->fill_doc_lens(ords, out);
            return;
        }
        lens_->fill(ords, out);
        for (std::size_t i = 0; i < ords.size(); ++i) {
            if (out[i] == 0) out[i] = base_->doc_len(ords[i]);
        }
    }

private:
    const bm25::LiveChecker* base_;
    const FieldDocLens* lens_;
};

// 一个(段, 字段)的打分件：表 + 用它的 checker（地址稳定，视图出借裸指针）。
struct FieldScoring {
    explicit FieldScoring(const bm25::LiveChecker* base,
                          FieldDocLens::Deriver d = {})
        : lens(std::move(d)), checker(base, &lens) {}
    FieldScoring(const FieldScoring&) = delete;
    FieldScoring& operator=(const FieldScoring&) = delete;

    FieldDocLens lens;
    FieldLenChecker checker;
};

}  // namespace bitcask::search
