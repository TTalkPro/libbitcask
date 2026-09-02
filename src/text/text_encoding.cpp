#include "bitcask/text_encoding.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>

namespace bitcask::text {

namespace {

struct ConverterCloser {
    void operator()(UConverter* c) const noexcept {
        if (c != nullptr) ucnv_close(c);
    }
};
using ConverterPtr = std::unique_ptr<UConverter, ConverterCloser>;

[[nodiscard]] ConverterPtr open_converter(const char* encoding,
                                          UErrorCode& ec) noexcept {
    ec = U_ZERO_ERROR;
    return ConverterPtr{ucnv_open(encoding, &ec)};
}

// ICU 转换器的默认回调是 SUBSTITUTE：坏字节静默变 U+FFFD（或目标编码的
// 替换字符），调用方拿不到任何信号。严格模式下换成 STOP，让 ec 带出
// U_INVALID_CHAR_FOUND / U_TRUNCATED_CHAR_FOUND / U_ILLEGAL_CHAR_FOUND。
void set_callbacks(UConverter* cv, bool lenient, bool to_unicode,
                   UErrorCode& ec) noexcept {
    if (lenient) return;  // 保留 ICU 默认的 SUBSTITUTE
    if (to_unicode) {
        ucnv_setToUCallBack(cv, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr,
                            nullptr, &ec);
    } else {
        ucnv_setFromUCallBack(cv, UCNV_FROM_U_CALLBACK_STOP, nullptr, nullptr,
                              nullptr, &ec);
    }
}

[[nodiscard]] bool is_malformed(UErrorCode ec) noexcept {
    return ec == U_INVALID_CHAR_FOUND || ec == U_TRUNCATED_CHAR_FOUND ||
           ec == U_ILLEGAL_CHAR_FOUND || ec == U_ILLEGAL_ESCAPE_SEQUENCE ||
           ec == U_UNSUPPORTED_ESCAPE_SEQUENCE;
}

// ICU 的 C API 长度参数是 int32_t。超过就直接拒，不做分块——分块会在多字节
// 序列中间切断，是另一类静默错。
[[nodiscard]] bool fits_int32(std::size_t n) noexcept {
    return n <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
}

}  // namespace

const char* to_string(TranscodeStatus st) noexcept {
    switch (st) {
        case TranscodeStatus::kOk:              return "ok";
        case TranscodeStatus::kUnknownEncoding: return "unknown encoding";
        case TranscodeStatus::kMalformedInput:  return "malformed input";
        case TranscodeStatus::kTooLarge:        return "input too large";
        case TranscodeStatus::kIcuError:        return "icu error";
    }
    return "unknown";
}

bool encoding_supported(const char* encoding) noexcept {
    if (encoding == nullptr) return false;
    UErrorCode ec = U_ZERO_ERROR;
    auto cv = open_converter(encoding, ec);
    return U_SUCCESS(ec) && cv != nullptr;
}

std::string canonical_encoding_name(const char* encoding) {
    if (encoding == nullptr) return {};
    UErrorCode ec = U_ZERO_ERROR;
    auto cv = open_converter(encoding, ec);
    if (U_FAILURE(ec) || cv == nullptr) return {};
    ec = U_ZERO_ERROR;
    const char* name = ucnv_getName(cv.get(), &ec);
    if (U_FAILURE(ec) || name == nullptr) return {};
    return std::string(name);
}

TranscodeStatus transcode_to_utf8(std::string_view input, const char* encoding,
                                  std::string& out, bool lenient) {
    out.clear();
    if (!fits_int32(input.size())) return TranscodeStatus::kTooLarge;

    UErrorCode ec = U_ZERO_ERROR;
    auto cv = open_converter(encoding, ec);
    if (U_FAILURE(ec) || cv == nullptr) return TranscodeStatus::kUnknownEncoding;
    if (input.empty()) return TranscodeStatus::kOk;

    set_callbacks(cv.get(), lenient, /*to_unicode=*/true, ec);
    if (U_FAILURE(ec)) return TranscodeStatus::kIcuError;

    const auto src_len = static_cast<std::int32_t>(input.size());

    // 第一步：<encoding> → UTF-16。先 preflight 问长度（ICU 的约定：目标容量
    // 传 0，它把所需长度写进返回值并置 U_BUFFER_OVERFLOW_ERROR）。
    ec = U_ZERO_ERROR;
    const auto u16_len =
        ucnv_toUChars(cv.get(), nullptr, 0, input.data(), src_len, &ec);
    if (is_malformed(ec)) return TranscodeStatus::kMalformedInput;
    if (ec != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) {
        return TranscodeStatus::kIcuError;
    }
    if (u16_len < 0) return TranscodeStatus::kIcuError;

    std::vector<UChar> u16(static_cast<std::size_t>(u16_len) + 1);
    ec = U_ZERO_ERROR;
    // preflight 之后转换器内部状态要复位，否则续着上一趟的状态解码。
    ucnv_resetToUnicode(cv.get());
    const auto u16_written = ucnv_toUChars(cv.get(), u16.data(), u16_len + 1,
                                           input.data(), src_len, &ec);
    if (is_malformed(ec)) return TranscodeStatus::kMalformedInput;
    if (U_FAILURE(ec)) return TranscodeStatus::kIcuError;

    // 第二步：UTF-16 → UTF-8。u_strToUTF8 对孤立代理会报错（源自非法输入时
    // 已被上一步拦住，这里是纯保险）。
    ec = U_ZERO_ERROR;
    std::int32_t u8_len = 0;
    u_strToUTF8(nullptr, 0, &u8_len, u16.data(), u16_written, &ec);
    if (ec != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) {
        return TranscodeStatus::kIcuError;
    }
    if (u8_len < 0) return TranscodeStatus::kIcuError;

    out.resize(static_cast<std::size_t>(u8_len));
    ec = U_ZERO_ERROR;
    u_strToUTF8(out.data(), u8_len, nullptr, u16.data(), u16_written, &ec);
    if (U_FAILURE(ec)) {
        out.clear();
        return TranscodeStatus::kIcuError;
    }
    return TranscodeStatus::kOk;
}

TranscodeStatus transcode_from_utf8(std::string_view utf8, const char* encoding,
                                    std::string& out, bool lenient) {
    out.clear();
    if (!fits_int32(utf8.size())) return TranscodeStatus::kTooLarge;

    UErrorCode ec = U_ZERO_ERROR;
    auto cv = open_converter(encoding, ec);
    if (U_FAILURE(ec) || cv == nullptr) return TranscodeStatus::kUnknownEncoding;
    if (utf8.empty()) return TranscodeStatus::kOk;

    set_callbacks(cv.get(), lenient, /*to_unicode=*/false, ec);
    if (U_FAILURE(ec)) return TranscodeStatus::kIcuError;

    const auto src_len = static_cast<std::int32_t>(utf8.size());

    // UTF-8 → UTF-16。非法 UTF-8 在这里就会被 u_strFromUTF8 拒掉。
    ec = U_ZERO_ERROR;
    std::int32_t u16_len = 0;
    u_strFromUTF8(nullptr, 0, &u16_len, utf8.data(), src_len, &ec);
    if (ec == U_INVALID_CHAR_FOUND || ec == U_TRUNCATED_CHAR_FOUND ||
        ec == U_ILLEGAL_CHAR_FOUND) {
        return TranscodeStatus::kMalformedInput;
    }
    if (ec != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) {
        return TranscodeStatus::kIcuError;
    }
    if (u16_len < 0) return TranscodeStatus::kIcuError;

    std::vector<UChar> u16(static_cast<std::size_t>(u16_len) + 1);
    ec = U_ZERO_ERROR;
    u_strFromUTF8(u16.data(), u16_len + 1, nullptr, utf8.data(), src_len, &ec);
    if (ec == U_INVALID_CHAR_FOUND || ec == U_TRUNCATED_CHAR_FOUND ||
        ec == U_ILLEGAL_CHAR_FOUND) {
        return TranscodeStatus::kMalformedInput;
    }
    if (U_FAILURE(ec)) return TranscodeStatus::kIcuError;

    // UTF-16 → <encoding>。
    ec = U_ZERO_ERROR;
    const auto need = ucnv_fromUChars(cv.get(), nullptr, 0, u16.data(), u16_len, &ec);
    if (is_malformed(ec)) return TranscodeStatus::kMalformedInput;
    if (ec != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(ec)) {
        return TranscodeStatus::kIcuError;
    }
    if (need < 0) return TranscodeStatus::kIcuError;

    std::vector<char> buf(static_cast<std::size_t>(need) + 1);
    ec = U_ZERO_ERROR;
    ucnv_resetFromUnicode(cv.get());
    const auto written =
        ucnv_fromUChars(cv.get(), buf.data(), need + 1, u16.data(), u16_len, &ec);
    if (is_malformed(ec)) return TranscodeStatus::kMalformedInput;
    if (U_FAILURE(ec) || written < 0) return TranscodeStatus::kIcuError;

    out.assign(buf.data(), static_cast<std::size_t>(written));
    return TranscodeStatus::kOk;
}

}  // namespace bitcask::text
