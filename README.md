# libbitcask

高性能嵌入式存储引擎，以 C++23 实现 Bitcask 追加日志 KV，并在其之上叠加 **BM25 全文检索**、**HNSW 向量检索** 与 **RRF 混合检索**。同一套核心既暴露为 C++23 库，也以 `extern "C"` ABI 编译为 `libbitcask.so`，供 Python / Rust / Go / Node 等跨语言 FFI 绑定使用。

- **KV 模式**：append-only 数据文件 + 内存 keydir，`get` 走 O(1) keydir + 单次 `pread` 读值
- **索引模式**：在 KV 之上叠加 BM25 倒排、HNSW 图、字段索引，支持文本 / 向量 / 混合检索
- **C++23**，无 Boost / abseil 依赖；第三方以 git submodule 形式 vendored 在 `third_party/`，构建无需联网
- **Apache 2.0** 协议

变更历史见 [`CHANGELOG.md`](CHANGELOG.md)。

---

## 功能一览

| 能力 | 接口（`bitcask::Cask`） | 说明 | 文档 |
|------|------------------------|------|------|
| KV 读写 | `put` / `get` / `remove` / `put_batch` | DocValue v4 编码（u64 tstamp），纯 KV 的 binary 走 text 段 | [`api-cpp.md`](doc/api-cpp.md) |
| 零拷贝读 | `get`（`GetResultView`）/ `get_owned` | view 借 `pread` 缓冲或 sealed mmap，无堆分配 | [`getresult-view-design-zh.md`](doc/getresult-view-design-zh.md) |
| 结构化文档 | `put_doc`（`DocInput`） | text + 可选 meta + 可选 vector + 多字段 | [`api-cpp.md`](doc/api-cpp.md) |
| 词袋检索 | `search_text` | BM25 + meta 过滤，支持 `offset` 分页 | [`api-cpp.md`](doc/api-cpp.md) |
| 短语 / 近邻 | `search_phrase` / `search_near` | 词位置感知 | [`api-cpp.md`](doc/api-cpp.md) |
| 布尔检索 | `bool_search` | AND / OR / NOT 查询语法 | [`api-cpp.md`](doc/api-cpp.md) |
| 多字段 | `search_fields` | `field:term^boost` 语法 | [`api-cpp.md`](doc/api-cpp.md) |
| 模糊 / 通配符 | `search_fuzzy` / `search_wildcard` | Levenshtein / `*?` 模式 | [`api-cpp.md`](doc/api-cpp.md) |
| 高亮 | `search_text_highlight` | 命中片段截取 | [`api-cpp.md`](doc/api-cpp.md) |
| 批量检索 | `search_text_batch` / `search_vector_batch` / `search_hybrid_batch` | 多条独立查询并发跑共享 Search 池（inter-query 并行），保序返回 | [`api-cpp.md`](doc/api-cpp.md) |
| 向量 ANN | `search_vector` | 三引擎可选：`hnsw`（默认，内存档）/ `ivfrq`（IVF 磁盘段，10M-100M 推荐）/ `diskann`（Vamana 图，实验性）；HNSW 支持 int8 量化与 int8-only 内存 | [`hnsw-overview-zh.md`](doc/hnsw-overview-zh.md) / [`vector-dual-engine-selection-zh.md`](doc/vector-dual-engine-selection-zh.md) |
| 向量引擎迁移 | `vec_engine_migrate`（CLI） | 离线切换向量引擎（只改 meta，首次 open 全量 fold 重建，可回滚） | [`vector-dual-engine-selection-zh.md`](doc/vector-dual-engine-selection-zh.md) |
| 混合检索 | `search_hybrid` | BM25 + 向量 RRF(60) 融合 | [`hybrid_searcher.hpp`](include/bitcask/hybrid_searcher.hpp) |
| 同义词 | `CaskOptions::synonym_map`（open-time） | 查询时自动展开，不可变、并发安全 | [`synonym_map.hpp`](include/bitcask/synonym_map.hpp) |
| 迭代 | `make_iter` | MVCC 快照（兄弟链 + pending 哈希） | [`keydir-sharding-design-zh.md`](doc/keydir-sharding-design-zh.md) |
| **有序 range 扫描** | `make_range_iter`（`RangeOptions{lo,hi,prefetch}`） | OKI 有序 key 索引：`[lo,hi)` 字典序遍历，**O(range)** 而非 O(全表)（实测 1/256 选择性 8.0 ms → 0.53 ms）；可选值预取 | [`ordered-key-index-design-zh.md`](doc/ordered-key-index-design-zh.md) |
| 并行扫描 | `parallel_scan` | 多线程全表扫描（快照 key → 分段并发 get；支持 key 前缀过滤） | [`api-cpp.md`](doc/api-cpp.md) |
| 合并 | `merge` / `needs_merge` | 与读写并发的独立 `merge.lock` 模型 | [`merge-policy-zh.md`](doc/merge-policy-zh.md) |
| 备份 | 文件级拷贝 + `flush_index` | WAL 一致点落盘 | [`wal-batch-design-zh.md`](doc/wal-batch-design-zh.md) |
| 升级 | `Cask::upgrade` | 离线把 KV 目录升为索引模式 | [`api-cpp.md`](doc/api-cpp.md) |
| 迁移 | `bitcask_migrate`（CLI） | 统一纪元迁移：`detect` / `be2le`（v1 大端）/ `tstamp64`（u32 → u64，5.0 flag-day）/ `hintord`（hint 补 ord，6.0 flag-day，data 零改动）；非破坏性 | [`migrate-le.md`](doc/migrate-le.md) |
| 状态 | `status` / `read_handle_count` | 内省 key 数 / fd 预算 / 索引错误计数 | [`api-cpp.md`](doc/api-cpp.md) |
| C ABI | `libbitcask.so` | `extern "C"` 不透明句柄 + slice + fault，跨 ABI 稳定 | [`api-c.md`](doc/api-c.md) |

---

## 线程安全模型摘要

**同一个 `bitcask::Cask` 句柄可被多线程安全共享**，无需每线程一个实例。本节给出顶层契约；锁层、不变量与各接口的同步原语见 [`doc/concurrency-zh.md`](doc/concurrency-zh.md) 与内部审计 [`docs/design/thread-safety.md`](docs/design/thread-safety.md)。

| 操作 | 并发语义 |
|------|----------|
| **读**（`get` / `search_*` / 批量检索） | 真并发（无锁 / `shared_lock`） |
| **写**（`put` / `remove` / `put_doc` / `sync` / `put_batch`） | 多线程安全。内部 `write_mu_` 串行化——单 append WAL 在文件层本就串行，锁不损吞吐；多写**安全但不提速** |
| **读写并发** | 安全；搜索可见性 near-real-time |
| **`merge`** | 与读写并发：`write.lock` 与 `merge.lock` 两把独立文件锁，merger 通过 `read()` write.lock 内容排除 live writer 的活动文件 |
| **`parallel_scan`** | 内部多线程并发 `get`；调用方提供的 `ScanFn` 必须线程安全 |
| **同义词词典** | open-time 不可变（`CaskOptions::synonym_map`）——无运行期 setter，无竞态 |
| **`CaskIter`** | 不可跨线程共享，每线程一个；需要并行遍历用 `parallel_scan` |
| **`close`** | 生命周期方法，须无在途调用。close 后新发起的调用 fail-fast 返回 `CaskError::kClosed`（不崩溃） |
| 跨进程 | `bitcask.write.lock` / `bitcask.merge.lock`（`O_CREAT\|O_EXCL`，stale 锁按 PID 探活回收） |

需要更高写并发 → **按目录分片多个 `Cask` 实例**（单 append WAL 的横向扩展）。

---

## 快速开始

### C++：KV 模式

```cpp
#include <bitcask/cask.hpp>

using bitcask::Cask, bitcask::CaskOptions;
using bitcask::keydir::KeyDirRegistry;

// registry 强制非空：管理同目录 Cask 间的共享 keydir（典型：每进程一个）。
KeyDirRegistry registry;
auto c = Cask::open("/tmp/db", CaskOptions{.read_write = true}, &registry);
if (!c) {
    fprintf(stderr, "open failed: %s\n", c.error().detail.c_str());
    return 1;
}

const std::vector<std::byte> key{std::byte{'h'}, std::byte{'i'}};
const std::vector<std::byte> val{std::byte{'w'}, std::byte{'o'}};

(*c)->put(key, val, /*tstamp=*/0);
auto r = (*c)->get_owned(key);
if (r) {
    // r->value == val
}

(*c)->close();
```

### C++：索引模式（BM25 + 向量 + 混合检索）

```cpp
using namespace bitcask;
using namespace bitcask::search;
using namespace bitcask::text;

CaskOptions opts;
opts.read_write    = true;
opts.enable_search = true;
opts.search_config = SearchLayerConfig{
    .analyzer_config = AnalyzerConfig{.type = AnalyzerType::Ngram,
                                      .min_n = 2, .max_n = 3},
};
opts.vector_dim    = 128;                                       // 启用向量
opts.vector_metric = meta::VectorMetric::kCosineNormalized;     // 写入端归一化

KeyDirRegistry registry;
auto c = Cask::open("/tmp/db", opts, &registry);
if (!c || !(*c)->has_search()) { return 1; }

const std::vector<std::byte> key{std::byte{'d'}, std::byte{'1'}};
DocInput doc{
    .text   = std::as_bytes(std::span{"北京今天天气晴朗"}),
    .vector = query_vec,   // 128 维
};

(*c)->put_doc(key, doc);
(*c)->flush_index();       // 等待异步索引排空（put 本身已持久化）

// 词袋检索
auto bm25 = (*c)->search_text("北京 天气", 10);
// 向量检索
auto knn  = (*c)->search_vector(query_vec, /*k=*/10);
// RRF 混合
auto hyb  = (*c)->search_hybrid("北京 天气", query_vec, /*k=*/10);
```

新代码推荐类型化门面 `bitcask::text::Searcher` / `bitcask::vec::Searcher`（`include/bitcask/searcher.hpp`），每次查询前自动经 `drain_plugins()` 读屏障；`Cask::search_*` 系列保留为兼容薄委托。

### C API（跨语言 FFI）

```c
#include "bitcask_c.h"

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
bitcask_put(cask, key, val, 0, NULL);

bitcask_get_result_t* res = NULL;
if (bitcask_get(cask, key, &res, NULL) == BITCASK_OK) {
    printf("value: %.*s\n", (int)res->value.size, (char*)res->value.data);
    bitcask_get_result_free(res);
}
bitcask_close(cask);
```

C API 设计要点：不透明句柄、显式 `*_free` 配对、错误码 + `bitcask_fault_t` 详情、二进制安全 `{data, size}` 切片、C++ 异常隔离。线程模型与 C++ 核心一致——同一 handle 多线程安全（读并发无锁、写由内部 `write_mu_` 串行化）。完整符号表见 [`doc/api-c.md`](doc/api-c.md)。

---

## 构建依赖

### 系统依赖

| 依赖 | 版本 | 说明 | Debian/Ubuntu 包 |
|------|------|------|------------------|
| C++ 编译器 | GCC 13+ / Clang 17+ | 需要 C++23 支持 | `gcc` `g++` |
| CMake | ≥ 3.20 | 构建系统 | `cmake` |
| ZLIB | — | CRC32 / 数据压缩 | `zlib1g-dev` |
| oneTBB | — | 并发容器；普通构建用系统包 | `libtbb-dev` |

> 普通构建用系统的 `libtbb`（`find_package(TBB)`）；仅 TSan 构建会改用 `third_party/oneTBB` 源码编译插桩版——系统 libtbb 未插桩，TSan 下会漏报/误报。

### Vendored 依赖（git submodule）

`.gitmodules` 全部条目位于 `third_party/`；首次 clone 后无需手动安装，构建无需联网。

| submodule | 来源 | 用途 |
|-----------|------|------|
| `third_party/utf8proc` | https://github.com/JuliaStrings/utf8proc | Unicode NFKC 归一化 + case fold |
| `third_party/cppjieba` | https://github.com/yanyiwu/cppjieba | 中文分词 |
| `third_party/limonp` | https://github.com/yanyiwu/limonp | cppjieba 的 header-only 依赖 |
| `third_party/googletest` | https://github.com/google/googletest | 测试（`BUILD_TESTING=ON` 时编） |
| `third_party/benchmark` | https://github.com/google/benchmark | 微基准（`BITCASK_BUILD_BENCHMARKS=ON` 时编） |
| `third_party/oneTBB` | https://github.com/uxlfoundation/oneTBB | TSan 插桩版（`BITCASK_SANITIZE=thread` 时编） |
| `third_party/unordered_dense` | https://github.com/martinus/unordered_dense | KeyDir 分片表的稠密哈希表（header-only INTERFACE 库） |

### 获取代码与构建

```bash
# 首次 clone —— 带 submodule
git clone --recurse-submodules <repo-url>
# 已 clone 的仓库补拉 submodule
git submodule update --init --recursive
```

```bash
# Release 构建（含 LTO / -falign-functions=64 / _FORTIFY_SOURCE=2 / Full RELRO）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 测试 + 基准
cmake -S . -B build -DBUILD_TESTING=ON -DBITCASK_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Sanitizers

```bash
# ASan + UBSan（ASan 与 TSan 互斥，一次设置一种）
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=address,undefined -DBUILD_TESTING=ON

# TSan 构建改用 third_party/oneTBB 源码编译插桩版（系统 libtbb 未插桩）
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=thread -DBUILD_TESTING=ON
```

### `-Werror` 库构建

默认关闭以避免新编译器新告警破坏下游；开启时只对 first-party 库目标生效，third_party 头（cppjieba / limonp）已标 SYSTEM 不受影响。CI `werror-lib` job 开启（GCC 13 + Release + 只建 `bitcask_static` / `bitcask_shared`）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DBITCASK_WERROR=ON -DBUILD_TESTING=OFF
cmake --build build -j --target bitcask_static bitcask_shared
```

### 主要 CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTING` | OFF | 编测试二进制 + ctest（CMake 内建开关） |
| `BITCASK_BUILD_BENCHMARKS` | OFF | 编 Google Benchmark 微基准 |
| `BITCASK_SANITIZE` | 空 | `address,undefined` / `thread`——ASan 与 TSan 互斥 |
| `BITCASK_WERROR` | OFF | first-party 告警升错（CI `werror-lib` 开启） |
| `BITCASK_NATIVE` | OFF | 为本机现编启用 `-march=native`（破坏二进制可移植性，勿与 `-ffast-math` 混用） |
| `BITCASK_LTO` | ON | Release 启用 LTO / IPO；sanitizer 构建自动关闭 |
| `BITCASK_PCH` | ON | 预编译头加速编译；排查 PCH 异常可临时关闭 |

版本信息由 `CMakeLists.txt` 的 `project(libbitcask VERSION 5.0.0)` 单一真源派生：`libbitcask.so` 的 `SOVERSION=5`，`VERSION=5.0.0`，C 端 `bitcask_version_{major,minor,patch,string}()` 同步。

### 产物

- `libbitcask.so` — 共享库，导出 C API（`extern "C"`，`SOVERSION=5`）
- `libbitcask.a` — 把全部静态归档合并为单一 `.a`
- `bitcask_migrate` — 统一纪元迁移入口：`detect` / `be2le`（v1 大端 → 当前）/ `tstamp64`（u32 → u64，5.0 flag-day）
- `migrate_le` — v1 大端 → 当前纪元（旧入口，等价于 `bitcask_migrate be2le`）
- `vec_engine_migrate` — HNSW / IVF-RaBitQ / DiskANN 离线引擎切换（S32-M4）
- `gen_inert_table` — NFKC 惰性区间表代码生成器（构建期自动执行）

### 安装

```bash
cmake --install build   # 头文件、libbitcask.{so,a}、bitcask_c.h
```

---

## 架构概览

```
┌─────────────────────────────────────────────────────────────────────┐
│  C API（c_api/bitcask_kv.{h,cpp} + bitcask_text + bitcask_vec +     │
│        bitcask_c.h 聚合头 + internal.h 共享助手）                  │
│  libbitcask.so：extern "C" 不透明句柄 + slice + fault（PIMPL）      │
└────────────────────────────┬────────────────────────────────────────┘
                               │ PIMPL：持 bitcask::Cask
┌────────────────────────────▼────────────────────────────────────────┐
│  Cask（KV + 搜索门面）                                              │
│  include/bitcask/cask.hpp（Cask / CaskOptions / CaskFault /          │
│  CaskIter / GetResult[View] / DocInput / StatusInfo / …）           │
│  src/cask/{cask,cask_iter,cask_search,cask_recovery}.cpp + meta_file│
│                                                                     │
│  ├─ KeyDir（256 分片 shared_mutex + MVCC 迭代器）                   │
│  ├─ DataFile 缓存（pread 句柄 + 近似 LRU 淘汰）                    │
│  ├─ HintFile（活跃写入器 + BCH5 trailer CRC + sealed-mmap hint）   │
│  ├─ OkiState（有序 key 索引：memdelta + BCOK run + BCOM manifest）  │
│  ├─ DocMap（Index：ord↔ext/live/meta 宿主服务；查询面 DocTable）   │
│  ├─ TextPlugin "bm25"（倒排/Analyzer/缓存/高亮/bm25.ckpt 文件族）  │
│  ├─ VectorPlugin "hnsw"（VectorEnginePlugin 契约：HNSW/IVF-RaBitQ/DiskANN │
│  │   三引擎按 meta.vector_engine 选定；归一化/vec.ckpt 族 + .vec/.qc8 侧车）│
│  ├─ HybridSearcher（RRF 融合器；持两插件引用）                     │
│  ├─ CaskPluginHost（read_at / run_serialized / log 窄反向接口）    │
│  ├─ MetaConfig（bitcask.meta v5：magic + version + CRC32 + 纪元门禁）│
│  └─ IndexPool（异步索引 MapReduce，借自 KeyDirRegistry）           │
└────────────────────────────┬────────────────────────────────────────┘
                               │
┌────────────────────────────▼────────────────────────────────────────┐
│  查询门面（推荐新代码使用）                                        │
│  include/bitcask/searcher.hpp                                      │
│  text::Searcher / vec::Searcher / search::CaskHybridSearcher       │
│  （每次查询先 drain_plugins() 读屏障 → 直调插件内核）              │
└────────────────────────────┬────────────────────────────────────────┘
                               │
┌────────────────────────────▼────────────────────────────────────────┐
│  基础实现层                                                         │
│  fileops        codec / data_file / hint_file / scanner / migrate  │
│  io / lock      PosixFile / FileLock（O_EXCL 仲裁）                 │
│  merge          纯函数策略 + Merger（CAS 重定位 + 插件广播）       │
│  bm25           InvertedIndex + WAND + k-way 交集 + 查询 AST        │
│  text           Analyzer（Ngram / Jieba / Whitespace / Stemming）   │
│  vector         HnswIndex（HNSW 图 + int8 量化 + mmap payload）    │
│  util           CRC32 / 高亮 / 同义词 / 模糊匹配 / Myers / 缓存    │
└─────────────────────────────────────────────────────────────────────┘
```

关键设计要点：

- **双持久化**：数据文件（append-only，KV 权威）+ per-component base + delta ckpt + manifest commit（BM25/HNSW 的派生缓存，校验失败回退全量 fold）。详见 [`recovery-unified-checkpoint-design-zh.md`](doc/recovery-unified-checkpoint-design-zh.md)。
- **双锁模型**：`bitcask.write.lock`（writer）与 `bitcask.merge.lock`（merger）独立，周期 merge 与 live writer 并行不互斥。merger 通过读取 `write.lock` 内容排除 live writer 的活动文件。
- **异步索引 MapReduce**：`put_doc` 入队有界 `IndexPool`（满则 push 阻塞做背压）→ N 个 map worker 并行分词（`hardware_concurrency` 真数据并行）→ per-lane reorder buffer（按 ord 排序）→ 单 reducer 串行 apply（库内单写者）。池由 `KeyDirRegistry` 共享，线程数 = N+1 与库数无关。详见 [`docs/design/async-index-pipeline.md`](docs/design/async-index-pipeline.md)。
- **查询并发**：批量查询接口（`search_*_batch`）在进程级共享 `search_arena`（TBB `task_arena`）上 inter-query 并行；单查询内部仍串行（WAND 顺序依赖、HNSW 图遍历）。
- **向量双引擎**（S32）：`vector_engine` 建库时一次性选定并持久化进 `bitcask.meta`——`hnsw`（内存图，≤数 M 向量）/ `ivfrq`（IVF-RaBitQ 磁盘段，10M-100M 推荐）/ `diskann`（Vamana 图，实验性）。引擎不符重开 → `kModeMismatch`；离线切换用 `vec_engine_migrate`（只改 meta，首次 open 全量 fold 重建，可回滚）。详见 [`vector-dual-engine-selection-zh.md`](doc/vector-dual-engine-selection-zh.md)。
- **小端 only**：所有多字节整数小端（LE 主机原生零转换）；不再与 legacy 大端 Erlang 字节互通，迁移用 `bitcask_migrate be2le`（旧名 `migrate_le`，见 [`migrate-le.md`](doc/migrate-le.md)）。
- **64 位时间戳（5.0 flag-day）**：`tstamp` / `expiry_at` 全链路扩为 `uint64_t`（C/C++ API + 盘上 record header 23B→27B + DocValue v4 + hint v4 + keydir 快照 v3 + docmap sidecar v3）。`bitcask.meta` v4 门禁**干净拒开** u32 纪元旧库（v1/v2/v3），提示 `rebuild — re-ingest data`；u32 纪元库可经 `bitcask_migrate tstamp64` **非破坏性离线迁移**到当前纪元（无需 re-ingest）。过期判定（`tstamp + expiry_secs`、`expiry_at` 与 `now` 比较）一律 u64 域算术，杜绝 u32 wrap 误判（极端 `expiry_secs ≈ 2^32` 此前会把全部 key 误判为已过期）。SONAME `libbitcask.so.4 → .so.5`，旧二进制由链接器层隔离。
- **插件化**：`CaskPlugin` 接口（`plugin_api.hpp`）是 KV 存储层与索引层的唯一契约，Cask 在写/恢复/merge/checkpoint 四条通路上向注册的插件广播事件。Text/Vector 是当前两个内建插件；新增插件（TTL、metrics、CDC）只需实现此接口。详见 [`plugin-arch-split-design-zh.md`](doc/plugin-arch-split-design-zh.md)。

---

## 目录结构

```
.
├── include/bitcask/   # 公共头文件（API 接口）
├── c_api/             # libbitcask.so 的 C ABI（bitcask_kv / text / vec + 聚合 bitcask_c.h）
├── src/               # 实现：fileops / io / lock / keydir / merge /
│                      #       cask / search / bm25 / text / vector
├── tests/             # GoogleTest 单元 + 集成测试（35 个测试二进制）
├── bench/             # Google Benchmark（keydir / cask / inverted / hnsw …）
├── tools/             # bitcask_migrate（统一）、migrate_le（旧）、vec_engine_migrate、gen_inert_table
├── cmake/             # BitcaskSanitizers 模块 + tsan.supp
├── third_party/       # 第三方依赖（git submodule，见「构建依赖」）
├── doc/               # 架构 / 格式 / API 参考 / 设计文档
└── docs/design/       # 设计稿（线程安全审计、异步索引流水线）
```

---

## 文档索引

### 用户向参考

| 文档 | 说明 |
|------|------|
| [`doc/api-cpp.md`](doc/api-cpp.md) | C++ API 参考（`bitcask::Cask` / `CaskOptions` / 全部方法） |
| [`doc/api-c.md`](doc/api-c.md) | C API 参考 / FFI 绑定（不透明句柄 + slice + fault） |
| [`doc/cpp-arch.md`](doc/cpp-arch.md) | C++ 代码库架构与 CMake target 表 |
| [`doc/format-zh.md`](doc/format-zh.md) | 字节级磁盘格式真源（record / hint / DocValue / ckpt / meta） |
| [`doc/concurrency-zh.md`](doc/concurrency-zh.md) | 并发契约用户向说明（锁层、不变量、可见性） |
| [`doc/migrate-le.md`](doc/migrate-le.md) | 大端 → 小端目录离线迁移工具（`migrate_le`） |

### 设计文档

| 文档 | 说明 |
|------|------|
| [`docs/design/thread-safety.md`](docs/design/thread-safety.md) | 内部线程安全审计（as-built 状态记录） |
| [`docs/design/async-index-pipeline.md`](docs/design/async-index-pipeline.md) | 异步索引 MapReduce 流水线设计稿（S6） |
| [`docs/design/s13-review-2026-07-02.md`](docs/design/s13-review-2026-07-02.md) | S12/S13 批次审查纪要 |
| [`docs/design/s27-3-b2b-recovery-design.md`](docs/design/s27-3-b2b-recovery-design.md) | S27-3 B2B 恢复设计 |
| [`docs/design/s27-4-dwpt-design.md`](docs/design/s27-4-dwpt-design.md) | S27-4 DWPT 并行 builder 设计 |
| [`docs/design/s29-6-keydir-lockfree-read.md`](docs/design/s29-6-keydir-lockfree-read.md) | S29-6 KeyDir 无锁读设计 |
| [`docs/design/s29-6b-inverted-term-cache.md`](docs/design/s29-6b-inverted-term-cache.md) | S29-6B 倒排 term 缓存设计 |
| [`docs/design/s30-mmap-segments.md`](docs/design/s30-mmap-segments.md) | S30 mmap 段设计 |

### HNSW / 向量

| 文档 | 说明 |
|------|------|
| [`doc/vector-db-design-zh.md`](doc/vector-db-design-zh.md) | 向量库设计：在 Bitcask 上原生扩展 |
| [`doc/vector-db-ann-landscape-zh.md`](doc/vector-db-ann-landscape-zh.md) | ANN 算法全景与 HNSW 定位 |
| [`doc/vector-dual-engine-selection-zh.md`](doc/vector-dual-engine-selection-zh.md) | S32 向量双引擎选择（HNSW / IVF-RaBitQ / DiskANN 定位与切换） |
| [`doc/hnsw-overview-zh.md`](doc/hnsw-overview-zh.md) | HNSW 算法全景与本实现优化点 |
| [`doc/hnsw-design-zh.md`](doc/hnsw-design-zh.md) | HNSW V3 设计定稿（并发协议、持久化） |
| [`doc/hnsw-graph-theory-zh.md`](doc/hnsw-graph-theory-zh.md) | HNSW 多层图原理与量化必要性 |
| [`doc/hnsw-graph-construction-zh.md`](doc/hnsw-graph-construction-zh.md) | 邻接表怎么"算"出来的（建图算法） |
| [`doc/hnsw-lifecycle-zh.md`](doc/hnsw-lifecycle-zh.md) | HNSW 图生命周期：构建、持久化、恢复 |
| [`doc/hnsw-memory-footprint-zh.md`](doc/hnsw-memory-footprint-zh.md) | HNSW 内存占用分析（按 V7 重新推导） |
| [`doc/hnsw-int8-only-design-zh.md`](doc/hnsw-int8-only-design-zh.md) | P5 int8-only 内存模式设计 |
| [`doc/int8-vnni-v4-zh.md`](doc/int8-vnni-v4-zh.md) | int8 量化 + AVX-VNNI 距离内核 |
| [`doc/simd-vnni-internals-zh.md`](doc/simd-vnni-internals-zh.md) | VNNI dpbusd 与偏置补偿指令内幕 |
| [`doc/vector-ondisk-quant-design-zh.md`](doc/vector-ondisk-quant-design-zh.md) | 向量落盘 int8 量化设计 |
| [`doc/s29-11-hnsw-deep-opt-design-zh.md`](doc/s29-11-hnsw-deep-opt-design-zh.md) | S29-11 HNSW 深度优化（AVX2 int8 内核 + 混合精度建图导航） |
| [`doc/rabitq-theory-zh.md`](doc/rabitq-theory-zh.md) | RaBitQ 量化理论（无偏估计、误差界、lite→full 升级路径） |

### BM25 / 倒排

| 文档 | 说明 |
|------|------|
| [`doc/posting-zero-copy-design-zh.md`](doc/posting-zero-copy-design-zh.md) | PostingList 数据布局与零拷贝 |
| [`doc/inoue-simd-intersection-zh.md`](doc/inoue-simd-intersection-zh.md) | Inoue 块过滤 + SIMD 精确匹配 |
| [`doc/intersect-kernel-internals-zh.md`](doc/intersect-kernel-internals-zh.md) | 旋转法交集内核内幕与输出段微优化 |
| [`doc/kway-blockmax-bmw-zh.md`](doc/kway-blockmax-bmw-zh.md) | k-way 交集 + 块级元数据 + BMW |
| [`doc/wand-blockmax-zh.md`](doc/wand-blockmax-zh.md) | WAND / BlockMax-WAND top-k 动态剪枝 |
| [`doc/segment-index-design-zh.md`](doc/segment-index-design-zh.md) | 封口段格式设计（v2 mmap 零驻留 / v1 全量回退） |
| [`doc/roaring-meta-bitmap-design-zh.md`](doc/roaring-meta-bitmap-design-zh.md) | Roaring bitmap 加速 meta 过滤设计 |

### KeyDir / 恢复 / Merge / 插件

| 文档 | 说明 |
|------|------|
| [`doc/keydir-sharding-design-zh.md`](doc/keydir-sharding-design-zh.md) | KeyDir 分片与 MVCC fold 设计 |
| [`doc/getresult-view-design-zh.md`](doc/getresult-view-design-zh.md) | `GetResultView` 零拷贝设计 |
| [`doc/recovery-unified-checkpoint-design-zh.md`](doc/recovery-unified-checkpoint-design-zh.md) | 恢复持久化：分段 ckpt + manifest commit |
| [`doc/merge-policy-zh.md`](doc/merge-policy-zh.md) | Merge 触发策略（碎片率 / 死字节 / 过期阈值） |
| [`doc/ord-recycling-design-zh.md`](doc/ord-recycling-design-zh.md) | ord 回收复用可行性与方案 |
| [`doc/plugin-arch-split-design-zh.md`](doc/plugin-arch-split-design-zh.md) | 插件化架构（SearchLayer 拆分后） |
| [`doc/read-handle-lru-design-zh.md`](doc/read-handle-lru-design-zh.md) | ReadHandle LRU 缓存设计 |
| [`doc/sealed-mmap-read-design-zh.md`](doc/sealed-mmap-read-design-zh.md) | Sealed DataFile mmap 只读路径 |
| [`doc/put-flow-zh.md`](doc/put-flow-zh.md) | `put(K, V)` 完整调用链（C API → 字节落定 → 索引入队） |
| [`doc/wal-batch-design-zh.md`](doc/wal-batch-design-zh.md) | 批量 flush 设计（当前代码版） |

---

## 许可证

[Apache License 2.0](LICENSE)。