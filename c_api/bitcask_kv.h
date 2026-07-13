// bitcask_kv.h — C API：基础类型/配置/生命周期/KV/迭代/Meta 过滤/管理
//（S19-5 自 bitcask_c.h 拆分；聚合头 bitcask_c.h 保全量兼容）。

#ifndef BITCASK_KV_H
#define BITCASK_KV_H

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
    BITCASK_ERR_CLOSED         = 13,  // 对已 bitcask_close 的 handle 发起调用（S12-5）
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

/* S32-M0/M3/M5：向量引擎（对应 bitcask::meta::VectorEngine）。建库时一次性
   选定并持久化进 bitcask.meta；重开不一致 → BITCASK_ERR_MODE_MISMATCH。
   运行期不可切换——唯一切换路径是离线工具 vec_engine_migrate。 */
typedef enum {
    BITCASK_VECTOR_ENGINE_HNSW    = 0,  /* 内存图（默认；≤2-4M 向量、µs 级延迟） */
    BITCASK_VECTOR_ENGINE_IVFRQ   = 1,  /* IVF 磁盘段（磁盘档推荐；10M-100M） */
    BITCASK_VECTOR_ENGINE_DISKANN = 2,  /* DiskANN（实验性——真实语料验证前不建议生产） */
} bitcask_vector_engine_t;

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
    int       vector_inmem_int8; // HNSW int8-only 内存模式（仅 hnsw 引擎）
    /* S32：向量引擎选择（默认 HNSW；磁盘档引擎要求 COSINE/DOT 度量，
       L2 组合返回 BITCASK_ERR_INVALID_OPTION）。 */
    bitcask_vector_engine_t vector_engine;

    /* S13-D11：HNSW 建图参数（0 = 默认：M=16 / ef_construction=200）。
       仅影响新插入与 merge 期重建出的图。 */
    uint32_t  hnsw_m;
    uint32_t  hnsw_ef_construction;
    /* S29-11-②：HNSW 建图导航 int8 混合精度（默认 1 = 开；仅 COSINE/DOT +
       int8 SIMD 内核可用时生效，入选邻居仍 f32 精选、召回零损失实测。
       0 = 回退全 f32 导航（召回门逃生闸）。 */
    int       hnsw_build_nav_int8;

    /* S32-M1：向量组件 base rebase 窗口门槛（崩溃恢复重放上界；全引擎）。
       0 = 关（仅链长门）；不设默认 262144。 */
    uint32_t  vector_rebase_min_docs;
    /* S32-M3：IVF 引擎参数（engine=IVFRQ 时生效；0 = 自动：
       nlist = 4·√N，nprobe = max(nlist/32, 8)）。 */
    uint32_t  vector_ivf_nlist;
    uint32_t  vector_ivf_nprobe;
    /* S32-M5：DiskANN 引擎参数（engine=DISKANN 时生效；0 = 自动：
       r = 32，l_build = max(64, 2r)）。 */
    uint32_t  vector_diskann_r;
    uint32_t  vector_diskann_l_build;

    /* S13-D7：日志回调（open-time 不可变）。库内 best-effort 静默失败点
       （checkpoint 保存失败、索引 worker 异常、merge 收尾异常等）经此上报。
       level: 0 = warn, 1 = error。msg 为 NUL 结尾单行文本，仅回调期间有效。
       可能从任意内部线程调用——回调须线程安全、不得回调进本 cask。
       NULL = 不上报（默认，零开销）。 */
    void (*log_fn)(int level, const char* msg, void* ctx);
    void* log_ctx;
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

// S13-D8：扩展状态（additive 新结构 + 新函数，既有 bitcask_status 的
// struct 布局与语义不动——ABI 兼容）。
typedef struct {
    uint64_t key_count;
    uint64_t key_bytes;
    uint64_t epoch;
    uint64_t index_errors;
    uint64_t hnsw_nodes;           // HNSW 图节点数（含软删；无向量索引 = 0）
    uint64_t search_cache_entries; // 查询缓存当前条目数（无索引 = 0）
    uint64_t read_handles;         // read 句柄缓存当前大小（fd+mmap 数）
} bitcask_status_ex_t;

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

/* S13-D5：带 per-key TTL 的写入。expiry_at = 绝对 unix 秒（0 = 永不过期，
 * 等价 bitcask_put）。过期后 get/iter 视作不存在，空间 merge 时回收。 */
BITCASK_API bitcask_error_t bitcask_put_ex(bitcask_t* cask,
                                           bitcask_slice_t key,
                                           bitcask_slice_t value,
                                           uint32_t tstamp,
                                           uint32_t expiry_at,
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
    uint32_t        expiry_at;  // S13-D5：per-key 过期时刻（绝对 unix 秒；0=永不）
} bitcask_doc_input_t;

// 写入结构化文档（索引模式）。
BITCASK_API bitcask_error_t bitcask_put_doc(bitcask_t* cask,
                                              bitcask_slice_t key,
                                              const bitcask_doc_input_t* doc,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault);


/* 批量写（S13-D1）。语义同逐条 bitcask_put，但整批一次提交：记录聚合写入、
 * 单次 flush 后才更新 keydir 并返回——本进程内 all-or-nothing 可见；
 * durability 与单条 put 的 sync 策略一致（o_sync 即时；sync_every_n>0 整批一次
 * 组提交；否则由 bitcask_sync 控制）。失败返回时整批不可见（磁盘可能残留
 * 前缀，重启后可见——与连续单条 put 的崩溃语义一致）。
 * 校验（key/value 大小）在任何写之前全批完成。items 借调用方存储。 */
typedef struct {
    bitcask_slice_t key;
    bitcask_slice_t value;
} bitcask_kv_pair_t;

BITCASK_API bitcask_error_t bitcask_put_batch(bitcask_t* cask,
                                              const bitcask_kv_pair_t* items,
                                              size_t n,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault);

/* ===========================================================================
 *  Meta 过滤（S13-D2）——V5 结构化 meta 过滤的 C 表示
 *
 *  一棵过滤树 = 叶子条件数组 + 子树数组，logic 决定 AND/OR 合流。全部指针
 *  借调用方存储（栈上/静态构造即可），仅在 search 调用期间读取，调用返回后
 *  即可释放——引擎内部会转换成自有表示。空树（无条件无子树）恒通过。
 *  类型规则与 C++ 端一致：Eq/Neq 同型比较；Gt/Gte/Lt/Lte 仅 int64/float64；
 *  In 用 values 数组（Eq 语义逐项）；Exists 只看 key 是否存在。
 * ========================================================================= */

typedef enum {
    BITCASK_META_OP_EQ     = 0,
    BITCASK_META_OP_NEQ    = 1,
    BITCASK_META_OP_GT     = 2,
    BITCASK_META_OP_GTE    = 3,
    BITCASK_META_OP_LT     = 4,
    BITCASK_META_OP_LTE    = 5,
    BITCASK_META_OP_IN     = 6,
    BITCASK_META_OP_EXISTS = 7
} bitcask_meta_op_t;

typedef enum {
    BITCASK_META_VALUE_NULL    = 0,
    BITCASK_META_VALUE_BOOL    = 1,
    BITCASK_META_VALUE_INT64   = 2,
    BITCASK_META_VALUE_FLOAT64 = 3,
    BITCASK_META_VALUE_STRING  = 4
} bitcask_meta_value_type_t;

typedef struct {
    bitcask_meta_value_type_t type;
    int64_t     i64;  /* BOOL（0/1）与 INT64 用 */
    double      f64;  /* FLOAT64 用 */
    const char* str;  /* STRING 用（NUL 结尾；其余类型忽略） */
} bitcask_meta_value_t;

typedef struct {
    const char*                 key;   /* 必填，NUL 结尾 */
    bitcask_meta_op_t           op;
    bitcask_meta_value_t        value;        /* IN/EXISTS 之外使用 */
    const bitcask_meta_value_t* values;       /* 仅 IN：候选值数组 */
    size_t                      values_count; /* 仅 IN */
} bitcask_meta_condition_t;

typedef struct bitcask_meta_filter {
    int logic_or;  /* 0 = AND（默认语义），非 0 = OR */
    const bitcask_meta_condition_t*   conditions;
    size_t                            conditions_count;
    const struct bitcask_meta_filter* children;  /* 子树数组（嵌套组合） */
    size_t                            children_count;
} bitcask_meta_filter_t;

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
// 错误（<0）时本函数已释放中途填充的条目缓冲——调用方无需（也不可）
// 对 entries 做任何 free。
BITCASK_API int bitcask_iter_next_batch(bitcask_iter_t* iter,
                                          bitcask_iter_entry_t* entries,
                                          size_t max_n,
                                          bitcask_fault_t* fault);

// 释放迭代器（可提前调用，之后不可再用）。
BITCASK_API void bitcask_iter_release(bitcask_iter_t* iter);

// 释放迭代器条目内部缓冲（key/value 的 malloc 缓冲）。
BITCASK_API void bitcask_iter_entry_free(bitcask_iter_entry_t* entry);

// 并行全表扫描回调（S12-5）。对每个 live 文档调用一次；**可能来自多个工作线程并发调用**。
//   ctx  : bitcask_parallel_scan 透传的用户指针（C 无闭包，用它带状态）
//   key/value: 零拷贝 view，仅在本次回调内有效（返回后即失效，需保留请自行拷贝）
// **回调必须线程安全**——不同工作线程并发调用，各处理不相交 key 段（若写共享状态需自行加锁）。
typedef void (*bitcask_scan_fn)(void* ctx,
                                bitcask_slice_t key,
                                bitcask_slice_t value);

// 并行全表扫描（S12-5）。单次快照所有 live key（调用线程串行，仅拷 key），按 n_threads
// 分段并发 get 读值并调 fn——把「多线程读安全」用于 analytics/export/reindex。
//   n_threads==0 → hardware_concurrency()。
//   并发删除致某 key get 时 not-found → 跳过（near-real-time）；IO/CRC 错误 → 停止并返回。
//   成功时 *out_count（可为 NULL）= 遍历到的 key 数。cask 已 close → BITCASK_ERR_CLOSED。
// 线程安全: 是（快照串行 + get 并发安全）。
BITCASK_API bitcask_error_t bitcask_parallel_scan(bitcask_t* cask,
                                                    size_t n_threads,
                                                    bitcask_scan_fn fn,
                                                    void* ctx,
                                                    size_t* out_count,
                                                    bitcask_fault_t* fault);

/* ===========================================================================
 *  管理
 * ========================================================================= */

// 获取状态信息。
BITCASK_API bitcask_error_t bitcask_status(bitcask_t* cask,
                                              bitcask_status_t* out,
                                              bitcask_fault_t* fault);

// S13-D8：扩展观测（见 bitcask_status_ex_t）。
BITCASK_API bitcask_error_t bitcask_status_ex(bitcask_t* cask,
                                              bitcask_status_ex_t* out,
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


#endif // BITCASK_KV_H
