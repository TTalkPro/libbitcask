#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bitcask/detail/icu_util.hpp"
#include "bitcask/detail/inert_table.hpp"

namespace bitcask::text::detail {

// ===========================================================================
// UTF-8 解码
// ===========================================================================
//
// S38：自带严格解码器，不用 ICU 的 U8_NEXT。理由有两条，都不是风格问题：
//   1. U8_NEXT 是**宏**，在我们的 TU 里展开。ICU 的头虽被当 SYSTEM（告警免疫），
//      但宏展开出来的 C 风格转换算在**调用方**头上，会被 -Wold-style-cast /
//      -Wconversion 逐个打中，只能靠 pragma 包起来——那比自己写还脏。
//   2. 这是分词热路径上每码点都要过的函数。手写内联版没有库调用，比原先的
//      utf8proc_iterate（一次真函数调用）还快。
//
// 严格性契约（与 ICU / utf8proc 一致，tests/analyzer_test.cpp 对拍全码点）：
// 拒绝过长编码（overlong）、代理区 U+D800..DFFF、> U+10FFFF、非法前导/续字节。
//
// 返回 {码点, 消耗字节数}；**consumed == 0 表示输入非法或为空**。
// 注意这与 S38 之前的行为不同：旧版对非法序列返回 {U+FFFD, 1}，于是所有
// `consumed == 0` 的判空守卫都形同虚设，GB18030 之类的非 UTF-8 字节能一路
// 混过快路径。现在非法即 0，守卫真正生效。
[[nodiscard]] inline std::pair<char32_t, std::size_t> decode_one(
    std::string_view sv) noexcept {
    if (sv.empty()) return {0, 0};

    const auto b0 = static_cast<unsigned char>(sv[0]);
    if (b0 < 0x80) return {static_cast<char32_t>(b0), 1};

    const auto cont = [&sv](std::size_t i) noexcept -> unsigned {
        return static_cast<unsigned char>(sv[i]) & 0xC0u;
    };

    if (b0 >= 0xC2 && b0 <= 0xDF) {  // 2 字节；< 0xC2 即 overlong
        if (sv.size() < 2 || cont(1) != 0x80) return {0, 0};
        const auto cp = (static_cast<char32_t>(b0 & 0x1Fu) << 6) |
                        static_cast<char32_t>(static_cast<unsigned char>(sv[1]) & 0x3Fu);
        return {cp, 2};
    }

    if (b0 >= 0xE0 && b0 <= 0xEF) {  // 3 字节
        if (sv.size() < 3 || cont(1) != 0x80 || cont(2) != 0x80) return {0, 0};
        const auto cp = (static_cast<char32_t>(b0 & 0x0Fu) << 12) |
                        (static_cast<char32_t>(static_cast<unsigned char>(sv[1]) & 0x3Fu) << 6) |
                        static_cast<char32_t>(static_cast<unsigned char>(sv[2]) & 0x3Fu);
        if (cp < 0x800) return {0, 0};                      // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return {0, 0};    // 代理半区
        return {cp, 3};
    }

    if (b0 >= 0xF0 && b0 <= 0xF4) {  // 4 字节
        if (sv.size() < 4 || cont(1) != 0x80 || cont(2) != 0x80 || cont(3) != 0x80) {
            return {0, 0};
        }
        const auto cp = (static_cast<char32_t>(b0 & 0x07u) << 18) |
                        (static_cast<char32_t>(static_cast<unsigned char>(sv[1]) & 0x3Fu) << 12) |
                        (static_cast<char32_t>(static_cast<unsigned char>(sv[2]) & 0x3Fu) << 6) |
                        static_cast<char32_t>(static_cast<unsigned char>(sv[3]) & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) return {0, 0};   // overlong / 越界
        return {cp, 4};
    }

    return {0, 0};  // 0x80..0xC1（孤立续字节 / overlong 前导）、0xF5..0xFF
}

// 整串是否合法 UTF-8。入口校验用——**索引入口必须先过这一关**，否则非 UTF-8
// 数据会以"能存进去但搜不出来"的形态静默失败（见 nfkc_fold 的注释）。
[[nodiscard]] inline bool validate_utf8(std::string_view sv) noexcept {
    std::size_t off = 0;
    while (off < sv.size()) {
        // ASCII 快扫：绝大多数语料的多数字节走这里。
        if (static_cast<unsigned char>(sv[off]) < 0x80) {
            ++off;
            continue;
        }
        const auto [cp, n] = decode_one(sv.substr(off));
        (void)cp;
        if (n == 0) return false;
        off += n;
    }
    return true;
}

// 把非法字节逐个替换为 U+FFFD，产出保证合法的 UTF-8。
// 只在 nfkc_fold 的非法输入分支上跑，不在热路径。
inline void sanitize_utf8(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    std::size_t off = 0;
    while (off < in.size()) {
        const auto [cp, n] = decode_one(in.substr(off));
        (void)cp;
        if (n == 0) {
            out.append("\xEF\xBF\xBD", 3);  // U+FFFD
            ++off;                          // 逐字节推进，不吞掉后面可能合法的序列
        } else {
            out.append(in.substr(off, n));
            off += n;
        }
    }
}

// ===========================================================================
// NFKC_Casefold
// ===========================================================================

enum class FoldStatus : std::uint8_t {
    kOk,           // 输入是合法 UTF-8，out 为其 NFKC_Casefold
    kInvalidUtf8,  // 输入含非法 UTF-8 字节；out 是把非法字节换成 U+FFFD 后的结果
    kIcuError,     // ICU 侧失败（数据缺失/OOM）；out 为空
};

// 慢路径：ICU NFKC_Casefold（快路径未命中时的回退）。
//
// S38 的行为变更，两处都是修 bug：
//   1. 旧版调 utf8proc_map，遇非法 UTF-8 返回负值，而调用点写的是
//      `if (n < 0) return;` —— out 已 clear，于是**整段文本静默变空**，该文档
//      零 term、永远搜不到，且不产生任何错误信号。
//   2. 换成 ICU 也不能直接信它：实测 icu::Normalizer2::normalizeUTF8 对非法
//      UTF-8 **不报错也不替换**，原样透传字节。那会让失败形态从"整段消失"
//      变成"整段黏成一个乱码 term"，一样是静默错。
// 所以非法输入在这里显式转成 U+FFFD 再归一化，并把状态回报给调用方。
[[nodiscard]] inline FoldStatus nfkc_map_slow(std::string_view input,
                                              std::string& out) {
    if (validate_utf8(input)) {
        return icu_nfkc_casefold(input, out) ? FoldStatus::kOk
                                             : FoldStatus::kIcuError;
    }
    std::string cleaned;
    sanitize_utf8(input, cleaned);
    if (!icu_nfkc_casefold(cleaned, out)) return FoldStatus::kIcuError;
    return FoldStatus::kInvalidUtf8;
}

// 出参版——写入 caller 缓冲（clear 保留容量），热路径稳态零分配。
// caller 用 thread_local 复用（如 jieba 逐词归一化）。
[[nodiscard]] inline FoldStatus nfkc_fold_checked(std::string_view input,
                                                  std::string& out) {
    out.clear();
    if (input.empty()) return FoldStatus::kOk;

    // P2.5/P2.5b 统一快路径：全部码点 ∈（NFKC_Casefold 恒等区段 ∪ ASCII）
    // 时，整个变换等价于「原串 + ASCII 字节 tolower」——纯 ASCII 文本与
    // 「中文 + 半角英文/标点」文本都命中，跳过整条 ICU 流水线。
    // ASCII 的 tolower 可安全按字节做：UTF-8 多字节序列的所有字节 ≥ 0x80，
    // 不会误伤。含全角标点（，：！？等会被 NFKC 折叠）即整串回退。
    // 语义对拍见 analyzer_test（穷举表成员 + 随机串黑盒 vs ICU）。
    bool fast = true;
    {
        std::size_t off = 0;
        while (off < input.size()) {
            const auto b = static_cast<unsigned char>(input[off]);
            if (b < 0x80) {
                // 可打印 ASCII + 常见空白恒等（A-Z 由下方 tolower 处理）。
                // 常见可打印区先判（绝大多数字节两次比较即过）。
                if (!((b >= 0x20 && b <= 0x7E) ||
                      b == 0x09 || b == 0x0A || b == 0x0D)) {
                    fast = false;
                    break;
                }
                ++off;
                continue;
            }
            const auto [cp, consumed] = decode_one(input.substr(off));
            // consumed == 0 现在真的表示"非法 UTF-8"（S38 前它只表示空输入），
            // 于是非 UTF-8 字节会正确地掉进慢路径去做校验与替换。
            if (consumed == 0 || !nfkc_casefold_inert(cp)) {
                fast = false;
                break;
            }
            off += consumed;
        }
    }
    if (fast) {
        out.assign(input);
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return FoldStatus::kOk;
    }

    return nfkc_map_slow(input, out);
}

// 宽容形态：丢弃状态。调用点若不关心输入合法性可用它——但**索引入口不该用**，
// 那里要的是 nfkc_fold_checked 的状态（否则又回到静默失败）。
inline void nfkc_fold(std::string_view input, std::string& out) {
    (void)nfkc_fold_checked(input, out);
}

[[nodiscard]] inline std::string nfkc_fold(std::string_view input) {
    std::string out;
    (void)nfkc_fold_checked(input, out);
    return out;
}

struct CpInfo {
    char32_t    cp;
    std::size_t byte_off;
    std::size_t byte_len;
};

// P4:出参版——复用 caller 的缓冲（clear 保留容量），热路径稳态零分配。
// 多线程并发调用安全（只写 out，无共享状态）；caller 用 thread_local 复用。
inline void to_codepoints(std::string_view text, std::vector<CpInfo>& cps) {
    cps.clear();
    cps.reserve(text.size() / 2);
    std::size_t off = 0;
    while (off < text.size()) {
        // P2.5：ASCII 免解码函数调用（每码点一次调用 + 分支判定，对拉丁/
        // 混合文本是纯开销）。
        const auto b = static_cast<unsigned char>(text[off]);
        if (b < 0x80) {
            cps.push_back({static_cast<char32_t>(b), off, 1});
            ++off;
            continue;
        }
        const auto [cp, consumed] = decode_one(text.substr(off));
        if (consumed == 0) break;
        cps.push_back({cp, off, consumed});
        off += consumed;
    }
}

[[nodiscard]] inline std::vector<CpInfo> to_codepoints(std::string_view text) {
    std::vector<CpInfo> cps;
    to_codepoints(text, cps);
    return cps;
}

// S29-8：nfkc_fold + to_codepoints 融合入口。
//
// 原两段式对 CJK 文本每码点解码**两遍**：nfkc_fold 快路径校验趟逐码点
// decode_one 只为判 nfkc_casefold_inert、产出字节与输入相同（纯拷贝），
// to_codepoints 又对同样的字节全量重解。本函数在校验的同一趟里直接产出
// CpInfo（ASCII 记 tolower 后的码点——与「解码 out」逐位一致；inert 非
// ASCII 原样），快路径命中即省整趟解码。回退慢路径时（ICU 产出与输入不同
// 的字节）行为同旧两段式：归一化后对映射串全量解码。
// 语义契约：(out, cps) 与 `nfkc_fold(input,out); to_codepoints(out,cps)`
// 逐位一致（analyzer_test 黑盒对拍覆盖）。
[[nodiscard]] inline FoldStatus nfkc_fold_codepoints_checked(
    std::string_view input, std::string& out, std::vector<CpInfo>& cps) {
    out.clear();
    cps.clear();
    if (input.empty()) return FoldStatus::kOk;
    cps.reserve(input.size() / 2);

    bool fast = true;
    std::size_t off = 0;
    while (off < input.size()) {
        const auto b = static_cast<unsigned char>(input[off]);
        if (b < 0x80) {
            if (!((b >= 0x20 && b <= 0x7E) ||
                  b == 0x09 || b == 0x0A || b == 0x0D)) {
                fast = false;
                break;
            }
            // out = 原串 tolower ⇒ 码点也记折叠后的（与重解 out 一致）。
            const auto cp = (b >= 'A' && b <= 'Z')
                                ? static_cast<char32_t>(b - 'A' + 'a')
                                : static_cast<char32_t>(b);
            cps.push_back({cp, off, 1});
            ++off;
            continue;
        }
        const auto [cp, consumed] = decode_one(input.substr(off));
        if (consumed == 0 || !nfkc_casefold_inert(cp)) {
            fast = false;
            break;
        }
        cps.push_back({cp, off, consumed});
        off += consumed;
    }
    if (fast) {
        out.assign(input);
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return FoldStatus::kOk;
    }

    // 慢路径：半程 cps 作废，归一化后全量重解（同旧两段式）。
    cps.clear();
    const auto st = nfkc_map_slow(input, out);
    to_codepoints(out, cps);
    return st;
}

inline void nfkc_fold_codepoints(std::string_view input, std::string& out,
                                 std::vector<CpInfo>& cps) {
    (void)nfkc_fold_codepoints_checked(input, out, cps);
}

// S29-8：融合版的 thread_local 复用形态（对齐 to_codepoints_reuse——含同款
// 防膨胀守卫与「同线程不可同时持两份返回引用」约束；normalized 出参由
// caller 持有，term string_view 借其字节）。
[[nodiscard]] inline const std::vector<CpInfo>& nfkc_fold_codepoints_reuse(
    std::string_view text, std::string& normalized_out) {
    thread_local std::vector<CpInfo> tls;
    nfkc_fold_codepoints(text, normalized_out, tls);
    constexpr std::size_t kRetain = 1u << 16;  // 65536 CpInfo
    if (tls.capacity() > kRetain && tls.size() <= kRetain) {
        tls.shrink_to_fit();
    }
    return tls;
}

[[nodiscard]] inline bool is_cjk_punct(char32_t cp) noexcept {
    if (cp >= 0x3000 && cp <= 0x303F) return true;
    if (cp >= 0xFE30 && cp <= 0xFE4F) return true;
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true;
    if (cp >= 0xFF1A && cp <= 0xFF20) return true;
    if (cp >= 0xFF3B && cp <= 0xFF40) return true;
    if (cp >= 0xFF5B && cp <= 0xFF60) return true;
    return false;
}

}  // namespace bitcask::text::detail
