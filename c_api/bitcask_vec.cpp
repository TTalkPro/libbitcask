// C API — 向量/混合检索（S19-5 自 bitcask_c.cpp 拆分，符号与实现不变）。
// 单查询入口统一形态（S20-1 R1）：参数校验 → 委托 C++ 查询 →
// internal.h::finish_single 物化结果/翻译错误。
#include "internal.h"

using namespace bitcask::capi;

namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

extern "C" {


BITCASK_API bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                                      const float* query,
                                                      size_t query_len,
                                                      size_t k,
                                                      size_t ef,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::span<const float> query_span{query, query_len};
    return finish_single(as_cpp_cask(cask)->search_vector(query_span, k, ef),
                         out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                                      const char* text_query,
                                                      const float* vec_query,
                                                      size_t vec_len,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::string_view text_sv;
    if (text_query) text_sv = text_query;
    std::span<const float> vec_span;
    if (vec_query && vec_len > 0) vec_span = {vec_query, vec_len};

    return finish_single(as_cpp_cask(cask)->search_hybrid(text_sv, vec_span, k),
                         out, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_vector_batch(bitcask_t* cask,
                                                        const float* const* queries,
                                                        size_t n,
                                                        size_t query_len,
                                                        size_t k,
                                                        size_t ef,
                                                        bitcask_search_result_t*** out_results,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out_results) return BITCASK_ERR_INVALID_OPTION;
    *out_results = nullptr;
    if (n == 0) return BITCASK_OK;
    if (!queries) return BITCASK_ERR_INVALID_OPTION;

    std::vector<std::span<const float>> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!queries[i] && query_len > 0) return BITCASK_ERR_INVALID_OPTION;
        qv.emplace_back(queries[i], query_len);
    }
    return fill_batch_results(as_cpp_cask(cask)->search_vector_batch(qv, k, ef),
                              n, out_results, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid_batch(bitcask_t* cask,
                                                        const bitcask_hybrid_query_t* queries,
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

    std::vector<bitcask::Cask::HybridQuery> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        bitcask::Cask::HybridQuery hq;
        if (queries[i].text) hq.text = queries[i].text;
        if (queries[i].vector && queries[i].vector_len > 0) {
            hq.vec = std::span<const float>(queries[i].vector, queries[i].vector_len);
        }
        qv.push_back(hq);
    }
    return fill_batch_results(as_cpp_cask(cask)->search_hybrid_batch(qv, k),
                              n, out_results, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_vector_filtered(
    bitcask_t* cask, const float* query, size_t query_len, size_t k, size_t ef,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    const auto pf = parse_meta_filter(filter);
    if (!pf.ok) return BITCASK_ERR_INVALID_OPTION;
    std::span<const float> query_span{query, query_len};
    return finish_single(
        as_cpp_cask(cask)->search_vector(query_span, k, ef, pf.get()), out,
        fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid_filtered(
    bitcask_t* cask, const char* text_query,
    const float* vec_query, size_t vec_len, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    const auto pf = parse_meta_filter(filter);
    if (!pf.ok) return BITCASK_ERR_INVALID_OPTION;
    std::string_view text_sv;
    if (text_query) text_sv = text_query;
    std::span<const float> vec_span;
    if (vec_query && vec_len > 0) vec_span = {vec_query, vec_len};

    return finish_single(
        as_cpp_cask(cask)->search_hybrid(text_sv, vec_span, k, pf.get()), out,
        fault);
    });
}

}  // extern "C"
