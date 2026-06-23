# libbitcask

高性能嵌入式存储引擎，在 Bitcask 追加日志 KV 之上集成 **BM25 全文检索**、**HNSW 向量检索** 与 **RRF 混合检索**，并提供跨语言 ABI 的 **C API**。

- **KV 模式**：append-only 数据文件 + 内存 keydir，O(1) `get` / `put`，单次 `pread` 读值
- **索引模式**：在 KV 之上叠加 BM25 倒排、HNSW 图、字段索引，支持向量/文本/混合检索
- **C++23**，无 Boost / abseil 依赖；第三方库以 git submodule 形式 vendored 在 `third_party/`（构建无需联网）
- **Apache 2.0** 协议

---

## 功能一览

| 能力 | 接口（`bitcask::Cask`） | 说明 |
|------|------------------------|------|
| KV 读写 | `put` / `get` / `remove` | DocValue v3 编码，纯 KV 的 binary 走 text 段 |
| 结构化文档 | `put_doc` | text + 可选 meta + 向量 + 多字段 |
| 词袋检索 | `search_text` | BM25 + meta 过滤 |
| 短语/近邻 | `search_phrase` / `search_near` | 词位置感知（需开启 `index_positions`）|
| 布尔检索 | `bool_search` | AND / OR / NOT 查询语法 |
| 多字段 | `search_fields` | `field:term^boost` 语法 |
| 模糊 / 通配符 | `search_fuzzy` / `search_wildcard` | 编辑距离 / `*?` 模式 |
| 向量 ANN | `search_vector` | HNSW，cosine / dot / L2 度量 |
| 混合检索 | `search_hybrid` | BM25 + 向量 RRF 融合 |
| 同义词 | `set_synonym_map` | 查询时自动展开 |
| 高亮 | `search_text_highlight` | 命中片段截取 |
| 迭代 | `make_iter` | MVCC 快照（兄弟链 + pending 哈希）|
| 维护 | `merge` / `needs_merge` / `status` | 并发 merge（不阻塞 writer）|

---

## 快速开始

### 构建依赖

**系统依赖**（需自行安装）：

| 依赖 | 版本 | 说明 | Debian/Ubuntu 包 |
|------|------|------|------------------|
| C++ 编译器 | GCC 13+ / Clang 17+ | 需 C++23 支持 | `gcc` `g++` |
| CMake | ≥ 3.20 | 构建系统 | `cmake` |
| ZLIB | — | CRC32 / 数据压缩 | `zlib1g-dev` |
| oneTBB | — | 并发容器；普通构建用系统包 | `libtbb-dev` |

> 普通构建用系统的 `libtbb`（`find_package(TBB)`）；仅 TSan 构建会改用 `third_party/oneTBB` 源码编译插桩版——系统 libtbb 未插桩，TSan 下会漏报/误报。

**Vendored 依赖**（git submodule，位于 `third_party/`，clone 后无需手动安装，构建无需联网）：

| submodule | 版本 | 来源 | 用途 |
|-----------|------|------|------|
| `third_party/utf8proc` | v2.10.0 | https://github.com/JuliaStrings/utf8proc | Unicode NFKC 归一化 |
| `third_party/cppjieba` | v5.6.7 | https://github.com/yanyiwu/cppjieba | 中文分词 |
| `third_party/limonp` | v1.0.2 | https://github.com/yanyiwu/limonp | cppjieba 的 header-only 依赖 |
| `third_party/googletest` | v1.15.2 | https://github.com/google/googletest | 测试（仅 `BUILD_TESTING=ON`）|
| `third_party/benchmark` | v1.9.0 | https://github.com/google/benchmark | 微基准（仅 `BITCASK_BUILD_BENCHMARKS=ON`）|
| `third_party/oneTBB` | v2022.0.0 | https://github.com/uxlfoundation/oneTBB | TSan 插桩版（仅 `-DBITCASK_SANITIZE=thread`）|
| `third_party/unordered_dense` | v4.8.1 | https://github.com/martinus/unordered_dense | KeyDir 分片表的稠密哈希表（header-only INTERFACE 库）|

### 获取代码与构建

```bash
# 首次 clone —— 带 submodule（.gitmodules 已配 shallow=true，浅克隆）
git clone --recurse-submodules <repo-url>
# 已 clone 的仓库补拉 submodule
git submodule update --init --recursive
```

```bash
# Release 构建（含 LTO / -falign-functions=64）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 测试 + 基准
cmake -S . -B build -DBUILD_TESTING=ON -DBITCASK_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Sanitizers

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=address,undefined -DBUILD_TESTING=ON
# TSan 构建用 third_party/oneTBB 源码编译插桩版（系统 libtbb 未插桩）
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=thread -DBUILD_TESTING=ON
```

### 产物

- `libbitcask.so` — 共享库，导出 C API（`extern "C"`，跨 ABI 稳定，`SOVERSION=1`）
- `libbitcask.a` — 把全部静态归档合并为单一 `.a`
- `migrate_le` — 旧大端目录 → 小端目录的离线迁移工具
- `gen_inert_table` — NFKC 惰性区间表代码生成器（构建期自动执行）

### 安装

```bash
cmake --install build        # 头文件、libbitcask.{so,a}、bitcask_c.h
```

---

## 用法示例

> 完整接口参考见 [`doc/api-cpp.md`](doc/api-cpp.md)（C++）与 [`doc/api-c.md`](doc/api-c.md)（C / FFI）。

### C++：KV 模式

```cpp
#include <bitcask/cask.hpp>

using bitcask::Cask, bitcask::CaskOptions;
using bitcask::keydir::KeyDirRegistry;

// registry 强制非空：管理同目录 Cask 间的共享 keydir（典型：每进程一个）。
KeyDirRegistry registry;
auto c = Cask::open("/tmp/db", CaskOptions{.read_write = true}, &registry);
assert(c);

std::vector<std::byte> key{std::byte{'h'}, std::byte{'i'}};
std::vector<std::byte> val{std::byte{'w'}, std::byte{'o'}};

(*c)->put(key, val, /*tstamp=*/0);
auto r = (*c)->get_owned(key);
assert(r && r->value == val);

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
assert(c && (*c)->has_search());

std::vector<std::byte> key{'d'_b, '1'_b};
DocInput doc{
    .text   = as_bytes("北京今天天气晴朗"),
    .vector = query_vec,   // 128 维
};

(*c)->put_doc(key, doc);
(*c)->flush_index();       // 等待异步索引排空（put 本身已持久化）

// 词袋检索
auto bm25 = (*c)->search_text("北京 天气", 10);
// 向量检索
auto knn  = (*c)->search_vector(query_vec, /*k=*/10);
// RRF 混合（两路各取 max(k×4,64) 后融合）
auto hyb  = (*c)->search_hybrid("北京 天气", query_vec, /*k=*/10);
```

> 完整示例见 `tests/cask_docvalue_test.cpp`（2756 行端到端用例）。

### C API（跨语言绑定）

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

C API 设计：不透明句柄、显式 `*_free` 配对、错误码 + `bitcask_fault_t` 详情、二进制安全 `{data, size}` 切片。线程模型与 C++ 核心一致（读路径线程安全、写路径需 caller 串行化）。

---

## 架构

```
┌───────────────────────────────────────────────────────────┐
│  Cask（KV + 检索门面：open/get/put_doc/search*/merge）    │
│  ├─ KeyDir（分片 shared_mutex + MVCC 迭代器）              │
│  ├─ DataFile 缓存（pread 句柄 + 近似 LRU 淘汰）            │
│  ├─ HintFile（活跃写入器；含整文件 CRC trailer）           │
│  ├─ SearchLayer（仅索引模式）                              │
│  │   ├─ Index（ext2ord / slots / ord2ext / live 侧表）    │
│  │   ├─ InvertedIndex（按字段隔离的 BM25 倒排）             │
│  │   ├─ HnswIndex（单写者 + 多读者无锁发布协议）            │
│  │   └─ Analyzer（Ngram / Whitespace / Jieba）            │
│  └─ MetaConfig（bitcask.meta 模式持久化）                  │
└───────────────────────────────────────────────────────────┘
              │
┌─────────────▼─────────────────────────────────────────────┐
│  fileops (codec / data_file / hint_file / scanner / migrate) │
│  io (PosixFile / FileLock)  merge (Merger / Policy)        │
│  text (Analyzer / JiebaAnalyzer)  bm25 (Inverted / WAL)   │
│  vector (HnswIndex)                                        │
└────────────────────────────────────────────────────────────┘
```

关键设计要点：

- **双持久化**：数据文件（append-only，KV 权威）+ 倒排 WAL + checkpoint（索引恢复优化，纯派生缓存，校验失败回退全量 fold）
- **双锁模型**：`bitcask.write.lock`（writer）与 `bitcask.merge.lock`（merger）独立，周期性 merge 与 live writer 并行不互斥
- **异步索引**：`put_doc` 入队有界 `IndexPool`（满则背压），单 worker 串行变更索引，查询线程并发读
- **并发协议**：HNSW 单写者 + 多读者（`atomic<NodeChunk*>` 发布 + per-node 自旋锁）；InvertedIndex 按词 hash 分片锁 + CoW posting；KeyDir 256 分片锁
- **小端 only**：所有多字节整数小端（LE 主机原生零转换），不再与 legacy 大端 Erlang 字节互通，迁移用 `tools/migrate_le`

更多详见 `doc/`：

| 文档 | 说明 |
|------|------|
| [`api-cpp.md`](doc/api-cpp.md) | C++ API 参考 |
| [`api-c.md`](doc/api-c.md) | C API 参考 / FFI 绑定 |
| [`cpp-arch.md`](doc/cpp-arch.md) | 架构 |
| [`format-zh.md`](doc/format-zh.md) | 字节级格式 |
| [`concurrency-zh.md`](doc/concurrency-zh.md) | 锁与并发 |
| [`hnsw-design-zh.md`](doc/hnsw-design-zh.md) | HNSW 设计 |
| [`recovery-unified-checkpoint-design-zh.md`](doc/recovery-unified-checkpoint-design-zh.md) | 恢复 / checkpoint 设计 |

---

## 目录结构

```
.
├── include/bitcask/   # 公共头文件（API 接口）
├── c_api/             # libbitcask.so 的 C ABI（bitcask_c.{cpp,h}）
├── src/               # 实现：fileops / io / lock / keydir / merge /
│                      #       cask / search / bm25 / text / vector
├── tests/             # GoogleTest 单元 + 集成测试（24 个测试二进制）
├── bench/             # Google Benchmark（keydir / cask / inverted / hnsw ...）
├── tools/             # migrate_le、gen_inert_table
├── cmake/             # BitcaskSanitizers 模块 + tsan.supp
├── third_party/       # 第三方依赖（git submodule，见「构建依赖」）
└── doc/               # 架构 / 格式 / 设计文档
```

---

## 许可证

[Apache License 2.0](LICENSE)。
