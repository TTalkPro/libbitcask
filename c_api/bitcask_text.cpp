// C API — BM25 文本搜索（S19-5 自 bitcask_c.cpp 拆分，符号与实现不变）。
// 单查询入口统一形态（S20-1 R1）：参数校验 → 委托 C++ 查询 →
// internal.h::finish_single 物化结果/翻译错误。
#include "internal.h"

using namespace bitcask::capi;

namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

extern "C" {


BITCASK_API bitcask_error_t bitcask_search_text(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_text(query, k), out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_text_batch(bitcask_t* cask,
                                                      const char* const* queries,
                                                      size_t n,
                                                      size_t k,
                                                      bitcask_search_result_t*** out_results,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out_results) return BITCASK_ERR_INVALID_OPTION;
    *out_results = nullptr;
    if (n == 0) return BITCASK_OK;
    if (!queries) return BITCASK_ERR_INVALID_OPTION;

    std::vector<std::string_view> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!queries[i]) return BITCASK_ERR_INVALID_OPTION;
        qv.emplace_back(queries[i]);
    }

    // C++ 返回 n 个 expected（每查询一个结果或错误）。
    return fill_batch_results(as_cpp_cask(cask)->search_text_batch(qv, k),
                              n, out_results, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_phrase(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_phrase(query, k), out,
                         fault);
    });
}

BITCASK_API bitcask_error_t bitcask_bool_search(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->bool_search(query, k), out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_fields(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_fields(query, k), out,
                         fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_near(bitcask_t* cask,
                                                   const char* query,
                                                   uint32_t slop,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_near(query, slop, k), out,
                         fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask,
                                                     const char* query,
                                                     size_t k,
                                                     uint32_t max_edit_distance,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(
        as_cpp_cask(cask)->search_fuzzy(query, k, max_edit_distance), out,
        fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_wildcard(bitcask_t* cask,
                                                        const char* pattern,
                                                        size_t k,
                                                        bitcask_search_result_t** out,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !pattern || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_wildcard(pattern, k), out,
                         fault);
    });
}

// S13-D2：带 meta 过滤的检索变体。filter==NULL 退化为无过滤；非法 filter →
// INVALID_OPTION。过滤树在调用期间转换为 C++ MetaFilter（调用返回后 C 侧
// 存储即可释放）。
BITCASK_API bitcask_error_t bitcask_search_text_filtered(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    const auto pf = parse_meta_filter(filter);
    if (!pf.ok) return BITCASK_ERR_INVALID_OPTION;
    return finish_single(as_cpp_cask(cask)->search_text(query, k, pf.get()),
                         out, fault);
    });
}


/* ===========================================================================
 *  S39：分页（offset）与高亮
 * ========================================================================= */

// search_text 的全参版：filter == NULL 且 offset == 0 时与 bitcask_search_text
// 逐字节同义；是既有 text / text_filtered 两个入口的超集。
BITCASK_API bitcask_error_t bitcask_search_text_ex(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    const auto pf = parse_meta_filter(filter);
    if (!pf.ok) return BITCASK_ERR_INVALID_OPTION;
    return finish_single(
        as_cpp_cask(cask)->search_text(query, k, pf.get(), offset), out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_phrase_ex(
    bitcask_t* cask, const char* query, size_t k, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->search_phrase(query, k, offset),
                         out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_bool_search_ex(
    bitcask_t* cask, const char* query, size_t k, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;
    return finish_single(as_cpp_cask(cask)->bool_search(query, k, offset),
                         out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_text_highlight(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_highlight_options_t* opts,
    bitcask_search_result_ex_t** out, bitcask_fault_t* fault) {
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    // opts == NULL 或某字段为 0/NULL → 保留 C++ 侧默认值（不覆盖）。
    // 这样 C 调用方 memset(&opts, 0, ...) 后只填想改的那项即可。
    search::HighlightOptions hopts;
    if (opts) {
        if (opts->pre_tag)  hopts.pre_tag  = opts->pre_tag;
        if (opts->post_tag) hopts.post_tag = opts->post_tag;
        if (opts->fragment_size > 0) hopts.fragment_size = opts->fragment_size;
        if (opts->max_fragments > 0) hopts.max_fragments = opts->max_fragments;
    }
    return finish_single_ex(
        as_cpp_cask(cask)->search_text_highlight(query, k, hopts), out, fault);
    });
}

}  // extern "C"
