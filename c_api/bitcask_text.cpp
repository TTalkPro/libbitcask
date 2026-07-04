// C API — BM25 文本搜索（S19-5 自 bitcask_c.cpp 拆分，符号与实现不变）。
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

    auto result = as_cpp_cask(cask)->search_text(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->search_phrase(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->bool_search(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->search_fields(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->search_near(query, slop, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->search_fuzzy(query, k, max_edit_distance);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->search_wildcard(pattern, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
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

    meta::MetaFilter mf;
    if (filter && !to_cpp_meta_filter(*filter, mf, 0)) {
        return BITCASK_ERR_INVALID_OPTION;
    }
    auto result = as_cpp_cask(cask)->search_text(query, k,
                                                 filter ? &mf : nullptr);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

}  // extern "C"
