# libbitcask

高性能嵌入式存储引擎，以 C++23 实现 Bitcask 追加日志 KV，并在其之上叠加 **BM25 全文检索**、**HNSW 向量检索** 与 **RRF 混合检索**。同一套核心既暴露为 C++23 库，也以 `extern "C"` ABI 编译为 `libbitcask.so`，供 Python / Rust / Go / Node 等跨语言 FFI 绑定使用。

- **KV 模式**：append-only data 文件是唯一持久真相；OKI（有序 key 索引）是运行期查找/提交权威，`get` 定位后经文件映射或单次 `pread` 读值。索引损坏时显式报错并从 data 受控重建
- **索引模式**：在 KV 之上叠加 BM25 倒排、HNSW 图、字段索引，支持文本 / 向量 / 混合检索
- **C++23**，无 Boost / abseil 依赖；第三方以 git submodule 形式 vendored 在 `third_party/`，构建无需联网
- **Apache 2.0** 协议

变更历史见 [`CHANGELOG.md`](CHANGELOG.md)。

---

## 能做什么

- **KV**：`put` / `get` / `remove`、原子批（崩溃后整批 all-or-nothing，提交对并发读者一次可见）、多键事务 `TxnCask`、零拷贝读、全表迭代、`[lo, hi)` 有序 range 扫描、并行扫描。
- **检索**：BM25 全文（词袋 / 短语 / 近邻 / 布尔 / 多字段 / 模糊 / 通配符 / 高亮 / 同义词）、向量 ANN（HNSW / IVF-RaBitQ / DiskANN）、RRF 混合检索。
- **运维**：后台分级归并、merge、备份、离线迁移工具、状态内省。

逐项能力与接口对照见 [`doc/features-zh.md`](doc/features-zh.md)。

## 线程安全模型摘要

**同一个 `bitcask::Cask` 句柄可被多线程安全共享**，无需每线程一个实例。本节给出顶层契约；锁层、不变量与各接口的同步原语见 [`doc/concurrency-zh.md`](doc/concurrency-zh.md) 与内部审计 [`docs/design/thread-safety.md`](docs/design/thread-safety.md)。

| 操作 | 并发语义 |
|------|----------|
| **读**（`get` / `search_*` / 批量检索） | 真并发（无锁 / `shared_lock`） |
| **写**（`put` / `remove` / `put_doc` / `sync` / `put_batch` / `put_batch_atomic`） | 多线程安全。内部 `write_mu_` 串行化——单 append WAL 在文件层本就串行，锁不损吞吐；多写**安全但不提速** |
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
using bitcask::oki::IndexRegistry;

// registry 强制非空：管理同目录 Cask 间的共享索引权威与索引池（典型：每进程一个）。
IndexRegistry registry;
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

IndexRegistry registry;
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

## 构建

需要 C++23 编译器（GCC 13+ / Clang 17+ / MSVC 19.4x+）、CMake ≥ 3.21、系统 zlib 与 oneTBB；其余依赖都是 git submodule。

```bash
git clone --recurse-submodules <repo-url>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
cmake --build build --target check   # 并行 ctest，照核数；直接跑 ctest 是串行的

cmake --install build                # 头文件、libbitcask.{so,a}、bitcask_c.h
```

Windows（MSVC）用 `cmake --preset msvc-debug` 一类 preset。依赖、sanitizer、`-Werror`、CMake 选项、ICU 来源（`BITCASK_ICU_PROVIDER`）与产物清单见 [`doc/build-zh.md`](doc/build-zh.md)。

## 文档

完整索引见 [`doc/README.md`](doc/README.md)。最常用的几份：

| 想知道什么 | 看哪里 |
|---|---|
| 逐项能力与接口对照 | [`doc/features-zh.md`](doc/features-zh.md) |
| C++ / C API 参考 | [`doc/api-cpp.md`](doc/api-cpp.md) / [`doc/api-c.md`](doc/api-c.md) |
| 构建：依赖、Windows、sanitizer、ICU 来源 | [`doc/build-zh.md`](doc/build-zh.md) |
| 架构：分层、关键设计点、目录结构 | [`doc/architecture-zh.md`](doc/architecture-zh.md) |
| 并发契约 | 本文「线程安全模型摘要」+ [`doc/concurrency-zh.md`](doc/concurrency-zh.md) |
| 磁盘格式 | [`doc/format-zh.md`](doc/format-zh.md) |
| 文本编码（UTF-8）与 ICU / Unicode 版本 | [`doc/text-encoding-zh.md`](doc/text-encoding-zh.md) |

---

## 许可证

[Apache License 2.0](LICENSE)。
