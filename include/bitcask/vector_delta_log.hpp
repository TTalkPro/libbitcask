// DeltaLog — 向量插入日志（S32-M0b；设计 doc/vector-dual-engine-selection-zh.md
// §4.1 共享域件）。
//
// delta 链 kHnswDelta 段的内存态与编解码**单一真源**：VectorPlugin（HNSW）
// 与 IvfPlugin（IVF）共用——此前两份手写实现是位级漂移的温床（quant 设计
// 文档明文警告过的坑型）。职责：
//   - record 窗口门：ord ≥ 窗口水位才入账（fold 重叠区的重放已在链里，
//     S18-1 门限语义）；
//   - 序列化：count u64 | dim u16 | 每条 ord u64 + f32[dim]（LE，与历史
//     kHnswDelta 字节格式逐字一致——盘上兼容是硬约束）；
//   - 重放解析：回调式（引擎自决把记录插进图还是窗口），dim 不符/结构
//     损坏返回 false。
//
// 线程模型：单写者上下文（reducer / save / load 串行化），与宿主插件的
// delta_ords_ 时代一致；无内部同步。

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace bitcask::vec {

class DeltaLog {
public:
    // 入账窗口（组件 save/load 推进；ord < 窗口水位的 record 被丢弃）。
    void set_window(std::uint64_t wm) noexcept { window_wm_ = wm; }
    [[nodiscard]] std::uint64_t window() const noexcept { return window_wm_; }

    [[nodiscard]] bool empty() const noexcept { return ords_.empty(); }
    void clear() {
        ords_.clear();
        data_.clear();
    }

    // 前置：v.size() == dim（调用方已校验，本类不持 dim）。
    void record(std::uint64_t ord, std::span<const float> v) {
        if (ord < window_wm_) return;
        ords_.push_back(ord);
        data_.insert(data_.end(), v.begin(), v.end());
    }

    // kHnswDelta 段序列化（LE；仅 LE 主机，与项目 flag-day 约定一致）。
    void serialize(std::uint16_t dim, std::vector<std::byte>& out) const {
        auto put = [&out](const void* src, std::size_t len) {
            const auto* b = static_cast<const std::byte*>(src);
            out.insert(out.end(), b, b + len);
        };
        const auto cnt = static_cast<std::uint64_t>(ords_.size());
        put(&cnt, 8);
        put(&dim, 2);
        const std::size_t d = dim;
        for (std::size_t i = 0; i < ords_.size(); ++i) {
            put(&ords_[i], 8);
            put(data_.data() + i * d, d * sizeof(float));
        }
    }

    // 重放解析：per_record(ord, std::span<const float> vec)。span 仅在
    // 回调期间有效。
    template <typename Fn>
    [[nodiscard]] static bool parse(std::span<const std::byte> payload,
                                    std::uint16_t dim, Fn&& per_record) {
        const auto* p = payload.data();
        const auto* end = p + payload.size();
        if (end - p < 10) return false;
        std::uint64_t cnt = 0;
        std::uint16_t fdim = 0;
        std::memcpy(&cnt, p, 8);
        p += 8;
        std::memcpy(&fdim, p, 2);
        p += 2;
        if (fdim != dim) return false;
        const std::size_t vb = static_cast<std::size_t>(dim) * sizeof(float);
        std::vector<float> v(dim);
        for (std::uint64_t i = 0; i < cnt; ++i) {
            if (end - p < static_cast<std::ptrdiff_t>(8 + vb)) return false;
            std::uint64_t ord = 0;
            std::memcpy(&ord, p, 8);
            p += 8;
            std::memcpy(v.data(), p, vb);
            p += vb;
            per_record(ord, std::span<const float>(v.data(), dim));
        }
        return p == end;
    }

private:
    std::uint64_t window_wm_ = 0;
    std::vector<std::uint64_t> ords_;
    // dim 步长紧凑拼接（S21-1 布局沿袭：免每条一次独立堆分配）。
    std::vector<float> data_;
};

}  // namespace bitcask::vec
