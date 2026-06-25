// libbitcask C API — 提供 C 语言 ABI 接口，用于 .so 动态库跨语言绑定。
//
// 设计原则：
//   - 不透明句柄 (opaque handle)：C 侧持有指针，不知道内部布局
//   - 显式内存管理：每个 alloc 的结果有配对的 free 函数
//   - 错误码 + out-param：函数返回错误码，详情经 bitcask_fault_t* 传出
//   - 二进制安全：使用 {ptr, len} 切片，不依赖 NUL 结尾
//
// 线程安全（与 C++ 核心一致）：
//   - open/close/factory：线程安全（产生/销毁独立对象）
//   - get/search_vector/search_hybrid：线程安全（读路径）
//   - put/delete/sync/search_text/search_phrase/search_fields/search_near/
//     search_fuzzy/search_wildcard/bool_search：非线程安全（caller 串行化）
//   - iter_*：非线程安全（同一 iter 不可并发使用）
//
// 用法示例：
//   bitcask_options_t opts;
//   bitcask_options_init(&opts);
//   opts.read_write = 1;
//
//   bitcask_t* cask = NULL;
//   bitcask_fault_t fault;
//   if (bitcask_open("/tmp/db", &opts, &cask, &fault) != BITCASK_OK) {
//       fprintf(stderr, "open failed: %s\n", fault.detail);
//       return 1;
//   }
//   bitcask_slice_t key = {"hello", 5};
//   bitcask_slice_t val = {"world", 5};
//   bitcask_put(cask, key, val, 0, NULL);
//
//   bitcask_get_result_t* result = NULL;
//   if (bitcask_get(cask, key, &result, NULL) == BITCASK_OK) {
//       printf("value: %.*s\n", (int)result->value.size, (char*)result->value.data);
//       bitcask_get_result_free(result);
//   }
//   bitcask_close(cask);

#ifndef BITCASK_C_H
#define BITCASK_C_H

#include <stddef.h>   // size_t
#include <stdint.h>   // uint32_t, uint64_t, int32_t

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 *  符号导出宏
 * ========================================================================= */
#if defined(BITCASK_STATIC_LIB)
    // 静态链接时无导出修饰
#   define BITCASK_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#   if defined(BITCASK_DLL_EXPORTS)
#       define BITCASK_API __declspec(dllexport)
#   else
#       define BITCASK_API __declspec(dllimport)
#   endif
#else
#   if defined(BITCASK_DLL_EXPORTS)
#       define BITCASK_API __attribute__((visibility("default")))
#   else
#       define BITCASK_API
#   endif
#endif

/* ===========================================================================
 *  版本信息
 * ========================================================================= */

BITCASK_API int bitcask_version_major(void);
BITCASK_API int bitcask_version_minor(void);
BITCASK_API int bitcask_version_patch(void);
// 返回 "major.minor.patch"，NUL 结尾，不需要 free
BITCASK_API const char* bitcask_version_string(void);

/* ===========================================================================
 *  基础类型
 * ========================================================================= */

// 不透明句柄 — 内部包装 bitcask::Cask
typedef struct bitcask_t bitcask_t;

// 不透明迭代器句柄 — 内部包装 bitcask::CaskIter
typedef struct bitcask_iter_t bitcask_iter_t;

// 二进制安全的数据切片（对应 C++ std::span<const std::byte>）
// data 指向的数据在调用期间必须有效，函数返回后不再引用
typedef struct {
    const void* data;
    size_t      size;
} bitcask_slice_t;

// 错误码（对应 bitcask::CaskError，数值固定不变）
typedef enum {
    BITCASK_OK                = 0,
    BITCASK_ERR_IO            = 1,
    BITCASK_ERR_BAD_CRC       = 2,
    BITCASK_ERR_NOT_FOUND     = 3,
    BITCASK_ERR_KEY_TOO_LARGE = 4,
    BITCASK_ERR_VALUE_TOO_LARGE = 5,
    BITCASK_ERR_ALREADY_EXISTS  = 6,
    BITCASK_ERR_READ_ONLY      = 7,
    BITCASK_ERR_WRITE_LOCKED   = 8,
    BITCASK_ERR_INVALID_OPTION = 9,
    BITCASK_ERR_NO_INDEX       = 10,
    BITCASK_ERR_MODE_MISMATCH  = 11,
    BITCASK_ERR_ANALYZER_MISMATCH = 12,
} bitcask_error_t;

// 错误详情（对应 bitcask::CaskFault）
// detail 是固定 512 字节缓冲，栈安全，不需要 free
// detail[0] = '\0' 表示无详情
#define BITCASK_DETAIL_MAX 512

typedef struct {
    bitcask_error_t code;
    int             errnum;   // errno 值（IO 错误时有效，否则 0）
    char            detail[BITCASK_DETAIL_MAX];
} bitcask_fault_t;

/* ===========================================================================
 *  配置类型
 * ========================================================================= */

// 分析器类型（对应 bitcask::text::AnalyzerType）
typedef enum {
    BITCASK_ANALYZER_NONE      = 0,  // 纯 KV 模式，不建索引
    BITCASK_ANALYZER_NGRAM     = 1,  // CJK n-gram + 拉丁空白切分
    BITCASK_ANALYZER_WHITESPACE = 2, // 纯空白切分
    BITCASK_ANALYZER_JIEBA     = 3,  // jieba 中文分词
} bitcask_analyzer_type_t;

// 向量距离度量（对应 bitcask::meta::VectorMetric）
typedef enum {
    BITCASK_VECTOR_METRIC_NONE    = 0,  // 无向量
    BITCASK_VECTOR_METRIC_COSINE  = 1,  // 归一化余弦（写入时归一，查询用内积）
    BITCASK_VECTOR_METRIC_L2      = 2,  // 欧氏距离
    BITCASK_VECTOR_METRIC_DOT     = 3,  // 内积
} bitcask_vector_metric_t;

// 打开选项（对应 bitcask::CaskOptions + search config 扁平化）
// 用 bitcask_options_init() 初始化为默认值，再按需修改字段
typedef struct {
    // --- KV 基础 ---
    int       read_write;        // 0 = 只读，1 = 读写
    uint64_t  max_file_size;     // 单 data file 上限（默认 2 GiB）
    size_t    max_read_handles;  // read 句柄缓存上限（0 = 不限）
    int       o_sync;            // 每条写 durable（O_SYNC）
    uint32_t  sync_every_n;      // 每 N 次写 group-commit 一次（0 = 关闭）
    uint32_t  expiry_secs;       // 过期秒数（0 = 禁用）
    int       merge_only;        // merge-only 模式（与 read_write 互斥语义）
    uint8_t   tombstone_version; // 墓碑格式版本（0 或 2）

    // --- 搜索/索引 ---
    int       enable_search;     // 启用索引模式
    // 分析器配置
    bitcask_analyzer_type_t analyzer_type;
    uint32_t  analyzer_min_n;    // Ngram: 最小 n（默认 2）
    uint32_t  analyzer_max_n;    // Ngram: 最大 n（默认 3）
    const char* jieba_dict_path; // Jieba: 词典目录（NULL = 用库内默认路径）
    int       enable_stop_words; // 启用停用词过滤
    const char* const* stop_words; // 自定义停用词表（NULL 结尾数组，NULL = 默认）
    uint32_t  min_token_length;  // 拉丁整词最小 codepoint 长度（默认 1）
    int       enable_stemming;   // 启用 Porter 词干提取
    // 同义词词典文件（NULL = 不启用）。open 时一次性加载，构造后不可变 → 并发查询
    // 安全。每行一组、逗号分隔，如 "番茄, 西红柿, tomato"。文件无法打开 → open 返
    // BITCASK_ERR_INVALID_OPTION。运行期更换词典请重开库。
    const char* synonym_file_path;

    // --- 向量 ---
    uint16_t  vector_dim;        // 向量维度（0 = 无向量）
    bitcask_vector_metric_t vector_metric;
    int       vector_quantized;  // 落盘 int8 量化
    int       vector_inmem_int8; // HNSW int8-only 内存模式
} bitcask_options_t;

// 初始化为默认值
BITCASK_API void bitcask_options_init(bitcask_options_t* opts);

/* ===========================================================================
 *  结果类型
 * ========================================================================= */

// get 返回结果（对应 bitcask::GetResult）
// 调用方负责调用 bitcask_get_result_free() 释放
typedef struct {
    bitcask_slice_t value;      // text 段（DocValue 解码后）
    bitcask_slice_t meta;       // meta 段（可为空：data=NULL, size=0）
    const float*    vector;     // 向量段（可为 NULL）
    size_t          vector_len; // 向量元素数（vector_dim 或 0）
    uint32_t        tstamp;     // 时间戳
    uint64_t        ord;        // 写入序号
} bitcask_get_result_t;

// 单条搜索命中（对应 bitcask::search::SearchHit）
typedef struct {
    char*     key;    // NUL 结尾，malloc 分配，由 bitcask_search_result_free 释放
    uint64_t  ord;    // 文档写入序号
    double    score;  // 相关性分数
} bitcask_search_hit_t;

// 搜索结果（对应 bitcask::TextSearchResult）
typedef struct {
    bitcask_search_hit_t* hits;
    size_t                count;
} bitcask_search_result_t;

// 迭代器条目（对应 bitcask::CaskIter::Entry）
typedef struct {
    bitcask_slice_t key;        // 指向内部 malloc 缓冲
    bitcask_slice_t value;      // 指向内部 malloc 缓冲
    uint32_t        tstamp;
    uint32_t        file_id;
    uint64_t        offset;
    uint32_t        total_sz;
    int             is_tombstone;
    uint64_t        ord;
} bitcask_iter_entry_t;

// 状态信息（对应 bitcask::StatusInfo，简化版——不含文件列表）
typedef struct {
    uint64_t key_count;
    uint64_t key_bytes;
    uint64_t epoch;
    // indexed worker 抛异常时自增；非零 = 索引可能漂移，搜索结果可能陈旧
    uint64_t index_errors;
} bitcask_status_t;

// needs_merge 结果
typedef struct {
    int      needs;       // 0 = 不需要，1 = 需要
    // 候选文件列表（需要 merge 的文件路径）
    char**   files;
    size_t   files_count;
} bitcask_needs_merge_t;

/* ===========================================================================
 *  生命周期
 * ========================================================================= */

// 打开 Cask 实例。
// dirname: 数据目录路径（NUL 结尾）
// opts:   打开选项（NULL = 使用默认值）
// out:    成功时 *out 指向新实例，失败时 *out = NULL
// fault:  错误详情（NULL = 忽略详情）
// 返回 BITCASK_OK 或错误码
BITCASK_API bitcask_error_t bitcask_open(const char* dirname,
                                          const bitcask_options_t* opts,
                                          bitcask_t** out,
                                          bitcask_fault_t* fault);

// 关闭并释放 Cask 实例。cask 句柄此后不可使用。
// 内部调用 Cask::close() 后 delete 句柄包装。
BITCASK_API void bitcask_close(bitcask_t* cask);

/* ===========================================================================
 *  KV 操作
 * ========================================================================= */

// 读取 key 对应的值。
// 成功返回 BITCASK_OK，*out 指向新建结果，调用方需 free。
// key 不存在返回 BITCASK_ERR_NOT_FOUND，*out = NULL。
BITCASK_API bitcask_error_t bitcask_get(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_get_result_t** out,
                                          bitcask_fault_t* fault);

// 写入 key-value。
// tstamp = 0 表示使用当前时间。
BITCASK_API bitcask_error_t bitcask_put(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_slice_t value,
                                          uint32_t tstamp,
                                          bitcask_fault_t* fault);

// 删除 key（写入墓碑）。
// tstamp = 0 表示使用当前时间。
BITCASK_API bitcask_error_t bitcask_delete(bitcask_t* cask,
                                             bitcask_slice_t key,
                                             uint32_t tstamp,
                                             bitcask_fault_t* fault);

// fsync active data file。
BITCASK_API bitcask_error_t bitcask_sync(bitcask_t* cask,
                                           bitcask_fault_t* fault);

// 关闭 active write file，释放 write lock。下次 put 自动重开。
BITCASK_API bitcask_error_t bitcask_close_write_file(bitcask_t* cask,
                                                       bitcask_fault_t* fault);

// 释放 get 结果
BITCASK_API void bitcask_get_result_free(bitcask_get_result_t* result);

/* ===========================================================================
 *  结构化文档写入
 * ========================================================================= */

// put_doc 输入（对应 bitcask::DocInput 简化版）
typedef struct {
    bitcask_slice_t text;       // required（多字段时可空，作默认字段）
    bitcask_slice_t meta;       // optional（data=NULL 跳过）
    const float*    vector;     // optional（NULL = 无向量）
    size_t          vector_len; // 向量元素数
} bitcask_doc_input_t;

// 写入结构化文档（索引模式）。
BITCASK_API bitcask_error_t bitcask_put_doc(bitcask_t* cask,
                                              bitcask_slice_t key,
                                              const bitcask_doc_input_t* doc,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault);

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

// 同义词词典已改为 **open 时配置**：见 bitcask_options_t::synonym_file_path
//（不可变、并发查询安全；运行期 setter 已移除）。

// 释放搜索结果
BITCASK_API void bitcask_search_result_free(bitcask_search_result_t* result);

/* ===========================================================================
 *  迭代
 * ========================================================================= */

// 启动迭代器快照。
// maxage:          freshness 容忍度（-1 = 不限）。
// maxputs:         容忍的 pending puts 数（-1 = 不限）。
// see_tombstones:  0 = 跳过墓碑，1 = 包含墓碑。
// 返回 BITCASK_OK 或 BITCASK_ERR_OUT_OF_DATE（快照过期，调用方重试）。
//   注：BITCASK_ERR_OUT_OF_DATE 为 BITCASK_ERR_INVALID_OPTION 的别名使用场景，
//   调用方应检查返回码并重试。
BITCASK_API bitcask_error_t bitcask_iter_start(bitcask_t* cask,
                                                 int maxage,
                                                 int maxputs,
                                                 int see_tombstones,
                                                 bitcask_iter_t** out,
                                                 bitcask_fault_t* fault);

// 取下一项。
// 返回 1 = 有数据（entry 已填充），0 = 迭代结束，<0 = 错误。
// entry 内部的 key/value 指向 malloc 缓冲，调用方需调 bitcask_iter_entry_free。
BITCASK_API int bitcask_iter_next(bitcask_iter_t* iter,
                                    bitcask_iter_entry_t* entry,
                                    bitcask_fault_t* fault);

// 批量取最多 max_n 条。
// 返回取到的条数（0 = 迭代结束），<0 = 错误。
// entries 是调用方分配的数组，max_n 为数组大小。
// 每条 entry 的 key/value 指向 malloc 缓冲，需逐条 free。
BITCASK_API int bitcask_iter_next_batch(bitcask_iter_t* iter,
                                          bitcask_iter_entry_t* entries,
                                          size_t max_n,
                                          bitcask_fault_t* fault);

// 释放迭代器（可提前调用，之后不可再用）。
BITCASK_API void bitcask_iter_release(bitcask_iter_t* iter);

// 释放迭代器条目内部缓冲（key/value 的 malloc 缓冲）。
BITCASK_API void bitcask_iter_entry_free(bitcask_iter_entry_t* entry);

/* ===========================================================================
 *  管理
 * ========================================================================= */

// 获取状态信息。
BITCASK_API bitcask_error_t bitcask_status(bitcask_t* cask,
                                              bitcask_status_t* out,
                                              bitcask_fault_t* fault);

// 检查是否需要 merge。
// 返回 BITCASK_OK，out->needs 标记是否需要，out->files 列出候选文件。
// 调用方负责调用 bitcask_needs_merge_free 释放 files。
BITCASK_API bitcask_error_t bitcask_needs_merge(bitcask_t* cask,
                                                  bitcask_needs_merge_t* out,
                                                  bitcask_fault_t* fault);

// 释放 needs_merge 结果
BITCASK_API void bitcask_needs_merge_free(bitcask_needs_merge_t* nm);

// 执行 merge（files 为 NULL 时自动调 needs_merge 决定）。
BITCASK_API bitcask_error_t bitcask_merge(bitcask_t* cask,
                                            bitcask_fault_t* fault);

// 是否空（写过 key 后即使删光也返回 0）。
BITCASK_API int bitcask_is_empty(bitcask_t* cask);

// keydir 是否被 fold/iterator pin 住。
BITCASK_API int bitcask_is_frozen(bitcask_t* cask);

// 刷新异步索引队列（索引模式下，确保 pending 写入被索引）。
BITCASK_API void bitcask_flush_index(bitcask_t* cask);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // BITCASK_C_H
