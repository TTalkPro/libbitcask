// bitcask_vec.h — C API：向量/RRF 混合检索（S19-5 自 bitcask_c.h 拆分）。

#ifndef BITCASK_VEC_H
#define BITCASK_VEC_H

#include "bitcask_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 *  向量检索（HNSW）与 RRF 混合检索
 * ========================================================================= */


// HNSW 向量搜索。
// query: f32 向量数组，query_len 必须等于 vector_dim。
// ef:    搜索时探索宽度（0 = max(k, 64)）。
BITCASK_API bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                                     const float* query,
                                                     size_t query_len,
                                                     size_t k,
                                                     size_t ef,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault);

// HNSW 向量批量搜索（S12-5）。一次 flush 覆盖全批，语义同 bitcask_search_vector。
//   queries    : n 个向量指针数组，每个指向 query_len 个 f32（query_len 须等于 vector_dim）
//   out_results: 见 bitcask_search_text_batch；用 bitcask_search_result_batch_free 释放
// 返回同 bitcask_search_text_batch（单查询失败体现为对应 NULL）。
BITCASK_API bitcask_error_t bitcask_search_vector_batch(bitcask_t* cask,
                                                           const float* const* queries,
                                                           size_t n,
                                                           size_t query_len,
                                                           size_t k,
                                                           size_t ef,
                                                           bitcask_search_result_t*** out_results,
                                                           bitcask_fault_t* fault);

// RRF 混合检索（BM25 + 向量融合）。
// text_query: NUL 结尾 UTF-8（NULL = 纯向量路径）。
// vec_query:  f32 向量数组（NULL = 纯文本路径）。
BITCASK_API bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                                     const char* text_query,
                                                     const float* vec_query,
                                                     size_t vec_len,
                                                     size_t k,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault);

// 混合批量查询的单条输入：text 与 vector 至少一非空（纯文本 / 纯向量 / 两路融合）。
typedef struct {
    const char*  text;        // NUL 结尾 UTF-8（NULL = 纯向量）
    const float* vector;      // f32 向量（NULL = 纯文本）
    size_t       vector_len;  // 向量元素数（vector==NULL 时忽略）
} bitcask_hybrid_query_t;

// RRF 混合批量检索（S12-5）。一次 flush 覆盖全批，语义同 bitcask_search_hybrid。
//   queries    : n 条 (text, vector) 查询
//   out_results: 见 bitcask_search_text_batch；用 bitcask_search_result_batch_free 释放
BITCASK_API bitcask_error_t bitcask_search_hybrid_batch(bitcask_t* cask,
                                                           const bitcask_hybrid_query_t* queries,
                                                           size_t n,
                                                           size_t k,
                                                           bitcask_search_result_t*** out_results,
                                                           bitcask_fault_t* fault);


// 在给定的一组 key 里按向量相似度排序取前 k（S67，对应 Cask::rank_by_vector）。
// 精确计算、不经向量索引、不需要先 flush；分数口径同 bitcask_search_vector，降序。
//   keys / key_lens：n_keys 个候选 key 的指针与长度；不存在的 key、没有向量的文档跳过，
//   重复的 key 只算一次。结果用 bitcask_search_result_free 释放。
// query_len 必须等于 vector_dim；库没有向量配置 → BITCASK_ERR_INVALID_OPTION。
BITCASK_API bitcask_error_t bitcask_vector_rank(
    bitcask_t* cask, const float* query, size_t query_len,
    const void* const* keys, const size_t* key_lens, size_t n_keys, size_t k,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

BITCASK_API bitcask_error_t bitcask_search_vector_filtered(
    bitcask_t* cask, const float* query, size_t query_len, size_t k, size_t ef,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

BITCASK_API bitcask_error_t bitcask_search_hybrid_filtered(
    bitcask_t* cask, const char* text_query,
    const float* vec_query, size_t vec_len, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

#ifdef __cplusplus
} // extern "C"
#endif


#endif // BITCASK_VEC_H
