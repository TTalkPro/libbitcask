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

    // 非 ASCII：utf8proc_map 接受显式长度（utf8proc_NFKC_Casefold 即
    // 它加 NULLTERM 的包装）——免去此前「输入拷贝求 null 终止」与
    // 「输出 strlen」两次全串遍历。选项与 NFKC_Casefold 完全一致。
    // 行为差异仅在含内嵌 \0 的输入：旧版在 \0 截断，本版处理全长（更正确）。
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

// P4:thread_local 复用版——返回对每线程复用缓冲的引用，分词热路径稳态零分配。
// 并发安全（thread_local 每线程独立；S3 恢复的 parallel analyze 各线程互不干扰）。
// ⚠️ 同一线程不可同时持有两份返回引用（会别名同一缓冲）——分词器每次只用一份。
[[nodiscard]] inline const std::vector<CpInfo>& to_codepoints_reuse(
    std::string_view text) {
    thread_local std::vector<CpInfo> tls;
    to_codepoints(text, tls);
    // 防膨胀:一次超大文本后不让缓冲长期占住线程内存（对齐 read_buf 策略）。
    // size 是本次内容、capacity 是历史峰值；峰值远超阈值且本次很小才回收。
    constexpr std::size_t kRetain = 1u << 16;  // 65536 CpInfo
    if (tls.capacity() > kRetain && tls.size() <= kRetain) {
        tls.shrink_to_fit();  // 收到 size()（≤kRetain），内容保留
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
