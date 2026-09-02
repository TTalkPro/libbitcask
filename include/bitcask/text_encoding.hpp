// 文本编码转换（S38）——把非 UTF-8 的输入转成库内部统一的 UTF-8。
//
// 为什么需要它：索引层全程假定 UTF-8（分词、归一化、highlight 的 byte offset
// 都以 UTF-8 码点边界为准）。喂进 GB18030 之类的字节，在 S38 之前是**静默
// 失败**——存得进去，一条也搜不出来。现在入口能校验（text_utils.hpp 的
// validate_utf8），而这里提供把它转对的手段。
//
// 编码名用 ICU 的转换器名或别名，大小写与连字符不敏感：
//   "GB18030"（→ 规范名 gb18030-2022）、"GBK"、"windows-936"、"Big5"、
//   "Shift_JIS"、"EUC-KR"、"windows-1252"、"ISO-8859-1" ……
// 用 encoding_supported() 先探测，或直接看返回的 kUnknownEncoding。
//
// 错误策略：**遇非法字节即失败，不替换**。ICU 转换器的默认回调是
// SUBSTITUTE（把坏字节悄悄变成 U+FFFD），本层显式改成 STOP —— 静默替换正是
// S38 要根除的那类失败：调用方以为转成功了，实际拿到一串问号。要宽容语义
// 请显式传 lenient=true。
//
// 线程安全：全部是纯函数，每次调用自开自关 UConverter（ICU 的 UConverter
// **不是**线程安全对象，不能跨线程共享，故不做缓存）。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bitcask::text {

enum class TranscodeStatus : std::uint8_t {
    kOk,               // 转换成功，out 是合法 UTF-8
    kUnknownEncoding,  // ICU 不认识这个编码名；out 为空
    kMalformedInput,   // 输入含该编码下的非法字节序列；out 为空（lenient 下不会出现）
    kTooLarge,         // 输入或输出超过 INT32_MAX（ICU C API 的长度上限）；out 为空
    kIcuError,         // 其它 ICU 失败（OOM 等）；out 为空
};

[[nodiscard]] const char* to_string(TranscodeStatus st) noexcept;

// ICU 是否支持该编码名。
[[nodiscard]] bool encoding_supported(const char* encoding) noexcept;

// ICU 为该编码名解析出的规范名（如 "GB18030" → "gb18030-2022"）。
// 不支持时返回空串。用于日志/元数据里记录"到底用了哪个转换器"。
[[nodiscard]] std::string canonical_encoding_name(const char* encoding);

// <encoding> → UTF-8。
// lenient=false（默认）：遇非法字节返回 kMalformedInput，out 清空。
// lenient=true：非法字节替换为 U+FFFD，返回 kOk。
[[nodiscard]] TranscodeStatus transcode_to_utf8(std::string_view input,
                                                const char* encoding,
                                                std::string& out,
                                                bool lenient = false);

// UTF-8 → <encoding>。目标编码表示不了的码点按 lenient 处理（false 即失败，
// true 用该编码的替换字符）。
[[nodiscard]] TranscodeStatus transcode_from_utf8(std::string_view utf8,
                                                  const char* encoding,
                                                  std::string& out,
                                                  bool lenient = false);

}  // namespace bitcask::text
