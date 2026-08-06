# C++ API 参考

本文档是 libbitcask C++ 接口的完整参考。权威来源为 `include/bitcask/` 下的公共头文件；所有符号位于 `namespace bitcask`（以及内嵌子命名空间 `bitcask::keydir` / `bitcask::text` / `bitcask::vec` / `bitcask::search` / `bitcask::bm25` / `bitcask::meta` / `bitcask::merge` / `bitcask::plugin` / `bitcask::format` / `bitcask::fileops` / `bitcask::io` / `bitcask::lock` / `bitcask::ckpt`）。配套阅读：[`cpp-arch.md`](cpp-arch.md)（架构）、[`format-zh.md`](format-zh.md)（磁盘格式）、[`design/thread-safety.md`](../docs/design/thread-safety.md)（线程安全契约）。

所有可能失败的接口用 `std::expected<T, CaskFault>` / `std::expected<T, MetaError>` / `std::expected<T, MergeFault>` 等返回；调用方应检查 `has_value()` 后再解引用。

---

## 1. 概述

libbitcask 有两种工作模式，由 `CaskOptions` 决定：

- **KV 模式**（`enable_search = false`）：Bitcask 追加日志 KV。`put` / `get` / `remove` 为 O(1)，读值单次 `pread`。纯 KV 的 binary 经 DocValue v3 编码进 text 段，对调用方透明。
- **索引模式**（`enable_search = true` + `search_config`）：在 KV 之上叠加 BM25 倒排、HNSW 向量图、字段索引，提供文本 / 向量 / 混合检索。模式在 `bitcask.meta` 持久化，重开必须一致，否则 `CaskError::kModeMismatch`。

---

## 2. 头文件与链接

```cpp
#include <bitcask/cask.hpp>      // 主门面：Cask / CaskOptions / GetResult* / CaskIter / CaskError / CaskFault
```

链接 `libbitcask`（静态聚合 `.a` 或共享 `.so`）。索引模式按需附带 include：

```cpp
#include <bitcask/searcher.hpp>        // 门面：text::Searcher / vec::Searcher / search::CaskHybridSearcher
#include <bitcask/search_config.hpp>   // search::SearchLayerConfig
#include <bitcask/search_types.hpp>    // search::SearchHit / SearchError / SearchHitEx / ReduceJob
#include <bitcask/search_cache.hpp>    // search::SearchCache / CacheKey
#include <bitcask/hnsw.hpp>            // vec::HnswConfig / HnswIndex / HnswMetric
#include <bitcask/analyzer.hpp>        // text::Analyzer / AnalyzerConfig / AnalyzerType / AnalyzerFactory
#include <bitcask/jieba_analyzer.hpp>  // text::JiebaAnalyzer
#include <bitcask/whitespace_analyzer.hpp>  // text::WhitespaceAnalyzer
#include <bitcask/ngram_analyzer.hpp>  // text::NgramAnalyzer
#include <bitcask/stemming_analyzer.hpp>    // text::StemmingAnalyzer
#include <bitcask/text_plugin.hpp>     // text::TextPlugin
#include <bitcask/vector_plugin.hpp>   // vec::VectorPlugin（HNSW 引擎）
#include <bitcask/vector_engine_plugin.hpp>  // vec::VectorEnginePlugin（引擎契约基类，S32）
#include <bitcask/ivf_plugin.hpp>     // vec::IvfPlugin（IVF-RaBitQ 引擎，S32-M3）
#include <bitcask/diskann_plugin.hpp> // vec::DiskannPlugin（DiskANN 引擎，S32-M5，实验性）
#include <bitcask/hybrid_searcher.hpp> // search::HybridSearcher
#include <bitcask/plugin_api.hpp>      // plugin::CaskPlugin / PluginHost / OpenContext
#include <bitcask/merge_policy.hpp>    // merge::PolicyOptions / Decision / FileStatus
#include <bitcask/merger.hpp>          // merge::run_merge / MergeStats
#include <bitcask/meta_filter.hpp>     // meta::MetaFilter / MetaCondition
#include <bitcask/highlighter.hpp>    // search::Snippet / HighlightOptions
#include <bitcask/synonym_map.hpp>     // text::SynonymMap
#include <bitcask/query.hpp>           // bm25::QueryNode / parse_query
#include <bitcask/keydir.hpp>          // keydir::KeyDir / IterHandle / FStatsEntry ...
#include <bitcask/keydir_registry.hpp> // keydir::KeyDirRegistry
```

---

## 3. 配置与错误类型

### 3.1 `bitcask::CaskOptions`（`cask.hpp`：`Cask::open` 的打开选项）

| 字段 | 类型 | 默认 | 含义 | 头文件 |
|------|------|------|------|--------|
| `read_write` | `bool` | `false` | `false`=只读；`true`=可写（持 `bitcask.write.lock`） | `cask.hpp` |
| `max_file_size` | `std::uint64_t` | `2 GiB` | 单个 data 文件上限，超过则 roll 到新文件 | `cask.hpp` |
| `max_read_handles` | `std::size_t` | `0` | read 句柄缓存上限（**一个句柄 = 1 fd + 1 sealed mmap**，故同时界定 fd 数与映射数）；`0`=自动（RLIMIT_NOFILE 一半，**夹在 [64, 1024]**）；`kUnlimitedReadHandles`=不设上限；其它 N=显式上限；超额近似 LRU 淘汰**空闲**句柄（在途读者持 shared_ptr 续命）。调优见 [§11.1 fd / mmap 预算](#111-fd--mmap-预算怎么降打开文件数) | `cask.hpp` |
| `o_sync` | `bool` | `false` | 每条写 durable（`O_SYNC`）；为真时 `sync_every_n` 无意义 | `cask.hpp` |
| `sync_every_n` | `std::uint32_t` | `0` | 单写者组提交：每 N 次写 fsync 一次；`0`=关闭 | `cask.hpp` |
| `auto_checkpoint_min_docs` | `std::uint32_t` | `65536` | 自动 checkpoint 阈值：自上次 ckpt 起 ord 增量 ≥ 本值则异步落快照；`0`=关闭；仅索引模式生效 | `cask.hpp` |
| `require_hint_crc` | `bool` | `false` | 是否要求 hint trailer CRC 通过 | `cask.hpp` |
| `expiry_secs` | `std::uint32_t` | `0` | TTL：tstamp < now − expiry_secs 的 record 在 get / fold 中被过滤，并触发 merge；`0`=禁用 | `cask.hpp` |
| `merge_only` | `bool` | `false` | merge-only 模式：拿 `bitcask.merge.lock`，不创建 active writer；可与 live writer 并行 merge | `cask.hpp` |
| `keydir_cache_entries` | `std::size_t` | `0` | **S36 Level B（keydir 磁盘驻留）**：`0`=不限（全内存，现状）；`>0`=热点缓存条目预算——超预算分片内采样逐出，点查落组合视图（memdelta + BCOK v2 run：bloom + 块 LRU，冷 get ≤2 次 pread）。1 亿 key 常驻 11GB → ~1.1GB。语义/约束见 [§11.3](#113-keydir-磁盘驻留level-b) | `cask.hpp` |
| `tombstone_version` | `std::uint8_t` | `0` | 墓碑格式：`0`=17B 前缀；`2`=22B 含 FileId。读时三种 (v0/v1/v2) 都接受 | `cask.hpp` |
| `policy` | `merge::PolicyOptions` | `{}` | merge 触发策略（碎片率 / 死字节 / 过期阈值） | `merge_policy.hpp` |
| `enable_search` | `bool` | `false` | 启用索引模式 | `cask.hpp` |
| `search_config` | `std::optional<search::SearchLayerConfig>` | `nullopt` | 有值时才创建搜索插件（Text/Vector） | `cask.hpp` / `search_config.hpp` |
| `vector_dim` | `std::uint16_t` | `0` | 向量维度；`>0` 即启用向量，要求 `enable_search`；库内恒定 | `cask.hpp` |
| `vector_quantized` | `bool` | `false` | 向量落盘 int8 量化（4× 磁盘，有损） | `cask.hpp` |
| `vector_inmem_int8` | `bool` | `false` | HNSW int8-only 内存（约 −80% 向量内存，仅 kDot）；与 `vector_quantized` 正交 | `cask.hpp` |
| `vector_metric` | `meta::VectorMetric` | `kCosineNormalized` | 向量距离度量 | `cask.hpp` / `meta_file.hpp` |
| `vector_engine` | `meta::VectorEngine` | `kHnsw` | **S32：向量引擎**。建库时一次性选定、写入 `bitcask.meta`，重开不符 → `kModeMismatch`，运行期不可切换（离线切换用 `vec_engine_migrate`）。`kHnsw`（默认，内存档）/ `kIvfRq`（IVF 磁盘段，10M-100M 推荐）/ `kDiskann`（Vamana 图，实验性）。磁盘档要求 `kCosineNormalized`/`kDot` 度量 | `cask.hpp` / `meta_file.hpp` |
| `synonym_map` | `std::shared_ptr<const text::SynonymMap>` | `nullptr` | 同义词词典（open-time、不可变）；查询时自动展开 | `cask.hpp` / `synonym_map.hpp` |
| `log_fn` | `std::function<void(LogLevel, std::string_view)>` | `nullptr` | 日志回调（open-time、不可变）；空=不回调 | `cask.hpp` |

嵌套成员：

- `CaskOptions::kUnlimitedReadHandles`：静态常量 `static constexpr std::size_t kUnlimitedReadHandles = static_cast<std::size_t>(-1);`
- `enum class CaskOptions::LogLevel : std::uint8_t { kWarn = 0, kError = 1 };`

> 向量配置（`vector_dim` / `vector_metric` / `vector_quantized` / `vector_inmem_int8` / `vector_engine`）创建即固定，写入 `bitcask.meta`；重开校验不符 → `kModeMismatch`。

### 3.2 `bitcask::CaskError`（错误码枚举，`cask.hpp`）

| 值 | 含义 |
|----|------|
| `kIo` | 底层 IO 错误（`CaskFault::errnum` 携带 errno）|
| `kBadCrc` | CRC 校验失败（数据损坏）|
| `kNotFound` | `get` 未命中 key |
| `kKeyTooLarge` | key 超长 |
| `kValueTooLarge` | value 超长 |
| `kAlreadyExists` | CAS 竞态（keydir put 检测到并发冲突）|
| `kReadOnly` | 对只读 cask 调用写操作 |
| `kWriteLocked` | 别人已持有 `write.lock` / `merge.lock` |
| `kInvalidOption` | 选项非法 |
| `kNoIndex` | 索引在本句柄上本就不存在：KV 模式调 search 接口；RO/merge_only 打开无 OKI 的目录调 range（重开读写可建）|
| `kModeMismatch` | 文件模式与打开选项不匹配 |
| `kAnalyzerMismatch` | 分析器类型不匹配 |
| `kClosed` | 对已 close 的 handle 发起调用 |
| `kIndexRebuildFailed` | OKI 试建而败——可写 open 重建失败（IO/环境问题，见日志）；修复后重开可重试 |

### 3.3 `bitcask::CaskFault`（错误详情，`cask.hpp`）

```cpp
struct CaskFault {
    CaskError   kind;        // 见 §3.2
    int         errnum = 0;  // IO 错误时为 errno，否则 0
    std::string detail;      // 人类可读描述
};
```

### 3.4 `bitcask::meta::VectorMetric`（`meta_file.hpp`）

| 值 | 含义 |
|----|------|
| `kNone` | 无向量 |
| `kCosineNormalized` | 归一化余弦（写入端归一，查询用内积）|
| `kL2` | 平方欧氏距离 |
| `kDot` | 内积 |

### 3.5 `bitcask::meta::Mode`（`meta_file.hpp`）

```cpp
enum class Mode : std::uint8_t {
    kKV    = 0,  // 纯 KV 模式
    kIndex = 1,  // 索引模式（BM25 搜索）
};
```

### 3.6 `bitcask::meta::MetaConfig`（`meta_file.hpp`）

```cpp
struct MetaConfig {
    Mode         mode              = Mode::kKV;
    VectorMetric vector_metric     = VectorMetric::kNone;
    std::uint16_t vector_dim       = 0;     // 0 = 无向量
    bool          vector_quantized = false; // P3b：向量落盘 int8 量化
    bool          vector_inmem_int8 = false;// P5b：HNSW int8-only 内存
    VectorEngine  vector_engine    = VectorEngine::kHnsw; // S32-M0：向量引擎
};
```

相关自由函数（`meta_file.hpp`）：

```cpp
[[nodiscard]] bool                 meta_exists(std::string_view dirname);
[[nodiscard]] std::expected<MetaConfig, MetaError>
                                  read_meta(std::string_view dirname);
[[nodiscard]] std::expected<void, MetaError>
                                  write_meta(std::string_view dirname, const MetaConfig& config);

struct MetaError { int errnum = 0; std::string message; };
```

### 3.7 `bitcask::meta::VectorEngine`（`meta_file.hpp`）

S32-M0 向量引擎枚举。建库时一次性选定、持久化进 `bitcask.meta`；重开不一致 → `kModeMismatch`。运行期不可切换——离线切换用 `vec_engine_migrate` 工具（只改 meta，首次 open 全量 fold 重建，可回滚）。`kHnsw = 0` 使旧 meta 保留区全零自然解码为 HNSW（与 `VectorMetric::kNone` 同款零升级）。

| 值 | 含义 |
|----|------|
| `kHnsw` | 内存图（≤数 M 向量档；现行实现）|
| `kIvfRq` | IVF-RaBitQ 磁盘档（S32-M3；10M-100M）：k-means 分簇 + int8 posting + 1-bit RaBitQ-lite 粗筛 + 两级质心索引 |
| `kDiskann` | DiskANN / Vamana 单层图（S32-M5；**实验性**，真实语料验证前不建议生产）|

> 磁盘档引擎（`kIvfRq` / `kDiskann`）要求 `kCosineNormalized` 或 `kDot` 度量（`kL2` → `kInvalidOption`）。

---

## 4. 结果类型与载荷结构

### 4.1 `bitcask::GetResultView`（零拷贝读结果，`cask.hpp`）

```cpp
struct GetResultView {
    std::span<const std::byte> value{};   // text 段（指向底层字节内部）
    std::span<const std::byte> meta{};    // meta 段（可为空）
    std::span<const float>     vector{};  // 向量段（空=无向量）
    std::uint32_t              tstamp     = 0;
    std::uint64_t              ord        = 0;
    std::uint32_t              expiry_at  = 0;  // per-key 过期时刻（0=永不）

    GetResult to_owned() const;           // 拷贝为 owned 版本

    // 可移动（std::expected 要求），不可拷贝
    GetResultView(GetResultView&& other) noexcept;
    GetResultView(const GetResultView&)            = delete;
    GetResultView& operator=(const GetResultView&) = delete;
};
```

`Cask::get()` 返回 view：`value` / `meta` / `vector` 是借用内部 pread 缓冲或 mmap 的 span，**生命周期与 `GetResultView` 绑定**。即时消费场景零拷贝；需持久化用 `Cask::get_owned()` 或本结构上的 `to_owned()`。量化文档（`kFlagVecQuantized`）无法零拷贝成 f32，引擎内部 dequant 进自有缓冲。

### 4.2 `bitcask::GetResult`（owned 读结果，`cask.hpp`）

```cpp
struct GetResult {
    std::vector<std::byte> value;     // DocValue 解码后的 text 段
    std::vector<std::byte> meta;      // DocValue 解码后的 meta 段（可为空）
    std::vector<float>     vector;    // 向量段（空=无向量）
    std::uint32_t          tstamp = 0;
    std::uint64_t          ord    = 0;
};
```

### 4.3 `bitcask::TextSearchResult`（`cask.hpp`）

```cpp
struct TextSearchResult {
    std::vector<search::SearchHit> hits;
};
```

### 4.4 `bitcask::Cask::HighlightSearchResult`（`cask.hpp`）

```cpp
struct Cask::HighlightSearchResult {
    std::vector<search::SearchHitEx> hits;
};
```

### 4.5 `bitcask::DocInput`（`Cask::put_doc` 输入，`cask.hpp`）

```cpp
struct DocInput {
    std::span<const std::byte> text;    // 必需（多字段时可空，作默认字段）
    std::span<const std::byte> meta;    // 可选
    std::span<const float>     vector{};// 可选（空=无；长度须 == 配置的 vector_dim）
    std::vector<std::pair<std::string, std::span<const std::byte>>>
                                     fields;    // 多字段（S8.6）
    std::uint32_t                 expiry_at = 0;  // per-key 过期时刻（绝对 unix 秒，0=永不）
};
```

`kCosineNormalized` 度量下引擎写入前归一化向量（存储的即归一化值）。

### 4.6 `bitcask::StatusInfo`（`Cask::status()` 返回，`cask.hpp`）

```cpp
struct StatusInfo {
    std::uint64_t key_count    = 0;
    std::uint64_t key_bytes    = 0;
    std::uint64_t epoch        = 0;
    std::vector<merge::FileStatus> files;
    std::uint64_t index_errors        = 0;  // indexed worker 抛异常时自增；非零=索引可能漂移
    std::uint64_t hnsw_nodes          = 0;  // HNSW 图节点数（含软删死节点）
    std::uint64_t search_cache_entries= 0;  // 查询缓存当前条目数
    std::uint64_t read_handles        = 0;  // read 句柄缓存当前大小（fd+mmap 数）
};
```

### 4.7 `bitcask::Cask::NeedsMerge`（`cask.hpp`）

```cpp
struct NeedsMerge {
    bool                     needs;          // needs_merge() 决策
    std::vector<std::string> files;          // 候选文件
    std::vector<std::string> expired_files;  // 过期文件
};
```

### 4.8 `bitcask::Cask::BatchItem`（`Cask::put_batch` 输入，`cask.hpp`）

```cpp
struct BatchItem {
    std::span<const std::byte> key;
    std::span<const std::byte> value;
};
```

### 4.9 `bitcask::Cask::HybridQuery`（`Cask::search_hybrid_batch` 输入，`cask.hpp`）

```cpp
struct HybridQuery {
    std::string_view       text;  // 文本查询（空=纯向量）
    std::span<const float> vec;   // 向量查询（空=纯文本）
};
```

### 4.10 `bitcask::Cask::ScanFn`（`Cask::parallel_scan` 回调，`cask.hpp`）

```cpp
using ScanFn = std::function<void(std::span<const std::byte> key,
                                  const GetResultView& value)>;
```

### 4.11 `bitcask::search::SearchHit`（`search_types.hpp`）

```cpp
struct SearchHit {
    std::string   key;    // 外部 key
    std::uint64_t ord;    // 文档 ord
    double        score;  // BM25 / 距离 / RRF 分数（按查询类型）
};
```

### 4.12 `bitcask::search::SearchHitEx`（带高亮，`search_types.hpp`）

```cpp
struct SearchHitEx {
    std::string          key;
    std::uint64_t        ord;
    double               score;
    std::vector<Snippet> highlights;  // search::Snippet
};
```

### 4.13 `bitcask::search::SearchError`（搜索层错误类型，`search_types.hpp`）

```cpp
enum class SearchError {
    kNoVectorIndex,     // 无向量索引配置
    kVectorDimMismatch, // 查询向量维度与配置不符
    kEmptyHybridQuery,  // hybrid 两路皆空
};
```

`SearchError` 全部映射到 `CaskError::kInvalidOption`，由 `Cask::search_error_fault` 翻译。

### 4.14 `bitcask::search::ReduceJob`（`search_types.hpp`）

`map_analyze` 产出 / `apply_job` 输入（reducer 在锁下逐字段 apply）。字段：

```cpp
struct ReduceJob {
    std::string key;
    std::uint64_t ord = 0;

    struct FieldResult {
        std::string            field_name;
        text::TermPositionsMap terms;
        std::uint32_t          doc_len = 0;  // Σ tf
    };
    std::vector<FieldResult> fields;

    std::uint32_t          total_doc_len = 0;
    text::TermPositionsMap ca_data;
    std::uint32_t          ca_len       = 0;
    bool                   wrote_default = false;
    std::string            doc_text;
    std::uint32_t          file_id  = 0;
    std::uint64_t          offset   = 0;
    std::uint32_t          total_sz = 0;
    std::uint32_t          tstamp   = 0;
};
```

### 4.15 `bitcask::search::kDefaultField`（`search_types.hpp`）

```cpp
inline constexpr std::string_view kDefaultField = "\xfa" "ult";
```

默认字段哨兵（S8.6）：旧单 text 文档与无字段限定查询都映射到此字段。

---

## 5. `bitcask::Cask` 类（核心门面，`cask.hpp`）

不可拷贝。`open()` 返回 `std::expected<std::unique_ptr<Cask>, CaskFault>`。

### 5.1 生命周期

#### `Cask::open`

```cpp
[[nodiscard]] static std::expected<std::unique_ptr<Cask>, CaskFault>
open(std::string_view dirname, const CaskOptions& opts,
     keydir::KeyDirRegistry* registry);
```

打开一个 Cask 实例。`registry` **强制非空**（管理同目录 Cask 间的共享 keydir；典型生产形态：每进程/实例一个全局 registry）——传 `nullptr` 返回 `CaskError::kInvalidOption`（无 fallback；异步索引双池归属 registry）。
- **错误**：`kIo`、`kWriteLocked`（锁被占）、`kInvalidOption`（含 registry 为空）、`kModeMismatch`、`kAnalyzerMismatch`。
- **线程安全**：是（每次调用产生独立 Cask 对象；registry 并发由其内部锁保证）。

#### `Cask::upgrade`

```cpp
[[nodiscard]] static std::expected<std::unique_ptr<Cask>, CaskFault>
upgrade(std::string_view dirname, const search::SearchLayerConfig& search_config);
```

离线把 KV 模式目录升级为索引模式。前提：目录为 KV 模式且**离线**（无活跃 writer）。流程：读 meta 验证 → 写新 meta → 建搜索插件（Text/Vector）→ 扫描全部数据文件重建索引 → 返回只读索引模式 Cask。
- **线程安全**：是。

#### `Cask::close`

```cpp
void close() noexcept;
```

释放资源。**幂等**（二次 close no-op）。**线程安全**：否（生命周期方法，caller 须保证关闭时刻无其它线程在调用 get / put / remove / sync / iter / merge / search / parallel_scan 等）。close 后**新发起**的公共调用 fail-fast 返回 `CaskError::kClosed`（"cask is closed"）而非解引用已释放状态；与 close **并发在途**的调用仍是 caller 责任（best-effort 防误用，非完整 rundown）。

### 5.2 读

#### `Cask::get`（零拷贝）

```cpp
[[nodiscard]] std::expected<GetResultView, CaskFault>
get(std::span<const std::byte> key);
```

单 key 读：keydir 查 → 一次 `pread`（或 mmap 命中）。`kTombstone` 当作 `kNotFound`。
- **错误**：`kNotFound`、`kBadCrc`、`kIo`。
- **线程安全**：是（读路径无锁；read 缓存受内部锁保护，`pread` 本身线程安全）。

#### `Cask::get_owned`（拷贝）

```cpp
[[nodiscard]] std::expected<GetResult, CaskFault>
get_owned(std::span<const std::byte> key);
```

拷贝语义版本——benchmark 等需 owned 数据的场景。线程安全同 `get`。

#### `Cask::read_handle_count`

```cpp
[[nodiscard]] std::size_t read_handle_count() const;
```

当前常驻 read 句柄数（内省用，测试断言 fd 预算上限）。线程安全：共享锁读。

#### `Cask::resolve_read_handle_cap`（静态）

```cpp
[[nodiscard]] static std::size_t
resolve_read_handle_cap(std::size_t opt, std::size_t nofile_soft) noexcept;
```

把 `CaskOptions::max_read_handles` 解析为 evict 使用的有效上限（纯函数，不查询系统，便于确定性单测）。`kUnlimitedReadHandles` → 0（evict 语义下的「不限」）；`0` → 由 `nofile_soft` 推导的安全默认（约一半，**夹在 `[kAutoReadHandleFloor, kAutoReadHandleCeiling]` = [64, 1024]**）；其它 N → N（原样，不夹）。

> **S33-6 加的绝对上限**：容器 / systemd 环境下 `RLIMIT_NOFILE` 常见 5×10⁵ 甚至 10⁶，"取一半"得出的 26 万形同虚设——实测 89 个 data 文件的库把 89 个 fd + 88 个 mmap 全留着不淘汰。封顶 1024 后 fd/mmap 与库规模脱钩；1024 个句柄在默认 `max_file_size = 2 GiB` 下对应约 2 TB 数据，正常库碰不到。需要更多请显式给 N 或 `kUnlimitedReadHandles`（caller 自负 fd 预算）。

#### `Cask::parallel_scan`（并行全表扫描）

```cpp
[[nodiscard]] std::expected<std::size_t, CaskFault>
parallel_scan(std::size_t n_threads, const ScanFn& fn,
              std::span<const std::byte> key_prefix = {});
```

全表并行扫描，用于 analytics / export / reindex。实现：① 在调用线程串行快照所有 live key（仅 key 拷贝，**不读 value**）② 按 `n_threads` 分段 ③ 各线程并发 `get()` 读值并调 `fn`——**被并行化的是读值的 pread+decode**（真正的成本）；单 append WAL 写串行不受影响（更高写并发请按目录分片）。
- `n_threads == 0` → `hardware_concurrency()`。
- `key_prefix` 非空时只扫描以该前缀开头的 key（keydir proxy 层过滤，非匹配 key 零拷贝零 pread）。
- `fn` **必须线程安全**（不同线程并发调用，各处理不相交 key 段）；`value` 是借用工作线程读缓冲的零拷贝 view，仅在回调内有效。
- 并发删除致某 key 在 `get` 时 `kNotFound` → 跳过（near-real-time，与搜索一致）；其它错误（IO/CRC）→ 停止并返回该错误。返回成功遍历到的 key 数。
- KV 模式亦可用（不依赖 search 层）。**线程安全：是**（快照串行建立 + get 并发安全）。Cask 已 close → `kClosed`。

> 单个 `CaskIter` 是有状态游标，**不可**跨线程共享（每线程一个）；需要并行遍历用本方法。

### 5.3 写

> **线程安全（S11-W1）**：写路径由内部 `write_mu_` 串行化——同一 handle 可被**多线程并发写**而不损坏。写在文件层本就串行（单 append WAL）→ 锁不损吞吐；需要更高写并发请**按目录分片多个 Cask 实例**。与并发 `merge` / 并发读（get / search）安全。

#### `Cask::put`

```cpp
[[nodiscard]] std::expected<void, CaskFault>
put(std::span<const std::byte> key,
    std::span<const std::byte> value,
    std::uint32_t tstamp    = 0,
    std::uint32_t expiry_at = 0);
```

`tstamp = 0` 用当前 wall-clock 秒；`expiry_at` 为 per-key 过期时刻（绝对 unix 秒，`0`=永不过期）。**错误**：`kReadOnly`、`kKeyTooLarge`、`kValueTooLarge`、`kAlreadyExists`（CAS 竞态，内部 roll 后重试）、`kIo`。**线程安全**：是（S11-W1：内部 `write_mu_` 串行化）。

#### `Cask::put_batch`

```cpp
[[nodiscard]] std::expected<void, CaskFault>
put_batch(std::span<const BatchItem> items, std::uint32_t tstamp = 0);
```

S13-D1 批量写（语义同 `put` 的 KV 路径）。整批一次提交：记录经 write_buffered 聚合成 1 MiB 块 pwrite、单次 flush **之后**才 apply keydir / 提交索引任务并返回。语义契约：
- 成功返回 ⟹ 整批已写入且全部可见（本进程内 all-or-nothing）。
- 失败返回 ⟹ 整批在本进程内不可见；磁盘上可能残留批前缀（每条记录独立自洽，崩溃重启 fold 后可见）。
- 校验（key / value 大小）在任何写发生前全批完成——校验失败零副作用。
- 整批写入同一 active 文件；巨批允许该文件超出 `max_file_size`（软上限）。

#### `Cask::put_batch_atomic`（S35：跨崩溃原子批）

```cpp
struct BatchOp {
    enum class Type : std::uint8_t { kPut = 0, kRemove = 1 };
    Type type;
    std::span<const std::byte> key;
    std::span<const std::byte> value{};   // kRemove 忽略
};
[[nodiscard]] std::expected<void, CaskFault>
put_batch_atomic(std::span<const BatchOp> ops, std::uint64_t tstamp = 0);
```

语义 = `put_batch` 的超集（设计 [`atomic-batch-design-zh.md`](atomic-batch-design-zh.md)）：

- **崩溃/掉电后整批要么全可见要么全不可见**——盘上 `kBatchHeader` 声明成员区间，恢复时区间不完整 ⟹ 整批截断（等价于从未写过）。
- 支持批内 REMOVE；批内 op 依序 apply（同 key 多次 = 批内 LWW）。
- **首次调用把目录 meta 懒升级为 v6**：旧于 5.1.0 的读端拒开该目录（`unsupported meta version`）。从不调用本方法的目录停留 v5（保守纪元标记）。
- durability 与 `put_batch` 相同（`o_sync` / `sync_every_n` / caller `sync()`）——原子性与持久性正交：未 fsync 掉电可能整批丢失，但绝不半批。
- 线程安全：是（同 `put_batch`，内部 `write_mu_`）。

#### `Cask::remove`

```cpp
[[nodiscard]] std::expected<void, CaskFault>
remove(std::span<const std::byte> key, std::uint32_t tstamp = 0);
```

软删除：写一条墓碑 record，空间在下次 merge 时回收。线程安全：是（同 `put`）。

#### `Cask::put_doc`

```cpp
[[nodiscard]] std::expected<void, CaskFault>
put_doc(std::span<const std::byte> key, const DocInput& doc,
        std::uint32_t tstamp = 0);
```

写入结构化文档（text + 选填 meta / vector / fields），用于索引模式。线程安全：是（同 `put`）。

#### 多键事务：`bitcask::TxnCask`（S34，`txn.hpp`）

```cpp
struct TxnOp {
    enum class Type : std::uint8_t { kPut = 0, kRemove = 1 };
    Type type;
    std::span<const std::byte> key;
    std::span<const std::byte> value{};   // kRemove 忽略
};
enum class TxnSyncPolicy : std::uint8_t { kSyncOnCommit = 0, kNone = 1 };

class TxnCask {  // 非拥有包装 Cask*；自身无状态，可随建随用
public:
    explicit TxnCask(Cask* cask,
                     TxnSyncPolicy sync = TxnSyncPolicy::kSyncOnCommit);
    [[nodiscard]] std::expected<std::size_t, CaskFault> recover();
    [[nodiscard]] std::expected<void, CaskFault> commit(std::span<const TxnOp> ops);
    [[nodiscard]] std::expected<std::vector<PendingTxn>, CaskFault> pending_txns();
};
```

多键事务 helper（原理 [`multikey-txn-zh.md`](multikey-txn-zh.md)；S35 起提交路径 = 引擎原子批，设计 [`atomic-batch-design-zh.md`](atomic-batch-design-zh.md)）。提供崩溃原子性（A）与持久性（D）；**不提供**隔离性（I）与 CAS——事务中间态对并发读者可见。要点：

- `commit`：一次 `put_batch_atomic`——崩溃/掉电后全生效或全不生效，无恢复重放依赖。校验失败（空批 / 空 key / 重复 key / `_txn:` 前缀）→ `kInvalidOption` 零副作用。首次 commit 懒升级目录 meta 至 v6（见 `put_batch_atomic` 契约）。`TxnSyncPolicy::kSyncOnCommit`（默认）在提交后显式 `sync()`（防掉电丢批——原子性与持久性正交）。
- `recover` / `pending_txns`（B2 起恒空）：方案 B 的意图重放已删除——意图日志从未随任何发布版本存在（TxnCask 与引擎原子批同版首发）。签名保留为 API 稳定面；开发期残留的 `_txn:` 前缀 key 可经普通 KV API 手工清理。
- 并发：键集不相交的并发 `commit` 安全；键集重叠无隔离/定序保证（应用层串行化）。
- `_txn:` 命名空间保留（legacy）；空间回收走 merge。

### 5.4 检索（索引模式）

无 search 层 → `kNoIndex`；无向量配置 → `kInvalidOption`。所有检索方法线程安全：是（并发读：cache_/doc_texts_ shared_mutex、倒排/HNSW shared_lock、analyzer const；与写并发遵循 near-real-time 可见性）。`filter` 非空时 meta 后过滤（overfetch 后截断到 `k`）。

#### `Cask::search_text`（词袋 BM25）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_text(std::string_view query, std::size_t k = 10,
            const meta::MetaFilter* filter = nullptr,
            std::size_t offset            = 0);
```

`offset`：跳过排名前 N 条（分页，overfetch `k + offset` 后截断——深分页成本线性增长）。

#### `Cask::search_text_batch`（批量 BM25）

```cpp
[[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
search_text_batch(std::span<const std::string_view> queries,
                  std::size_t k = 10,
                  const meta::MetaFilter* filter = nullptr);
```

S7-4：K 条**独立**查询并发跑在进程级共享「有界 Search 池」上（inter-query 并发；非每 Cask 一个线程），按输入序返回各自结果。**一次 flush（`prepare_search`）覆盖全批**。单条查询失败只影响该槽。

#### `Cask::search_phrase`（短语）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_phrase(std::string_view query, std::size_t k = 10,
              std::size_t offset = 0);
```

term 连续出现。需 `index_positions=true`。

#### `Cask::bool_search`（布尔）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
bool_search(std::string_view query, std::size_t k = 10,
            std::size_t offset = 0);
```

AND / OR / NOT 查询语法（`+term` MUST / `-term` MUST_NOT / 裸 SHOULD；含括号嵌套与引号短语时走 `parse_query_tree`）。

#### `Cask::search_fields`（多字段）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_fields(std::string_view query, std::size_t k = 10);
```

S8.6：解析 `field:term^boost` 语法：有字段限定的词查对应字段，无限定的查默认字段；各词得分 × boost，跨字段累加。不含字段语法时等价默认字段词袋。

#### `Cask::search_near`（近邻）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_near(std::string_view query, std::uint32_t slop, std::size_t k = 10);
```

term 按序出现且相邻间隙 ≤ `slop`；`slop = 0` 即短语。

#### `Cask::search_fuzzy`（模糊）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance);
```

S8.3：Levenshtein 编辑距离匹配。

#### `Cask::search_wildcard`（通配符）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_wildcard(std::string_view pattern, std::size_t k);
```

S8.4：`*` / `?` 模式匹配。

#### `Cask::search_vector`（向量 ANN）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_vector(std::span<const float> query, std::size_t k = 10,
              std::size_t ef = 0,
              const meta::MetaFilter* filter = nullptr);
```

向量 ANN 检索——引擎由 `CaskOptions::vector_engine` 建库时选定（`kHnsw` / `kIvfRq` / `kDiskann`）。`query.size()` 必须 == `vector_dim`；`cosine` 配置时内部归一化（零向量返回空命中）；`ef = 0` → `max(k, 64)`（IVF 引擎下 `ef` 按 `nprobe` 解释）。结果按相似度降序（`kDot`=内积；`kL2`=负平方距离），死文档经 live 过滤不出现。`filter` 非空时与 `is_live` 组合成 live callback（无需 overfetch），结果可能少于 `k`。

#### `Cask::search_vector_batch`（批量向量）

```cpp
[[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
search_vector_batch(std::span<const std::span<const float>> queries,
                    std::size_t k = 10, std::size_t ef = 0,
                    const meta::MetaFilter* filter = nullptr);
```

S7-4：K 条独立向量查询并发跑共享 Search 池，保序返回。引擎由 `vector_engine` 选定（同 `search_vector`）。

#### `Cask::search_hybrid`（RRF 混合检索）

```cpp
[[nodiscard]] std::expected<TextSearchResult, CaskFault>
search_hybrid(std::string_view text_query,
              std::span<const float> vec_query, std::size_t k = 10,
              const meta::MetaFilter* filter = nullptr);
```

V3.6：两路各取 `K' = max(k × 4, 64)`：BM25 走 `search_text` 内核，向量走 `search_vector` 内核；融合 `score = Σ 1/(60 + rank)`，`rank` 从 1 起；平局 → `ord` 小者在前。
- `text_query` 空 → 纯向量；`vec_query` 空 → 纯文本；两路都空 / 无向量配置 / 维度不符 → `kInvalidOption`。
- `filter` 同时作用于两路（text 后过滤；vec 折 HNSW live callback），仅双路都通过的文档进 RRF 融合。
- 返回 `TextSearchResult`，`score` = RRF 分。

#### `Cask::search_hybrid_batch`（批量 RRF 混合）

```cpp
[[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
search_hybrid_batch(std::span<const HybridQuery> queries,
                    std::size_t k = 10,
                    const meta::MetaFilter* filter = nullptr);
```

S7-4：K 条独立 `(text, vec)` 查询并发跑共享 Search 池，保序返回。每条 hybrid 内部仍串行两路。

#### `Cask::search_text_highlight`（带高亮的 BM25）

```cpp
struct HighlightSearchResult {
    std::vector<search::SearchHitEx> hits;
};
[[nodiscard]] std::expected<HighlightSearchResult, CaskFault>
search_text_highlight(std::string_view query, std::size_t k = 10,
                      const search::HighlightOptions& opts = {});
```

S13-D3：命中含高亮片段（`SearchHitEx.highlights`），截取策略由 `opts` 控制。

#### 同义词词典（open-time 配置）

同义词词典在 **`Cask::open` 时**经 `CaskOptions::synonym_map` 配置：`search_text` / `search_fields` 查询时自动展开同义词。构造后**不可变** → 并发查询天然安全，无需锁。运行期 setter 已移除（曾是配置项里唯一的 reader-vs-writer 竞态源）。运行期更换词典请重开库；按请求用不同词典需自行在查询串里展开。

```cpp
auto sm = std::make_shared<text::SynonymMap>();
sm->add_group({"番茄", "西红柿", "tomato"});   // 或 sm->load_from_file(path)
CaskOptions opts;
opts.enable_search = true;
opts.synonym_map   = sm;
auto c = Cask::open(dir, opts, &registry);
```

### 5.5 搜索基础设施访问

S19-2 起 `SearchLayer` 门面已解体：`Cask` 直持 Text/Vector 插件，查询经 `Cask::search_*` 门面（源兼容）或 `Searcher` 类型化门面（§6.4，推荐）。底层访问器：

```cpp
[[nodiscard]] bool has_search() const;                         // 是否启用索引模式
void                     flush_index();                        // 排空异步索引队列
[[nodiscard]] std::expected<void, CaskFault>
                          drain_plugins();                    // 读屏障：submitted ⇒ applied
[[nodiscard]] static CaskFault
                          search_error_fault(search::SearchError e);  // SearchError → CaskFault

// 插件句柄（高级用法 / Searcher 门面 / C API 层用；所有权在 Cask；未启用搜索 = nullptr）
[[nodiscard]] const text::TextPlugin*             text_plugin()        const;
[[nodiscard]] const vec::VectorEnginePlugin*      vector_plugin()      const;  // S32-M3：引擎契约基类（HNSW/IVF/DiskANN 按 meta.vector_engine 定）
[[nodiscard]] const search::HybridSearcher*       hybrid_searcher()    const;

// S16-1：DocMap 宿主服务句柄（索引模式下非空；与插件借用的 docmap 同一实例）
[[nodiscard]] const std::shared_ptr<index::Index>& docmap()     const;

// 借用自 registry 的共享 IndexPool
[[nodiscard]] IndexPool* index_pool();
```

### 5.6 持久化与写文件管理

#### `Cask::sync`

```cpp
[[nodiscard]] std::expected<void, CaskFault> sync();
```

fsync active data file。`o_sync` 模式下退化为 no-op。线程安全：是（S11-W1：内部 `write_mu_`，与 put/remove 互斥）。

#### `Cask::close_write_file`

```cpp
[[nodiscard]] std::expected<void, CaskFault> close_write_file();
```

finalize 当前 active write file（写 hint trailer、丢句柄、释放 `write.lock`）。Cask 仍可用——下次 put 自动重开新 active file。只读 / `merge_only` 句柄返回 `kReadOnly`。线程安全：是（S11-W1：内部 `write_mu_`，与 put/remove/sync 互斥）。

#### `Cask::backup`

```cpp
[[nodiscard]] std::expected<void, CaskFault> backup(std::string_view dst_dir);
```

S13-D6：不停机备份到 `dst_dir`（不存在则创建）。流程：持 `write_mu_` 关闭 active writer（finalize hint trailer，下一次 put 自动重建）→ 快照文件清单 → 逐文件 hardlink（跨设备回退 copy）data/hint + `bitcask.meta` + `field.schema` + keydir/search checkpoint（有则带上，加速备份目录首次 open）。sealed 文件不可变 → hardlink 即一致快照。备份目录可直接以只读或读写模式 open。
- **锁要求**：caller 须保证 backup 与 merge 不并发。
- 线程安全：是（与 put/get 并发；put 被 `write_mu_` 挡在备份期间外，get 不受影响）。

#### `Cask::checkpoint`

```cpp
[[nodiscard]] std::expected<void, CaskFault> checkpoint();
```

手动 checkpoint：把 keydir 快照 + search.ckpt 主动落盘，把崩溃恢复的重放窗口收敛到「自本次调用以来的增量」。调用节奏由 caller 决定（每 N 万写 / 定时 / 业务低峰），库内不做周期策略。纯 KV 库（无 search）只落 keydir 快照。保存顺序：先 keydir 快照（较早水位），后 search.ckpt（覆盖水位 ≥ 快照水位）——并发写入下方向安全。
- 阻塞语义：search.ckpt 序列化经 RunFn 在 reducer 线程按 ord 序执行，本调用等待**自己的 RunFn**完成，不等整条队列排空——持续写入下等待仍有界。大库序列化可达秒~分钟级，期间 reducer 停摆、队列积压。
- 只读 / `merge_only` 句柄返回 `kReadOnly`。
- 线程安全：是。checkpoint 间由内部 `ckpt_mu_` 串行；与 put/get 并发安全；与 close 并发由 `WriteOpGate` 收敛。

### 5.7 状态与 merge

#### `Cask::status`

```cpp
[[nodiscard]] StatusInfo status();
```

线程安全：是（只读 keydir + opts 快照）。

#### `Cask::is_empty_estimate`

```cpp
[[nodiscard]] bool is_empty_estimate();
```

O(1) 估算 keydir 是否为空。写过 key 后即使删光也不再回 `true`。线程安全：是。

#### `Cask::is_frozen`

```cpp
[[nodiscard]] bool is_frozen();
```

keydir 是否被某 fold/iterator pin 住（影响 pending 表合并时机）。线程安全：是。

#### `Cask::needs_merge`

```cpp
[[nodiscard]] NeedsMerge needs_merge(std::uint32_t now_sec = 0);
```

返回是否需要 merge + 候选 / 过期文件列表。线程安全：是（读 keydir 快照 + 纯函数策略）。

#### `Cask::merge`

```cpp
[[nodiscard]] std::expected<merge::MergeStats, CaskFault>
merge(std::vector<std::string> files = {}, std::uint32_t now_sec = 0);
```

在指定文件上跑 merge。`files` 为空时先调 `needs_merge`。caller 负责外部调度 / 锁——这个方法只是把 `merge::run_merge` 包了一层。
- KV 路径：merge 与并发 put/remove/get 安全（keydir 重定位是条件 CAS；收尾对 stuck 文件跳过 unlink 兜底）。
- 索引模式：merge 内的 compact / ckpt 序列化经 RunFn 任务在 reducer 线程内执行。
- **锁要求**：caller 须保证同一 dirname 上同时仅一次 merge 在跑。
- `now_sec`：TTL 判定时刻（`0` = 不判 TTL）。

### 5.8 迭代

#### `Cask::make_iter`

```cpp
[[nodiscard]] std::unique_ptr<CaskIter> make_iter();
```

线程安全：是（产生对象）；返回的 `CaskIter` 本身非线程安全。

#### `Cask::make_range_iter`（S33-5/S33-6）

```cpp
[[nodiscard]] std::expected<std::unique_ptr<CaskRangeIter>, CaskFault>
make_range_iter(const RangeOptions& opts);
```

按 key 字典序遍历 `[lo, hi)`，走 OKI（有序 key 索引）——**O(range)** 而非 `CaskIter::start(key_prefix)` 的 O(全表)。OKI 不可用按成因拆码：只读/merge_only 打开且目录没建过 OKI（本就不建，重开读写即自动重建）→ `kNoIndex`；读写打开但 open 时重建失败（IO/环境问题）→ `kIndexRebuildFailed`。

线程安全：是（可多线程各自 make 并发迭代）；返回的迭代器自身非线程安全。

#### `Cask::dirname` / `keydir` / `options`

```cpp
[[nodiscard]] std::string_view          dirname() const noexcept;
[[nodiscard]] keydir::KeyDir&           keydir()  noexcept;
[[nodiscard]] const CaskOptions&        options() const noexcept;
```

---

## 6. `bitcask::CaskIter`（快照迭代器，`cask.hpp`）

遍历 `make_iter()` 时刻的全部活跃 `(key, value)`。snapshot 语义靠 `keydir::IterHandle` 提供；每条 entry 的 value 在 `next()` 时按需 `pread`。设计为「per-step 一次调用」，便于上层在 scheduler 之间让出。

**线程模型**：CaskIter 自身非线程安全——同一对象只能由一个线程使用；但不同 CaskIter 对象可多线程并发使用同一个 parent Cask（读路径并行 + KeyDir::IterHandle 支持多 fold）。

```cpp
class CaskIter {
public:
    explicit CaskIter(Cask* parent) noexcept;
    ~CaskIter() noexcept;

    CaskIter(const CaskIter&)            = delete;
    CaskIter& operator=(const CaskIter&) = delete;

    struct Entry {
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        std::uint32_t tstamp      = 0;
        std::uint32_t file_id     = 0;
        std::uint64_t offset      = 0;
        std::uint32_t total_sz    = 0;
        bool          is_tombstone = false;
        std::uint64_t ord         = 0;
    };

    [[nodiscard]] std::expected<keydir::StartIterResult, CaskFault>
    start(int maxage          = -1,
          int maxputs         = -1,
          std::uint32_t now_sec        = 0,
          bool see_tombstones           = false,
          std::span<const std::byte> key_prefix = {});

    [[nodiscard]] std::expected<std::optional<Entry>, CaskFault> next();

    [[nodiscard]] std::expected<std::vector<Entry>, CaskFault>
    next_batch(std::size_t max_n);

    void                release() noexcept;
    [[nodiscard]] bool  is_iterating() const noexcept;
};
```

### `CaskIter::start`

- `see_tombstones = true`：被删除的 key 也作为 entry 出现（`is_tombstone = true`，`value` 是墓碑标记字节）；`false`（默认）跳过墓碑。
- `key_prefix` 非空时只产出以该前缀开头的 key——过滤发生在 keydir proxy 层（不 pread value）。
- 返回底层 keydir 的 `StartIterResult`：`kOk`（开始迭代）/ `kAlreadyIterating`（已经在迭代）/ `kOutOfDate`（pending 表 freshness 未过，caller 稍后重试）。`CaskFault` 留给真正的失败。
- 线程安全：否（修改自身字段）；同一 CaskIter 不可并发使用。

### `CaskIter::next`

- 取下一项；end-of-iteration 返回 `nullopt`。Entry 内部 vector 拥有自己的存储，调用方持有期间可任意使用。
- 线程安全：否（推进 iter_ + 内部 pread）；同一对象不可并发使用。

### `CaskIter::next_batch`

- 批量取最多 `max_n` 条 entry；内部循环调 `next()`。
- **空 vector = EOI（正常结束）；unexpected = 错误**——两者语义不同。

### `CaskIter::release` / `is_iterating`

- `release` 幂等。线程安全：否；与 start/next 串行调用。
- `is_iterating`：当前是否处于迭代中（`iter_ != nullptr`）。

### `bitcask::RangeOptions` / `bitcask::CaskRangeIter`（有序 range 迭代器，S33-5/S33-6）

按 key 字典序遍历 `[lo, hi)`。实现：manifest 快照 pin 住 OKI run 的共享 Reader → k 路归并（runs + memdelta，同 key 取 max-ord、墓碑抵消）→ **逐 key 回查哈希 keydir**（活性与位置的权威）→ 现有 `get` 路径取值。OKI 行允许陈旧（死 key 被回查过滤），因此 merge 搬迁与本迭代器零交互。格式见 [`format-zh.md`](format-zh.md) §OKI，设计见 [`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md)。

```cpp
struct RangeOptions {
    std::span<const std::byte> lo{};   // inclusive；空 = 从头
    std::span<const std::byte> hi{};   // exclusive；空 = 到尾
    std::size_t prefetch = 0;          // 0/1 = 关闭；>1 = 批量并发预取值
    std::size_t prefetch_threads = 0;  // 0 = min(hardware_concurrency, 4)
};

class CaskRangeIter {
public:
    struct Entry {
        std::vector<std::byte> key;
        std::vector<std::byte> value;   // DocValue text 段（纯 KV 即 value）
        std::uint64_t tstamp = 0;
        std::uint64_t ord    = 0;       // keydir 权威 ord（非 OKI 行 ord）
    };
    [[nodiscard]] std::expected<std::optional<Entry>, CaskFault> next();
};
```

- **一致性：per-key 弱一致**（与 `parallel_scan` 同档）——迭代期间的并发写可能部分可见，**不是** `CaskIter` 的 fold 快照语义。需要快照请先用 `CaskIter` 冻结。
- **生命周期**：不可跨线程共享；须在 `Cask::close()` 之前用完（内部 pin KeyDir，但取值经 Cask 读路径）。无 `release()`——析构即释放。
- **`prefetch`（S33-6）**：>1 时一次归并出 `prefetch` 个 key，再用 `prefetch_threads` 个线程并发取值填缓冲，`next()` 从缓冲出货。**只改变取值时机，输出序与内容和惰性路径完全一致**。线程按批创建（无常驻池），故内部按「每线程至少 64 个 key」收窄线程数——小批自动退化为串行。实测（tmpfs、1 KiB 值、6250 条命中窗口）：惰性 10.8 ms、`prefetch=64` 11.6 ms（无收益）、`prefetch=256/4 线程` 7.6 ms（**1.4×**）。收益形态是「大窗口 + 值不在页缓存」；小窗口或值已缓存时线程成本可能反超，故默认关闭。
- 错误：run 损坏 → `kBadCrc`（该 run 整体不可信，重开会重建）；取值路径的 IO 错误原样上抛。并发删除导致的 `kNotFound` 静默跳过（同 `parallel_scan`）。

```cpp
bitcask::RangeOptions ro;
ro.lo = as_bytes("user:1000"); ro.hi = as_bytes("user:2000");
ro.prefetch = 256;                     // 可选：值预取
auto it = (*c)->make_range_iter(ro);
if (!it) { /* kNoIndex：只读打开且无 OKI；kIndexRebuildFailed：重建失败 */ }
while (auto e = (*it)->next()) {
    if (!e->has_value()) break;        // EOI
    auto& entry = **e;                 // entry.key / value / tstamp / ord
}
```

---

## 7. 索引子系统

### 7.1 `bitcask::search::SearchLayerConfig`（`search_config.hpp`）

`CaskOptions::search_config` 的载荷：`text_config()` / `vector_config()` 拆分产出 Text/Vector 两插件各自的配置子集。

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `analyzer_config` | `text::AnalyzerConfig` | — | 分词器配置（`analyzer.hpp`）|
| `bm25_params` | `bm25::Bm25Params` | — | BM25 参数（k1 / b / delta）|
| `cache_max_entries` | `std::size_t` | `256` | 查询缓存上限；`0`=禁用 |
| `doc_text_cache_max` | `std::size_t` | `1024` | 高亮原文 LRU 上限；`0`=不缓存（高亮降级为无片段）|
| `index_positions` | `bool` | `true` | 是否索引词位置；`false` 时省内存但 `search_phrase` / `search_near` 失效 |
| `index_catch_all` | `bool` | `true` | S26-2：catch-all 开关。`false` 时非默认字段词项不合并进默认字段（多字段库倒排量/内存/ckpt ~减半），代价 `search_text` 不再命中多字段文档 |
| `vector_dim` | `std::uint16_t` | `0` | `>0` 时构造向量索引 |
| `vector_metric` | `meta::VectorMetric` | `kNone` | `kCosineNormalized`/`kDot` → HNSW `kDot`；`kL2` → `kL2` |
| `hnsw_m` | `std::uint32_t` | `0` | HNSW 建图参数（0=HnswConfig 默认 M=16）|
| `hnsw_ef_construction` | `std::uint32_t` | `0` | HNSW 建图参数（0=HnswConfig 默认 ef_construction=200）|
| `vector_inmem_int8` | `bool` | `false` | HNSW int8-only 内存（仅 kDot）|
| `vector_rebase_min_docs` | `std::uint32_t` | `262144` | S32-M1：向量组件 base rebase 窗口门（崩溃恢复重放上界；全引擎）；`0`=关，仅链长门 |
| `vector_ivf_nlist` | `std::uint32_t` | `0` | S32-M3：IVF 簇数（`0`=自动 4·√N）|
| `vector_ivf_nprobe` | `std::uint32_t` | `0` | S32-M3：IVF 查询探簇数（`0`=自动）|
| `vector_diskann_r` | `std::uint32_t` | `0` | S32-M5：DiskANN 邻接容量（`0`=32）|
| `vector_diskann_l_build` | `std::uint32_t` | `0` | S32-M5：DiskANN 建图 beam 宽（`0`=max(64, 2r)）|
| `hnsw_build_nav_int8` | `bool` | `true` | S29-11-②：HNSW 建图导航 int8 混合精度（入选邻居 f32 精选，召回零损失；`false`=全 f32 回退闸）|
| `builder_threads` | `std::size_t` | `0` | S27-4 P2：文本插件 builder 线程数。`0`=内联（默认）；`>=1`=DWPT 并行 builder |
| `seal_v2_segments` | `bool` | `true` | S30-P2：封口段格式（`true`=v2 mmap 零驻留；`false`=v1 全量驻留回退）|
| `seal_ram_budget_bytes` | `std::size_t` | `0` | S30-P2：building 段 RAM 预算（`>0` 超预算就地封口；`0`=关）|
| `merge_fan_in` | `std::size_t` | `8` | 段 merge fan-in |
| `mmap_verify_crc` | `bool` | `true` | mmap 段是否校验 CRC |
| `auto_compact_dead_ratio` | `double` | `0.0` | 后台自动 compaction 的 per-list 死占比阈值；`0`=关闭 |
| `synonym_map` | `std::shared_ptr<const text::SynonymMap>` | `nullptr` | 同义词词典（open-time、不可变）|
| `max_delta_chain` | `std::uint32_t` | `64` | delta 链长上限，达到后 flush 强制全量 base（坍缩链）；`0`=不设限 |

```cpp
struct SearchLayerConfig {
    // 字段如上
    [[nodiscard]] text::TextPluginConfig text_config() const;
    [[nodiscard]] vec::VectorPluginConfig vector_config() const;
};
```

### 7.2 `bitcask::bm25::Bm25Params`（`bm25_params.hpp`）

```cpp
struct Bm25Params {
    float k1    = 1.2F;
    float b     = 0.75F;
    float delta = 0.0F;  // BM25+ 下界常数 δ（S8.10）；0=标准 BM25
};
```

### 7.3 `bitcask::vec::HnswConfig` / `HnswIndex` / `HnswMetric`（`hnsw.hpp`）

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `dim` | `std::uint16_t` | `0` | 向量维度 |
| `metric` | `HnswMetric` | `kDot` | `kDot`=内积（cosine 已在上游归一化）/ `kL2`=平方欧氏 |
| `M` | `std::uint32_t` | `16` | 上层邻居容量；L0 = `2M` |
| `ef_construction` | `std::uint32_t` | `200` | 构建时搜索宽度 |
| `seed` | `std::uint64_t` | `0x5EEDF00D` | 层抽样种子（测试可复现）|
| `inmem_int8` | `bool` | `false` | int8-only 内存模式（仅 kDot）|

```cpp
enum class HnswMetric : std::uint8_t {
    kDot = 0,  // 内积相似度
    kL2  = 1,  // 平方欧氏距离
};

class HnswIndex {
public:
    explicit HnswIndex(const HnswConfig& cfg);
    ~HnswIndex();

    HnswIndex(const HnswIndex&)            = delete;
    HnswIndex& operator=(const HnswIndex&) = delete;

    // 插入（仅单写者线程）。前置:vec.size()==dim;ord 全局单调。
    // 水位幂等:ord <= max_inserted_ord_ 时丢弃。
    void insert(std::uint64_t ord, std::span<const float> vec);

    struct Hit {
        std::uint64_t ord;
        float score;   // kDot:内积(越大越近);kL2:-平方距离
    };
    // 查询 top-k。线程安全(多读者,可与单写者 insert 并发)。
    // ef >= k(内部取 max);live 非空时结果侧过滤(死节点仍参与导航)。
    [[nodiscard]] std::vector<Hit> search(
        std::span<const float> query, std::size_t k, std::size_t ef,
        const std::function<bool(std::uint64_t)>* live = nullptr) const;

    [[nodiscard]] std::size_t     size() const noexcept;       // count_.load()
    [[nodiscard]] bool            empty() const noexcept;
    [[nodiscard]] std::uint64_t   max_inserted_ord() const noexcept;
    [[nodiscard]] const HnswConfig& config() const noexcept;

    // V3.5：merge 重建用只读访问。前置:id < size()。
    [[nodiscard]] std::uint64_t       node_ord(std::uint32_t id) const;
    [[nodiscard]] std::span<const float> node_vec(std::uint32_t id) const;

    // S13-P8：结构化拷贝活子图（merge 期 rebuild 用）。O(节点+边) memcpy 级。
    [[nodiscard]] std::shared_ptr<HnswIndex>
    clone_live(const std::function<bool(std::uint64_t)>& is_live) const;

    // V7 BCVS v2 快照（search.ckpt kHnsw 段 + .vec/.qc8 侧车）
    [[nodiscard]] bool save_vec_payload(std::string_view path) const;
    [[nodiscard]] bool load_vec_payload(std::string_view path);
    [[nodiscard]] bool save_qc_payload(std::string_view path)  const;
    [[nodiscard]] bool load_qc_payload(std::string_view path);
    [[nodiscard]] bool qc_payload_pending() const noexcept;  // v3 反序列化后是否欠 qc8 载入

    [[nodiscard]] bool serialize(std::vector<std::uint8_t>& out) const;
    [[nodiscard]] bool deserialize(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool save(std::string_view base_path) const;   // = save_vec_payload + serialize → fwrite
    [[nodiscard]] bool load(std::string_view base_path);         // = fread → deserialize → load_vec_payload
};
```

> HNSW 算法参考：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs"*, TPAMI 2018。设计详见 [`hnsw-design-zh.md`](hnsw-design-zh.md)。

### 7.4 `bitcask::text::Analyzer` 系列（`analyzer.hpp` + 各 analyzer.hpp）

```cpp
// analyzer.hpp
using TermFreqMap      = std::unordered_map<std::string, std::uint32_t>;
using TermPositionsMap = std::unordered_map<std::string,
    std::pair<std::uint32_t, std::vector<std::uint32_t>>>;
using TermTokenMap     = std::unordered_map<std::string, std::vector<TokenInfo>>;

struct TokenInfo {
    std::uint32_t position;     // 词在文本中的位置序号（从 0 开始）
    std::uint32_t start_byte;   // token 在原文中的起始字节偏移
    std::uint32_t end_byte;     // token 在原文中的结束字节偏移（不含）
};

enum class AnalyzerType {
    Ngram,        // CJK 字符级 n-gram + 拉丁空白切分（默认，V2.1）
    Whitespace,   // 纯空白切分（调试 / 纯拉丁场景）
    Jieba,        // jieba 词典分词 + CutForSearch + CJK 回退 n-gram（V2.10）
};

struct AnalyzerConfig {
    AnalyzerType  type               = AnalyzerType::Ngram;
    std::uint32_t min_n              = 2;
    std::uint32_t max_n              = 3;
    bool          enable_stop_words  = false;
    std::vector<std::string> stop_words;     // 空=用内置默认
    std::string   dict_path;                 // jieba 词典目录（必须有效）
    std::uint32_t min_token_length   = 1;    // 拉丁整词最小 codepoint 长度
    bool          enable_stemming    = false;// Porter 词干化
};

class Analyzer {
public:
    virtual ~Analyzer() = default;

    [[nodiscard]] virtual auto analyze(std::string_view text) const
        -> TermFreqMap;
    [[nodiscard]] virtual auto analyze_with_positions(std::string_view text) const
        -> TermPositionsMap = 0;
    [[nodiscard]] virtual auto analyze_with_offsets(std::string_view text) const
        -> TermTokenMap;
    [[nodiscard]] virtual auto type() const noexcept -> AnalyzerType = 0;
};

using AnalyzerCreator = std::unique_ptr<Analyzer>(*)(const AnalyzerConfig&);

class AnalyzerFactory {
public:
    static void register_creator(AnalyzerType type, AnalyzerCreator creator);
    [[nodiscard]] static auto create(const AnalyzerConfig& config)
        -> std::unique_ptr<Analyzer>;
};
```

各分词器子类：

| 类 | 头 | 关键构造参数 |
|----|----|------|
| `text::NgramAnalyzer` | `ngram_analyzer.hpp` | `(min_n=2, max_n=3, enable_stop_words=false, custom_stop_words={}, min_token_length=1)`；`min_n() / max_n()` |
| `text::WhitespaceAnalyzer` | `whitespace_analyzer.hpp` | `() = default` / `explicit (min_token_length)` |
| `text::JiebaAnalyzer` | `jieba_analyzer.hpp` | `(dict_dir={}, min_n=2, max_n=3, enable_stop_words=false, custom_stop_words={}, min_token_length=1)`；dict_dir 必须有效（运行时由 caller 保证）|
| `text::StemmingAnalyzer` | `stemming_analyzer.hpp` | `explicit (unique_ptr<Analyzer> inner)`——叠加 Porter 词干提取 |

### 7.5 `bitcask::bm25::QueryNode` / `parse_query`（`query.hpp`）

```cpp
enum class QueryOp : std::uint8_t {
    MUST,      // 必需匹配：所有 MUST term 必须在文档中出现
    SHOULD,    // 可选匹配：任意 SHOULD term 出现即加分
    MUST_NOT,  // 必须不匹配：排除
};

struct QueryNode {
    QueryOp               op;
    std::string           term;        // 叶子节点查询词
    std::string           field;       // 字段限定（S8.6，空=默认字段）
    float                 boost = 1.0F;
    std::vector<QueryNode> children;
    bool                  is_phrase = false;             // S13-D9 短语叶子
    std::vector<std::string> phrase_terms;

    // 工厂方法：叶子节点
    static QueryNode must_term(std::string t);
    static QueryNode should_term(std::string t);
    static QueryNode must_not_term(std::string t);
    // 工厂方法：组合节点
    static QueryNode must_all(std::vector<QueryNode> children);
    static QueryNode should_any(std::vector<QueryNode> children);
};

// 解析查询字符串为 QueryNode AST。
// 语法：以空白分割 token;+term MUST;-term MUST_NOT;裸 term SHOULD;
//       所有 SHOULD 时返回单一 SHOULD 节点。
[[nodiscard]] auto parse_query(std::string_view input) -> QueryNode;

// S13-D9：递归下降解析（括号嵌套 + 引号短语）。
[[nodiscard]] auto parse_query_tree(std::string_view input) -> QueryNode;

// 展平 AST 为 must/should/must_not 三个 term 集合。
void collect_terms(const QueryNode& node,
                   std::vector<std::string>& must_terms,
                   std::vector<std::string>& should_terms,
                   std::vector<std::string>& must_not_terms);
```

### 7.6 `bitcask::search::SearchCache`（`search_cache.hpp`）

LRU 缓存。`get` 共享锁并发；`put` / `invalidate` / `invalidate_terms` 独占。LRU 由 per-node 访问计数（atomic）表达（get 不修改链表）。

```cpp
struct CacheKey {
    std::uint64_t hash;
    static CacheKey make(std::string_view query_type,
                         std::string_view query,
                         std::size_t k);
};
struct CacheEntry {
    std::vector<bm25::SearchResult> results;
};

class SearchCache {
public:
    explicit SearchCache(std::size_t max_entries = 256);

    // 命中返回结果拷贝（锁内拷出，锁外消费）。
    [[nodiscard]] std::optional<std::vector<bm25::SearchResult>>
    get(const CacheKey& key) const;

    // 写入缓存。如果已满则淘汰 LRU 条目。
    // terms：该查询命中的查询词集合，供 invalidate_terms 做交集判定。
    void put(const CacheKey& key, std::vector<bm25::SearchResult> results,
             std::vector<std::string> terms);

    // 整缓存失效（拿不到变更文档词集时的降级路径）。
    void invalidate();

    // 选择性失效：移除「查询词与 changed_terms 有交集」的缓存条目（S9.2）。
    void invalidate_terms(std::span<const std::string_view> changed_terms);

    [[nodiscard]] std::size_t size()        const;
    [[nodiscard]] std::size_t max_entries() const;
};
```

### 7.7 `bitcask::search::Snippet` / `HighlightOptions`（`highlighter.hpp`）

```cpp
struct Snippet {
    std::string text;
    double      score;
};

struct HighlightOptions {
    std::string pre_tag        = "<em>";
    std::string post_tag       = "</em>";
    std::size_t fragment_size  = 120;
    std::size_t max_fragments  = 3;
};

struct HighlightResult {
    std::vector<Snippet> snippets;
};

// 生成高亮片段。
HighlightResult highlight(
    std::string_view text,
    const std::unordered_map<std::string, std::vector<text::TokenInfo>>& query_token_offsets,
    const HighlightOptions& opts = {});
```

### 7.8 `bitcask::meta::MetaFilter` / `MetaCondition`（`meta_filter.hpp`）

```cpp
enum class MetaOp : std::uint8_t {
    Eq     = 0,
    Neq    = 1,
    Gt     = 2,
    Gte    = 3,
    Lt     = 4,
    Lte    = 5,
    In     = 6,
    Exists = 7,
};

struct MetaCondition {
    std::string key;
    MetaOp      op   = MetaOp::Eq;
    MetaValue   value;
    std::vector<MetaValue> values;  // 仅 In 操作用

    // 在 meta blob 上求值。key 不存在 → false（Exists 例外）。
    // 类型不匹配或非法 op → false。
    [[nodiscard]] bool evaluate(std::span<const std::byte> blob) const;
};

struct MetaFilter {
    enum class Logic : std::uint8_t { And = 0, Or = 1 };
    Logic logic = Logic::And;
    std::vector<MetaCondition> conditions;
    std::vector<std::unique_ptr<MetaFilter>> children;

    [[nodiscard]] bool evaluate(std::span<const std::byte> blob) const;
};
```

类型系统：`Eq` / `Neq` 同 type 才比较；`Gt` / `Gte` / `Lt` / `Lte` 仅 int64 / float64 参与；`In` 在 values 列表里查；`Exists` 只看 key 是否出现。空 filter（无 conditions 无 children）恒返回 true。

`MetaValue`（在 `meta_codec.hpp`）：

```cpp
using MetaValue = std::variant<
    std::monostate,  // Null
    bool,
    std::int64_t,
    double,
    std::string>;
```

### 7.9 `bitcask::text::SynonymMap`（`synonym_map.hpp`）

```cpp
class SynonymMap {
public:
    SynonymMap() = default;

    // 从文件加载：每行一组，`term1,term2,term3`（行内按 `,` 切 + 去空白）；
    // 空行忽略，< 2 term 的行忽略。
    [[nodiscard]] bool load_from_file(const std::string& path);

    // 合并入已有的同义词组（O(n log n) 排序去重 + O(n) 指针赋值）。
    void add_group(std::vector<std::string> terms);

    // 命中返回同组所有 term 列表（含自己）；未命中返回空 span。
    [[nodiscard]] std::span<const std::string>
    expand(const std::string& term) const;

    // 多 term 展开去重。
    [[nodiscard]] std::vector<std::string>
    expand_terms(const std::vector<std::string>& terms) const;
};
```

### 7.10 `bitcask::text::TextPlugin`（`text_plugin.hpp`）

`plugin::CaskPlugin` 实现。reducer 单写者 + 查询线程多读者。

```cpp
class TextPlugin final : public plugin::CaskPlugin {
public:
    TextPlugin(const TextPluginConfig& config, const bm25::DocTable& docs,
               bm25::DocLenWriter& doc_len, bm25::CompactionStats& stats);
    TextPlugin(const TextPluginConfig& config, const bm25::DocTable& docs,
               bm25::DocLenWriter& doc_len, bm25::CompactionStats& stats,
               std::unique_ptr<Analyzer> injected_analyzer);  // S10-A1 测试专用

    TextPlugin(const TextPlugin&)            = delete;
    TextPlugin& operator=(const TextPlugin&) = delete;

    [[nodiscard]] bool has_analyzer() const noexcept;
    void replace_analyzer(std::unique_ptr<Analyzer> a);   // 测试注入通道

    // plugin::CaskPlugin 接口
    [[nodiscard]] std::string_view name() const override;            // "bm25"
    plugin::PluginStatus open(const plugin::OpenContext& ctx) override;
    [[nodiscard]] std::uint64_t watermark() const override;
    plugin::PluginStatus close() override;
    [[nodiscard]] bool wants_prepare() const override;
    [[nodiscard]] plugin::PreparedPtr prepare(const plugin::PutEvent& e) const override;
    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr prep) override;
    void on_delete(const plugin::DeleteEvent& e) override;
    plugin::FlushResult flush(const plugin::FlushRequest& req) override;
    void on_merge_commit(const plugin::MergeCommitEvent&) override;
    void force_rebase() noexcept;
    [[nodiscard]] bool rebase_needed() const noexcept;

    // 写（reducer 单写者）
    void apply_text(std::string_view key, std::uint64_t ord, std::string_view text);
    [[nodiscard]] search::ReduceJob map_analyze(
        std::string_view key, std::uint64_t ord,
        std::span<const std::pair<std::string_view, std::string_view>> fields,
        std::uint32_t file_id, std::uint64_t offset,
        std::uint32_t total_sz, std::uint32_t tstamp) const;
    void apply_job(search::ReduceJob& job);
    void apply_job(const search::ReduceJob& job);
    void on_delete(std::string_view key, std::uint64_t tomb_ord, std::uint64_t prior_ord);

    // 查询面（线程安全）
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_text(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr,
                const meta::MetaFilter* filter = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_phrase(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_near(std::string_view query, std::uint32_t slop, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    bool_search(std::string_view query, std::size_t k,
                const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance,
                 const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_fields(std::string_view query, std::size_t k,
                  const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search_wildcard(std::string_view pattern, std::size_t k,
                    const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::optional<bm25::ScoreExplanation>
    explain(std::string_view query, std::string_view key,
            const bm25::Bm25Params* params_override = nullptr) const;
    [[nodiscard]] std::expected<std::vector<search::SearchHitEx>, search::SearchError>
    search_text_highlight(std::string_view query, std::size_t k,
                          const search::HighlightOptions& opts = {}) const;

    // 维护
    std::size_t compact(double dead_ratio_threshold = 0.5);
    void rebuild_index(
        const std::function<void(
            const std::function<void(std::uint64_t, const std::string&)>&)>&
            for_each_doc);
    [[nodiscard]] std::size_t total_postings() const;
    [[nodiscard]] std::size_t cache_entries() const { return cache_.size(); }

    // 记账（S14-3 语义；default/fields 两段独立脏位）
    [[nodiscard]] bool dirty()          const noexcept;
    [[nodiscard]] bool dirty_default()  const noexcept;
    [[nodiscard]] bool dirty_fields()   const noexcept;
    void clear_dirty()          noexcept;
    void clear_dirty_default()  noexcept;
    void clear_dirty_fields()   noexcept;

    // legacy 统一 ckpt 容器原语（P5 随 legacy 收编后删除）
    [[nodiscard]] bool serialize_default(std::vector<std::byte>& out) const;
    [[nodiscard]] bool serialize_fields(std::vector<std::byte>& out) const;
    [[nodiscard]] bool serialize_default_delta(std::vector<std::byte>& out, std::uint64_t from) const;
    [[nodiscard]] bool serialize_fields_delta(std::vector<std::byte>& out, std::uint64_t from) const;
    [[nodiscard]] bool deserialize_default(std::span<const std::byte> payload);
    [[nodiscard]] bool deserialize_fields(std::span<const std::byte> payload);
    [[nodiscard]] bool apply_default_delta(std::span<const std::byte> payload);
    [[nodiscard]] bool apply_fields_delta(std::span<const std::byte> payload);

    // bm25 组件 checkpoint
    [[nodiscard]] bool        save_component_base(std::string_view dir, std::uint64_t watermark);
    void apply_job_impl(const search::ReduceJob& job, std::string&& doc_text);
    [[nodiscard]] ckpt::DeltaSaveResult save_component_delta(std::string_view dir, std::uint64_t watermark);
    [[nodiscard]] ckpt::LoadResult      load_component(std::string_view dir, std::uint64_t expected_base_wm, std::uint32_t chain_seq);

    // 链状态（Cask 转发同步）
    using ChainState = ckpt::ChainState;
    [[nodiscard]] ChainState chain_state() const;
    void set_chain_state(const ChainState& st);
};
```

### 7.11 `bitcask::vec::VectorEnginePlugin`（引擎契约基类）与三引擎实现

S32-M3 起向量引擎抽象为 `VectorEnginePlugin` 契约基类（`vector_engine_plugin.hpp`）——`Cask::vector_plugin()` 返回此基类指针，实际实现按 `meta.vector_engine` 选定：

| 引擎 | `meta::VectorEngine` | 实现类 | 头文件 | 定位 |
|------|---------------------|--------|--------|------|
| HNSW | `kHnsw`（默认）| `vec::VectorPlugin` | `vector_plugin.hpp` | 内存图（≤数 M 向量）|
| IVF-RaBitQ | `kIvfRq` | `vec::IvfPlugin` | `ivf_plugin.hpp` | IVF 磁盘段（10M-100M 推荐）|
| DiskANN | `kDiskann`（实验性）| `vec::DiskannPlugin` | `diskann_plugin.hpp` | Vamana 单层图 |

> 下方 `VectorPlugin` 文档以 HNSW 实现为代表；IVF/DiskANN 实现同一 `CaskPlugin` 接口契约，差异在内部段结构与查询内核。

### 7.11a `bitcask::vec::VectorPlugin`（HNSW 引擎，`vector_plugin.hpp`）

`plugin::CaskPlugin` 实现。reducer 单写者 + 查询线程多读者（`hnsw_` 为 `atomic<shared_ptr>`：rebuild 旁路建新图 + 原子换指针，旧图由引用计数续命）。

```cpp
class VectorPlugin final : public plugin::CaskPlugin {
public:
    VectorPlugin(const VectorPluginConfig& config, const bm25::DocTable& docs);

    VectorPlugin(const VectorPlugin&)            = delete;
    VectorPlugin& operator=(const VectorPlugin&) = delete;

    [[nodiscard]] bool          enabled() const noexcept;  // = config_.dim > 0
    [[nodiscard]] std::uint16_t dim()     const noexcept;

    // plugin::CaskPlugin 接口
    [[nodiscard]] std::string_view name() const override;     // "hnsw"
    plugin::PluginStatus open(const plugin::OpenContext& ctx) override;
    [[nodiscard]] std::uint64_t watermark() const override;
    plugin::PluginStatus close() override;
    void on_put(const plugin::PutEvent& e, plugin::PreparedPtr) override;
    void on_delete(const plugin::DeleteEvent&) override;
    plugin::FlushResult flush(const plugin::FlushRequest& req) override;
    void on_merge_commit(const plugin::MergeCommitEvent&) override;
    void force_rebase() noexcept;
    [[nodiscard]] bool rebase_needed() const noexcept;

    // 写入端归一化（cosine 归一化进 norm_buf；非 cosine 直接返回 input 的 span）
    [[nodiscard]] std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const;

    // 写（reducer 单写者）
    void insert(std::uint64_t ord, std::span<const float> v);

    // 查询（线程安全）
    [[nodiscard]] std::expected<std::vector<search::SearchHit>, search::SearchError>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter) const;

    // merge 重建（物理清死节点；单写者上下文）
    void rebuild();

    // 图节点数（含软删死节点；观测用）。无图 = 0。
    [[nodiscard]] std::size_t size() const;

    // 图句柄（legacy 统一 ckpt 容器路径用）
    [[nodiscard]] std::shared_ptr<HnswIndex> graph() const;
    void set_graph(std::shared_ptr<HnswIndex> g);

    // 记账
    [[nodiscard]] bool dirty() const noexcept;
    void clear_dirty() noexcept;
    void begin_delta_window(std::uint64_t wm);
    [[nodiscard]] bool delta_log_empty() const;
    void clear_delta_log();
    void serialize_delta_log(std::vector<std::byte>& out) const;
    [[nodiscard]] bool apply_delta_log(std::span<const std::byte> payload);

    // vec 组件 checkpoint
    [[nodiscard]] bool        save_component_base(std::string_view dir, std::uint64_t watermark);
    [[nodiscard]] ckpt::DeltaSaveResult save_component_delta(std::string_view dir, std::uint64_t watermark);
    [[nodiscard]] ckpt::LoadResult      load_component(std::string_view dir, std::uint64_t expected_base_wm, std::uint32_t chain_seq);

    // 链状态
    using ChainState = ckpt::ChainState;
    [[nodiscard]] ChainState chain_state() const;
    void set_chain_state(const ChainState& st);

    // legacy 统一 ckpt 的 kHnsw 段载入
    [[nodiscard]] bool load_graph_section(std::span<const std::byte> payload,
                                          const std::string& vec_path,
                                          const std::string& qc_path);
};
```

### 7.12 `bitcask::search::HybridSearcher`（`hybrid_searcher.hpp`）

RRF 融合器。持 `TextPlugin` / `VectorEnginePlugin` 的查询接口引用，两路各超采 `K' = max(4k, 64)`，RRF(60) 融合，ord 决胜。**非插件**（只装一个插件的部署不链接）。

```cpp
class HybridSearcher {
public:
    HybridSearcher(const text::TextPlugin& text, const vec::VectorEnginePlugin& vec);

    [[nodiscard]] std::expected<std::vector<SearchHit>, SearchError>
    search(std::string_view text_query, std::span<const float> vec_query,
           std::size_t k, const meta::MetaFilter* filter = nullptr) const;
};
```

### 7.13 `bitcask::plugin::CaskPlugin` / `PluginHost`（`plugin_api.hpp`）

KV 存储层插件回调接口。本头自包含：只依赖标准库，不 include 任何 bitcask 头。

```cpp
enum class PluginStatus : std::uint8_t { kOk = 0, kFailed = 1 };
enum class LogLevel    : std::uint8_t { kWarn = 0, kError = 1 };

struct RecordLoc { std::uint32_t file_id = 0; std::uint64_t offset = 0; std::uint32_t total_sz = 0; };

using FieldKV = std::pair<std::string_view, std::string_view>;

struct DocView {
    std::string_view           text;
    std::span<const FieldKV>   fields;
    std::span<const float>     vec;
    std::span<const std::byte> meta;
};

// 所有 view/span 仅在回调期间有效，插件要留存就拷贝。
struct PutEvent {
    std::uint64_t    ord = 0;
    std::string_view key;
    std::string_view value;            // 原始 value 字节（KV 视角）
    const DocView*   doc = nullptr;    // 结构化视图；纯 KV 写为 nullptr
    RecordLoc        loc;
    std::uint32_t    tstamp = 0;
    bool             replay = false;   // S18-8：恢复重放标记
};

inline constexpr std::uint64_t kNoPriorOrd = ~std::uint64_t{0};

struct DeleteEvent {
    std::uint64_t    ord = 0;
    std::string_view key;
    std::uint64_t    prior_ord = kNoPriorOrd;  // S16-2：被删文档原 ord
};

struct RelocateEvent {
    std::uint64_t    ord = 0;
    std::string_view key;
    RecordLoc        loc;        // 新定位
    std::string_view value;      // 仅回调期间有效
};

struct MergeBeginEvent {
    std::span<const std::uint32_t> input_file_ids;
    std::uint64_t                  watermark = 0;
};
struct MergeCommitEvent {
    std::span<const std::uint32_t> output_file_ids;
    double                         dead_ratio = 0.0;
};

struct MaintainEvent {
    enum class Reason : std::uint8_t { kPostMerge = 0, kAuto = 1 };
    Reason reason = Reason::kAuto;
    double dead_ratio_hint = 0.0;
};

struct FlushRequest {
    enum class Reason : std::uint8_t { kClose = 0, kMerge = 1, kAuto = 2, kManual = 3 };
    Reason          reason       = Reason::kManual;
    bool            force_rebase = false;
    std::uint64_t   watermark    = 0;  // 本次落盘的覆盖水位
};
struct FlushResult {
    PluginStatus  status      = PluginStatus::kOk;
    std::uint64_t covered_ord = 0;     // 本次落盘覆盖到的 ord 水位
    std::uint64_t generation  = 0;     // 插件自定义的代号（manifest 记录用）
    std::uint32_t chain_seq   = 0;     // S20-3：链回执
    std::uint64_t chain_wm    = 0;
};

struct Prepared { virtual ~Prepared() = default; };
using PreparedPtr = std::unique_ptr<Prepared>;

// 宿主服务（插件 → 宿主的窄反向接口）。生命周期覆盖 open..close。
class PluginHost {
public:
    virtual ~PluginHost() = default;
    virtual std::optional<std::string> read_at(RecordLoc loc) = 0;
    virtual void run_serialized(std::function<void()> fn) = 0;  // 在 reducer 静止点串行执行
    virtual void log(LogLevel level, std::string_view msg) = 0;
};

struct OpenContext {
    std::string_view dir;            // 库目录
    PluginHost*      host = nullptr; // 宿主服务句柄
    // S18-6：宿主 manifest 里本插件的已提交状态提示。
    std::uint64_t committed_base_watermark  = 0;
    std::uint64_t committed_chain_watermark = 0;
    std::uint32_t committed_chain_seq       = 0;
};

class CaskPlugin {
public:
    virtual ~CaskPlugin() = default;

    virtual std::string_view name() const = 0;            // "bm25" / "hnsw" / ...

    // 生命周期
    virtual PluginStatus  open(const OpenContext& ctx) = 0;
    virtual std::uint64_t watermark() const = 0;
    virtual PluginStatus  close() = 0;                     // 含终止性 flush

    // 数据事件（reducer 单写者，ord 严格升序）
    virtual void on_put(const PutEvent& e, PreparedPtr prep) = 0;
    virtual void on_delete(const DeleteEvent& e) = 0;

    // 可选能力：并行预处理（纯函数，任意线程）
    virtual bool          wants_prepare() const { return false; }
    virtual PreparedPtr   prepare(const PutEvent& e) const { (void)e; return nullptr; }

    // merge 参与（默认空实现）
    virtual void on_relocate(const RelocateEvent& e)   { (void)e; }
    virtual void on_merge_begin(const MergeBeginEvent& e)  { (void)e; }
    virtual void on_merge_commit(const MergeCommitEvent& e){ (void)e; }
    virtual void on_merge_abort() {}
    virtual void maintain(const MaintainEvent& e)       { (void)e; }

    // 持久化
    virtual FlushResult flush(const FlushRequest& req) = 0;
};
```

### 7.14 `bitcask::text::TextPluginConfig` / `bitcask::vec::VectorPluginConfig`（轻量配置 POD）

```cpp
// text_plugin_config.hpp
struct TextPluginConfig {
    text::AnalyzerConfig       analyzer_config;
    bm25::Bm25Params           bm25_params;
    std::size_t                cache_max_entries       = 256;
    std::size_t                doc_text_cache_max      = 1024;
    bool                       index_positions         = true;
    double                     auto_compact_dead_ratio = 0.0;
    std::shared_ptr<const text::SynonymMap> synonym_map;
    std::uint32_t              max_delta_chain         = 64;
};

// vector_plugin_config.hpp
struct VectorPluginConfig {
    std::uint16_t      dim = 0;                               // 0 = 无向量
    meta::VectorMetric metric = meta::VectorMetric::kNone;
    std::uint32_t      hnsw_m = 0;                            // 0 = HnswConfig 默认
    std::uint32_t      hnsw_ef_construction = 0;
    bool               inmem_int8 = false;                    // P5b：HNSW int8-only 内存
    std::uint32_t      max_delta_chain = 64;                  // delta 链长上限
    // S32-M1：base rebase 窗口门（自 base 起实际入图向量数达此值即强制全量 base）
    std::uint32_t      rebase_min_docs = 262144;              // 0 = 关，仅链长门
    // S32-M3：IVF 引擎参数（engine=kIvfRq 时生效；HNSW 忽略）。0 = 自动
    std::uint32_t      ivf_nlist = 0;                         //   nlist = 4·√N
    std::uint32_t      ivf_nprobe = 0;                        //   nprobe = max(nlist/32, 8)
    // S32-M5：DiskANN 引擎参数（engine=kDiskann 时生效）。0 = 自动
    std::uint32_t      diskann_r = 0;                         //   r = 32（邻接容量）
    std::uint32_t      diskann_l_build = 0;                   //   l_build = max(64, 2r)
    // S29-11-②：HNSW 建图导航 int8 混合精度（默认开；false = 全 f32 回退闸）
    bool               hnsw_build_nav_int8 = true;
};
```

### 7.15 `bitcask::bm25::DocTable` / `CompactionStats` / `DocLenWriter`（`doc_table.hpp`）

查询面只读身份表接口与配套窄接口。

```cpp
class DocTable : public LiveChecker {
public:
    [[nodiscard]] virtual std::optional<std::string>
    ord_to_ext(std::uint64_t ord) const = 0;
    [[nodiscard]] virtual bool
    eval_meta(std::uint64_t ord, const meta::MetaFilter& filter) const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t>
    ord_of(std::string_view ext_id) const = 0;
};

class CompactionStats {
public:
    virtual ~CompactionStats() = default;
    [[nodiscard]] virtual std::uint64_t retired_since_compact() const = 0;
    virtual void reset_retired_since_compact() = 0;
    [[nodiscard]] virtual std::uint64_t live_docs() const = 0;
};

class DocLenWriter {
public:
    virtual ~DocLenWriter() = default;
    virtual void set_doc_len(std::uint64_t ord, std::uint32_t len) = 0;
};
```

`LiveChecker` 基类（`live_checker.hpp`）：

```cpp
class LiveChecker {
public:
    virtual ~LiveChecker() = default;
    [[nodiscard]] virtual bool         is_live(std::uint64_t ord) const = 0;
    [[nodiscard]] virtual std::uint32_t doc_len(std::uint64_t ord) const = 0;

    // P2.1 批量接口（默认实现退化为逐个调用；Index 覆写为持锁数组直读）。
    virtual void fill_is_live(std::span<const std::uint64_t> ords,
                              std::span<char> out) const;
    virtual void fill_doc_lens(std::span<const std::uint64_t> ords,
                               std::span<std::uint32_t> out) const;
};
```

### 7.16 `bitcask::search::parallel_for_queries`（`search_arena.hpp`）

```cpp
// S7-4：把 [0, n) 并发跑在进程级共享「有界 Search 池」上（inter-query 并发）。
// body(i) 执行第 i 条**独立**查询、写各自结果槽（槽间不重叠 → 无需锁）。
// n<=1 直跑（零池开销）。
void parallel_for_queries(std::size_t n,
                          const std::function<void(std::size_t)>& body);
```

### 7.17 组件 checkpoint 公共类型（`component_ckpt.hpp`）

```cpp
struct ChainState {
    std::uint64_t base_gen = 0;  // base 世代
    std::uint64_t chain_wm = 0;  // 链覆盖水位
    std::uint32_t next_seq = 1;  // 下一 delta 序号
};
struct DeltaSaveResult {
    bool          wrote   = false;
    std::uint32_t new_seq = 0;
};
struct LoadResult {
    bool          loaded         = false;
    std::uint64_t watermark      = 0;
    bool          from_prev      = false;
    bool          all_segments_ok = false;
};
```

---

## 8. KeyDir 与 Registry

### 8.1 `bitcask::keydir::KeyDir`（`keydir.hpp`）

全内存 keydir：分片 mutex + 屏障 v2 写者闸门；所有 public 方法线程安全。

```cpp
inline constexpr std::uint32_t kMaxTime   = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint64_t kMaxEpoch  = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t kMaxSize   = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kMaxFileId = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint64_t kMaxOffset = std::numeric_limits<std::uint64_t>::max();

struct SingleEntry {
    std::uint32_t file_id  = 0;
    std::uint32_t total_sz = 0;
    std::uint64_t offset   = 0;
    std::uint64_t epoch    = 0;
    std::uint32_t tstamp   = 0;
    std::uint64_t ord      = 0;
};
struct MultiEntry {
    std::vector<SingleEntry> revisions;  // [0]=最新
};
using Entry = std::variant<SingleEntry, MultiEntry>;

struct EntryProxy {
    std::uint32_t   file_id     = 0;
    std::uint32_t   total_sz    = 0;
    std::uint64_t   offset      = 0;
    std::uint64_t   epoch       = 0;
    std::uint32_t   tstamp      = 0;
    std::uint64_t   ord         = 0;
    bool            is_tombstone = false;
    std::string_view key;                 // zero-copy view，仅持锁期间有效
};

struct FStatsEntry {
    std::uint32_t file_id          = 0;
    std::uint64_t live_keys        = 0;
    std::uint64_t total_keys       = 0;
    std::uint64_t live_bytes       = 0;
    std::uint64_t total_bytes      = 0;
    std::uint32_t oldest_tstamp    = 0;
    std::uint32_t newest_tstamp    = 0;
    std::uint64_t expiration_epoch = kMaxEpoch;
};

enum class PutResult       { kOk, kAlreadyExists };
enum class StartIterResult { kOk, kAlreadyIterating, kOutOfDate };

struct IterInfo {
    std::uint64_t iter_generation    = 0;
    std::uint64_t keyfolders         = 0;
    bool          frozen             = false;
    std::optional<std::uint64_t> pending_start_epoch;
};
struct KeyDirInfo {
    std::uint64_t key_count = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t epoch     = 0;
    IterInfo      iter_info;
    std::vector<FStatsEntry> fstats;
};

class KeyDir {
public:
    KeyDir() = default;
    ~KeyDir() = default;
    KeyDir(const KeyDir&)            = delete;
    KeyDir& operator=(const KeyDir&) = delete;

    // 写入或更新 key。newest_put=false 即 CAS（用 old_file_id/old_offset 比对）。
    PutResult put(std::string_view key,
                  std::uint32_t file_id, std::uint32_t total_sz,
                  std::uint64_t offset, std::uint32_t tstamp,
                  std::uint32_t now_sec,
                  bool newest_put,
                  std::uint32_t old_file_id, std::uint64_t old_offset,
                  std::uint64_t ord = 0);

    // 无条件删除。返回 true 表示原本有这条 key。
    bool remove(std::string_view key, std::uint32_t remove_time);

    // 条件删除（CAS）：只有 (tstamp, file_id, offset) 匹配当前 entry 才删。
    PutResult conditional_remove(std::string_view key,
                                 std::uint32_t tstamp,
                                 std::uint32_t file_id,
                                 std::uint64_t offset,
                                 std::uint32_t remove_time);

    // 默认拿最新 revision；epoch != kMaxEpoch 时拿那个 epoch 之前的最新。
    // 返回的 EntryProxy.key 是 zero-copy view，仅持锁期间有效。
    std::optional<EntryProxy> get(std::string_view key,
                                   std::uint64_t epoch = kMaxEpoch) const;

    [[nodiscard]] std::uint64_t get_epoch() const;     // atomic
    [[nodiscard]] std::uint64_t alloc_ord();           // atomic fetch_add
    void advance_ord(std::uint64_t ord);                // atomic CAS-max

    [[nodiscard]] std::unique_ptr<IterHandle> make_iter();

    void mark_ready();                  // atomic 写
    [[nodiscard]] bool is_ready() const;// atomic 读

    [[nodiscard]] std::uint32_t biggest_file_id() const;
    std::uint32_t increment_file_id();
    std::uint32_t increment_file_id_at_least(std::uint32_t conditional_id);

    [[nodiscard]] std::uint64_t peek_next_ord() const;

    // S14-7：keydir 元数据 delta（"BKMD" v1）——per-file 字节水位 + 单调标量 + fstats
    void serialize_meta_delta(
        std::vector<std::byte>& out,
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watermarks) const;
    [[nodiscard]] std::optional<std::vector<std::pair<std::uint32_t, std::uint64_t>>>
    apply_meta_delta(std::span<const std::byte> bytes);

    // 链重放删除：仅当当前 entry 的 ord < tomb_ord 才删除。
    bool remove_if_older(std::string_view key, std::uint64_t tomb_ord);

    [[nodiscard]] bool save_snapshot(
        std::string_view path,
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& watermarks) const;
    [[nodiscard]] std::optional<std::vector<std::pair<std::uint32_t, std::uint64_t>>>
    load_snapshot(std::string_view path);

    // 文件统计
    void set_pending_delete(std::uint32_t file_id);
    std::uint32_t trim_fstats(std::span<const std::uint32_t> file_ids);

    // 快照
    [[nodiscard]] KeyDirInfo info() const;

    // 诊断探针：key 长度分布
    struct KeyLenHistogram {
        std::uint64_t total = 0;
        std::uint64_t sso   = 0;  // ≤15B：SSO 内联
        std::uint64_t heap  = 0;  // >15B：每键一次堆分配
        std::array<std::uint64_t, 8> buckets{};  // [0,8) [8,16) ... [128,∞)
    };
    [[nodiscard]] KeyLenHistogram key_length_histogram() const;
};

class IterHandle {
public:
    explicit IterHandle(KeyDir* parent) noexcept;
    ~IterHandle() noexcept;

    IterHandle(const IterHandle&)            = delete;
    IterHandle& operator=(const IterHandle&) = delete;
    IterHandle(IterHandle&&)                 = delete;
    IterHandle& operator=(IterHandle&&)      = delete;

    StartIterResult start(std::uint32_t now_sec, int maxage, int maxputs);
    std::optional<EntryProxy> next(bool include_tombstones = false);
    void release();

    [[nodiscard]] bool          is_iterating() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
};
```

### 8.2 `bitcask::keydir::KeyDirRegistry`（`keydir_registry.hpp`）

进程内全局的命名 KeyDir 注册表。`acquire` 同名 → 同一 KeyDir（refcount 计数）。初始化协议：

- 第一个 `acquire` 同名：拿到 `kCreated` + 一个全新的 `is_ready() == false` KeyDir（初始化者）；
- 在 `mark_ready()` 之前的并发 `acquire` 拿到 `kNotReady` + nullptr；
- `mark_ready()` 之后所有 `acquire` 拿到 `kReady` + refcount 加 1。

```cpp
enum class AcquireStatus { kCreated, kReady, kNotReady };
struct AcquireResult { AcquireStatus status; std::shared_ptr<KeyDir> keydir; };

class KeyDirRegistry {
public:
    KeyDirRegistry() = default;
    ~KeyDirRegistry();   // out-of-line；dtor 停共享 index pool 并 join 线程

    KeyDirRegistry(const KeyDirRegistry&)            = delete;
    KeyDirRegistry& operator=(const KeyDirRegistry&) = delete;

    // S6-P3：本 registry 共享的索引双池（懒创建，进程内每 registry 一对
    // Map/Reduce 线程 → 线程数与库数解耦）。
    [[nodiscard]] bitcask::IndexPool* index_pool();

    [[nodiscard]] AcquireResult acquire(std::string_view name);
    [[nodiscard]] AcquireResult query(std::string_view name) const;
    void release(std::string_view name);  // name 必须跟 acquire 时一致

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    saved_biggest_file_id(std::string_view name) const;
};
```

---

## 9. Merge 策略与 Merger

### 9.1 `bitcask::merge::PolicyOptions` / `FileStatus` / `Decision` / `Reason`（`merge_policy.hpp`）

```cpp
struct PolicyOptions {
    // trigger（任一满足整体就要 merge）
    int          frag_merge_trigger          = 60;  // 百分比
    std::uint64_t dead_bytes_merge_trigger   = 512ULL * 1024ULL * 1024ULL;

    // V4：索引删除率触发
    int          deletion_rate_trigger       = 0;   // 百分比，0=禁用

    // per-file 阈值（任一满足该文件入选）
    int          frag_threshold              = 40;
    std::uint64_t dead_bytes_threshold       = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t small_file_threshold       = 10ULL * 1024ULL * 1024ULL;  // 0=禁用

    // 过期
    std::uint32_t expiry_secs                = 0;  // 0=禁用
    std::uint32_t expiry_grace_time          = 0;

    // 输出体积上限
    std::uint64_t max_merge_size             = 0;  // 0=无上限
};

struct FileStatus {
    std::uint32_t file_id;
    std::string   filename;
    int           fragmented;        // 0..100 碎片率百分比
    std::uint64_t dead_bytes;
    std::uint64_t total_bytes;
    std::uint32_t oldest_tstamp;
    std::uint32_t newest_tstamp;
    std::uint64_t expiration_epoch;
};

struct Decision {
    bool needs_merge = false;
    std::vector<FileStatus> files;           // 要并的候选
    std::vector<FileStatus> expired_files;   // 子集：因过期入选
};

struct Reason {
    enum class Kind {
        kFragmented,
        kDeadBytes,
        kSmallFile,
        kDataExpired,
    };
    Kind kind;
    std::uint64_t value  = 0;   // kFragmented 百分比；其它字节
    std::uint64_t cutoff = 0;   // 仅 kDataExpired 用
};

// 把单条 fstats 转成 FileStatus。
// total_bytes==0 && total_keys==0 的 fstats（空文件统计）隐式过滤。
[[nodiscard]] FileStatus
summarize(std::string_view dirname, const keydir::FStatsEntry& f);

// 列出某文件命中了哪些 per-file 阈值；空列表表示该文件不参与 merge。
[[nodiscard]] std::vector<Reason>
per_file_reasons(const FileStatus& f, const PolicyOptions& opts, std::uint32_t now_sec);

// 完整决策。dead_doc_rate=(total_ords - live_docs)/total_ords*100。
[[nodiscard]] Decision
decide(const std::vector<FileStatus>& summary, const PolicyOptions& opts,
       std::uint32_t now_sec, int dead_doc_rate = 0);

// 应用 max_merge_size 上限：第一个文件无条件保留。
[[nodiscard]] std::vector<FileStatus>
cap_size(const std::vector<FileStatus>& files,
         const std::vector<std::uint64_t>& sizes,
         std::uint64_t max_merge_size);
```

### 9.2 `bitcask::merge::run_merge` / `MergeStats` / `MergeFault`（`merger.hpp`）

```cpp
enum class MergeError {
    kInputOpenFailed,
    kOutputOpenFailed,
    kInputReadFailed,
    kOutputWriteFailed,
    kFinalizeFailed,
};
struct MergeFault {
    MergeError  kind;
    int         errnum = 0;
    std::string detail;
};
struct MergeStats {
    std::string   output_data_path;
    std::string   output_hint_path;
    std::uint32_t output_file_id = 0;
    std::uint64_t records_seen    = 0;
    std::uint64_t records_kept    = 0;
    std::uint64_t records_stale   = 0;
    std::uint64_t records_tombs   = 0;
    std::uint64_t records_expired = 0;   // S13-D5：per-key TTL 过期
    std::uint64_t bytes_written   = 0;
    std::uint64_t relocations_stuck = 0;  // S13-F1：CAS 被拒且复查发现仍指向输入文件
    std::vector<std::uint32_t> stuck_file_ids;  // 已排序去重
};

// 把 input_data_paths 合并到 output_dir 下一个新 data file + hint。
// new file_id 经 keydir.increment_file_id() 分配。
// sync_output=true → 输出文件以 O_SYNC 打开。
// plugins 非空时按 [DocmapRelocator, ...] 顺序派发 merge 事件。
// now_sec=0 → 不判 TTL。
[[nodiscard]] std::expected<MergeStats, MergeFault>
run_merge(std::span<const std::string> input_data_paths,
          std::string_view output_dir,
          keydir::KeyDir& keydir,
          bool sync_output = false,
          std::span<plugin::CaskPlugin* const> plugins = {},
          std::uint32_t now_sec = 0);
```

---

## 10. 用法示例

### 10.1 KV 模式：增删改查

```cpp
#include <bitcask/cask.hpp>

using bitcask::Cask, bitcask::CaskOptions;
using bitcask::keydir::KeyDirRegistry;

int main() {
    // registry 强制非空（管理同目录 Cask 的共享 keydir；典型每进程一个）。
    KeyDirRegistry registry;
    auto c = Cask::open("/tmp/mydb", CaskOptions{.read_write = true}, &registry);
    if (!c) return 1;                       // c.error() 是 CaskFault

    std::vector<std::byte> key{std::byte{'h'}, std::byte{'i'}};
    std::vector<std::byte> val{std::byte{'w'}, std::byte{'o'}};

    (*c)->put(key, val);                          // 写（tstamp=0, expiry_at=0）
    if (auto r = (*c)->get_owned(key)) {          // 读（owned）
        // r->value, r->tstamp, r->ord, r->expiry_at
    }
    (*c)->remove(key);                            // 软删

    (*c)->close();
}
```

### 10.2 索引模式：BM25 + 向量 + 混合检索

```cpp
#include <bitcask/cask.hpp>
#include <bitcask/searcher.hpp>
#include <bitcask/search_config.hpp>
#include <bitcask/analyzer.hpp>

using namespace bitcask;
using namespace bitcask::search;
using namespace bitcask::text;
namespace meta = bitcask::meta;

int main() {
    CaskOptions opts;
    opts.read_write    = true;
    opts.enable_search = true;
    opts.search_config = SearchLayerConfig{
        .analyzer_config = AnalyzerConfig{.type = AnalyzerType::Ngram,
                                          .min_n = 2, .max_n = 3},
    };
    opts.vector_dim    = 128;                                        // 启用向量
    opts.vector_metric = meta::VectorMetric::kCosineNormalized;      // 写入端归一化

    keydir::KeyDirRegistry registry;
    auto c = Cask::open("/tmp/db", opts, &registry);
    if (!c || !(*c)->has_search()) return 1;

    std::vector<std::byte> key{std::byte{'d'}, std::byte{'1'}};
    std::vector<std::byte> text(reinterpret_cast<const std::byte*>("北京今天天气晴朗"), 24);
    std::vector<float> vec(128, 0.0f); /* ...填查询/文档向量... */

    DocInput doc{.text = text, .vector = vec};
    (*c)->put_doc(key, doc);
    (*c)->flush_index();              // 等待异步索引排空（put 本身已持久化）

    auto bm25 = (*c)->search_text("北京 天气", 10);        // 词袋
    auto knn  = (*c)->search_vector(vec, /*k=*/10);        // 向量
    auto hyb  = (*c)->search_hybrid("北京 天气", vec, 10);  // RRF 融合

    (*c)->close();
}
```

### 10.3 搜索器门面（`Searcher`，推荐）

```cpp
#include "bitcask/searcher.hpp"
#include "bitcask/cask.hpp"

auto* tp = cask->text_plugin();
bitcask::text::Searcher ts(*cask, *tp);
auto hits = ts.search_text("query", 10);                 // expected<TextSearchResult, CaskFault>

auto* vp = cask->vector_plugin();
bitcask::vec::Searcher vs(*cask, *vp);
auto vhits = vs.search(query_vec, 5);

bitcask::search::CaskHybridSearcher hs(*cask, *cask->hybrid_searcher());
auto rrf = hs.search("text", vec_query, 10);             // RRF 融合
```

`Searcher` 每次查询先经 `Cask::drain_plugins()` 读屏障（read-your-writes：submitted ⇒ applied），再直调插件内核，错误统一翻译为 `CaskFault`。

| 门面 | 主要方法 |
|------|----------|
| `bitcask::text::Searcher` | `search_text` / `search_phrase` / `search_near` / `bool_search` / `search_fields` / `search_fuzzy` / `search_wildcard` / `search_text_highlight` / `search_text_batch` |
| `bitcask::vec::Searcher` | `search` / `search_batch` |
| `bitcask::search::CaskHybridSearcher` | `search`（文本+向量 RRF）|

### 10.4 CaskIter 迭代

```cpp
auto it = (*c)->make_iter();
if (auto st = it->start(/*maxage=*/-1, /*maxputs=*/-1, /*now_sec=*/0,
                        /*see_tombstones=*/false); st) {
    while (auto e = it->next()) {
        if (!e->has_value()) break;          // EOI
        auto& entry = **e;
        // entry.key / entry.value / entry.tstamp / entry.is_tombstone ...
    }
}
it->release();
```

### 10.4b 有序 range 迭代（OKI）

```cpp
bitcask::RangeOptions ro;
ro.lo = as_bytes("user:");
ro.hi = as_bytes("user;");        // ':' + 1 = 前缀窗口的开区间上界
ro.prefetch = 256;                 // 可选：并发预取值（大窗口 + 冷值才划算）
auto it = (*c)->make_range_iter(ro);
while (auto e = (*it)->next()) {
    if (!e->has_value()) break;
    auto& entry = **e;             // key 升序
}
```

### 10.5 元过滤器

```cpp
#include "bitcask/meta_filter.hpp"
using bitcask::meta::MetaFilter, bitcask::meta::MetaCondition, bitcask::meta::MetaOp;

MetaFilter f;
f.logic = MetaFilter::Logic::And;
f.conditions.push_back(MetaCondition{
    .key = "category", .op = MetaOp::Eq, .value = std::string("news")});
f.conditions.push_back(MetaCondition{
    .key = "score", .op = MetaOp::Gte, .value = std::int64_t{50}});

// Cask 自动物化：(*c)->search_text("北京", 10, &f);
```

### 10.6 同义词词典

```cpp
#include "bitcask/synonym_map.hpp"

auto sm = std::make_shared<bitcask::text::SynonymMap>();
sm->add_group({"番茄", "西红柿", "tomato"});   // 或 sm->load_from_file(path)
CaskOptions opts;
opts.enable_search = true;
opts.synonym_map   = sm;
auto c = bitcask::Cask::open(dir, opts, &registry);
```

---

## 11. 运维调优

### 11.1 fd / mmap 预算：怎么降"打开文件数"

一个 Cask 实例常驻的 fd 由四部分构成（实测形态，`bench` 同款负载下用 `/proc/self/fd` 数出来的）：

| 来源 | 数量 | 是否有界 |
|---|---|---|
| 封口 data 文件的 read 句柄 | **大头**（每句柄 1 fd + 1 sealed mmap）| ✅ 受 `max_read_handles` 封顶 |
| active data 写句柄 + active hint | 2 | ✅ 常数（封口 hint 不常驻）|
| OKI run（`kv.oki.seg-*`）| ≤ `kCompactRunLimit + 1` = 9 | ✅ 由 run 归并保证（S33-6）|
| `bitcask.write.lock` + `field.schema` | 2 | ✅ 常数 |

**实测（1500 key、`max_file_size` 压到 4 KiB 逼出 89 个 data 文件）**：

| 配置 | 盘上 data 文件 | 本库 fd | mmap |
|---|---|---|---|
| 默认 | 89 | 96 | 88 |
| `max_read_handles = 16` | 89 | **24** | 16 |
| `max_file_size = 1 MiB` | **1** | **8** | 0 |

按这个顺序调：

1. **先量**：`ulimit -n` 与目录里的 data 文件数。自动档取 `RLIMIT_NOFILE` 的一半——rlimit 很大时该值形同虚设（S33-6 起夹到 1024 封顶，但仍建议按业务显式给值）。
2. **`max_read_handles = N`**：立即生效、不需要任何后台动作。超额时近似 LRU 淘汰**空闲**句柄；在途读者持 `shared_ptr` 续命，不会被抽走。代价是淘汰后再读该文件要重新 open + 重建 mmap，热点集中的负载几乎无感，全表随机扫会有 churn。
3. **`max_file_size`**（默认 2 GiB）：从源头决定文件个数。100 GB 的库在默认值下约 50 个 data 文件；若线上被调小过，那才是文件数暴涨的根因。
4. **别指望 merge 降 fd**：merge 的触发判据是**碎片率 / 死字节**（`frag_merge_trigger = 60%`、`dead_bytes_merge_trigger = 512 MiB`），不是文件个数。纯追加、无覆盖写与删除的库 `needs_merge()` 恒为 `false`（实测：89 个文件跑完 merge 仍是 89 个）。merge 解决的是**空间放大**，不是 fd 预算。

> `vm.max_map_count`（默认 65530）是 mmap 侧的系统上限；`max_read_handles` 同时封顶映射数，故一并受控。

### 11.2 merge 调度

库内**不做周期策略**——`merge()` 由 caller 按业务低峰/写入量自行调度（`Cask::checkpoint` 同理）。用 `needs_merge()` 拿判据与候选文件列表；同一目录同时只能有一次 merge 在跑（caller 保证）。策略阈值见 [`merge-policy-zh.md`](merge-policy-zh.md)。

> **空间回收时序（B4）**：merge 的输入文件先**退休**（留在原路径，消除在途读者的 ENOENT 窗口），到下一个落点（下次 `merge()` 开始 / `checkpoint()` 入口 / `close()`）才真正删除——merge 返回后磁盘占用**延后一拍**下降。崩溃残留的退休文件无害且会被后续 merge 自愈收编。

### 11.3 keydir 磁盘驻留（Level B）

`keydir_cache_entries > 0` 把 keydir 从「全量内存权威」降级为「热点缓存」，点查权威变成 **缓存 → memdelta → 磁盘 run（BCOK v2：内嵌 bloom + 稀疏索引 + 块 LRU）** 的组合视图。设计与格式见 [`keydir-disk-resident-design-zh.md`](keydir-disk-resident-design-zh.md) 与 [`format-zh.md` §15.4](format-zh.md)。

**收益锚点**（tmpfs，`doc:<n>` 形态 1 亿 key，预算 500 万，2026-08-06 实测）：常驻 11GB → 加载峰值 **1.14GB** / 重开 **0.80GB**（-90%）；重开走 BCKS v4 子集快照 ~1 秒；热 get 零回归（缓存命中即现状路径）；冷 get tmpfs 锚点 p50 ~4-5µs / p99 ~14µs（块缓存命中 ~4µs/9µs；SSD 预算门 ≤300µs）。

使用要点：

1. **opt-in 且可回退**：默认 0 = 现状。首次对旧目录开启会**全量重建 OKI**（既有 run 无位置字段/不可信，一次 fold + 外排；重建后 manifest 带 Level B 模式戳，此后重开走快路径）。回到 `0`（Level A）随时可以——open 自动清戳、快照回落 v3，再开启时重建自愈。
2. **merge_only 旁车互斥**：旁车 merge 不维护 run 位置，对带戳目录 `open` 直接拒绝（`kIo`，报文说明）。同理，Level A 写者动过的目录回 Level B 会自动重建。
3. **预算是软目标**：fold/scan 活跃期间暂停逐出；预算按 256 分片均摊（每分片下限 8 条）。读热但被逐的 key 由「二次命中读升温」自动回填。
4. **计数语义不变**：`key_count`/fstats 是逻辑值（逐出不减、覆盖被逐 key 不虚增——写路径经组合视图裁决），merge 触发判据不受逐出影响。
5. **只读句柄**同样受益：带戳目录 + `keydir_cache_entries>0` 的 RO 打开走子集快照，内存有界。

---

## 12. 线程模型汇总

> **定位**：libbitcask 是**通用 C++ 库**——同一 Cask handle 可被多线程安全共享。详见 [`design/thread-safety.md`](../docs/design/thread-safety.md)。

图例：✅ = 同一 handle 多线程调用安全；⚠️ = 有条件/不安全（见说明）。

| 操作 | 线程安全 | 机制 |
|------|---------|------|
| `open` / `upgrade` | ✅ | 产生独立对象；registry 内部锁 |
| `close` | ⚠️ 生命周期（幂等）| caller 须保证无在途调用；close 后新调用 fail-fast 返回 `kClosed` |
| `get` / `get_owned` / `read_handle_count` | ✅ | keydir 分片 shared_lock + `pread`（无状态、thread-safe）；read_files_ 由 `read_cache_mu_` 护 |
| `put` / `put_batch` / `remove` / `put_doc` / `sync` / `close_write_file` / `checkpoint` | ✅ | **S11-W1：内部 `write_mu_` 串行化**整个写序列；多线程并发写安全（写本就串行 → 吞吐不变） |
| `search_text` / `_phrase` / `_bool` / `_fields` / `_near` / `_fuzzy` / `_wildcard` / `search_text_highlight` | ✅ | 并发读：`cache_`/`doc_texts_` shared_mutex、InvertedIndex 分片 shared_lock、analyzer const |
| `search_vector` / `search_hybrid` | ✅ | HNSW `atomic<shared_ptr>` 快照（读者引用计数续命）；两路内核读路径均并发安全 |
| `search_*_batch` | ✅ | inter-query 并发跑共享 Search 池（`search_arena`）；各结果槽独立 |
| `parallel_scan` | ✅ | 串行快照 live key → 分 N 段 → 并发 `get`；`fn` 须线程安全（各处理不相交段） |
| 同义词词典（`CaskOptions::synonym_map`） | ✅ | open-time 不可变 → 并发查询安全（无运行期 setter，无竞态） |
| `status` / `is_empty_estimate` / `is_frozen` / `needs_merge` / `flush_index` / `drain_plugins` | ✅ | 只读 keydir 快照 / IndexPool flush 自带 cv 同步 |
| `merge` | ✅ | 写自有输出文件 + keydir shared_mutex 协调（不取 `write_mu_`，与读写并发）；跨进程经 `merge.lock` |
| `backup` | ✅ | 与 put/get 并发（put 被 `write_mu_` 挡在备份期间外，get 不受影响）；须与 merge 串行 |
| `CaskIter::start` / `next` / `next_batch` / `release` | ⚠️ 每线程一个 | 有状态游标，同一对象不可并发；不同 CaskIter 可并发；并行遍历用 `parallel_scan` |
| `make_range_iter` | ✅ | 取 OKI 只读视图（runs 的 shared_ptr + memdelta 快照拷贝）|
| `CaskRangeIter::next` | ⚠️ 每线程一个 | 有状态游标，同一对象不可并发；不同 range 迭代器可与写/merge 并发（per-key 弱一致）|

> **读写并发**：搜索可见性遵循 near-real-time 契约（`drain_plugins()` flush 覆盖调用前的写）。
> **更高写并发**：写在文件层本就串行（单 append WAL），`write_mu_` 不损吞吐；需要更高写并发请**按目录分片多个 Cask 实例**（横向扩展）。
> 并发 merge 与 live writer 通过双锁模型（`bitcask.write.lock` / `bitcask.merge.lock`）并行不互斥。详见 [`concurrency-zh.md`](concurrency-zh.md)。
