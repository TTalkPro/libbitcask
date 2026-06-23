# C++ API 参考

本文档是 libbitcask C++ 接口的完整参考。权威来源为 `include/bitcask/` 下的公共头文件；符号位于 `namespace bitcask`。配套阅读：[`cpp-arch.md`](cpp-arch.md)（架构）、[`format-zh.md`](format-zh.md)（磁盘格式）。

所有可能失败的接口用 `std::expected<T, CaskFault>` 返回；调用方应检查 `.has_value()` 后再解引用。

---

## 1. 概述

libbitcask 有两种工作模式，由打开选项决定：

- **KV 模式**（`enable_search = false`）：Bitcask 追加日志 KV。`put`/`get`/`remove` 为 O(1)，读值单次 `pread`。纯 KV 的 binary 经 DocValue v3 编码进 text 段，对调用方透明。
- **索引模式**（`enable_search = true` + `search_config`）：在 KV 之上叠加 BM25 倒排、HNSW 向量图、字段索引，提供文本/向量/混合检索。模式在 `bitcask.meta` 持久化，重开必须一致，否则 `kModeMismatch`。

---

## 2. 头文件与链接

```cpp
#include <bitcask/cask.hpp>      // 主门面：Cask / CaskOptions / GetResult ...
```

链接 `libbitcask`（静态聚合 `.a` 或共享 `.so`）。索引模式按需附带 include：

```cpp
#include <bitcask/search_layer.hpp>   // SearchLayer / SearchLayerConfig / SearchHit
#include <bitcask/hnsw.hpp>           // HnswConfig / HnswMetric
#include <bitcask/analyzer.hpp>       // AnalyzerType / AnalyzerConfig
```

---

## 3. 配置与错误类型

### 3.1 `CaskOptions`（`Cask::open` 的打开选项）

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `read_write` | `bool` | `false` | `false`=只读；`true`=可写（持 `bitcask.write.lock`）|
| `max_file_size` | `std::uint64_t` | `2 GiB` | 单个 data 文件上限，超过则 roll 到新文件 |
| `max_read_handles` | `std::size_t` | `0` | read 句柄缓存上限；`0`=不限；超额近似 LRU 淘汰空闲句柄，控 fd/mmap 数 |
| `o_sync` | `bool` | `false` | 每条写 durable（`O_SYNC`）；为真时 `sync_every_n` 无意义 |
| `sync_every_n` | `std::uint32_t` | `0` | 单写者组提交：每 N 次写 fsync 一次；`0`=关闭 |
| `require_hint_crc` | `bool` | `false` | 是否要求 hint trailer CRC 通过 |
| `expiry_secs` | `std::uint32_t` | `0` | TTL：`tstamp < now - expiry_secs` 的 record 在 get/fold 中被过滤，并触发 merge；`0`=禁用 |
| `merge_only` | `bool` | `false` | merge-only 模式：拿 `bitcask.merge.lock`，不创建 active writer，可与 live writer 并行 merge；与 `read_write` 互斥语义 |
| `tombstone_version` | `std::uint8_t` | `0` | 墓碑格式：`0`=17B 前缀；`2`=22B 含 FileId（支持并发 merge 精细回收）。读时三种都接受 |
| `policy` | `merge::PolicyOptions` | `{}` | merge 触发策略（碎片率/死字节/过期阈值）|
| `enable_search` | `bool` | `false` | 启用索引模式 |
| `search_config` | `std::optional<search::SearchLayerConfig>` | `nullopt` | 有值时才创建 SearchLayer |
| `vector_dim` | `std::uint16_t` | `0` | 向量维度；`>0` 即启用向量，要求 `enable_search`；库内恒定 |
| `vector_quantized` | `bool` | `false` | 向量落盘 int8 量化（4× 磁盘，有损）|
| `vector_inmem_int8` | `bool` | `false` | HNSW int8-only 内存（约 −80% 向量内存，仅 kDot）；与 `quantized` 正交 |
| `vector_metric` | `meta::VectorMetric` | `kCosineNormalized` | 向量距离度量 |

> 向量配置（`vector_dim`/`vector_metric`/`vector_quantized`/`vector_inmem_int8`）创建即固定，写入 `bitcask.meta`；重开校验不符 → `kModeMismatch`。

### 3.2 `CaskError`（错误码枚举）

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
| `kNoIndex` | KV 模式下调用了 search 接口 |
| `kModeMismatch` | 文件模式与打开选项不匹配 |
| `kAnalyzerMismatch` | 分析器类型不匹配 |

### 3.3 `CaskFault`（错误详情）

```cpp
struct CaskFault {
    CaskError    kind;
    int          errnum = 0;   // IO 错误时为 errno，否则 0
    std::string  detail;        // 人类可读描述
};
```

### 3.4 `meta::VectorMetric`（向量度量）

| 值 | 含义 |
|----|------|
| `kNone` | 无向量 |
| `kCosineNormalized` | 归一化余弦（写入端归一，查询用内积）|
| `kL2` | 平方欧氏距离 |
| `kDot` | 内积 |

---

## 4. 结果类型

### 4.1 `GetResultView`（零拷贝读结果）

```cpp
struct GetResultView {
    std::span<const std::byte> value;    // text 段（指向底层缓冲内部）
    std::span<const std::byte> meta;     // meta 段（可为空）
    std::span<const float>     vector;   // 向量段（空=无向量）
    std::uint32_t              tstamp = 0;
    std::uint64_t              ord    = 0;

    GetResult to_owned() const;          // 拷贝为 owned 版本
    // 可移动，不可拷贝
};
```

`get()` 返回 view：`value`/`meta`/`vector` 是借用内部 pread 缓冲的 span，**生命周期与 `GetResultView` 绑定**。即时消费场景零拷贝；需持久化用 `get_owned()` 或 `to_owned()`。量化文档无法零拷贝成 f32，引擎内部 dequant 进自有缓冲。

### 4.2 `GetResult`（owned 读结果）

```cpp
struct GetResult {
    std::vector<std::byte> value;    // text 段
    std::vector<std::byte> meta;     // meta 段（可为空）
    std::vector<float>     vector;   // 向量段（空=无向量）
    std::uint32_t          tstamp = 0;
    std::uint64_t          ord    = 0;
};
```

### 4.3 `DocInput`（`put_doc` 输入）

```cpp
struct DocInput {
    std::span<const std::byte> text;    // 必需（多字段时可空，作默认字段）
    std::span<const std::byte> meta;    // 可选
    std::span<const float>     vector{};  // 可选（空=无；长度须 == 配置的 vector_dim）
    std::vector<std::pair<std::string, std::span<const std::byte>>> fields;  // 多字段
};
```

`cosine_normalized` 度量下引擎写入前归一化向量（存储的即归一化值）。

### 4.4 `TextSearchResult`（检索结果）

```cpp
struct TextSearchResult {
    std::vector<search::SearchHit> hits;
};
```

### 4.5 `StatusInfo`

```cpp
struct StatusInfo {
    std::uint64_t                   key_count    = 0;
    std::uint64_t                   key_bytes    = 0;
    std::uint64_t                   epoch        = 0;
    std::vector<merge::FileStatus>  files;
    std::uint64_t                   index_errors = 0;  // indexed worker 抛异常时自增；非零 = 索引可能漂移
};
```

### 4.6 `Cask::NeedsMerge`

```cpp
struct NeedsMerge {
    bool                         needs;
    std::vector<std::string>     files;          // 候选文件
    std::vector<std::string>     expired_files;  // 过期文件
};
```

---

## 5. `Cask` 类（核心门面）

不可拷贝。`open()` 返回 `std::expected<std::unique_ptr<Cask>, CaskFault>`。

### 5.1 生命周期

#### `open`
```cpp
static std::expected<std::unique_ptr<Cask>, CaskFault>
open(std::string_view dirname, const CaskOptions& opts,
     keydir::KeyDirRegistry* registry);
```
打开一个 Cask 实例。`registry` **强制非空**（管理同目录 Cask 间的共享 keydir；典型生产形态：每进程/实例一个全局 registry）——传 `nullptr` 返回 `kInvalidOption`（无 fallback；异步索引双池归属 registry）。
- **错误**：`kIo`、`kWriteLocked`（锁被占）、`kInvalidOption`（含 registry 为空）、`kModeMismatch`、`kAnalyzerMismatch`。
- **线程安全**：是（每次调用产生独立 Cask 对象；registry 并发由其内部锁保证）。

#### `upgrade`
```cpp
static std::expected<std::unique_ptr<Cask>, CaskFault>
upgrade(std::string_view dirname, const search::SearchLayerConfig& search_config);
```
离线把 KV 模式目录升级为索引模式。前提：目录为 KV 模式且**离线**（无活跃 writer）。流程：读 meta 验证 → 写新 meta → 建 SearchLayer → 扫描全部数据文件重建索引 → 返回只读索引模式 Cask。
- **线程安全**：是。

#### `close`
```cpp
void close() noexcept;
```
释放资源。此后对象不可再用。**线程安全**：否（caller 保证关闭时刻无其它线程在调用 get/put/remove/sync/iter）。

### 5.2 读

#### `get`（零拷贝）
```cpp
std::expected<GetResultView, CaskFault>
get(std::span<const std::byte> key);
```
单 key 读：keydir 查 → 一次 `pread`。`type=kTombstone` 当作 `kNotFound`。
- **错误**：`kNotFound`、`kBadCrc`、`kIo`。
- **线程安全**：是（读路径无锁；read 缓存受内部锁保护，`pread` 本身线程安全）。

#### `get_owned`（拷贝）
```cpp
std::expected<GetResult, CaskFault>
get_owned(std::span<const std::byte> key);
```
拷贝语义版本——benchmark 等需 owned 数据的场景。线程安全同 `get`。

#### `read_handle_count`
```cpp
std::size_t read_handle_count() const;
```
当前常驻 read 句柄数（内省用，测试断言 fd 预算上限）。线程安全：共享锁读。

### 5.3 写

> 写路径要求**「一个 Cask 同时只有一个写线程」**——本类不提供互斥。同一 Cask 与并发 `merge_only` 句柄安全（双方共享 keydir 的 shared_mutex）。

#### `put`
```cpp
std::expected<void, CaskFault>
put(std::span<const std::byte> key,
    std::span<const std::byte> value,
    std::uint32_t tstamp = 0);
```
`tstamp=0` 用当前 wall-clock 秒。**错误**：`kReadOnly`、`kKeyTooLarge`、`kValueTooLarge`、`kAlreadyExists`（CAS 竞态，内部 roll 后重试）、`kIo`。**线程安全**：否（caller 串行化所有 put/remove/sync/close_write_file）。

#### `remove`
```cpp
std::expected<void, CaskFault>
remove(std::span<const std::byte> key, std::uint32_t tstamp = 0);
```
软删除：写一条墓碑 record，空间在下次 merge 时回收。线程安全：否（同 `put`）。

#### `put_doc`
```cpp
std::expected<void, CaskFault>
put_doc(std::span<const std::byte> key, const DocInput& doc,
        std::uint32_t tstamp = 0);
```
写入结构化文档（text + 选填 meta/vector/fields），用于索引模式。线程安全：否。

### 5.4 检索（索引模式）

无 search 层 → `kNoIndex`；无向量配置 → `kInvalidOption`。

#### `search_text`（词袋 BM25）
```cpp
std::expected<TextSearchResult, CaskFault>
search_text(std::string_view query, std::size_t k = 10,
            const meta::MetaFilter* filter = nullptr);
```
`filter` 非空时 meta 后过滤（overfetch `k×4` 再截断到 `k`）。线程安全：否（search_ 非线程安全）。

#### `search_phrase`（短语）
```cpp
std::expected<TextSearchResult, CaskFault>
search_phrase(std::string_view query, std::size_t k = 10);
```
term 连续出现。需 `index_positions=true`。线程安全：否。

#### `bool_search`（布尔）
```cpp
std::expected<TextSearchResult, CaskFault>
bool_search(std::string_view query, std::size_t k = 10);
```
AND / OR / NOT 查询语法。线程安全：否。

#### `search_fields`（多字段）
```cpp
std::expected<TextSearchResult, CaskFault>
search_fields(std::string_view query, std::size_t k = 10);
```
解析 `field:term^boost` 语法：有字段限定的词查对应字段，无限定的查默认字段；各词得分 × boost，跨字段累加。不含字段语法时等价默认字段词袋。线程安全：否。

#### `search_near`（近邻）
```cpp
std::expected<TextSearchResult, CaskFault>
search_near(std::string_view query, std::uint32_t slop, std::size_t k = 10);
```
term 按序出现且相邻间隙 ≤ `slop`；`slop=0` 即短语。线程安全：否。

#### `search_fuzzy`（模糊）
```cpp
std::expected<TextSearchResult, CaskFault>
search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance);
```
Levenshtein 编辑距离匹配。线程安全：否。

#### `search_wildcard`（通配符）
```cpp
std::expected<TextSearchResult, CaskFault>
search_wildcard(std::string_view pattern, std::size_t k);
```
`*` / `?` 模式匹配。线程安全：否。

#### `search_vector`（HNSW 向量 ANN）
```cpp
std::expected<TextSearchResult, CaskFault>
search_vector(std::span<const float> query, std::size_t k = 10,
              std::size_t ef = 0,
              const meta::MetaFilter* filter = nullptr);
```
`query` 长度须 == 配置的 `vector_dim`；`cosine` 配置时内部归一化（零向量返回空命中）；`ef=0` → `max(k,64)`。结果按相似度降序（kDot=内积；kL2=负平方距离），死文档经 live 过滤不出现。
- `filter` 非空时与 `is_live` 组合成 HNSW live callback（无需 overfetch），结果可能少于 `k`。
- **线程安全**：是（HNSW 读路径线程安全）。

#### `search_hybrid`（RRF 混合检索）
```cpp
std::expected<TextSearchResult, CaskFault>
search_hybrid(std::string_view text_query,
              std::span<const float> vec_query, std::size_t k = 10,
              const meta::MetaFilter* filter = nullptr);
```
两路各取 `K'=max(k×4,64)`：BM25 走 `search_text` 内核，向量走 `search_vector` 内核。融合 `score = Σ 1/(60+rank)`，`rank` 从 1 起；平局 → `ord` 小者在前。
- `text_query` 空 → 纯向量；`vec_query` 空 → 纯文本；两路都空 / 无向量配置 / 维度不符 → `kInvalidOption`。
- `filter` 同时作用于两路。返回 score = RRF 分。
- **线程安全**：同两条内核（文本路否，向量路是）。

#### `set_synonym_map`
```cpp
void set_synonym_map(std::unique_ptr<text::SynonymMap> map);
```
设置同义词词典，查询时自动展开。

### 5.5 搜索基础设施访问

```cpp
bool has_search() const;             // 是否启用索引模式
search::SearchLayer* search();       // 访问内部 SearchLayer（高级用法 / C API 层用）
void flush_index();                  // 排空异步索引队列
```

### 5.6 持久化与写文件管理

#### `sync`
```cpp
std::expected<void, CaskFault> sync();
```
fsync active data file。`o_sync` 模式下退化为 no-op。线程安全：否（操作 active_data_，与 put/remove 互斥）。

#### `close_write_file`
```cpp
std::expected<void, CaskFault> close_write_file();
```
finalize 当前 active write file（写 hint trailer、丢句柄、释放 `write.lock`）。Cask 仍可用——下次 put 自动重开。只读/`merge_only` 句柄返回 `kReadOnly`。线程安全：否。

### 5.7 状态与 merge

#### `status`
```cpp
StatusInfo status();
```
线程安全：是（只读 keydir + opts 快照）。

#### `is_empty_estimate`
```cpp
bool is_empty_estimate();
```
O(1) 估算 keydir 是否为空。写过 key 后即使删光也不再回 `true`。线程安全：是。

#### `is_frozen`
```cpp
bool is_frozen();
```
keydir 是否被某 fold/iterator pin 住（影响 pending 表合并时机）。线程安全：是。

#### `needs_merge`
```cpp
NeedsMerge needs_merge(std::uint32_t now_sec = 0);
```
返回是否需要 merge + 候选/过期文件列表。线程安全：是（读 keydir 快照 + 纯函数策略）。

#### `merge`
```cpp
std::expected<merge::MergeStats, CaskFault>
merge(std::vector<std::string> files = {}, std::uint32_t now_sec = 0);
```
`files` 为空时先调 `needs_merge`。caller 负责外部调度/锁。**仅在 `merge_only` 模式下被调用才与 put/remove 并发安全**；read_write Cask 上的并发 merge() 与 put/remove 不兼容。线程安全：是（前提 merge_only + 持 `merge.lock`）。

### 5.8 迭代

#### `make_iter`
```cpp
std::unique_ptr<CaskIter> make_iter();
```
线程安全：是（产生对象）；返回的 CaskIter 本身非线程安全。

#### `dirname` / `keydir` / `options`
```cpp
std::string_view          dirname() const noexcept;
keydir::KeyDir&           keydir()  noexcept;
const CaskOptions&        options() const noexcept;
```

---

## 6. `CaskIter`（快照迭代器）

遍历 `make_iter()` 时刻的全部活跃 `(key, value)`。snapshot 语义靠 `KeyDir::IterHandle` 提供；每条 entry 的 value 在 `next()` 时按需 `pread`。设计为「per-step 一次调用」，便于上层在 scheduler 之间让出。

**线程模型**：CaskIter 自身非线程安全——同一对象只能由一个线程使用；但不同 CaskIter 对象可多线程并发使用同一个 parent Cask。

### `start`
```cpp
std::expected<keydir::StartIterResult, CaskFault>
start(int maxage = -1, int maxputs = -1, std::uint32_t now_sec = 0,
      bool see_tombstones = false);
```
- `see_tombstones=true`：被删除的 key 也作为 entry 出现（`is_tombstone=true`，value 为墓碑标记字节）；`false`（默认）跳过墓碑。
- 返回底层 keydir 的 `StartIterResult`：`kOk`（开始迭代）/ `kOutOfDate`（pending 表 freshness 未过，caller 稍后重试）。
- 线程安全：否。

### `next`
```cpp
struct Entry {
    std::vector<std::byte> key;
    std::vector<std::byte> value;
    std::uint32_t tstamp = 0;
    std::uint32_t file_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t total_sz = 0;
    bool          is_tombstone = false;
    std::uint64_t ord = 0;
};
std::expected<std::optional<Entry>, CaskFault> next();
```
取下一项；end-of-iteration 返回 `nullopt`。线程安全：否。

### `next_batch`
```cpp
std::expected<std::vector<Entry>, CaskFault> next_batch(std::size_t max_n);
```
批量取最多 `max_n` 条。**空 vector = EOI（正常结束）；unexpected = 错误**——两者语义不同。

### `release` / `is_iterating`
```cpp
void release() noexcept;
bool is_iterating() const noexcept;
```
`release` 幂等，线程安全：否。

---

## 7. 检索子系统

`Cask::search()` 返回内部 `SearchLayer` 指针（高级用法或 C API 层访问）。普通用户经 `Cask` 的 `search_*` 方法间接使用。

### 7.1 `search::SearchLayerConfig`

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `analyzer_config` | `text::AnalyzerConfig` | — | 分词器配置 |
| `bm25_params` | `bm25::Bm25Params` | — | BM25 参数（k1/b）|
| `cache_max_entries` | `std::size_t` | `256` | 查询缓存上限；`0`=禁用 |
| `doc_text_cache_max` | `std::size_t` | `1024` | 高亮原文 LRU 上限；`0`=不缓存（高亮降级为无片段）|
| `index_positions` | `bool` | `true` | 是否索引词位置；`false` 时省内存但 `search_phrase`/`search_near` 失效 |
| `vector_dim` | `std::uint16_t` | `0` | `>0` 时构造 HnswIndex |
| `vector_metric` | `meta::VectorMetric` | `kNone` | `kCosineNormalized`/`kDot` → HNSW `kDot`；`kL2` → `kL2` |
| `vector_inmem_int8` | `bool` | `false` | HNSW int8-only 内存（仅 `kDot`）|
| `wal_batch_size` | `std::size_t` | `1` | WAL 批量刷新阈值；`1`=即时；`>1` 减少 sync 次数 |

### 7.2 `search::SearchHit`

```cpp
struct SearchHit {
    std::string   key;
    std::uint64_t ord;
    double        score;
};
```

### 7.3 `vec::HnswConfig`（向量图配置）

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `dim` | `std::uint16_t` | `0` | 向量维度 |
| `metric` | `HnswMetric` | `kDot` | `kDot`=内积（cosine 已在上游归一化）/ `kL2`=平方欧氏 |
| `M` | `std::uint32_t` | `16` | 上层邻居容量；L0 = `2M` |
| `ef_construction` | `std::uint32_t` | `200` | 构建时搜索宽度 |
| `seed` | `std::uint64_t` | `0x5EEDF00D` | 层抽样种子（测试可复现）|
| `inmem_int8` | `bool` | `false` | int8-only 内存模式（仅 `kDot`）|

> HNSW 算法参考：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs"*, TPAMI 2018。设计详见 [`hnsw-design-zh.md`](hnsw-design-zh.md)。

### 7.4 `text::AnalyzerType` 与 `AnalyzerConfig`

| `AnalyzerType` | 含义 |
|----------------|------|
| `Ngram` | CJK 字符级 n-gram + 拉丁空白切分（默认）|
| `Whitespace` | 纯空白切分（调试 / 纯拉丁）|
| `Jieba` | jieba 词典分词 + CutForSearch + CJK 回退 n-gram |

`AnalyzerConfig` 字段：

| 字段 | 类型 | 默认 | 含义 |
|------|------|------|------|
| `type` | `AnalyzerType` | `Ngram` | 分词器类型 |
| `min_n` | `std::uint32_t` | `2` | Ngram 最小 n |
| `max_n` | `std::uint32_t` | `3` | Ngram 最大 n |
| `enable_stop_words` | `bool` | `false` | 启用停用词过滤 |
| `stop_words` | `std::vector<std::string>` | `{}` | 自定义停用词表（空则用内置默认）|
| `dict_path` | `std::string` | `""` | jieba 词典目录（必须有效）|
| `min_token_length` | `std::uint32_t` | `1` | 拉丁整词最小 codepoint 长度；`1`=不过滤 |
| `enable_stemming` | `bool` | `false` | 启用 Porter 词干提取（`running`→`run`）|

---

## 8. 用法示例

### 8.1 KV 模式：增删改查

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

    (*c)->put(key, val, /*tstamp=*/0);       // 写

    if (auto r = (*c)->get_owned(key)) {     // 读（owned）
        // r->value, r->tstamp, r->ord
    }

    (*c)->remove(key);                       // 软删

    (*c)->close();
}
```

### 8.2 索引模式：BM25 + 向量 + 混合检索

```cpp
#include <bitcask/cask.hpp>
#include <bitcask/search_layer.hpp>
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

### 8.3 CaskIter 迭代

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

---

## 9. 线程模型汇总

| 操作 | 线程安全 | 说明 |
|------|---------|------|
| `open` / `close` / `upgrade` | 是（产生独立对象）| close 要求 caller 保证无在途调用 |
| `get` / `get_owned` / `read_handle_count` | 是 | 读路径无锁；pread thread-safe |
| `put` / `remove` / `put_doc` / `sync` / `close_write_file` | **否** | 单写者；caller 串行化 |
| `search_text` / `_phrase` / `_bool` / `_fields` / `_near` / `_fuzzy` / `_wildcard` | 否 | search_ 非线程安全 |
| `search_vector` | 是 | HNSW 读路径线程安全 |
| `search_hybrid` | 否（文本路）/ 是（向量路）| 取决于两路内核 |
| `set_synonym_map` | 否 | 写 search_ |
| `status` / `is_empty_estimate` / `is_frozen` / `needs_merge` | 是 | 只读快照 |
| `merge` | 是（前提 `merge_only` + 持 `merge.lock`）| read_write Cask 上不可与 put 并发 |
| `CaskIter::start` / `next` / `next_batch` / `release` | 否（同一对象）| 不同 CaskIter 对象可并发 |

> 并发 merge 与 live writer 通过双锁模型（`write.lock` / `merge.lock`）并行不互斥。详见 [`concurrency-zh.md`](concurrency-zh.md)。
