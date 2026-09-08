// bitcask_text.h — C API：BM25 文本搜索（S19-5 自 bitcask_c.h 拆分）。

#ifndef BITCASK_TEXT_H
#define BITCASK_TEXT_H

#include "bitcask_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 *  搜索（BM25 全文检索 + HNSW 向量检索 + RRF 混合检索）
 *  需要索引模式（open 时 enable_search = 1 + analyzer_type != NONE）
 * ========================================================================= */

// BM25 词袋搜索。
// query: NUL 结尾的 UTF-8 查询字符串。
// k:     返回 top-k 结果。
// out:   成功时 *out 指向新建结果，调用方需 free。
BITCASK_API bitcask_error_t bitcask_search_text(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault);

// BM25 文本批量搜索（S12-5）。一次 prepare_search flush 覆盖全批，比逐条调用省重复
// 索引 flush；语义/可见性同 bitcask_search_text（并发读安全）。
//   queries : n 个 NUL 结尾查询串的数组（n==0 时 *out_results 置 NULL 并返回 OK）
//   out_results : 成功时 *out_results 指向 malloc 的 n 元数组，out_results[i] 为第 i 个
//                 查询的结果指针（该查询失败则为 NULL）。用 bitcask_search_result_batch_free
//                 (*out_results, n) 释放整个数组。
//   fault   : 若有查询失败，回填首个失败查询的错误详情（best-effort 诊断）。
// 返回：BITCASK_OK（批量已执行；单查询错误体现为对应元素 NULL）；
//       BITCASK_ERR_INVALID_OPTION（cask/out_results 为空，或某 query 指针为空）。
BITCASK_API bitcask_error_t bitcask_search_text_batch(bitcask_t* cask,
                                                         const char* const* queries,
                                                         size_t n,
                                                         size_t k,
                                                         bitcask_search_result_t*** out_results,
                                                         bitcask_fault_t* fault);

// 释放 bitcask_search_text_batch 分配的结果数组（逐个 result_free 后释放数组本身）。
// results 为 NULL 时是 no-op。
BITCASK_API void bitcask_search_result_batch_free(bitcask_search_result_t** results,
                                                     size_t n);

// BM25 短语搜索。
BITCASK_API bitcask_error_t bitcask_search_phrase(bitcask_t* cask,
                                                     const char* query,
                                                     size_t k,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault);

// BM25 布尔搜索。
BITCASK_API bitcask_error_t bitcask_bool_search(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault);

// BM25 多字段搜索（field:term^boost 语法）。
BITCASK_API bitcask_error_t bitcask_search_fields(bitcask_t* cask,
                                                     const char* query,
                                                     size_t k,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault);

// BM25 近邻搜索。
// slop: term 间允许的最大间隙（0 = 短语）。
BITCASK_API bitcask_error_t bitcask_search_near(bitcask_t* cask,
                                                   const char* query,
                                                   uint32_t slop,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault);

// BM25 模糊搜索（Levenshtein 编辑距离）。
BITCASK_API bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask,
                                                    const char* query,
                                                    size_t k,
                                                    uint32_t max_edit_distance,
                                                    bitcask_search_result_t** out,
                                                    bitcask_fault_t* fault);

// BM25 通配符搜索（* 和 ? 模式匹配）。
BITCASK_API bitcask_error_t bitcask_search_wildcard(bitcask_t* cask,
                                                       const char* pattern,
                                                       size_t k,
                                                       bitcask_search_result_t** out,
                                                       bitcask_fault_t* fault);

/* 带 meta 过滤的检索变体（S13-D2）。filter==NULL 等价于无过滤版本；
 * filter 非法（key 为 NULL、STRING 值缺 str、嵌套深度 > 32、op/type 越界）
 * → BITCASK_ERR_INVALID_OPTION。其余语义与同名无 _filtered 函数一致。
 * 注意：filter 非空时**没有 meta 段的文档一律不通过**（引擎「空 blob 不通过」
 * 约定，与 C++ MetaFilter 行为一致）——含 Neq/Exists 等否定式条件。 */
BITCASK_API bitcask_error_t bitcask_search_text_filtered(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

/* ===========================================================================
 *  分页（S39）——C++ 侧 search_text / search_phrase / bool_search 一直有
 *  offset 参数（S13-D10），C 侧此前三个函数全无，纯 C 调用方做不了分页。
 *
 *  additive 新符号，既有四个函数签名零改动。
 *
 *  offset = 跳过排名前 offset 条。实现是 overfetch k+offset 后截断——**深分页
 *  成本线性增长**，offset 很大时应改用别的方案（如按 score/ord 游标）。
 *  另：本 API **不提供总命中数**——WAND/BMW 剪枝下 total 只能给下界，误导大于
 *  价值（C++ 侧同样不给，见 cask.hpp S13-D10 注）。
 * ========================================================================= */

/* search_text 的全参版：filter 与 offset 都能给。filter == NULL 等价无过滤，
 * offset == 0 等价 bitcask_search_text——即本函数是既有两个 text 入口的超集。
 * 注意 filter 非空时**没有 meta 段的文档一律不通过**（同 *_filtered）。 */
BITCASK_API bitcask_error_t bitcask_search_text_ex(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

/* 短语/布尔搜索的分页版（C++ 侧这两个没有 filter 形参，故只加 offset）。 */
BITCASK_API bitcask_error_t bitcask_search_phrase_ex(
    bitcask_t* cask, const char* query, size_t k, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

BITCASK_API bitcask_error_t bitcask_bool_search_ex(
    bitcask_t* cask, const char* query, size_t k, size_t offset,
    bitcask_search_result_t** out, bitcask_fault_t* fault);

/* ===========================================================================
 *  高亮检索（S39）——补上 C++ Cask::search_text_highlight（S13-D3）的 C 表示
 * ========================================================================= */

/* BM25 词袋搜索 + 高亮片段。语义同 bitcask_search_text，额外为每条命中生成
 * 片段（结果类型/所有权/LRU 降级语义见 bitcask_kv.h 的 bitcask_search_result_ex_t）。
 *   opts : NULL = 全默认（<em>/</em>、fragment_size=120、max_fragments=3）
 *   out  : 成功时 *out 指向新建结果，调用方须调 bitcask_search_result_ex_free
 * 线程安全：是（并发读安全，同 bitcask_search_text）。 */
BITCASK_API bitcask_error_t bitcask_search_text_highlight(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_highlight_options_t* opts,
    bitcask_search_result_ex_t** out, bitcask_fault_t* fault);

#ifdef __cplusplus
} // extern "C"
#endif


#endif // BITCASK_TEXT_H
