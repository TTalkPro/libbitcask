# libbitcask C API 参考

本文档是 libbitcask C ABI 的权威参考，面向跨语言 FFI 绑定作者（Python / Rust / Go / Node 等）。所有符号以 `c_api/bitcask_kv.h`、`c_api/bitcask_text.h`、`c_api/bitcask_vec.h` 为准；C 实现位于 `c_api/bitcask_kv.cpp`、`bitcask_text.cpp`、`bitcask_vec.cpp`。被包装的 C++ 接口见 [`api-cpp.md`](api-cpp.md)，架构见 [`cpp-arch.md`](cpp-arch.md)。

C API 是 C++ `bitcask::Cask` 的薄 `extern "C"` 包装，编译产物：

| 产物 | 说明 |
|------|------|
| `libbitcask.so` | 共享库，导出全部 C API（`SOVERSION=6`，`VERSION=6.1.0`，由 `CMakeLists.txt` 的 `project(libbitcask VERSION 6.1.0)` 单一真源派生）|
| `libbitcask.a` | 合并全部静态归档的单一 `.a`（定义 `BITCASK_STATIC_LIB` 去掉导出修饰）|

符号导出由 `BITCASK_API` 宏控制（`bitcask_kv.h` §符号导出宏），Windows 下退化为 `__declspec(dllimport/dllexport)`，其它平台默认 `__attribute__((visibility("default")))`。

---

## 目录

1. [设计原则](#1-设计原则)
2. [头文件与链接](#2-头文件与链接)
   - [2.1 为什么跨模块只能用 C API（Windows）](#21-为什么跨模块只能用-c-api-windows)
3. [版本信息](#3-版本信息)
4. [基础类型](#4-基础类型)
5. [错误处理](#5-错误处理)
6. [配置与打开选项](#6-配置与打开选项)
7. [结果类型](#7-结果类型)
8. [生命周期：打开与关闭](#8-生命周期打开与关闭)
9. [KV 操作](#9-kv-操作)
10. [结构化文档与批量写](#10-结构化文档与批量写)
11. [迭代与并行扫描](#11-迭代与并行扫描)
12. [状态与合并管理](#12-状态与合并管理)
13. [BM25 文本检索](#13-bm25-文本检索)
14. [HNSW 向量检索与 RRF 混合检索](#14-hnsw-向量检索与-rrf-混合检索)
15. [内存管理：ownership 配对](#15-内存管理ownership-配对)
16. [线程安全模型](#16-线程安全模型)
17. [完整 C 示例](#17-完整-c-示例)

---

## 1. 设计原则

| 原则 | 说明 |
|------|------|
| **不透明句柄** | `bitcask_t*` / `bitcask_iter_t*` 不暴露内部布局，C 侧仅作为指针持有 |
| **显式内存管理** | 每个返回堆内存的函数配对一个 `*_free` 函数，由 C 侧负责释放（详见 [§15](#15-内存管理ownership-配对)）|
| **错误码 + out-param** | 函数返回 `bitcask_error_t` 枚举码，详情经 `bitcask_fault_t*` out-param 传出（固定 512B 栈缓冲，无堆）|
| **二进制安全** | 用 `{data, size}` 切片 `bitcask_slice_t`，不依赖 NUL 结尾 |
| **extern "C" 异常隔离** | 所有入口包在 `guarded()` 中，C++ 异常穿越 C 栈帧被翻译为 `BITCASK_ERR_IO` + `ENOMEM`/异常消息（详见 `c_api/internal.h` §`fault_from_exception`）|

---

## 2. 头文件与链接

`bitcask_c.h` 是聚合头（S19-5 起按域拆分；include 它获得全量 API，既有代码零改动）：

| 头文件 | 内容 |
|--------|------|
| `bitcask_kv.h` | 基础类型 / 配置 / 生命周期 / KV 读写 / 迭代 / Meta 过滤 / 管理 |
| `bitcask_text.h` | BM25 文本搜索（`search_text/phrase/bool/fields/near/fuzzy/wildcard` + 批量 + `_filtered`）|
| `bitcask_vec.h` | HNSW 向量与 RRF 混合检索（`search_vector/hybrid` + 批量 + `_filtered`）|
| `bitcask_c.h` | 聚合头：`#include` 三者，用户零改动 |

```c
#include "bitcask_c.h"   // 或按域只 include bitcask_kv.h 等
```

链接：

```bash
gcc app.c -I<c_api 头目录> -L<lib 目录> -lbitcask -o app
# 静态链接（libbitcask.a）需先定义 BITCASK_STATIC_LIB
gcc -DBITCASK_STATIC_LIB app.c -I<c_api 头目录> libbitcask.a -o app
```

Windows（MSVC）：

```bat
cl app.c /I<c_api 头目录> bitcask.lib          :: 动态：链导入库，运行时需 bitcask.dll 同目录
cl /DBITCASK_STATIC_LIB app.c /I<c_api 头目录> bitcask_static.lib   :: 静态
```

### 2.1 为什么跨模块只能用 C API（Windows）

**C API 是本库唯一的模块边界。C++ 头（`include/bitcask/*.hpp`）不是** ——
它要求调用方与库**在同一模块内、或至少共用同一份 CRT**。

原因是 Windows 的 CRT 模型：`/MT`（静态 CRT）下**每个模块各带一份自己的 CRT，
各有一份自己的堆和自己的 fd 表**。而 C++ 头有若干处让 CRT 拥有的对象跨过
调用方与库的边界：

| 出处 | 谁分配 | 谁释放 / 使用 |
|---|---|---|
| `io::File::pread` 返回 `ReadOk{std::vector<std::byte>}` | 库 | 调用方析构 → 跨堆 free |

> 曾经还有两处 `std::FILE*` 跨界（`io::open_stream` 返回、`io::flush_and_sync_stream` 收），
> **已在 W5 消除**：`io.hpp` 现在只交换**内核句柄**，`FILE*` 的生成与拆解一律由
> `detail/file_util.hpp` 里的 `adopt_stream` / `stream_handle` 完成——两者都是
> `inline`，在调用方模块内展开，于是 `FILE*` 从头到尾只属于一份 CRT。
> 代价是 Windows 上 `detail/file_util.hpp` 会引入 `<io.h>` / `<fcntl.h>`
> 两个 CRT 小头（不引 `windows.h`，不带 `min`/`max` 宏）。
>
> 剩下的 `std::vector` 那条是常规 C++ ABI 约束（任何按值返回 STL 容器的 C++ 库
> 都有），不值得为它改 API——按下面的规则用即可。

在 `/MD`（动态 CRT，**CMake 的默认，也是本项目的
构建方式**）下，全进程只有一份 CRT，上述三处都成立。只有当有人把它改成 `/MT`
并跨模块使用 C++ 头时才会出事。

> ⚠️ **失败形态是静默的，值得单独记住。** CRT 的 invalid-parameter handler 直接
> `__fastfail`：进程当场消失，退出码 `0xC0000409`，**`terminate` 与 `abort` 两个
> 钩子都不触发**，现场只剩一个退出码。该状态码名为 `STACK_BUFFER_OVERRUN`，
> **与栈毫无关系** —— 它只是 CRT 致命退出的统一出口，照字面查会往完全错的方向走。
>
> 这不是推演：姊妹项目 coxswain 在 `/MT` + 多 DLL 下踩过同一形态（一个 `FILE*`
> 在两个 DLL 之间穿过），表现为「所有模型文件都加载不了、进程无声消失」，
> 最后靠 `cdb` 取栈才定位。**上游不会碰到，因为默认动态 CRT；`/MT` 才是没人
> 测过的那个组合。**

因此：

- **同一模块内**（直接把本库编进你的可执行文件）——C++ 头随便用，不受此限；
- **跨模块**（用 `bitcask.dll`，或把本库放进另一个 DLL）——**只用 C API**。
  C API 的边界上只有不透明句柄、POD 结构和 `bitcask_*_free()` 配对的所有权，
  没有任何 CRT 对象穿过。

`CMakeLists.txt` 在配置期检查 CRT 选择：选了静态 CRT 会给出指名道姓的告警。
给告警而非硬错，是因为「单模块静态链接」与「跨模块但只用 C API」两种 `/MT`
用法本身成立。

---

## 3. 版本信息

版本号由 `CMakeLists.txt` 的 `project(libbitcask VERSION 6.1.0)` 单一真源派生，configure 时通过 `c_api/bitcask_version.h.in` 生成 `bitcask_version.h`。

```c
BITCASK_API int          bitcask_version_major(void);
BITCASK_API int          bitcask_version_minor(void);
BITCASK_API int          bitcask_version_patch(void);
BITCASK_API const char*  bitcask_version_string(void);   // "major.minor.patch"，NUL 结尾
```

- `bitcask_version_string()` 返回的是库内静态字符串（指向 `BITCASK_VERSION_STRING` 宏展开的字符串字面量），**不需要 free**。
- 运行时返回值与 `libbitcask.so.6.1.0` 文件名完全对应；SOVERSION 是 `6`（大版本号），反映 ABI 兼容性。

---

## 4. 基础类型

### 4.1 不透明句柄

```c
typedef struct bitcask_t            bitcask_t;             // 包装 bitcask::Cask
typedef struct bitcask_iter_t       bitcask_iter_t;        // 包装 bitcask::CaskIter
typedef struct bitcask_range_iter_t bitcask_range_iter_t;  // 包装 bitcask::CaskRangeIter（S33-6）
```

句柄由 `bitcask_open` / `bitcask_iter_start` / `bitcask_range_iter_start` 分配并返回，**调用方负责在生命周期结束时调用配对的 `*_free` 释放**。已释放的句柄不可再使用（后置调用返回 `BITCASK_ERR_CLOSED`，不会崩溃）。

### 4.2 `bitcask_slice_t`：二进制安全切片

```c
typedef struct {
    const void* data;
    size_t      size;
} bitcask_slice_t;
```

输入参数语义：`data` 在调用期间必须有效，函数返回后不再引用（即栈/静态缓冲可；临时 buffer 也可——只要跨调用边界存活）。

### 4.3 句柄与全局 registry

C API 是进程级 host：所有经 `bitcask_open` 打开的句柄**共享同一个进程级 `KeyDirRegistry`**（`c_api/internal.h::c_api_registry()`）。同目录多次 open 共享同一 keydir（refcount），与既有 NIF host 语义一致。

---

## 5. 错误处理

### 5.1 `bitcask_error_t`：错误码枚举

数值固定不变，对应 C++ `bitcask::CaskError`，ABI 稳定：

| 值 | 名称 | 含义 |
|----|------|------|
| `0`  | `BITCASK_OK`                | 成功 |
| `1`  | `BITCASK_ERR_IO`            | 底层 IO 错误（`fault.errnum` 为 errno）|
| `2`  | `BITCASK_ERR_BAD_CRC`       | CRC 校验失败 |
| `3`  | `BITCASK_ERR_NOT_FOUND`     | key 不存在 |
| `4`  | `BITCASK_ERR_KEY_TOO_LARGE` | key 超长 |
| `5`  | `BITCASK_ERR_VALUE_TOO_LARGE` | value 超长 |
| `6`  | `BITCASK_ERR_ALREADY_EXISTS`  | CAS 竞态 |
| `7`  | `BITCASK_ERR_READ_ONLY`       | 对只读 cask 写 |
| `8`  | `BITCASK_ERR_WRITE_LOCKED`    | 锁被占 |
| `9`  | `BITCASK_ERR_INVALID_OPTION`  | 选项非法 / 迭代器快照过期（见下注）|
| `10` | `BITCASK_ERR_NO_INDEX`        | 索引在本句柄上本就不存在：KV 模式调搜索；RO/merge-only 打开无 OKI 的目录调 range（重开读写可建）|
| `11` | `BITCASK_ERR_MODE_MISMATCH`   | 模式不匹配 |
| `12` | `BITCASK_ERR_ANALYZER_MISMATCH` | 分析器类型不匹配 |
| `13` | `BITCASK_ERR_CLOSED`          | 对已 `bitcask_close` 的 handle 发起调用 |
| `14` | `BITCASK_ERR_INDEX_REBUILD_FAILED` | OKI 试建而败——可写打开时重建失败（IO/环境问题，见日志）；修复后重开可重试 |

> **快照过期**：迭代器快照过期时 `bitcask_iter_start` 返回 `BITCASK_ERR_INVALID_OPTION`（头文件中无独立的 `BITCASK_ERR_OUT_OF_DATE`），调用方应捕获并重试。

### 5.2 `bitcask_fault_t`：错误详情

```c
#define BITCASK_DETAIL_MAX 512

typedef struct {
    bitcask_error_t code;
    int             errnum;                       // errno（IO 错误时有效，否则 0）
    char            detail[BITCASK_DETAIL_MAX];   // 固定 512B 栈缓冲
} bitcask_fault_t;
```

**所有权契约**：

- `detail` 是 `bitcask_fault_t` 结构体内嵌的**固定 512 字节数组**（栈/嵌入即合法），由实现端通过 `snprintf(detail, BITCASK_DETAIL_MAX, ...)` 写入。
- **库负责写入，调用方不需要（也不应该）free**——它随 `bitcask_fault_t` 实例的栈/堆生命周期自动消亡。
- `detail[0] = '\0'` 表示无详情。`errnum` 仅在 `code == BITCASK_ERR_IO` 时携带 errno（如 `EIO` / `ENOSPC` / `ENOENT`），其它错误码时为 `0`。
- 多数函数允许 `fault == NULL`（跳过详情回填）。

### 5.3 异常隔离（S13-M2）

所有 C API 入口都包在 `guarded(fault, lambda)` 中：

- `std::bad_alloc` → `BITCASK_ERR_IO` + `errnum = ENOMEM`，`detail = "out of memory"`
- 其它 `std::exception` → `BITCASK_ERR_IO` + `errnum = 0`，`detail = "unexpected exception: <what>"`
- 未知异常 → `BITCASK_ERR_IO` + `detail = "unexpected exception"`

C++ 异常**永远不会**穿越 `extern "C"` 边界，避免 terminate。

---

## 6. 配置与打开选项

### 6.1 分析器类型 `bitcask_analyzer_type_t`

```c
typedef enum {
    BITCASK_ANALYZER_NONE       = 0,  // 纯 KV 模式，不建索引
    BITCASK_ANALYZER_NGRAM      = 1,  // CJK n-gram + 拉丁空白切分
    BITCASK_ANALYZER_WHITESPACE = 2,  // 纯空白切分
    BITCASK_ANALYZER_JIEBA      = 3,  // jieba 中文分词
} bitcask_analyzer_type_t;
```

> `BITCASK_ANALYZER_NONE` 在 C 端语义为"不建索引"；若选项中同时设了 `enable_search = 1`，C++ 内部会把 `NONE` 映射为 `Ngram` 以让索引能跑（实现见 `c_api/internal.h::to_cpp_analyzer_type`）。若**确实**不想要索引，应让 `enable_search = 0`。

### 6.2 向量度量 `bitcask_vector_metric_t`

```c
typedef enum {
    BITCASK_VECTOR_METRIC_NONE   = 0,  // 无向量
    BITCASK_VECTOR_METRIC_COSINE = 1,  // 归一化余弦（写入时归一，查询用内积）
    BITCASK_VECTOR_METRIC_L2     = 2,  // 欧氏距离
    BITCASK_VECTOR_METRIC_DOT    = 3,  // 内积
} bitcask_vector_metric_t;
```

### 6.3 Meta 操作符 `bitcask_meta_op_t`

```c
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
```

- `Eq/Neq` 同型比较；`Gt/Gte/Lt/Lte` 仅 `int64` / `float64`；`In` 用 `values` 数组（Eq 语义逐项）；`Exists` 只看 key 是否存在。

### 6.4 Meta 值类型 `bitcask_meta_value_type_t`

```c
typedef enum {
    BITCASK_META_VALUE_NULL    = 0,
    BITCASK_META_VALUE_BOOL    = 1,
    BITCASK_META_VALUE_INT64   = 2,
    BITCASK_META_VALUE_FLOAT64 = 3,
    BITCASK_META_VALUE_STRING  = 4
} bitcask_meta_value_type_t;
```

### 6.5 `bitcask_options_t`：打开选项

调用方先 `bitcask_options_init()` 拿到默认值，再按需修改字段：

```c
BITCASK_API void bitcask_options_init(bitcask_options_t* opts);
```

完整字段：

**KV 基础**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `read_write`        | `int`      | `0`      | `0`=只读，`1`=读写 |
| `max_file_size`     | `uint64_t` | `2 GiB`  | 单 data 文件上限（决定 data 文件个数，见 §6.5.1）|
| `max_read_handles`  | `size_t`   | `0`      | read 句柄缓存上限（**一个句柄 = 1 fd + 1 sealed mmap**）；`0`=自动（`RLIMIT_NOFILE` 一半，夹在 **[64, 1024]**）；`SIZE_MAX`=不限；其它 N=显式上限。见 §6.5.1 |
| `o_sync`            | `int`      | `0`      | 每条写 durable（O_SYNC）|
| `sync_every_n`      | `uint32_t` | `0`      | 每 N 次写 group-commit 一次（`0`=关闭）|
| `expiry_secs`       | `uint32_t` | `0`      | TTL 秒数（`0`=禁用）|
| `merge_only`        | `int`      | `0`      | merge-only 模式 |
| `tombstone_version` | `uint8_t`  | `0`      | 墓碑格式版本（`0` 或 `2`）|
| `keydir_cache_entries` | `size_t` | `0` | **S36 Level B（keydir 磁盘驻留）**：`0`=不限（全内存，现状）；`>0`=热点缓存条目预算，点查落组合视图（冷 get ≤2 次 pread），1 亿 key 常驻 11GB → ~1.1GB。首次对旧目录开启会全量重建 OKI（一次性）；merge_only 旁车对 Level B 目录 open 返回 `BITCASK_ERR_IO`。语义详见 api-cpp.md §11.3。⚠ 结构体 5.0.0 后新增字段（布局变更，见 CHANGELOG） |

**搜索 / 索引**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `enable_search`      | `int`      | `0`     | 启用索引模式 |
| `analyzer_type`      | `bitcask_analyzer_type_t` | `NONE` | 分析器类型 |
| `analyzer_min_n`     | `uint32_t` | `2`     | Ngram 最小 n |
| `analyzer_max_n`     | `uint32_t` | `3`     | Ngram 最大 n |
| `jieba_dict_path`    | `const char*` | `NULL` | Jieba 词典目录（`NULL`=库内默认）|
| `enable_stop_words`  | `int`      | `0`     | 启用停用词过滤 |
| `stop_words`         | `const char* const*` | `NULL` | 自定义停用词表（`NULL` 结尾数组，`NULL`=默认）|
| `min_token_length`   | `uint32_t` | `1`     | 拉丁整词最小 codepoint 长度 |
| `enable_stemming`    | `int`      | `0`     | 启用 Porter 词干提取 |
| `synonym_file_path`  | `const char*` | `NULL` | 同义词词典文件路径（`NULL`=不启用）|

**向量**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `vector_dim`         | `uint16_t`            | `0`     | 向量维度（`0`=无向量）|
| `vector_metric`      | `bitcask_vector_metric_t` | `NONE` | 距离度量 |
| `vector_quantized`   | `int`                 | `0`     | 落盘 int8 量化 |
| `vector_inmem_int8`  | `int`                 | `0`     | HNSW int8-only 内存模式（仅 hnsw 引擎） |
| `vector_engine`      | `bitcask_vector_engine_t` | `HNSW` | **S32：向量引擎**。`HNSW`（默认，≤2-4M 向量内存档）/ `IVFRQ`（磁盘档推荐，10M-100M）/ `DISKANN`（实验性）。建库时一次性选定、持久化进 `bitcask.meta`；重开不一致 → `BITCASK_ERR_MODE_MISMATCH`；运行期切换用离线工具 `vec_engine_migrate`。磁盘档引擎要求 COSINE/DOT 度量（L2 → `INVALID_OPTION`） |

**HNSW 建图（S13-D11）**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `hnsw_m`              | `uint32_t` | `0` | `M`（`0`=默认 16）|
| `hnsw_ef_construction` | `uint32_t` | `0` | `ef_construction`（`0`=默认 200）|
| `hnsw_build_nav_int8`  | `int`      | `1` | S29-11-②：建图导航 int8 混合精度（入选邻居 f32 精选，召回零损失实测；`0`=全 f32 回退闸）|

**向量引擎调优（S32）**

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `vector_rebase_min_docs` | `uint32_t` | `262144` | 向量组件 base rebase 窗口门（崩溃恢复重放上界；全引擎；`0`=关，仅链长门） |
| `vector_ivf_nlist`   | `uint32_t` | `0` | IVFRQ：簇数（`0`=自动 4·√N） |
| `vector_ivf_nprobe`  | `uint32_t` | `0` | IVFRQ：查询探簇数（`0`=自动；`bitcask_search_vector` 的 `ef` 参数非 0 时按 nprobe 解释） |
| `vector_diskann_r`   | `uint32_t` | `0` | DISKANN：邻接容量（`0`=32） |
| `vector_diskann_l_build` | `uint32_t` | `0` | DISKANN：建图 beam 宽（`0`=max(64, 2r)；查询 beam 宽走 `ef` 参数） |

**日志回调（S13-D7）**

| 字段 | 类型 | 含义 |
|------|------|------|
| `log_fn` | `void (*)(int level, const char* msg, void* ctx)` | open-time 不可变回调。库内 best-effort 静默失败点（checkpoint 保存失败、索引 worker 异常、merge 收尾异常等）经此上报。`level`：`0`=warn，`1`=error。`msg` 为 NUL 结尾单行文本，仅回调期间有效。可能从任意内部线程调用——**回调须线程安全、不得回调进本 cask**。`NULL`=不上报（默认零开销）。|
| `log_ctx` | `void*` | 透传给 `log_fn` |

### 6.5.1 fd / mmap 预算：怎么降"打开文件数"

一个句柄实例常驻的 fd 分四块：封口 data 文件的 read 句柄（**大头**，每句柄 1 fd + 1 sealed mmap）、active data + active hint（2）、OKI run（≤9，由 run 归并保证）、`write.lock` + `field.schema`（2）。

实测（1500 key、`max_file_size` 压到 4096 逼出 89 个 data 文件）：

| 配置 | 盘上 data 文件 | 本库 fd | mmap |
|---|---|---|---|
| 默认 | 89 | 96 | 88 |
| `max_read_handles = 16` | 89 | **24** | 16 |
| `max_file_size = 1 MiB` | **1** | **8** | 0 |

调优顺序：

1. **先量** `ulimit -n` 和目录里的 data 文件数。自动档取 `RLIMIT_NOFILE` 的一半——容器/systemd 下 rlimit 常见 5×10⁵，该值形同虚设（已夹到 1024 封顶，但仍建议按业务显式给值）。
2. **`opts.max_read_handles = N`**：立即生效，超额近似 LRU 淘汰**空闲**句柄（在途读者不受影响）。代价是淘汰后再读要重开 fd + 重建 mmap。
3. **`opts.max_file_size`**（默认 2 GiB）：从源头决定文件个数；线上若被调小过，那是文件数暴涨的根因。
4. **`bitcask_merge` 不是降 fd 的手段**：它按碎片率/死字节触发，纯追加负载下 `bitcask_needs_merge` 恒为 `needs=0`（实测 merge 前后都是 89 个文件）。merge 解决空间放大。

### 6.6 同义词词典

通过 `bitcask_options_t::synonym_file_path` 在 **open-time** 配置（运行期 `bitcask_set_synonym_map` 已移除）：

- 每行一组、逗号分隔（如 `"番茄, 西红柿, tomato"`）。
- open 时一次性加载；构造后不可变 → 并发查询安全。
- 文件无法打开 → `bitcask_open` 返回 `BITCASK_ERR_INVALID_OPTION`（不静默忽略）。
- 运行期更换请重开库。

---

## 7. 结果类型

### 7.1 `bitcask_get_result_t`

```c
typedef struct {
    bitcask_slice_t value;       // text 段（DocValue 解码后）
    bitcask_slice_t meta;        // meta 段（可为空：data=NULL, size=0）
    const float*    vector;      // 向量段（可为 NULL）
    size_t          vector_len;  // 向量元素数（vector_dim 或 0）
    uint32_t        tstamp;      // 时间戳
    uint64_t        ord;         // 写入序号
} bitcask_get_result_t;
```

**所有权**：由 `bitcask_get` malloc 分配；`value.data` / `meta.data` / `vector` 是 malloc 缓冲。调用方负责 `bitcask_get_result_free()`。

### 7.2 `bitcask_search_hit_t` / `bitcask_search_result_t`

```c
typedef struct {
    char*     key;    // NUL 结尾，strdup 分配，由 bitcask_search_result_free 释放
    uint64_t  ord;    // 文档写入序号
    double    score;  // 相关性分数
} bitcask_search_hit_t;

typedef struct {
    bitcask_search_hit_t* hits;
    size_t                count;
} bitcask_search_result_t;
```

**所有权**：每个 `hit.key` 是 strdup 的；整个 `hits[]` 数组与 `result` 结构本身是 malloc 的。`bitcask_search_result_free()` 逐条 free key，再 free hits 数组，最后 free result。批量变体见 [§7.6](#76-批量搜索结果数组所有权)。

### 7.3 `bitcask_doc_input_t`

```c
typedef struct {
    bitcask_slice_t text;        // 必需（多字段时可空，作默认字段）
    bitcask_slice_t meta;        // 可选（data=NULL 跳过）
    const float*    vector;      // 可选（NULL=无向量）
    size_t          vector_len;  // 向量元素数
    uint32_t        expiry_at;   // S13-D5：per-key 过期时刻（绝对 unix 秒；0=永不）
} bitcask_doc_input_t;
```

### 7.4 `bitcask_kv_pair_t`（批量写）

```c
typedef struct {
    bitcask_slice_t key;
    bitcask_slice_t value;
} bitcask_kv_pair_t;
```

### 7.5 `bitcask_iter_entry_t`

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

**所有权**：`key.data` / `value.data` 是 malloc 缓冲（来自 `fill_iter_entry`，见 `c_api/internal.h`）。`bitcask_iter_entry_free` 释放二者并清零——**逐条释放**（每条 entry 一次调用）。

### 7.5b `bitcask_range_entry_t`（S33-6）

```c
typedef struct {
    bitcask_slice_t key;    // 指向内部 malloc 缓冲
    bitcask_slice_t value;  // 指向内部 malloc 缓冲
    uint64_t        tstamp;
    uint64_t        ord;
} bitcask_range_entry_t;
```

比 `bitcask_iter_entry_t` 少了 `file_id` / `offset` / `total_sz` / `is_tombstone`——range 迭代只产出活 key，位置信息在内部回查 keydir 时已消化。**所有权**同上：`bitcask_range_entry_free` 逐条释放 key/value 并清零。

### 7.6 批量搜索结果数组所有权

`bitcask_search_*_batch` 返回 `bitcask_search_result_t**`（指针的指针）：

- 外层数组是 `calloc(n, sizeof(...))`。
- 每个元素 `out_results[i]`：
  - 成功 → 指向独立 malloc 的 `bitcask_search_result_t`（其 `hits[i].key` 都是 strdup）
  - 失败 → `NULL`（单查询错误体现为对应槽 NULL，整体仍返回 `BITCASK_OK`）
- 用 `bitcask_search_result_batch_free(out_results, n)` 释放：内部逐元素调 `bitcask_search_result_free`（`NULL` 是 no-op），最后 free 外层数组。

### 7.7 `bitcask_status_t` / `bitcask_status_ex_t`

```c
typedef struct {
    uint64_t key_count;
    uint64_t key_bytes;
    uint64_t epoch;
    uint64_t index_errors;  // indexed worker 抛异常时自增；非零 = 索引可能漂移
} bitcask_status_t;

typedef struct {
    uint64_t key_count;
    uint64_t key_bytes;
    uint64_t epoch;
    uint64_t index_errors;
    uint64_t hnsw_nodes;           // HNSW 图节点数（含软删；无向量索引 = 0）
    uint64_t search_cache_entries; // 查询缓存当前条目数（无索引 = 0）
    uint64_t read_handles;         // read 句柄缓存当前大小（fd+mmap 数）
} bitcask_status_ex_t;
```

两个结构都是调用方栈/静态分配；`bitcask_status` / `bitcask_status_ex` 写入字段后返回，**无堆分配、无需 free**。

### 7.8 `bitcask_needs_merge_t`

```c
typedef struct {
    int     needs;        // 0=不需要，1=需要
    char**  files;        // 候选文件路径列表（strdup 数组，可能为 NULL）
    size_t  files_count;
} bitcask_needs_merge_t;
```

`bitcask_needs_merge` 内部对每个 path 调 `strdup`，最后 malloc `char**` 数组；调用方负责 `bitcask_needs_merge_free`。

### 7.9 Meta 过滤结构

```c
typedef struct {
    bitcask_meta_value_type_t type;
    int64_t     i64;  // BOOL（0/1）与 INT64 用
    double      f64;  // FLOAT64 用
    const char* str;  // STRING 用（NUL 结尾；其它类型忽略）
} bitcask_meta_value_t;

typedef struct {
    const char*                 key;   // 必填，NUL 结尾
    bitcask_meta_op_t           op;
    bitcask_meta_value_t        value;        // IN/EXISTS 之外使用
    const bitcask_meta_value_t* values;       // 仅 IN：候选值数组
    size_t                      values_count; // 仅 IN
} bitcask_meta_condition_t;

typedef struct bitcask_meta_filter {
    int logic_or;  // 0=AND（默认语义），非 0=OR
    const bitcask_meta_condition_t*   conditions;
    size_t                            conditions_count;
    const struct bitcask_meta_filter* children;  // 子树数组（嵌套组合）
    size_t                            children_count;
} bitcask_meta_filter_t;
```

**所有权**：所有指针借调用方存储（栈上/静态构造即可），仅在 `*_filtered` 搜索调用期间读取；调用返回后即可释放——引擎内部会转换成自有表示。空树（无条件无子树）恒通过。

非法条件：`key == NULL`、`STRING` 值缺 `str`、`op`/`type` 越界、嵌套深度 > 32 → `BITCASK_ERR_INVALID_OPTION`。

---

## 8. 生命周期：打开与关闭

### 8.1 `bitcask_open`

```c
BITCASK_API bitcask_error_t bitcask_open(const char* dirname,
                                         const bitcask_options_t* opts,
                                         bitcask_t** out,
                                         bitcask_fault_t* fault);
```

| 参数 | 含义 |
|------|------|
| `dirname` | 数据目录路径（NUL 结尾）|
| `opts`    | 打开选项（`NULL`=使用默认值）|
| `out`     | 成功时 `*out` 指向新实例，失败时 `*out = NULL` |
| `fault`   | 错误详情（`NULL`=忽略）|

**返回**：`BITCASK_OK` 或错误码。

**打开模式三态**（由 `bitcask_options_t` 字段决定，C API 无独立 `open_mode` 枚举）：

| 模式 | `read_write` | `merge_only` | 语义 |
|------|-------------|--------------|------|
| 只读 | `0` | `0` | 默认；不允许任何写操作 |
| 读写 | `1` | `0` | 允许 put/delete/sync |
| merge-only | `0` | `1` | 仅执行 `bitcask_merge`，拒绝读写 |

可能错误：`BITCASK_ERR_IO`、`BITCASK_ERR_WRITE_LOCKED`、`BITCASK_ERR_INVALID_OPTION`（含同义词文件无法打开）、`BITCASK_ERR_MODE_MISMATCH`、`BITCASK_ERR_ANALYZER_MISMATCH`。

### 8.2 `bitcask_close`

```c
BITCASK_API void bitcask_close(bitcask_t* cask);
```

关闭并释放实例。内部调 `Cask::close()` 后 delete 句柄包装。`cask` 句柄此后不可使用。`cask == NULL` 是 no-op。**注意**：`bitcask_close` 调用 `delete`，因此句柄本身被 free——之后任何使用该指针的调用返回 `BITCASK_ERR_CLOSED`，不崩溃。

**内存配对**：`bitcask_open` ↔ `bitcask_close`。

---

## 9. KV 操作

### 9.1 `bitcask_get`

```c
BITCASK_API bitcask_error_t bitcask_get(bitcask_t* cask,
                                        bitcask_slice_t key,
                                        bitcask_get_result_t** out,
                                        bitcask_fault_t* fault);
```

成功返回 `BITCASK_OK`，`*out` 指向新建结果（`malloc`，内含 malloc 的 value/meta/vector 缓冲），调用方负责 `bitcask_get_result_free`。key 不存在返回 `BITCASK_ERR_NOT_FOUND`，`*out = NULL`。

### 9.2 `bitcask_put`

```c
BITCASK_API bitcask_error_t bitcask_put(bitcask_t* cask,
                                        bitcask_slice_t key,
                                        bitcask_slice_t value,
                                        uint32_t tstamp,
                                        bitcask_fault_t* fault);
```

`tstamp = 0` 表示使用当前时间。

### 9.3 `bitcask_put_ex`（带 per-key TTL）

```c
BITCASK_API bitcask_error_t bitcask_put_ex(bitcask_t* cask,
                                           bitcask_slice_t key,
                                           bitcask_slice_t value,
                                           uint32_t tstamp,
                                           uint32_t expiry_at,
                                           bitcask_fault_t* fault);
```

`expiry_at` = 绝对 unix 秒（`0` = 永不过期，等价 `bitcask_put`）。过期后 `get` / `iter` 视作不存在，空间 merge 时回收。

### 9.4 `bitcask_delete`

```c
BITCASK_API bitcask_error_t bitcask_delete(bitcask_t* cask,
                                           bitcask_slice_t key,
                                           uint32_t tstamp,
                                           bitcask_fault_t* fault);
```

软删除（写入墓碑）。`tstamp = 0` 使用当前时间。

### 9.5 `bitcask_sync`

```c
BITCASK_API bitcask_error_t bitcask_sync(bitcask_t* cask,
                                         bitcask_fault_t* fault);
```

`fsync` active data file。`o_sync` 模式下 no-op（已每条 fsync）。

### 9.6 `bitcask_close_write_file`

```c
BITCASK_API bitcask_error_t bitcask_close_write_file(bitcask_t* cask,
                                                     bitcask_fault_t* fault);
```

关 active write file，释放 write lock。下次 `put` 自动重开。

### 9.7 `bitcask_get_result_free`

```c
BITCASK_API void bitcask_get_result_free(bitcask_get_result_t* result);
```

**所有权**：`result == NULL` 是 no-op；否则依次 `free(value.data)` / `free(meta.data)` / `free(vector)`，最后 `free(result)`（实现见 `c_api/bitcask_kv.cpp::bitcask_get_result_free`）。

**内存配对**：`bitcask_get` ↔ `bitcask_get_result_free`。

---

## 10. 结构化文档与批量写

### 10.1 `bitcask_put_doc`

```c
BITCASK_API bitcask_error_t bitcask_put_doc(bitcask_t* cask,
                                            bitcask_slice_t key,
                                            const bitcask_doc_input_t* doc,
                                            uint32_t tstamp,
                                            bitcask_fault_t* fault);
```

写入结构化文档（索引模式）。`doc == NULL` 返回 `BITCASK_ERR_INVALID_OPTION`。

### 10.2 `bitcask_put_batch`（批量写）

```c
BITCASK_API bitcask_error_t bitcask_put_batch(bitcask_t* cask,
                                              const bitcask_kv_pair_t* items,
                                              size_t n,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault);
```

语义同逐条 `bitcask_put`，但整批一次提交：记录聚合写入、单次 flush 后才更新 keydir 并返回——**本进程内 all-or-nothing 可见**。`items` 借调用方存储；校验（key/value 大小）在任何写之前全批完成。

- `n == 0`：no-op，返回 `BITCASK_OK`。
- `items == NULL && n > 0`：`BITCASK_ERR_INVALID_OPTION`。
- durability 与单条 `put` 的 sync 策略一致（`o_sync` 即时；`sync_every_n > 0` 整批一次组提交；否则由 `bitcask_sync` 控制）。
- 失败返回时整批不可见（磁盘可能残留前缀，重启后可见——与连续单条 `put` 的崩溃语义一致）。

### 10.3 原子批与多键事务（S34/S35：`bitcask_put_batch_atomic` / `bitcask_txn_*`）

```c
typedef struct {
    uint8_t         op;     /* 0 = put, 1 = remove（remove 忽略 value） */
    bitcask_slice_t key;
    bitcask_slice_t value;
} bitcask_txn_op_t;

BITCASK_API bitcask_error_t bitcask_put_batch_atomic(bitcask_t* cask,
                                                     const bitcask_txn_op_t* ops,
                                                     size_t n_ops,
                                                     bitcask_fault_t* fault);
BITCASK_API bitcask_error_t bitcask_txn_commit(bitcask_t* cask,
                                               const bitcask_txn_op_t* ops,
                                               size_t n_ops,
                                               int sync_on_commit,
                                               bitcask_fault_t* fault);
BITCASK_API bitcask_error_t bitcask_txn_recover(bitcask_t* cask,
                                                size_t* out_replayed,
                                                bitcask_fault_t* fault);
BITCASK_API bitcask_error_t bitcask_txn_pending_count(bitcask_t* cask,
                                                      size_t* out_count,
                                                      bitcask_fault_t* fault);
```

S35 引擎原子批（设计 `doc/atomic-batch-design-zh.md`；模式原理 `doc/multikey-txn-zh.md`）。崩溃原子性（A）+ 持久性（D）；**不提供**隔离性（I）与 CAS。key 命名空间 `_txn:` 保留。

- `bitcask_put_batch_atomic`：引擎原子批直通——崩溃/掉电后整批要么全可见要么全不可见；批内 op 依序 apply（同 key 多次 = 批内 LWW，不校验重复）。**首次调用把目录 meta 懒升级为 v6**（旧于 5.1.0 的读端拒开该目录；从不调用则停留 v5）。durability 同 `bitcask_put_batch`。
- `bitcask_txn_commit`：= 原子批 + 事务级校验（空批 / 空 key / 重复 key / `_txn:` 前缀 → `BITCASK_ERR_INVALID_OPTION` 零副作用）。`sync_on_commit` 非零 = 提交后显式 fsync（防掉电丢批——原子性与持久性正交）。
- `bitcask_txn_recover` / `bitcask_txn_pending_count`（B2 起恒返回 0）：方案 B 意图重放已删除（意图日志从未随发布版本存在）；签名保留。`out_replayed` 可为 `NULL`。
- `bitcask_txn_pending_count`：legacy 巡检；S35 后正常恒 0。

---

## 11. 迭代与并行扫描

### 11.1 `bitcask_iter_start`

```c
BITCASK_API bitcask_error_t bitcask_iter_start(bitcask_t* cask,
                                               int maxage,
                                               int maxputs,
                                               int see_tombstones,
                                               bitcask_iter_t** out,
                                               bitcask_fault_t* fault);
```

启动迭代器快照。

- `maxage`：freshness 容忍度（`-1`=不限）
- `maxputs`：容忍的 pending puts 数（`-1`=不限）
- `see_tombstones`：`0`=跳过墓碑，`1`=包含
- 返回 `BITCASK_OK` 或 `BITCASK_ERR_INVALID_OPTION`（快照过期，caller 重试）

**内存配对**：`bitcask_iter_start` ↔ `bitcask_iter_release`。

#### `bitcask_iter_start_prefix`（S33-6）

```c
BITCASK_API bitcask_error_t bitcask_iter_start_prefix(bitcask_t* cask,
                                                      int maxage,
                                                      int maxputs,
                                                      int see_tombstones,
                                                      bitcask_slice_t key_prefix,
                                                      bitcask_iter_t** out,
                                                      bitcask_fault_t* fault);
```

带 key 前缀过滤的迭代器快照（C++ `CaskIter::start(..., key_prefix)` 在 C 侧的补齐）。`key_prefix` 为空切片（`{NULL, 0}`）时与 `bitcask_iter_start` 完全等价——后者就是以空切片调用本函数。

- 过滤发生在 keydir proxy 层（不读 value），但 **keydir 是哈希表 ⟹ 仍是 O(全表) 扫描**，省的是 value 读取与跨界拷贝。
- 有序范围扫描（O(range)）请用本章 §11.7 `bitcask_range_iter_*`。
- 其余参数、返回码、内存配对同 `bitcask_iter_start`。

### 11.2 `bitcask_iter_next`

```c
BITCASK_API int bitcask_iter_next(bitcask_iter_t* iter,
                                  bitcask_iter_entry_t* entry,
                                  bitcask_fault_t* fault);
```

返回值：

| 返回 | 含义 |
|------|------|
| `1`  | 有数据（`entry` 已填充，key/value 指向 malloc 缓冲）|
| `0`  | 迭代结束 |
| `< 0` | 错误（`fault` 已回填）|

**所有权**：`entry` 内的 `key.data` / `value.data` 是 malloc 缓冲，**每次返回 `1` 后调用方必须调 `bitcask_iter_entry_free`**。

### 11.3 `bitcask_iter_next_batch`

```c
BITCASK_API int bitcask_iter_next_batch(bitcask_iter_t* iter,
                                        bitcask_iter_entry_t* entries,
                                        size_t max_n,
                                        bitcask_fault_t* fault);
```

取最多 `max_n` 条。返回取到的条数（`0`=迭代结束），`< 0`=错误。`entries` 是调用方分配的数组，`max_n` 为数组大小。

**所有权**：

- 返回 `>= 0`：每条 entry 的 key/value 是 malloc 缓冲，调用方**逐条**调 `bitcask_iter_entry_free`。
- 返回 `< 0`（错误）：**实现已释放中途填充的条目缓冲**——调用方无需（也不可）对 `entries` 做任何 free（见 `c_api/bitcask_kv.cpp::bitcask_iter_next_batch` 中错误路径的释放循环）。

### 11.4 `bitcask_iter_release`

```c
BITCASK_API void bitcask_iter_release(bitcask_iter_t* iter);
```

释放迭代器（可提前调用，之后不可再用）。`iter == NULL` 是 no-op。内部 `delete` 句柄包装。

### 11.5 `bitcask_iter_entry_free`

```c
BITCASK_API void bitcask_iter_entry_free(bitcask_iter_entry_t* entry);
```

释放 entry 内部缓冲（key/value 的 malloc 缓冲），并把 `data` 置 `NULL`、`size` 置 `0`。`entry == NULL` 是 no-op。

**内存配对**：`bitcask_iter_next` / `_next_batch` 的每条 entry ↔ `bitcask_iter_entry_free`（返回 `1` 时调用；错误路径实现已自行释放）。

### 11.6 `bitcask_parallel_scan`：并行全表扫描

```c
typedef void (*bitcask_scan_fn)(void* ctx,
                                bitcask_slice_t key,
                                bitcask_slice_t value);

BITCASK_API bitcask_error_t bitcask_parallel_scan(bitcask_t* cask,
                                                  size_t n_threads,
                                                  bitcask_scan_fn fn,
                                                  void* ctx,
                                                  size_t* out_count,
                                                  bitcask_fault_t* fault);
```

单次快照所有 live key（调用线程串行，仅拷 key），按 `n_threads` 分段并发 `get` 读值并调 `fn`——把"多线程读安全"用于 analytics / export / reindex。

- `n_threads == 0` → `hardware_concurrency()`。
- `*out_count`（可为 NULL）= 遍历到的 key 数。
- **回调可能来自多个工作线程并发调用**——回调内写共享状态须自行加锁/用原子。
- `key` / `value` 是零拷贝 view，**仅在本次回调内有效**（需保留请自行拷贝）。
- 并发删除致某 key `get` 时 not-found → 跳过（near-real-time）；IO/CRC 错误 → 停止并返回。
- `cask` 已 close → `BITCASK_ERR_CLOSED`。

**所有权**：回调参数 `key` / `value` 是非拥有视图，无需 free。

#### `bitcask_parallel_scan_prefix`（S33-6）

```c
BITCASK_API bitcask_error_t bitcask_parallel_scan_prefix(bitcask_t* cask,
                                                         size_t n_threads,
                                                         bitcask_slice_t key_prefix,
                                                         bitcask_scan_fn fn,
                                                         void* ctx,
                                                         size_t* out_count,
                                                         bitcask_fault_t* fault);
```

带 key 前缀过滤的并行扫描。过滤在快照阶段（keydir proxy 层）完成——只有匹配的 key 会进入并发 `get`。空切片等价 `bitcask_parallel_scan`（后者即以空切片调用本函数）。其余语义完全相同。

### 11.7 `bitcask_range_iter_*`：有序 range 迭代（S33-6）

按 key **字典序**遍历 `[lo, hi)`，走 OKI（有序 key 索引）——**O(range)**，与 `bitcask_iter_start_prefix` 的 O(全表) 相对。对应 C++ 的 `Cask::make_range_iter` / `CaskRangeIter`（语义细节见 [`api-cpp.md`](api-cpp.md) §6）。

```c
typedef struct bitcask_range_iter_t bitcask_range_iter_t;

typedef struct {
    bitcask_slice_t lo;               /* inclusive；空 = 从头 */
    bitcask_slice_t hi;               /* exclusive；空 = 到尾 */
    size_t          prefetch;         /* 0/1 = 关闭；>1 = 批量并发预取值 */
    size_t          prefetch_threads; /* 0 = min(hardware_concurrency, 4) */
} bitcask_range_options_t;

typedef struct {
    bitcask_slice_t key;    /* malloc 缓冲 */
    bitcask_slice_t value;  /* malloc 缓冲 */
    uint64_t        tstamp;
    uint64_t        ord;
} bitcask_range_entry_t;

BITCASK_API void bitcask_range_options_init(bitcask_range_options_t* opts);

BITCASK_API bitcask_error_t bitcask_range_iter_start(
    bitcask_t* cask, const bitcask_range_options_t* opts,
    bitcask_range_iter_t** out, bitcask_fault_t* fault);

BITCASK_API int  bitcask_range_iter_next(bitcask_range_iter_t* iter,
                                         bitcask_range_entry_t* entry,
                                         bitcask_fault_t* fault);
BITCASK_API int  bitcask_range_iter_next_batch(bitcask_range_iter_t* iter,
                                               bitcask_range_entry_t* entries,
                                               size_t max_n,
                                               bitcask_fault_t* fault);
BITCASK_API void bitcask_range_iter_release(bitcask_range_iter_t* iter);
BITCASK_API void bitcask_range_entry_free(bitcask_range_entry_t* entry);
```

- **一致性：per-key 弱一致**（与 `bitcask_parallel_scan` 同档）——迭代期间的并发写可能部分可见，**不是** `bitcask_iter_*` 的快照语义。
- `opts == NULL` 等价「全域 + 无预取」；`opts` 指向的切片仅在 `bitcask_range_iter_start` 调用期间被引用（内部已拷贝边界），返回后调用方可释放。
- `prefetch > 1`：一次归并出 `prefetch` 个 key 后并发取值。**只改变取值时机，输出序与内容不变**。收益形态是「大窗口 + 值不在页缓存」；小窗口/值已缓存时线程成本可能反超（实测数字见 `api-cpp.md` §6 的 `prefetch` 条）。
- 错误：`BITCASK_ERR_NO_INDEX` = 只读/merge-only 打开且目录未建过 OKI（本句柄本就不建；读写方式打开会自动重建）；`BITCASK_ERR_INDEX_REBUILD_FAILED` = 读写打开时 OKI 重建失败（IO/环境问题，见日志；修复后重开可重试）；`BITCASK_ERR_BAD_CRC` = run 损坏；其余同 `bitcask_get`。
- `next` 返回值 / `next_batch` 的错误路径所有权规则与 `bitcask_iter_next` / `_next_batch` **完全一致**（错误时实现已释放中途填充的条目）。
- 生命周期：迭代器须在 `bitcask_close` 之前 release。

**内存配对**：`bitcask_range_iter_start` ↔ `bitcask_range_iter_release`；每条 entry ↔ `bitcask_range_entry_free`。

```c
bitcask_range_options_t ro;
bitcask_range_options_init(&ro);
ro.lo.data = "user:";  ro.lo.size = 5;
ro.hi.data = "user;";  ro.hi.size = 5;   /* ':' + 1 */
ro.prefetch = 256;                        /* 可选 */

bitcask_range_iter_t* it = NULL;
if (bitcask_range_iter_start(cask, &ro, &it, &fault) == BITCASK_OK) {
    bitcask_range_entry_t e;
    while (bitcask_range_iter_next(it, &e, &fault) == 1) {
        /* e.key / e.value 升序；用完即 free */
        bitcask_range_entry_free(&e);
    }
    bitcask_range_iter_release(it);
}
```

---

## 12. 状态与合并管理

### 12.1 `bitcask_status`

```c
BITCASK_API bitcask_error_t bitcask_status(bitcask_t* cask,
                                           bitcask_status_t* out,
                                           bitcask_fault_t* fault);
```

填充 `bitcask_status_t`（栈/调用方持有）。无堆分配。

### 12.2 `bitcask_status_ex`（扩展观测）

```c
BITCASK_API bitcask_error_t bitcask_status_ex(bitcask_t* cask,
                                              bitcask_status_ex_t* out,
                                              bitcask_fault_t* fault);
```

填充 `bitcask_status_ex_t`（栈/调用方持有）。无堆分配。在 `bitcask_status_t` 基础上追加 `hnsw_nodes`、`search_cache_entries`、`read_handles`。

### 12.3 `bitcask_needs_merge`

```c
BITCASK_API bitcask_error_t bitcask_needs_merge(bitcask_t* cask,
                                               bitcask_needs_merge_t* out,
                                               bitcask_fault_t* fault);
```

返回 `BITCASK_OK`，`out->needs` 标记是否需要（`0`/`1`），`out->files` 列出候选文件路径（strdup 数组）。调用方负责 `bitcask_needs_merge_free`。

### 12.4 `bitcask_needs_merge_free`

```c
BITCASK_API void bitcask_needs_merge_free(bitcask_needs_merge_t* nm);
```

**所有权**：`nm == NULL` 是 no-op；否则依次 `free(nm->files[i])`（i = 0..files_count-1），再 `free(nm->files)`，最后置 `files = NULL`、`files_count = 0`（实现见 `c_api/bitcask_kv.cpp::bitcask_needs_merge_free`）。

**内存配对**：`bitcask_needs_merge` ↔ `bitcask_needs_merge_free`。

### 12.5 `bitcask_merge`

```c
BITCASK_API bitcask_error_t bitcask_merge(bitcask_t* cask,
                                          bitcask_fault_t* fault);
```

执行 merge（内部自动调 `needs_merge` 决定）。与读写并发，不阻塞 writer。

### 12.6 `bitcask_is_empty` / `bitcask_is_frozen`

```c
BITCASK_API int bitcask_is_empty(bitcask_t* cask);   // 写过 key 后即使删光也返回 0
BITCASK_API int bitcask_is_frozen(bitcask_t* cask);  // keydir 是否被 fold/iter pin 住
```

返回 `0` / `1`。`cask == NULL` 时分别返回 `1` / `0`。

### 12.7 `bitcask_flush_index`

```c
BITCASK_API void bitcask_flush_index(bitcask_t* cask);
```

刷新异步索引队列（索引模式下，确保 pending 写入被索引）。`cask == NULL` 是 no-op。

---

## 13. BM25 文本检索

> 需要索引模式（`enable_search = 1` 且 `analyzer_type != NONE`）。无索引调用 → `BITCASK_ERR_NO_INDEX`。

### 13.1 `bitcask_search_text`（词袋）

```c
BITCASK_API bitcask_error_t bitcask_search_text(bitcask_t* cask,
                                                const char* query,
                                                size_t k,
                                                bitcask_search_result_t** out,
                                                bitcask_fault_t* fault);
```

`query`：NUL 结尾 UTF-8。`k`：返回 top-k。`out`：成功 `*out` 指向 malloc 结果，调用方 `bitcask_search_result_free`。

### 13.2 `bitcask_search_text_filtered`（词袋 + meta 过滤）

```c
BITCASK_API bitcask_error_t bitcask_search_text_filtered(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);
```

- `filter == NULL` → 退化为 `bitcask_search_text`。
- `filter` 非法（`key == NULL`、`STRING` 值缺 `str`、嵌套深度 > 32、`op`/`type` 越界）→ `BITCASK_ERR_INVALID_OPTION`。
- **注意**：`filter` 非空时**没有 meta 段的文档一律不通过**（引擎"空 blob 不通过"约定，与 C++ `MetaFilter` 行为一致）——含 `Neq`/`Exists` 等否定式条件。

### 13.3 `bitcask_search_text_batch`（批量词袋）

```c
BITCASK_API bitcask_error_t bitcask_search_text_batch(bitcask_t* cask,
                                                      const char* const* queries,
                                                      size_t n,
                                                      size_t k,
                                                      bitcask_search_result_t*** out_results,
                                                      bitcask_fault_t* fault);
```

一次 `prepare_search` flush 覆盖全批，比逐条调用省重复索引 flush。语义/可见性同 `bitcask_search_text`（并发读安全）。

- `queries`：`n` 个 NUL 结尾查询串的数组。
- `n == 0` → `*out_results = NULL` 且返回 `BITCASK_OK`。
- 任一 `queries[i] == NULL` → `BITCASK_ERR_INVALID_OPTION`。
- 返回 `BITCASK_OK`：批量已执行；**单查询错误体现为对应元素 `NULL`**（无命中则 `count == 0` 的非空结果）。
- `fault`：回填首个失败查询的错误详情（best-effort 诊断）。

### 13.4 `bitcask_search_phrase`（短语）

```c
BITCASK_API bitcask_error_t bitcask_search_phrase(bitcask_t* cask,
                                                  const char* query,
                                                  size_t k,
                                                  bitcask_search_result_t** out,
                                                  bitcask_fault_t* fault);
```

### 13.5 `bitcask_bool_search`（布尔 AND/OR/NOT）

```c
BITCASK_API bitcask_error_t bitcask_bool_search(bitcask_t* cask,
                                                const char* query,
                                                size_t k,
                                                bitcask_search_result_t** out,
                                                bitcask_fault_t* fault);
```

### 13.6 `bitcask_search_fields`（多字段 `field:term^boost`）

```c
BITCASK_API bitcask_error_t bitcask_search_fields(bitcask_t* cask,
                                                  const char* query,
                                                  size_t k,
                                                  bitcask_search_result_t** out,
                                                  bitcask_fault_t* fault);
```

### 13.7 `bitcask_search_near`（近邻）

```c
BITCASK_API bitcask_error_t bitcask_search_near(bitcask_t* cask,
                                                const char* query,
                                                uint32_t slop,
                                                size_t k,
                                                bitcask_search_result_t** out,
                                                bitcask_fault_t* fault);
```

`slop` = term 间允许的最大间隙（`0` = 短语）。

### 13.8 `bitcask_search_fuzzy`（模糊，Levenshtein）

```c
BITCASK_API bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask,
                                                 const char* query,
                                                 size_t k,
                                                 uint32_t max_edit_distance,
                                                 bitcask_search_result_t** out,
                                                 bitcask_fault_t* fault);
```

`max_edit_distance` = Levenshtein 编辑距离上限。

### 13.9 `bitcask_search_wildcard`（通配符 `*` / `?`）

```c
BITCASK_API bitcask_error_t bitcask_search_wildcard(bitcask_t* cask,
                                                    const char* pattern,
                                                    size_t k,
                                                    bitcask_search_result_t** out,
                                                    bitcask_fault_t* fault);
```

### 13.10 `bitcask_search_result_free`（单查询结果）

```c
BITCASK_API void bitcask_search_result_free(bitcask_search_result_t* result);
```

**所有权**：`result == NULL` 是 no-op；否则对每个 `hits[i].key` 调 `free`，再 `free(hits)`，最后 `free(result)`（实现见 `c_api/bitcask_kv.cpp::bitcask_search_result_free`）。

**内存配对**：所有 `bitcask_search_*` 单查询入口 ↔ `bitcask_search_result_free`。

### 13.11 `bitcask_search_result_batch_free`（批量结果数组）

```c
BITCASK_API void bitcask_search_result_batch_free(bitcask_search_result_t** results,
                                                  size_t n);
```

**所有权**：`results == NULL` 是 no-op；否则对 `i = 0..n-1` 逐元素调 `bitcask_search_result_free(results[i])`（`NULL` 元素是 no-op），最后 `free(results)` 外层数组。

**内存配对**：`bitcask_search_text_batch` ↔ `bitcask_search_result_batch_free`；同理 `bitcask_search_vector_batch`、`bitcask_search_hybrid_batch`。

---

## 14. 向量检索（HNSW / IVF-RaBitQ / DiskANN）与 RRF 混合检索

> 向量引擎由 `bitcask_options_t::vector_engine` 建库时选定（见 [§6.5](#65-bitcask_options_t打开选项)），持久化进 `bitcask.meta`。`HNSW`（默认，内存图）/ `IVFRQ`（IVF 磁盘段，10M-100M 推荐）/ `DISKANN`（Vamana 图，实验性）。以下接口对三引擎统一——查询参数 `ef` 在 IVF 引擎下按 `nprobe` 解释。

### 14.1 `bitcask_search_vector`（向量 ANN）

```c
BITCASK_API bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                                  const float* query,
                                                  size_t query_len,
                                                  size_t k,
                                                  size_t ef,
                                                  bitcask_search_result_t** out,
                                                  bitcask_fault_t* fault);
```

- `query`：`query_len` 个 f32（`query_len` 必须等于 `vector_dim`）。
- `ef`：搜索时探索宽度（`0` = `max(k, 64)`）。

### 14.2 `bitcask_search_vector_batch`（批量向量）

```c
BITCASK_API bitcask_error_t bitcask_search_vector_batch(bitcask_t* cask,
                                                        const float* const* queries,
                                                        size_t n,
                                                        size_t query_len,
                                                        size_t k,
                                                        size_t ef,
                                                        bitcask_search_result_t*** out_results,
                                                        bitcask_fault_t* fault);
```

一次 flush 覆盖全批。`queries`：`n` 个向量指针（每个 `query_len` 个 f32）。结果数组语义/释放同 `bitcask_search_text_batch`（`bitcask_search_result_batch_free`）。

### 14.3 `bitcask_search_vector_filtered`（向量 + meta 过滤）

```c
BITCASK_API bitcask_error_t bitcask_search_vector_filtered(
    bitcask_t* cask, const float* query, size_t query_len, size_t k, size_t ef,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);
```

过滤语义同 `bitcask_search_text_filtered`。

### 14.4 `bitcask_search_hybrid`（RRF 混合）

```c
BITCASK_API bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                                  const char* text_query,
                                                  const float* vec_query,
                                                  size_t vec_len,
                                                  size_t k,
                                                  bitcask_search_result_t** out,
                                                  bitcask_fault_t* fault);
```

- `text_query`：NUL 结尾 UTF-8（`NULL` = 纯向量路径）
- `vec_query`：f32 向量（`NULL` = 纯文本路径）
- 两路都 `NULL` → `BITCASK_ERR_INVALID_OPTION`

### 14.5 `bitcask_hybrid_query_t`：批量混合的单条输入

```c
typedef struct {
    const char*  text;        // NUL 结尾 UTF-8（NULL = 纯向量）
    const float* vector;      // f32 向量（NULL = 纯文本）
    size_t       vector_len;  // 向量元素数（vector==NULL 时忽略）
} bitcask_hybrid_query_t;
```

### 14.6 `bitcask_search_hybrid_batch`（批量混合）

```c
BITCASK_API bitcask_error_t bitcask_search_hybrid_batch(bitcask_t* cask,
                                                        const bitcask_hybrid_query_t* queries,
                                                        size_t n,
                                                        size_t k,
                                                        bitcask_search_result_t*** out_results,
                                                        bitcask_fault_t* fault);
```

一次 flush 覆盖全批。每条查询是 `(text, vector)` 对——text / vector 至少一非空。结果数组语义/释放同 `bitcask_search_text_batch`（`bitcask_search_result_batch_free`）。

### 14.7 `bitcask_search_hybrid_filtered`（混合 + meta 过滤）

```c
BITCASK_API bitcask_error_t bitcask_search_hybrid_filtered(
    bitcask_t* cask, const char* text_query,
    const float* vec_query, size_t vec_len, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault);
```

过滤语义同 `bitcask_search_text_filtered`；两路查询语义同 `bitcask_search_hybrid`。

---

## 15. 内存管理：ownership 配对

**FFI 头号事故来源**就是误配 `*_free`（泄漏 / double-free）。下表精确列出每个返回堆内存的函数 + 对应释放函数。所有 `*_free` 函数的语义均经 `c_api/*.cpp` 实现逐条核对（实现仅调用 `std::free`，与 C `free()` 等价）。

| 分配方（返回堆内存/句柄） | 释放方 | 释放实现细节 | 备注 |
|--------------------------|--------|------|------|
| `bitcask_open` → `bitcask_t*` | `bitcask_close` | `delete` 句柄包装（内部先调 `Cask::close()`）| 同一 handle 必须恰好 1 次 `close`；句柄指针之后失效 |
| `bitcask_get` → `bitcask_get_result_t*` | `bitcask_get_result_free` | 依次 `free(value.data)` / `free(meta.data)` / `free(vector)`，最后 `free(result)` | 仅在 `BITCASK_OK` 时有效；`BITCASK_ERR_NOT_FOUND` 时 `*out = NULL` 不需 free |
| `bitcask_search_*`（单查询）→ `bitcask_search_result_t*` | `bitcask_search_result_free` | 对每个 `hits[i].key` 调 `free`，再 `free(hits)`，最后 `free(result)` | 包含 `search_text / _phrase / _bool / _fields / _near / _fuzzy / _wildcard / _vector / _hybrid` 及对应 `_filtered` 共 12 个单查询入口 |
| `bitcask_search_*_batch` → `bitcask_search_result_t**` | `bitcask_search_result_batch_free` | 逐元素调 `bitcask_search_result_free`（`NULL` no-op），最后 `free(results)` | 适用于 `search_text_batch / _vector_batch / _hybrid_batch` |
| `bitcask_needs_merge` → `bitcask_needs_merge_t.files` | `bitcask_needs_merge_free` | 依次 `free(files[i])`，再 `free(files)`，最后置 `files = NULL`、`files_count = 0` | 即使 `needs == 0` 也调用以保证幂等（`files` 可能为 NULL → no-op）|
| `bitcask_iter_start` → `bitcask_iter_t*` | `bitcask_iter_release` | `delete` 句柄包装 | 可早于迭代结束调用 |
| `bitcask_iter_next` / `_next_batch` → 每条 `bitcask_iter_entry_t` 内的 key/value | `bitcask_iter_entry_free` | `free(key.data)` / `free(value.data)` 并清零 | **逐条调用**；`bitcask_iter_next_batch` 错误路径已自行释放，调用方不可再 free |
| `bitcask_range_iter_start` → `bitcask_range_iter_t*` | `bitcask_range_iter_release` | `delete` 句柄包装 | 可早于迭代结束调用；须在 `bitcask_close` 之前 |
| `bitcask_range_iter_next` / `_next_batch` → 每条 `bitcask_range_entry_t` 内的 key/value | `bitcask_range_entry_free` | `free(key.data)` / `free(value.data)` 并清零 | **逐条调用**；错误路径同 `bitcask_iter_next_batch`（实现已释放，不可再 free）|

**无需 free** 的"看起来返回指针"的函数：

| 函数 | 实际语义 |
|------|---------|
| `bitcask_version_string()` | 库内 `BITCASK_VERSION_STRING` 字符串字面量（静态生命周期）|
| `bitcask_options_init()` | 写入调用方栈/堆上的 `bitcask_options_t`，不分配 |
| `bitcask_status()` / `bitcask_status_ex()` | 写入调用方栈/堆上的结构，无堆分配 |
| `bitcask_iter_next()` / `_next_batch()` 的 `entry` | 由调用方提供，库写入字段；`key/value` 数据才是堆 |
| `bitcask_range_options_init()` / `bitcask_range_iter_next()` 的 `entry` | 同上：结构由调用方提供，只有 `key/value` 数据在堆上 |
| `bitcask_fault_t::detail` | 结构内嵌 512B 数组，随实例生命周期 |
| 所有 `*_slice_t` / `*_scan_fn` 回调的 key/value | 非拥有视图 |
| `bitcask_meta_filter_t` / `bitcask_meta_value_t` / `bitcask_doc_input_t` / `bitcask_hybrid_query_t` / `bitcask_kv_pair_t` | 调用方栈/静态构造即可 |

**所有权陷阱**：

- `bitcask_iter_entry_free` **只释放 entry 内的 malloc 缓冲**（`key.data` / `value.data`），**不释放** entry 结构本身（entry 是栈/调用方提供）。
- `bitcask_iter_next_batch` 错误返回 `< 0` 时**实现已释放已填充条目**；调用方不可再对 `entries` 调 `bitcask_iter_entry_free`（否则 double-free）。
- `bitcask_search_result_batch_free` 第二个参数 `n` 必须与分配时的 `n` 一致；外层数组 `calloc(n, ...)` 后 free 必须按 `n` 释放（实现按 `n` 遍历）。
- 已 `bitcask_close` 的 `bitcask_t*` 再调任何函数返回 `BITCASK_ERR_CLOSED`，不崩溃（fail-fast 生命周期）。

---

## 16. 线程安全模型

与 C++ 核心一致（**通用 C++ 库：同一 handle 多线程安全**）。C API 是 `bitcask::Cask` 的薄包装，无 C 层共享可变态，完全继承其契约。

图例：✅ 多线程安全；⚠️ 有条件/生命周期。

| 操作 | 线程安全 |
|------|---------|
| `bitcask_open` | ✅（产生独立对象，可并发打开不同目录/同目录）|
| `bitcask_close` | ⚠️（生命周期：close 即 `delete` 句柄包装；caller 须保证无在途调用且不再使用）|
| `bitcask_get` | ✅（读路径无锁）|
| `bitcask_put` / `bitcask_put_ex` / `bitcask_put_doc` / `bitcask_put_batch` / `bitcask_delete` / `bitcask_sync` / `bitcask_close_write_file` | ✅（内部 `write_mu_` 串行化；同一 handle 多线程写安全。单写吞吐不受锁影响；多写**安全但不提速**——更高写并发按目录分片多实例横向扩展）|
| `bitcask_search_text` / `_phrase` / `_bool` / `_fields` / `_near` / `_fuzzy` / `_wildcard` / `_filtered` | ✅（并发读安全，shared_lock / 无锁）|
| `bitcask_search_vector` / `_hybrid` 及对应 `_filtered` / `_batch` | ✅（向量引擎读路径；批量接口走共享 `search_arena` inter-query 并行）|
| 同义词词典（`options.synonym_file_path`，open-time） | ✅（不可变 → 并发查询安全；无运行期 setter）|
| `bitcask_iter_*` | ⚠️（同一 iter 不可并发；每线程一个迭代器；不同 iter 之间并发安全）|
| `bitcask_range_iter_start` | ✅（取 OKI 只读视图）|
| `bitcask_range_iter_next` / `_next_batch` / `_release` | ⚠️（同一 iter 不可并发；每线程一个；与并发写/merge 安全——per-key 弱一致）|
| `bitcask_parallel_scan` / `_prefix` | ✅（内部多线程并发 `get`；**回调可能多线程并发触发**——回调须线程安全）|
| `bitcask_status` / `bitcask_status_ex` / `bitcask_needs_merge` / `bitcask_merge` / `bitcask_is_empty` / `bitcask_is_frozen` / `bitcask_flush_index` | ✅ |
| 读 / 写并发 | ✅（搜索可见性 near-real-time）|
| `bitcask_merge` 与读写并发 | ✅（keydir `shared_mutex` 协调 + 独立 `merge.lock`，不阻塞 writer）|

---

## 17. 完整 C 示例

### 17.1 最小 KV（open → put → get → close）

```c
#include "bitcask_c.h"
#include <stdio.h>

int main(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    if (bitcask_open("/tmp/db", &opts, &cask, &fault) != BITCASK_OK) {
        fprintf(stderr, "open: %s\n", fault.detail);
        return 1;
    }

    bitcask_slice_t key = {"hello", 5}, val = {"world", 5};
    if (bitcask_put(cask, key, val, 0, &fault) != BITCASK_OK) {
        fprintf(stderr, "put: %s\n", fault.detail);
    }

    bitcask_get_result_t* res = NULL;
    if (bitcask_get(cask, key, &res, &fault) == BITCASK_OK) {
        printf("value: %.*s\n", (int)res->value.size, (char*)res->value.data);
        bitcask_get_result_free(res);
    }

    bitcask_close(cask);
    return 0;
}
```

### 17.2 索引模式（BM25 + 向量 + 混合）

```c
#include "bitcask_c.h"
#include <stdio.h>

int main(void) {
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
        .text = {"北京今天天气晴朗", 24},
        .vector = vec, .vector_len = 4,
    };
    bitcask_put_doc(cask, key, &doc, 0, &fault);
    bitcask_flush_index(cask);

    /* ---- 词袋检索 ---- */
    bitcask_search_result_t* res = NULL;
    if (bitcask_search_text(cask, "北京 天气", 10, &res, &fault) == BITCASK_OK) {
        for (size_t i = 0; i < res->count; ++i)
            printf("hit[%zu] %s score=%.4f\n",
                   i, res->hits[i].key, res->hits[i].score);
        bitcask_search_result_free(res);
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

    bitcask_close(cask);
    return 0;
}
```

### 17.3 编译

```bash
gcc demo.c -I<c_api 头目录> -L<lib 目录> -lbitcask -o demo
```

更多端到端用例见 `tests/c_api_test.c`。

---

> 配套文档：[`api-cpp.md`](api-cpp.md)（C++ 接口）/ [`cpp-arch.md`](cpp-arch.md)（架构）/ [`concurrency-zh.md`](concurrency-zh.md)（并发模型）/ [`format-zh.md`](format-zh.md)（字节级格式）。