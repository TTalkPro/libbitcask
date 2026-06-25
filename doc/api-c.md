# C API 参考（C Wrapper）

本文档是 libbitcask C ABI 的完整参考，面向跨语言 FFI 绑定作者（Python / Rust / Go / Node 等）。权威来源为 `c_api/bitcask_c.h`。配套阅读：[`api-cpp.md`](api-cpp.md)（被包装的 C++ 接口）、[`cpp-arch.md`](cpp-arch.md)（架构）。

C API 是 C++ `bitcask::Cask` 的薄包装，符号导出由 `BITCASK_API` 宏控制，编译为 `libbitcask.so`（`SOVERSION=3`）。

---

## 1. 概述

设计原则（见 `bitcask_c.h` 顶部注释）：

- **不透明句柄**：C 侧持有 `bitcask_t*` / `bitcask_iter_t*`，不知内部布局。
- **显式内存管理**：每个返回堆内存的结果都有配对的 `*_free` 函数，由 C 侧负责释放。
- **错误码 + out-param**：函数返回 `bitcask_error_t`，详情经 `bitcask_fault_t*` 传出。
- **二进制安全**：用 `{data, size}` 切片 `bitcask_slice_t`，不依赖 NUL 结尾。

---

## 2. 头文件与链接

```c
#include "bitcask_c.h"
```

链接 `libbitcask.so`（或静态 `libbitcask.a`，此时定义 `BITCASK_STATIC_LIB` 以去掉导出修饰）。

```bash
gcc app.c -I<c_api 头文件目录> -L<lib 目录> -lbitcask -o app
```

---

## 3. 版本信息

```c
int         bitcask_version_major(void);
int         bitcask_version_minor(void);
int         bitcask_version_patch(void);
const char* bitcask_version_string(void);   // "major.minor.patch"，NUL 结尾，无需 free
```

---

## 4. 基础类型

### 4.1 句柄（不透明）

```c
typedef struct bitcask_t      bitcask_t;       // 包装 bitcask::Cask
typedef struct bitcask_iter_t bitcask_iter_t;  // 包装 bitcask::CaskIter
```

### 4.2 `bitcask_slice_t`（二进制安全切片）

```c
typedef struct {
    const void* data;
    size_t      size;
} bitcask_slice_t;
```

`data` 在调用期间必须有效，函数返回后不再引用（输入参数语义）。

### 4.3 `bitcask_fault_t`（错误详情）

```c
#define BITCASK_DETAIL_MAX 512

typedef struct {
    bitcask_error_t code;
    int             errnum;                  // errno（IO 错误时有效，否则 0）
    char            detail[BITCASK_DETAIL_MAX]; // 固定 512B 栈缓冲，无需 free；detail[0]='\0' 表示无详情
} bitcask_fault_t;
```

### 4.4 `bitcask_error_t`（错误码）

数值固定不变，对应 C++ `bitcask::CaskError`：

| 值 | 名称 | 含义 |
|----|------|------|
| `0` | `BITCASK_OK` | 成功 |
| `1` | `BITCASK_ERR_IO` | 底层 IO 错误（`fault.errnum` 为 errno）|
| `2` | `BITCASK_ERR_BAD_CRC` | CRC 校验失败 |
| `3` | `BITCASK_ERR_NOT_FOUND` | key 不存在 |
| `4` | `BITCASK_ERR_KEY_TOO_LARGE` | key 超长 |
| `5` | `BITCASK_ERR_VALUE_TOO_LARGE` | value 超长 |
| `6` | `BITCASK_ERR_ALREADY_EXISTS` | CAS 竞态 |
| `7` | `BITCASK_ERR_READ_ONLY` | 对只读 cask 写 |
| `8` | `BITCASK_ERR_WRITE_LOCKED` | 锁被占 |
| `9` | `BITCASK_ERR_INVALID_OPTION` | 选项非法 |
| `10` | `BITCASK_ERR_NO_INDEX` | KV 模式调用 search |
| `11` | `BITCASK_ERR_MODE_MISMATCH` | 模式不匹配 |
| `12` | `BITCASK_ERR_ANALYZER_MISMATCH` | 分析器类型不匹配 |

> `bitcask_iter_next` 遇快照过期（caller 应重试）时返回 `BITCASK_ERR_INVALID_OPTION`——头文件中无独立的 `BITCASK_ERR_OUT_OF_DATE` 常量。

---

## 5. 配置类型

### 5.1 `bitcask_analyzer_type_t`

| 值 | 名称 | 含义 |
|----|------|------|
| `0` | `BITCASK_ANALYZER_NONE` | 纯 KV 模式，不建索引 |
| `1` | `BITCASK_ANALYZER_NGRAM` | CJK n-gram + 拉丁空白切分 |
| `2` | `BITCASK_ANALYZER_WHITESPACE` | 纯空白切分 |
| `3` | `BITCASK_ANALYZER_JIEBA` | jieba 中文分词 |

### 5.2 `bitcask_vector_metric_t`

| 值 | 名称 | 含义 |
|----|------|------|
| `0` | `BITCASK_VECTOR_METRIC_NONE` | 无向量 |
| `1` | `BITCASK_VECTOR_METRIC_COSINE` | 归一化余弦（写入时归一，查询用内积）|
| `2` | `BITCASK_VECTOR_METRIC_L2` | 欧氏距离 |
| `3` | `BITCASK_VECTOR_METRIC_DOT` | 内积 |

### 5.3 `bitcask_options_t`（打开选项，扁平化 CaskOptions + search config）

用 `bitcask_options_init(&opts)` 初始化为默认值，再按需修改字段：

**KV 基础**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `read_write` | `int` | `0` | `0`=只读，`1`=读写 |
| `max_file_size` | `uint64_t` | `2 GiB` | 单 data 文件上限 |
| `max_read_handles` | `size_t` | `0` | read 句柄缓存上限（`0`=不限）|
| `o_sync` | `int` | `0` | 每条写 durable（O_SYNC）|
| `sync_every_n` | `uint32_t` | `0` | 每 N 次写 group-commit 一次（`0`=关闭）|
| `expiry_secs` | `uint32_t` | `0` | TTL 秒数（`0`=禁用）|
| `merge_only` | `int` | `0` | merge-only 模式 |
| `tombstone_version` | `uint8_t` | `0` | 墓碑格式版本（`0` 或 `2`）|

**搜索/索引**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `enable_search` | `int` | `0` | 启用索引模式 |
| `analyzer_type` | `bitcask_analyzer_type_t` | `NONE` | 分析器类型（`NONE` 内部映射为 `NGRAM`）|
| `analyzer_min_n` | `uint32_t` | `2` | Ngram 最小 n |
| `analyzer_max_n` | `uint32_t` | `3` | Ngram 最大 n |
| `jieba_dict_path` | `const char*` | `NULL` | Jieba 词典目录（`NULL`=库内默认）|
| `enable_stop_words` | `int` | `0` | 启用停用词过滤 |
| `stop_words` | `const char* const*` | `NULL` | 自定义停用词表（`NULL` 结尾数组，`NULL`=默认）|
| `min_token_length` | `uint32_t` | `1` | 拉丁整词最小 codepoint 长度 |
| `enable_stemming` | `int` | `0` | 启用 Porter 词干提取 |

**向量**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `vector_dim` | `uint16_t` | `0` | 向量维度（`0`=无向量）|
| `vector_metric` | `bitcask_vector_metric_t` | `NONE` | 距离度量 |
| `vector_quantized` | `int` | `0` | 落盘 int8 量化 |
| `vector_inmem_int8` | `int` | `0` | HNSW int8-only 内存 |

```c
void bitcask_options_init(bitcask_options_t* opts);
```

---

## 6. 结果类型

### 6.1 `bitcask_get_result_t`
```c
typedef struct {
    bitcask_slice_t value;       // text 段（DocValue 解码后）
    bitcask_slice_t meta;        // meta 段（空：data=NULL, size=0）
    const float*    vector;      // 向量段（可为 NULL）
    size_t          vector_len;  // 向量元素数（vector_dim 或 0）
    uint32_t        tstamp;
    uint64_t        ord;
} bitcask_get_result_t;
```
调用方负责 `bitcask_get_result_free()`。

### 6.2 `bitcask_search_hit_t`
```c
typedef struct {
    char*     key;    // NUL 结尾，malloc 分配，由 bitcask_search_result_free 释放
    uint64_t  ord;
    double    score;
} bitcask_search_hit_t;
```

### 6.3 `bitcask_search_result_t`
```c
typedef struct {
    bitcask_search_hit_t* hits;
    size_t                count;
} bitcask_search_result_t;
```

### 6.4 `bitcask_doc_input_t`（`bitcask_put_doc` 输入）
```c
typedef struct {
    bitcask_slice_t text;        // 必需（多字段时可空，作默认字段）
    bitcask_slice_t meta;        // 可选（data=NULL 跳过）
    const float*    vector;      // 可选（NULL=无向量）
    size_t          vector_len;  // 向量元素数
} bitcask_doc_input_t;
```

### 6.5 `bitcask_iter_entry_t`
```c
typedef struct {
    bitcask_slice_t key;         // 指向内部 malloc 缓冲
    bitcask_slice_t value;       // 指向内部 malloc 缓冲
    uint32_t        tstamp;
    uint32_t        file_id;
    uint64_t        offset;
    uint32_t        total_sz;
    int             is_tombstone;
    uint64_t        ord;
} bitcask_iter_entry_t;
```

### 6.6 `bitcask_status_t`
```c
typedef struct {
    uint64_t key_count;
    uint64_t key_bytes;
    uint64_t epoch;
    uint64_t index_errors;  // indexed worker 抛异常时自增；非零 = 索引可能漂移
} bitcask_status_t;
```

### 6.7 `bitcask_needs_merge_t`
```c
typedef struct {
    int     needs;        // 0=不需要，1=需要
    char**  files;        // 候选文件路径列表
    size_t  files_count;
} bitcask_needs_merge_t;
```

---

## 7. 生命周期

### `bitcask_open`
```c
bitcask_error_t bitcask_open(const char* dirname,
                             const bitcask_options_t* opts,
                             bitcask_t** out,
                             bitcask_fault_t* fault);
```
打开 Cask 实例。`opts=NULL` 用默认值；成功时 `*out` 指向新实例，失败 `*out=NULL`。`fault=NULL` 忽略详情。
- **可能错误**：`ERR_IO`、`ERR_WRITE_LOCKED`、`ERR_INVALID_OPTION`、`ERR_MODE_MISMATCH`、`ERR_ANALYZER_MISMATCH`。

### `bitcask_close`
```c
void bitcask_close(bitcask_t* cask);
```
关闭并释放实例，内部调 `Cask::close()` 后 delete 句柄包装。此后句柄不可用。
- **内存配对**：`bitcask_open` ↔ `bitcask_close`。

---

## 8. KV 操作

### `bitcask_get`
```c
bitcask_error_t bitcask_get(bitcask_t* cask,
                            bitcask_slice_t key,
                            bitcask_get_result_t** out,
                            bitcask_fault_t* fault);
```
成功返回 `BITCASK_OK`，`*out` 指向新建结果（调用方需 `bitcask_get_result_free`）。key 不存在返回 `BITCASK_ERR_NOT_FOUND`，`*out=NULL`。

### `bitcask_put`
```c
bitcask_error_t bitcask_put(bitcask_t* cask,
                            bitcask_slice_t key,
                            bitcask_slice_t value,
                            uint32_t tstamp,
                            bitcask_fault_t* fault);
```
`tstamp=0` 用当前时间。

### `bitcask_delete`
```c
bitcask_error_t bitcask_delete(bitcask_t* cask,
                               bitcask_slice_t key,
                               uint32_t tstamp,
                               bitcask_fault_t* fault);
```
软删除（写墓碑）。

### `bitcask_sync`
```c
bitcask_error_t bitcask_sync(bitcask_t* cask, bitcask_fault_t* fault);
```
fsync active data file；`o_sync` 模式下 no-op。

### `bitcask_close_write_file`
```c
bitcask_error_t bitcask_close_write_file(bitcask_t* cask, bitcask_fault_t* fault);
```
关 active write file，释放 write lock；下次 put 自动重开。

### `bitcask_get_result_free`
```c
void bitcask_get_result_free(bitcask_get_result_t* result);
```

---

## 9. 结构化文档

### `bitcask_put_doc`
```c
bitcask_error_t bitcask_put_doc(bitcask_t* cask,
                                bitcask_slice_t key,
                                const bitcask_doc_input_t* doc,
                                uint32_t tstamp,
                                bitcask_fault_t* fault);
```
写入结构化文档（索引模式）。`doc` 为 `NULL` 行为未定义，请传有效指针。

---

## 10. 检索（需索引模式：`enable_search=1` 且 `analyzer_type != NONE`）

无索引 → `BITCASK_ERR_NO_INDEX`；向量相关查询在无向量配置时 → `BITCASK_ERR_INVALID_OPTION`。

### `bitcask_search_text`（词袋）
```c
bitcask_error_t bitcask_search_text(bitcask_t* cask, const char* query,
                                    size_t k, bitcask_search_result_t** out,
                                    bitcask_fault_t* fault);
```

### `bitcask_search_phrase`（短语）
```c
bitcask_error_t bitcask_search_phrase(bitcask_t* cask, const char* query,
                                      size_t k, bitcask_search_result_t** out,
                                      bitcask_fault_t* fault);
```

### `bitcask_bool_search`（布尔 AND/OR/NOT）
```c
bitcask_error_t bitcask_bool_search(bitcask_t* cask, const char* query,
                                    size_t k, bitcask_search_result_t** out,
                                    bitcask_fault_t* fault);
```

### `bitcask_search_fields`（多字段 `field:term^boost`）
```c
bitcask_error_t bitcask_search_fields(bitcask_t* cask, const char* query,
                                      size_t k, bitcask_search_result_t** out,
                                      bitcask_fault_t* fault);
```

### `bitcask_search_near`（近邻）
```c
bitcask_error_t bitcask_search_near(bitcask_t* cask, const char* query,
                                    uint32_t slop, size_t k,
                                    bitcask_search_result_t** out,
                                    bitcask_fault_t* fault);
```
`slop` = term 间允许的最大间隙（`0`=短语）。

### `bitcask_search_fuzzy`（模糊，Levenshtein）
```c
bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask, const char* query,
                                     size_t k, uint32_t max_edit_distance,
                                     bitcask_search_result_t** out,
                                     bitcask_fault_t* fault);
```

### `bitcask_search_wildcard`（通配符 `*` / `?`）
```c
bitcask_error_t bitcask_search_wildcard(bitcask_t* cask, const char* pattern,
                                        size_t k, bitcask_search_result_t** out,
                                        bitcask_fault_t* fault);
```

### `bitcask_search_vector`（HNSW 向量 ANN）
```c
bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                      const float* query, size_t query_len,
                                      size_t k, size_t ef,
                                      bitcask_search_result_t** out,
                                      bitcask_fault_t* fault);
```
`query_len` 必须等于 `vector_dim`；`ef=0` → `max(k,64)`。

### `bitcask_search_hybrid`（RRF 混合）
```c
bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                      const char* text_query,
                                      const float* vec_query, size_t vec_len,
                                      size_t k, bitcask_search_result_t** out,
                                      bitcask_fault_t* fault);
```
`text_query=NULL` → 纯向量；`vec_query=NULL` → 纯文本；两路都空 → `ERR_INVALID_OPTION`。

### 同义词词典（open-time 配置）
运行期 `bitcask_set_synonym_map` **已移除**。改在 open 时设
`bitcask_options_t::synonym_file_path`（同义词文件路径，每行一组、逗号分隔）：
词典一次性加载、**不可变 → 并发查询安全**；文件无法打开 → `bitcask_open` 返
`BITCASK_ERR_INVALID_OPTION`。运行期更换请重开库。

### `bitcask_search_result_free`
```c
void bitcask_search_result_free(bitcask_search_result_t* result);
```
**内存配对**：所有 `bitcask_search_*` ↔ `bitcask_search_result_free`。

---

## 11. 迭代

### `bitcask_iter_start`
```c
bitcask_error_t bitcask_iter_start(bitcask_t* cask,
                                   int maxage, int maxputs, int see_tombstones,
                                   bitcask_iter_t** out,
                                   bitcask_fault_t* fault);
```
- `maxage`：freshness 容忍度（`-1`=不限）；`maxputs`：容忍的 pending puts 数（`-1`=不限）；`see_tombstones`：`0`=跳过墓碑，`1`=包含。
- 返回 `BITCASK_OK`，或 `BITCASK_ERR_INVALID_OPTION`（快照过期，caller 重试）。
- **内存配对**：`bitcask_iter_start` ↔ `bitcask_iter_release`。

### `bitcask_iter_next`
```c
int bitcask_iter_next(bitcask_iter_t* iter,
                      bitcask_iter_entry_t* entry,
                      bitcask_fault_t* fault);
```
返回值：`1`=有数据（`entry` 已填充）；`0`=迭代结束；`<0`=错误。`entry` 内的 key/value 指向 malloc 缓冲，**需逐条调 `bitcask_iter_entry_free`**。

### `bitcask_iter_next_batch`
```c
int bitcask_iter_next_batch(bitcask_iter_t* iter,
                            bitcask_iter_entry_t* entries, size_t max_n,
                            bitcask_fault_t* fault);
```
返回取到的条数（`0`=迭代结束），`<0`=错误。`entries` 由调用方分配，`max_n` 为数组大小。每条需逐条 free。

### `bitcask_iter_release`
```c
void bitcask_iter_release(bitcask_iter_t* iter);
```
释放迭代器（可提前调用，之后不可再用）。

### `bitcask_iter_entry_free`
```c
void bitcask_iter_entry_free(bitcask_iter_entry_t* entry);
```
释放 entry 内部缓冲（key/value 的 malloc 缓冲）。
- **内存配对**：`bitcask_iter_next` / `_next_batch` 的每条 entry ↔ `bitcask_iter_entry_free`。

---

## 12. 管理

### `bitcask_status`
```c
bitcask_error_t bitcask_status(bitcask_t* cask, bitcask_status_t* out,
                               bitcask_fault_t* fault);
```

### `bitcask_needs_merge`
```c
bitcask_error_t bitcask_needs_merge(bitcask_t* cask, bitcask_needs_merge_t* out,
                                    bitcask_fault_t* fault);
```
返回 `BITCASK_OK`，`out->needs` 标记是否需要，`out->files` 列出候选。调用方负责 `bitcask_needs_merge_free`。
- **内存配对**：`bitcask_needs_merge` ↔ `bitcask_needs_merge_free`。

### `bitcask_needs_merge_free`
```c
void bitcask_needs_merge_free(bitcask_needs_merge_t* nm);
```

### `bitcask_merge`
```c
bitcask_error_t bitcask_merge(bitcask_t* cask, bitcask_fault_t* fault);
```
执行 merge（内部自动调 `needs_merge` 决定）。

### `bitcask_is_empty` / `bitcask_is_frozen`
```c
int bitcask_is_empty(bitcask_t* cask);   // 写过 key 后即使删光也返回 0
int bitcask_is_frozen(bitcask_t* cask);  // keydir 是否被 fold/iter pin 住
```

### `bitcask_flush_index`
```c
void bitcask_flush_index(bitcask_t* cask);
```
索引模式下，确保 pending 写入被索引（排空异步索引队列）。

---

## 13. 内存管理（FFI 关键）

| 分配方（返回堆内存/句柄） | 释放方 | 说明 |
|--------------------------|--------|------|
| `bitcask_open` → `bitcask_t*` | `bitcask_close` | 实例句柄 |
| `bitcask_get` → `bitcask_get_result_t*` | `bitcask_get_result_free` | get 结果 |
| `bitcask_search_*` → `bitcask_search_result_t*` | `bitcask_search_result_free` | 所有检索结果（含其中每个 hit 的 key）|
| `bitcask_needs_merge` → `bitcask_needs_merge_t.files` | `bitcask_needs_merge_free` | 候选文件列表 |
| `bitcask_iter_start` → `bitcask_iter_t*` | `bitcask_iter_release` | 迭代器句柄 |
| `bitcask_iter_next` / `_next_batch` → `bitcask_iter_entry_t` 内的 key/value | `bitcask_iter_entry_free`（逐条）| entry 内部缓冲 |

`bitcask_version_string()` 返回的字符串是**静态**的，无需 free。

---

## 14. 线程安全模型

与 C++ 核心一致（S11：通用库，**同一 handle 多线程安全**）。C API 透传 `Cask` 的并发
契约。**各接口实现机制详见 [`design/thread-safety.md`](design/thread-safety.md) §7。**
图例：✅ 多线程安全；⚠️ 有条件/不安全（见备注）。

| 操作 | 线程安全 |
|------|---------|
| `bitcask_open` | ✅（产生独立对象）|
| `bitcask_close` | ⚠️（生命周期：close 即 **free 句柄**，此后 handle 失效，caller 须保证无在途调用且不再使用）|
| `bitcask_get` | ✅（读路径无锁）|
| `bitcask_put` / `bitcask_delete` / `bitcask_sync` / `bitcask_close_write_file` | ✅（S11-W1：内部 `write_mu_` 串行化；同一 handle 多线程写安全。更高写并发 → 按目录分片多实例）|
| `bitcask_search_text` / `_phrase` / `_bool` / `_fields` / `_near` / `_fuzzy` / `_wildcard` | ✅（并发读安全）|
| `bitcask_search_vector` / `bitcask_search_hybrid` | ✅（HNSW 读路径）|
| 同义词词典（`options.synonym_file_path`，open-time） | ✅（不可变 → 并发查询安全；无运行期 setter）|
| `bitcask_iter_*` | ⚠️（同一 iter 不可并发；每线程一个迭代器）|
| `bitcask_status` / `bitcask_needs_merge` / `bitcask_merge` / `bitcask_is_empty` / `bitcask_is_frozen` / `bitcask_flush_index` | ✅ |

> 读写并发安全（搜索可见性 near-real-time）。merge 与读写并发（经 keydir shared_mutex 协调）。
> 并行全表扫描（`Cask::parallel_scan`）是 C++-only API；C host 可自行多线程并发 `bitcask_get`
> 达到等价效果（get 线程安全）。

---

## 15. 完整 C 示例

```c
#include "bitcask_c.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* ---- 索引模式打开（BM25 + 向量）---- */
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write    = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_NGRAM;
    opts.vector_dim    = 4;
    opts.vector_metric = BITCASK_VECTOR_METRIC_COSINE;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    if (bitcask_open("/tmp/libbitcask_demo", &opts, &cask, &fault) != BITCASK_OK) {
        fprintf(stderr, "open failed: %s\n", fault.detail);
        return 1;
    }

    /* ---- 写入文档 ---- */
    bitcask_slice_t key = {"doc:1", 5};
    float vec[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    bitcask_doc_input_t doc = {
        .text       = {"北京今天天气晴朗", 24},
        .vector     = vec,
        .vector_len = 4,
    };
    if (bitcask_put_doc(cask, key, &doc, 0, &fault) != BITCASK_OK) {
        fprintf(stderr, "put_doc failed: %s\n", fault.detail);
    }
    bitcask_flush_index(cask);

    /* ---- 词袋检索 ---- */
    bitcask_search_result_t* res = NULL;
    if (bitcask_search_text(cask, "北京 天气", 10, &res, &fault) == BITCASK_OK) {
        for (size_t i = 0; i < res->count; i++) {
            printf("hit[%zu] key=%s score=%.4f\n",
                   i, res->hits[i].key, res->hits[i].score);
        }
        bitcask_search_result_free(res);
    }

    /* ---- 向量检索 ---- */
    if (bitcask_search_vector(cask, vec, 4, 10, 0, &res, &fault) == BITCASK_OK) {
        for (size_t i = 0; i < res->count; i++) {
            printf("knn[%zu] key=%s score=%.4f\n",
                   i, res->hits[i].key, res->hits[i].score);
        }
        bitcask_search_result_free(res);
    }

    /* ---- KV 读 ---- */
    bitcask_get_result_t* gr = NULL;
    if (bitcask_get(cask, key, &gr, &fault) == BITCASK_OK) {
        printf("value: %.*s\n", (int)gr->value.size, (char*)gr->value.data);
        bitcask_get_result_free(gr);
    }

    /* ---- 迭代 ---- */
    bitcask_iter_t* it = NULL;
    if (bitcask_iter_start(cask, -1, -1, 0, &it, &fault) == BITCASK_OK) {
        bitcask_iter_entry_t e;
        while (bitcask_iter_next(it, &e, &fault) == 1) {
            printf("iter key: %.*s\n", (int)e.key.size, (char*)e.key.data);
            bitcask_iter_entry_free(&e);
        }
        bitcask_iter_release(it);
    }

    /* ---- 状态 / merge ---- */
    bitcask_status_t st;
    bitcask_status(cask, &st, &fault);
    printf("status: %llu keys, %llu bytes\n",
           (unsigned long long)st.key_count,
           (unsigned long long)st.key_bytes);

    bitcask_needs_merge_t nm;
    if (bitcask_needs_merge(cask, &nm, &fault) == BITCASK_OK) {
        if (nm.needs) bitcask_merge(cask, &fault);
        bitcask_needs_merge_free(&nm);
    }

    bitcask_close(cask);
    return 0;
}
```

> 编译：`gcc demo.c -I<headers> -L<lib> -lbitcask -o demo`。更多用法见 `tests/c_api_test.c`。
