// 纯空白切分分词器（调试 / 纯拉丁场景）。
//
// WhitespaceAnalyzer 按 Unicode 空白字符切分，对小写拉丁文本做 case fold。
// 不做 n-gram，不识别 CJK。适用于调试基准或纯英文文档。

#pragma once

#include <cstdint>

#include "bitcask/analyzer.hpp"

namespace bitcask::text {

class WhitespaceAnalyzer final : public Analyzer {
public:
    WhitespaceAnalyzer() = default;
    explicit WhitespaceAnalyzer(std::uint32_t min_token_length,
                                std::uint32_t max_token_bytes = 1024)
        : min_token_length_(min_token_length),
          max_token_bytes_(max_token_bytes) {}

    [[nodiscard]] auto analyze_with_positions(std::string_view text) const
        -> TermPositionsMap override;

    [[nodiscard]] auto analyze_with_offsets(std::string_view text) const
        -> TermTokenMap override;

    [[nodiscard]] auto type() const noexcept -> AnalyzerType override {
        return AnalyzerType::Whitespace;
    }

private:
    std::uint32_t min_token_length_ = 1;   // 整词最小 codepoint 长度（S9.8），1=不过滤
public:  // analyzer.cpp 的两个 analyze 实现直接读(同 TU 权衡:免 getter 样板)
    std::uint32_t max_token_bytes_ = 1024;  // S31:单 token 字节上限,0=不限
};

}  // namespace bitcask::text
