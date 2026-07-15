// 分词器工厂实现 + NgramAnalyzer / WhitespaceAnalyzer 实现。

#include "bitcask/analyzer.hpp"
#include "bitcask/cjk_detect.hpp"
#include "bitcask/jieba_analyzer.hpp"
#include "bitcask/ngram_analyzer.hpp"
#include "bitcask/stemming_analyzer.hpp"
#include "bitcask/text_utils.hpp"
#include "bitcask/whitespace_analyzer.hpp"

#include "bitcask/detail/stop_words.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <utf8proc.h>

namespace bitcask::text {

// ===========================================================================
// 工厂（注册表模式）
// ===========================================================================

namespace {
auto registry() -> std::unordered_map<AnalyzerType, AnalyzerCreator>& {
    static std::unordered_map<AnalyzerType, AnalyzerCreator> r;
    return r;
}
}  // namespace

void AnalyzerFactory::register_creator(AnalyzerType type, AnalyzerCreator creator) {
    registry()[type] = creator;
}

auto AnalyzerFactory::create(const AnalyzerConfig& config)
    -> std::unique_ptr<Analyzer>
{
    const auto& reg = registry();
    auto it = reg.find(config.type);
    if (it == reg.end()) return nullptr;
    auto analyzer = it->second(config);
    if (analyzer && config.enable_stemming) {
        analyzer = std::make_unique<StemmingAnalyzer>(std::move(analyzer));
    }
    return analyzer;
}

// Ngram / Whitespace 自注册（定义在同一 TU）。
static const bool s_reg_ngram = [] {
    AnalyzerFactory::register_creator(
        AnalyzerType::Ngram,
        [](const AnalyzerConfig& c) -> std::unique_ptr<Analyzer> {
            if (c.min_n < 1 || c.max_n < c.min_n) return nullptr;
            return std::make_unique<NgramAnalyzer>(
                c.min_n, c.max_n, c.enable_stop_words, c.stop_words,
                c.min_token_length, c.max_token_bytes);
        });
    return true;
}();

static const bool s_reg_ws = [] {
    AnalyzerFactory::register_creator(
        AnalyzerType::Whitespace,
        [](const AnalyzerConfig& c) -> std::unique_ptr<Analyzer> {
            return std::make_unique<WhitespaceAnalyzer>(c.min_token_length,
                                                        c.max_token_bytes);
        });
    return true;
}();

// Jieba 注册放在工厂同一 TU（analyzer.cpp 必被链接）——而非 jieba_analyzer.cpp
// 内自注册：bitcask_text 是 STATIC 库，若没有其它符号引用 jieba_analyzer.o，
// 链接器会丢弃整个 TU，静态初始化不执行 → create(Jieba) 返回 nullptr →
// TextPlugin::analyzer_ 为空 → 首次带 text 的 put 段错误。这里的 lambda 体
// 引用 JiebaAnalyzer 构造符号，强制把 jieba_analyzer.o 拉进链接并完成注册。
static const bool s_reg_jieba = [] {
    AnalyzerFactory::register_creator(
        AnalyzerType::Jieba,
        [](const AnalyzerConfig& c) -> std::unique_ptr<Analyzer> {
            return std::make_unique<JiebaAnalyzer>(
                c.dict_path, c.min_n, c.max_n,
                c.enable_stop_words, c.stop_words,
                c.min_token_length, c.max_token_bytes);
        });
    return true;
}();

// ===========================================================================
// Analyzer 基类默认实现（Template Method）
// ===========================================================================

auto Analyzer::analyze(std::string_view text) const -> TermFreqMap {
    auto tpm = analyze_with_positions(text);
    TermFreqMap tfs;
    tfs.reserve(tpm.size());
    for (auto& [term, data] : tpm) {
        tfs.emplace(term, data.first);
    }
    return tfs;
}

// ===========================================================================

auto Analyzer::analyze_with_offsets(std::string_view text) const -> TermTokenMap {
    auto tpm = analyze_with_positions(text);
    TermTokenMap ttm;
    ttm.reserve(tpm.size());
    for (auto& [term, data] : tpm) {
        auto& infos = ttm[term];
        infos.reserve(data.second.size());
        for (auto p : data.second) {
            infos.push_back(TokenInfo{p, 0, 0});
        }
    }
    return ttm;
}

// ===========================================================================
// 内部辅助（detail 命名空间中仅 analyzer.cpp 使用的函数）
// ===========================================================================

namespace detail {

[[nodiscard]] bool is_unicode_space(char32_t cp) noexcept {
    auto cat = utf8proc_category(static_cast<utf8proc_int32_t>(cp));
    if (cat == UTF8PROC_CATEGORY_ZS) return true;
    if (cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x0B || cp == 0x0C) {
        return true;
    }
    return false;
}

[[nodiscard]] bool is_ascii_punct(char32_t cp) noexcept {
    if (cp >= 0x21 && cp <= 0x2F) return true;
    if (cp >= 0x3A && cp <= 0x40) return true;
    if (cp >= 0x5B && cp <= 0x60) return true;
    if (cp >= 0x7B && cp <= 0x7E) return true;
    return false;
}

}  // namespace detail

namespace {

// S29-8：Ngram 分词主循环（CJK run → n-gram 滑窗；拉丁 run → 整词），
// positions 版（analyze_with_positions）与 tf-only 版（analyze 覆写）共享。
// emit 回调负责词项聚合与 pos 语义（含「短词丢弃但 pos 仍递增」）。
template <class EmitNgrams, class EmitWord>
void ngram_tokenize(const std::vector<detail::CpInfo>& cps,
                    EmitNgrams&& emit_ngrams, EmitWord&& emit_word) {
    std::size_t i = 0;
    while (i < cps.size()) {
        if (detail::is_cjk(cps[i].cp) && !detail::is_cjk_punct(cps[i].cp)) {
            std::size_t run_start = i;
            while (i < cps.size() &&
                   detail::is_cjk(cps[i].cp) &&
                   !detail::is_cjk_punct(cps[i].cp)) {
                ++i;
            }
            emit_ngrams(run_start, i);
            if (i < cps.size() && detail::is_cjk_punct(cps[i].cp)) {
                ++i;
            }
        } else if (detail::is_unicode_space(cps[i].cp)) {
            ++i;
        } else if (detail::is_cjk_punct(cps[i].cp) || detail::is_ascii_punct(cps[i].cp)) {
            ++i;
        } else {
            std::size_t word_start = i;
            while (i < cps.size() &&
                   !detail::is_cjk(cps[i].cp) &&
                   !detail::is_unicode_space(cps[i].cp) &&
                   !detail::is_cjk_punct(cps[i].cp) &&
                   !detail::is_ascii_punct(cps[i].cp)) {
                ++i;
            }
            emit_word(word_start, i);
        }
    }
}

// T22-4a：Ngram 词项产出，analyze_with_positions 与 analyze（tf-only）共享。
// 在 ngram_tokenize 之上再包一层，把**过滤语义**（min_token_length /
// max_token_bytes / 空 term）也纳入共享——原两份各写一遍，而 S29-8 注释
// 断言两版「term 集与 tf 值逐位一致」，该不变量是索引路径（positions 版）
// 与 BOW 查询路径（tf 版）的一致性前提，却全靠复制粘贴维护。此处把断言
// 变成结构保证（对拍测试见 analyzer_test 的 NgramTfMatchesPositions*）。
//
// sink(term, pos) 按值收 pos：tf 版直接忽略形参 → 零 positions 分配，
// S29-8 的性能取舍（一篇 CJK 文档数千唯一 n-gram，每个一次 vector 堆分配）
// 完整保留。故此处不采用 Jieba collect_tokens 的物化 token 向量方案。
template <class Sink>
void ngram_collect(const std::vector<detail::CpInfo>& cps,
                   const std::string& normalized, std::uint32_t min_n,
                   std::uint32_t max_n, std::uint32_t min_token_length,
                   std::uint32_t max_token_bytes, Sink&& sink) {
    std::uint32_t pos = 0;

    auto emit_ngrams = [&](std::size_t start, std::size_t end) {
        const auto n = end - start;
        for (std::size_t gram = min_n; gram <= max_n; ++gram) {
            if (gram > n) break;
            for (std::size_t j = start; j + gram <= end; ++j) {
                const auto& first_cp = cps[j];
                const auto& last_cp = cps[j + gram - 1];
                std::string_view term(
                    normalized.data() + first_cp.byte_off,
                    (last_cp.byte_off + last_cp.byte_len) - first_cp.byte_off);
                sink(term, pos);
            }
        }
        ++pos;
    };

    auto emit_word = [&](std::size_t start, std::size_t end) {
        // S9.8：拉丁整词按 codepoint 长度过滤；短词丢弃但 pos 仍递增。
        if (end - start >= min_token_length &&
            (max_token_bytes == 0 ||
             (cps[end - 1].byte_off + cps[end - 1].byte_len) -
                     cps[start].byte_off <=
                 max_token_bytes)) {  // S31:超长 token 丢弃(pos 语义不变)
            const auto& first = cps[start];
            const auto& last = cps[end - 1];
            std::string_view term(
                normalized.data() + first.byte_off,
                (last.byte_off + last.byte_len) - first.byte_off);
            if (!term.empty()) sink(term, pos);
        }
        ++pos;
    };

    ngram_tokenize(cps, emit_ngrams, emit_word);  // S29-8：共享主循环
}

// T22-4a：view-map → string-map 物化 + 停用词过滤，两版共享。
// W1：内部以 string_view 去重，仅在此对每个唯一 term 分配一次 std::string。
// 停用词按最终 string key 查（两版一致）。
template <class OutMap, class ViewMap>
OutMap materialize_and_filter(ViewMap& vm, bool enable_stop_words,
                              const std::unordered_set<std::string>& stops) {
    OutMap out;
    out.reserve(vm.size());
    for (auto& [view, val] : vm) {
        out.emplace(std::string(view), std::move(val));
    }
    if (enable_stop_words && !stops.empty()) {
        for (auto it = out.begin(); it != out.end();) {
            if (stops.count(it->first) != 0) {
                it = out.erase(it);
            } else {
                ++it;
            }
        }
    }
    return out;
}

// T22-4b：空白分词主循环，analyze_with_positions 与 analyze_with_offsets
// 共享（原两份前 39 行逐字相同，仅末尾 4 行 sink 不同 → 过滤语义单边修改
// 即静默分叉）。sink(term, pos, start_byte, end_byte) 只在词通过全部过滤后
// 调用；短词/超长词丢弃但 pos 仍递增（S9.8/S31 位置语义）。
// 参照 JiebaAnalyzer::collect_tokens 的双出口先例，但不物化 token 向量——
// 直接回调，省一次中间分配。
template <class Sink>
void whitespace_tokenize(const std::vector<detail::CpInfo>& cps,
                         const std::string& normalized,
                         std::uint32_t min_token_length,
                         std::uint32_t max_token_bytes, Sink&& sink) {
    std::size_t i = 0;
    std::uint32_t pos = 0;
    while (i < cps.size()) {
        if (detail::is_unicode_space(cps[i].cp)) {
            ++i;
            continue;
        }
        const std::size_t word_start = i;
        while (i < cps.size() && !detail::is_unicode_space(cps[i].cp)) {
            ++i;
        }
        // S9.8：按 codepoint 长度过滤短词；短词丢弃但 pos 仍递增。
        if (i - word_start >= min_token_length &&
            (max_token_bytes == 0 ||
             (cps[i - 1].byte_off + cps[i - 1].byte_len) -
                     cps[word_start].byte_off <=
                 max_token_bytes)) {  // S31:超长 token 丢弃(pos 语义不变)
            const auto& first = cps[word_start];
            const auto& last = cps[i - 1];
            const std::size_t start_byte = first.byte_off;
            const std::size_t end_byte = last.byte_off + last.byte_len;
            std::string_view term(normalized.data() + start_byte,
                                  end_byte - start_byte);
            if (!term.empty()) sink(term, pos, start_byte, end_byte);
        }
        ++pos;
    }
}

}  // namespace

// ===========================================================================
// NgramAnalyzer
// ===========================================================================

NgramAnalyzer::NgramAnalyzer(std::uint32_t min_n, std::uint32_t max_n,
                             bool enable_stop_words,
                             std::vector<std::string> custom_stop_words,
                             std::uint32_t min_token_length,
                             std::uint32_t max_token_bytes)
    : min_n_(min_n), max_n_(max_n), enable_stop_words_(enable_stop_words),
      min_token_length_(min_token_length),
      max_token_bytes_(max_token_bytes) {
    if (enable_stop_words_) {
        const auto& defaults = bitcask::detail::default_stop_words();
        const auto& src = custom_stop_words.empty()
                              ? defaults
                              : custom_stop_words;
        stop_words_.insert(src.begin(), src.end());
    }
}

auto NgramAnalyzer::analyze_with_positions(std::string_view text) const -> TermPositionsMap {
    if (text.empty()) return {};

    // S29-8：融合解码——nfkc_fold 快路径校验趟直接产出 CpInfo，CJK 文本
    // 免第二遍 to_codepoints 全量重解（原每码点解码两次）。
    std::string normalized;
    const auto& cps = detail::nfkc_fold_codepoints_reuse(text, normalized);
    if (cps.empty()) return {};

    // W1：内部以 string_view 去重，仅在末尾对每个唯一 term 分配一次 std::string。
    // 安全前提：normalized 在本函数内持有全部字节，vpm 不超过其生命周期。
    std::unordered_map<std::string_view,
                       std::pair<std::uint32_t, std::vector<std::uint32_t>>>
        vpm;
    ngram_collect(cps, normalized, min_n_, max_n_, min_token_length_,
                  max_token_bytes_,
                  [&](std::string_view term, std::uint32_t pos) {
                      auto& [tf, positions] = vpm[term];
                      ++tf;
                      positions.push_back(pos);
                  });
    return materialize_and_filter<TermPositionsMap>(vpm, enable_stop_words_,
                                                    stop_words_);
}

// S29-8：tf-only 覆写。基类默认从 analyze_with_positions 派生后丢弃
// positions——一篇 CJK 文档数千个唯一 n-gram，每个一次 positions vector
// 堆分配，BOW 查询（search_text 的 analyzer_->analyze）等 tf 消费方随即
// 全部丢弃。本覆写与 positions 版共享 ngram_collect（含全部过滤语义）+
// materialize_and_filter（含停用词），仅 sink 不同——**term 集与 tf 值
// 逐位一致由结构保证**（T22-4a：原两版各写一遍过滤，该不变量靠复制粘贴
// 维护）。sink 忽略 pos 形参 → 零 positions 分配，本覆写的性能理由不变。
auto NgramAnalyzer::analyze(std::string_view text) const -> TermFreqMap {
    if (text.empty()) return {};

    std::string normalized;
    const auto& cps = detail::nfkc_fold_codepoints_reuse(text, normalized);
    if (cps.empty()) return {};

    std::unordered_map<std::string_view, std::uint32_t> vfm;
    ngram_collect(cps, normalized, min_n_, max_n_, min_token_length_,
                  max_token_bytes_,
                  [&](std::string_view term, std::uint32_t) { ++vfm[term]; });
    return materialize_and_filter<TermFreqMap>(vfm, enable_stop_words_,
                                               stop_words_);
}

// ===========================================================================
// WhitespaceAnalyzer
// ===========================================================================

auto WhitespaceAnalyzer::analyze_with_positions(std::string_view text) const -> TermPositionsMap {
    if (text.empty()) return {};

    // S29-8：融合解码（同 NgramAnalyzer——校验趟直接产出 CpInfo）。
    std::string normalized;
    const auto& cps = detail::nfkc_fold_codepoints_reuse(text, normalized);
    if (cps.empty()) return {};

    TermPositionsMap tpm;
    whitespace_tokenize(cps, normalized, min_token_length_, max_token_bytes_,
                        [&](std::string_view term, std::uint32_t pos,
                            std::size_t, std::size_t) {
                            auto& [tf, positions] = tpm[std::string(term)];
                            ++tf;
                            positions.push_back(pos);
                        });
    return tpm;
}

auto WhitespaceAnalyzer::analyze_with_offsets(std::string_view text) const -> TermTokenMap {
    if (text.empty()) return {};

    // S29-8：融合解码（同 NgramAnalyzer）。
    std::string normalized;
    const auto& cps = detail::nfkc_fold_codepoints_reuse(text, normalized);
    if (cps.empty()) return {};

    TermTokenMap ttm;
    whitespace_tokenize(cps, normalized, min_token_length_, max_token_bytes_,
                        [&](std::string_view term, std::uint32_t pos,
                            std::size_t start_byte, std::size_t end_byte) {
                            ttm[std::string(term)].push_back(TokenInfo{
                                pos, static_cast<std::uint32_t>(start_byte),
                                static_cast<std::uint32_t>(end_byte)});
                        });
    return ttm;
}

// ===========================================================================
// NgramAnalyzer
// ===========================================================================

auto NgramAnalyzer::analyze_with_offsets(std::string_view text) const -> TermTokenMap {
    auto tpm = analyze_with_positions(text);
    TermTokenMap ttm;
    ttm.reserve(tpm.size());
    for (auto& [term, data] : tpm) {
        auto& infos = ttm[term];
        infos.reserve(data.second.size());
        for (auto p : data.second) {
            infos.push_back(TokenInfo{p, 0, 0});
        }
    }
    return ttm;
}

}  // namespace bitcask::text
