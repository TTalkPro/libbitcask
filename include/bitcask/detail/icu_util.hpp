// ICU 接入层（S38）——全库唯一直接摸 ICU C++ API 的地方。
//
// 为什么要这一层：
//   1. `Normalizer2` 实例的获取要传 UErrorCode 且要缓存（每次 get 都走 ICU
//      内部的 mutex + hash 查表）。集中在这里做一次函数局部 static。
//   2. ICU 的 `normalizeUTF8` 对**非法 UTF-8 不报错**——实测它原样透传字节
//      （既不是替换成 U+FFFD，也不是返回 U_INVALID_CHAR_FOUND）。所以「输入
//      是否合法 UTF-8」必须由我们自己判，不能指望库。这一层把该语义封死。
//   3. ICU 头很重且用大量 C 风格宏；限制在少数 TU 里包含，编译时间可控。
//
// 线程安全：本文件所有函数都是无共享可变状态的纯函数（`nfkc_cf_instance()`
// 返回的是 ICU 自己持有的不可变单例，ICU 保证其 const 方法线程安全）。

#pragma once

#include <string>
#include <string_view>

#include <unicode/bytestream.h>
#include <unicode/normalizer2.h>
#include <unicode/stringpiece.h>
#include <unicode/uchar.h>
#include <unicode/utypes.h>
#include <unicode/uversion.h>

namespace bitcask::text::detail {

// NFKC_Casefold 归一化器。ICU 的 `nfkc_cf` 与 utf8proc 的
// STABLE|COMPOSE|COMPAT|CASEFOLD|IGNORE 语义等价（同为 Unicode 16.0 时全 BMP
// + SMP 抽样共 79840 码点对拍，差异仅 3769 个**未分配且 Default_Ignorable**
// 的码点：ICU 按 NFKC_Casefold 定义删除，utf8proc 保留——ICU 更贴标准）。
//
// 返回 nullptr 仅在 ICU 数据缺失时发生（静态 data 构建下不可能；动态 data
// 下是 icudt*.dat 找不到）。调用方必须判空——这不是"理论上的错误分支"，
// 它是 vendored/系统 ICU 装歪时的唯一症状。
[[nodiscard]] inline const icu::Normalizer2* nfkc_cf_instance() noexcept {
    static const icu::Normalizer2* const kInstance = [] () noexcept
        -> const icu::Normalizer2* {
        UErrorCode ec = U_ZERO_ERROR;
        const auto* n = icu::Normalizer2::getNFKCCasefoldInstance(ec);
        return U_SUCCESS(ec) ? n : nullptr;
    }();
    return kInstance;
}

// NFKC_Casefold 归一化。**要求 input 已是合法 UTF-8**（调用方保证；见
// text_utils.hpp 的 validate/sanitize）。返回 false = ICU 侧失败（数据缺失或
// 内存不足），此时 out 为空。
//
// 用 normalizeUTF8 而非 normalize：后者只吃 UTF-16，我们全库是 UTF-8，走它
// 等于每次多两趟 u_strFromUTF8/u_strToUTF8 全串转换。normalizeUTF8 起于 ICU 60
// （BitcaskICU.cmake 里的最低版本要求即由此而来）。
[[nodiscard]] inline bool icu_nfkc_casefold(std::string_view input,
                                            std::string& out) {
    out.clear();
    const auto* n2 = nfkc_cf_instance();
    if (n2 == nullptr) return false;

    UErrorCode ec = U_ZERO_ERROR;
    icu::StringByteSink<std::string> sink(&out);
    n2->normalizeUTF8(0,
                      icu::StringPiece(input.data(),
                                       static_cast<std::int32_t>(input.size())),
                      sink, nullptr, ec);
    if (U_FAILURE(ec)) {
        out.clear();
        return false;
    }
    return true;
}

// 码点是否 NFKC_Casefold 恒等且不参与重排（即 gen_inert_table 的 "inert"）。
// 供表生成器与测试的 oracle 使用；热路径走生成出来的区间表，不调这里。
[[nodiscard]] inline bool icu_is_nfkc_cf_inert(char32_t cp) {
    const auto* n2 = nfkc_cf_instance();
    if (n2 == nullptr) return false;
    // ICU 的 isInert 恰好就是我们要的三条：归一化恒等 + 不与前后重排
    // （含 ccc == 0）+ 不是可组合的起始字符。
    return n2->isInert(static_cast<UChar32>(cp)) != 0;
}

// 归一化数据的来源版本，写进生成头的注释里做溯源。
[[nodiscard]] inline const char* icu_version() noexcept { return U_ICU_VERSION; }
[[nodiscard]] inline const char* icu_unicode_version() noexcept {
    return U_UNICODE_VERSION;
}

}  // namespace bitcask::text::detail
