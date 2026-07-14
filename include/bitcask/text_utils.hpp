#pragma once

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <utf8proc.h>

#include "bitcask/detail/inert_table.hpp"

namespace bitcask::text::detail {

struct Utf8ProcDeleter {
    void operator()(void* p) const noexcept { std::free(p); }
};
using Utf8ProcBuf = std::unique_ptr<uint8_t[], Utf8ProcDeleter>;

[[nodiscard]] inline std::pair<char32_t, std::size_t> decode_one(
    std::string_view sv) noexcept {
    if (sv.empty()) return {0, 0};

    auto* ptr = reinterpret_cast<const utf8proc_uint8_t*>(sv.data());
    auto len = static_cast<utf8proc_ssize_t>(sv.size());

    utf8proc_int32_t cp = 0;
    auto consumed = utf8proc_iterate(ptr, len, &cp);
    if (consumed < 0 || cp < 0) return {0xFFFD, 1};
    return {static_cast<char32_t>(cp), static_cast<std::size_t>(consumed)};
}

// 慢路径：utf8proc_map 全量 NFKC_Casefold（快路径未命中时的回退）。
// utf8proc_map 接受显式长度（utf8proc_NFKC_Casefold 即它加 NULLTERM 的
// 包装）——免去此前「输入拷贝求 null 终止」与「输出 strlen」两次全串遍历。
// 行为差异仅在含内嵌 \0 的输入：旧版在 \0 截断，本版处理全长（更正确）。
inline void nfkc_map_slow(std::string_view input, std::string& out) {
    utf8proc_uint8_t* mapped = nullptr;
    auto n = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(input.data()),
        static_cast<utf8proc_ssize_t>(input.size()), &mapped,
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE |
                                       UTF8PROC_COMPAT | UTF8PROC_CASEFOLD |
                                       UTF8PROC_IGNORE));
    if (n < 0 || mapped == nullptr) return;  // out 已 clear
    Utf8ProcBuf guard(mapped);
    out.assign(reinterpret_cast<const char*>(mapped),
               static_cast<std::size_t>(n));
}

// P6:出参版——写入 caller 缓冲（clear 保留容量），热路径稳态零分配。
// caller 用 thread_local 复用（如 jieba 逐词归一化）。语义与返回值版完全一致。
inline void nfkc_fold(std::string_view input, std::string& out) {
    out.clear();
    if (input.empty()) return;

    // P2.5/P2.5b 统一快路径：全部码点 ∈（NFKC_Casefold 恒等区段 ∪ ASCII）
    // 时，整个变换等价于「原串 + ASCII 字节 tolower」——纯 ASCII 文本与
    // 「中文 + 半角英文/标点」文本都命中，跳过整条 utf8proc 流水线。
    // ASCII 的 tolower 可安全按字节做：UTF-8 多字节序列的所有字节 ≥ 0x80，
    // 不会误伤。含全角标点（，：！？等会被 NFKC 折叠）即整串回退。
    // 语义对拍见 analyzer_test（穷举表成员 + 随机串黑盒 vs utf8proc）。
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
            auto [cp, consumed] = decode_one(input.substr(off));
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
        return;
    }

    nfkc_map_slow(input, out);
}

[[nodiscard]] inline std::string nfkc_fold(std::string_view input) {
    std::string out;
    nfkc_fold(input, out);
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
        // P2.5：ASCII 免 utf8proc_iterate 库调用（每码点一次函数调用 +
        // 分支判定，对拉丁/混合文本是纯开销）。
        const auto b = static_cast<unsigned char>(text[off]);
        if (b < 0x80) {
            cps.push_back({static_cast<char32_t>(b), off, 1});
            ++off;
            continue;
        }
        auto [cp, consumed] = decode_one(text.substr(off));
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
// ASCII 原样），快路径命中即省整趟解码。回退慢路径时（utf8proc_map 产出
// 与输入不同的字节）行为同旧两段式：map 后对映射串全量解码。
// 语义契约：(out, cps) 与 `nfkc_fold(input,out); to_codepoints(out,cps)`
// 逐位一致（analyzer_test 黑盒对拍覆盖）。
inline void nfkc_fold_codepoints(std::string_view input, std::string& out,
                                 std::vector<CpInfo>& cps) {
    out.clear();
    cps.clear();
    if (input.empty()) return;
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
        auto [cp, consumed] = decode_one(input.substr(off));
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
        return;
    }

    // 慢路径：半程 cps 作废，map 后全量重解（同旧两段式）。
    cps.clear();
    nfkc_map_slow(input, out);
    to_codepoints(out, cps);
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
