# HNSW 内存占用分析（基于当前 V7 实现重新推导）

> 前置阅读：`hnsw-design-zh.md`（HNSW 基础设计）、`hnsw-graph-construction-zh.md`（建图算法）、`hnsw-int8-only-design-zh.md`（int8-only 模式）
> 实现位置：`include/bitcask/hnsw.hpp` 的 `NodeChunk` 结构与 `HnswIndex` 私有成员；运行时分配点在 `src/vector/hnsw.cpp` 的 `HnswIndex::NodeChunk::NodeChunk` 构造函数与 `HnswIndex::HnswIndex` 构造函数
> 状态：分析文档（按当前 struct 布局重新推导，未沿用旧数字）

## 1. 关键常量

| 名称 | 值 | 位置 |
|---|---|---|
| `kChunkBits` | `16` | `HnswIndex` 私有静态常量（`hnsw.hpp`） |
| `kChunkSize` | `1 << kChunkBits = 65536` | 同上 |
| `kChunkMask` | `kChunkSize - 1` | 同上 |
| `kMaxChunks` | `1024`（容量上限 64M 节点） | 同上 |
| `kAdjSlabWords` | `1 << 18 = 262144` u32 = 1 MiB | 同上 |
| `M` | `16`（默认；`HnswConfig::M`） | `HnswConfig` 结构体（`hnsw.hpp`） |
| `layer_cap(0)` | `2M = 32` | `HnswIndex::layer_cap`（`hnsw.hpp`） |
| `layer_cap(l≥1)` | `M = 16` | 同上 |
| `mL = 1/ln(M)` | `1/ln(16) ≈ 0.3607` | `HnswIndex::inv_log_m_`（构造时预算，`hnsw.cpp`） |

## 2. 分块结构

节点按 `kChunkSize = 65536` 一块管理。`NodeChunk` 全部成员构造时定容，**生命周期内地址不变**（hot chunk 内 `vecs` 由 mmap 覆盖部分历史值，详见 §6）。

| 节点规模 | chunks 数 | 总槽位 | 未使用槽位 | 过分配率 |
|---|---|---|---|---|
| 100k | 2 | 131 072 | 31 072 | 31.07% |
| 1M | 16 | 1 048 576 | 48 576 | 4.86% |
| 10M | 153 | 10 027 008 | 27 008 | 0.27% |
| 64M（kMaxChunks 上限） | 1024 | 67 108 864 | n/a | 0% |

chunk 目录：`std::array<std::atomic<NodeChunk*>, kMaxChunks>`（`HnswIndex::chunks_`，`hnsw.hpp`）。定容 1024 × 8 B = 8 KiB 常驻，**全部使用前是 8 KiB、不是按节点增长**。

## 3. NodeChunk 每槽位内存布局

字段定义见 `include/bitcask/hnsw.hpp` 的 `NodeChunk` 结构；分配规则见 `src/vector/hnsw.cpp` 的 `HnswIndex::NodeChunk::NodeChunk` 构造函数（构造时按 `needs_vecs = !cfg_.inmem_int8`、`needs_qcodes_ = cfg_.inmem_int8 || (int8_dot_ && kDot)` 分支）。

| 字段 | 类型 | 每槽位字节（实际占位） | 分配条件 |
|---|---|---|---|
| `vecs` | `std::vector<float>`（按 `kChunkSize * dim` 预分配） | **`4·dim`**（slot × dim 的 float） | `needs_vecs` 为真（即非 `inmem_int8`） |
| `qcodes` | `std::vector<std::int8_t>`（按 `kChunkSize * dim` 预分配） | **`dim`**（slot × dim 的 int8） | `needs_qcodes_` 为真 |
| `qscales` | `std::vector<float>` | **4** | `needs_qcodes_` 为真 |
| `qsums` | `std::vector<std::int32_t>` | **4** | `needs_qcodes_` 为真 |
| `ords` | `std::vector<std::uint64_t>` | **8** | 总是 |
| `levels` | `std::vector<std::uint8_t>` | **1** | 总是 |
| `adj` | `std::vector<std::uint32_t*>` | **8**（x86-64 指针） | 总是（邻接块指针进 arena，见 §4） |
| `locks` | `std::unique_ptr<std::atomic<std::uint32_t>[]>` | **4**（atomic<u32> = 4 B） | 总是（per-node seqlock 序号） |

**关键观察**：

- `locks` 字段是 `std::unique_ptr<std::atomic<std::uint32_t>[]>`——指针 8 B（chunk 级开销），数组本身 `kChunkSize` 个 `std::atomic<std::uint32_t>`（x86-64 上各 4 B）堆分配。所以**每槽位实际占 4 B**。
- `adj[slot]` 是 `uint32_t*` 指针（x86-64 上 8 B），指向 §4 的 slab arena。slot 本身不存邻接块，只存指针。
- `vecs`/`qcodes`/`qscales`/`qsums`/`ords`/`levels`/`adj`/`locks` 都是 `kChunkSize` 个槽位的紧凑 SoA 数组，**按槽位索引** `slot = id & kChunkMask`。

## 4. 邻接块（adjacency）

分配函数 `NodeChunk::alloc_adj`（`hnsw.hpp` 内 `NodeChunk` 结构体成员，`hnsw.cpp` 实现）。**关键改造**：从 per-node `new u32[]` 改为 **per-chunk bump-slab arena**：

```cpp
std::uint32_t* alloc_adj(std::size_t n) {
    assert(n <= kAdjSlabWords && "adj block exceeds slab");
    if (adj_slabs.empty() || adj_slab_used + n > kAdjSlabWords) {
        adj_slabs.push_back(
            std::make_unique<std::uint32_t[]>(kAdjSlabWords));   // 1 MiB
        adj_slab_used = 0;
    }
    std::uint32_t* p = adj_slabs.back().get() + adj_slab_used;
    adj_slab_used += n;
    return p;
}
```

块大小计算（`HnswIndex::insert` 与 `HnswIndex::clone_live`）：

```cpp
const std::size_t slots =
    (1 + cfg_.M * 2) + static_cast<std::size_t>(level) * (1 + cfg_.M);
```

| 层 | `M=16` 时槽位数 | 字节数（4 B/u32） |
|---|---|---|
| L0 | `1 + 2·16 = 33` | **132 B** |
| L1 | `33 + 17 = 50` | **200 B** |
| L2 | `33 + 17·2 = 67` | **268 B** |
| L3 | `33 + 17·3 = 84` | **336 B** |
| Lk | `33 + 17·k` | `(132 + 68·k)` B |

**层级分布**（`level = floor(-ln(U) / ln(M))`，`U ~ Uniform(0,1)`）：

| 层 | 概率 `P(level = l)` | `M=16` 时 |
|---|---|---|
| L0 | `1 - 1/M` | 93.750% |
| L1 | `(1/M)·(1 - 1/M)` | 5.859% |
| L2 | `(1/M²)·(1 - 1/M)` | 0.366% |
| L3 | `(1/M³)·(1 - 1/M)` | 0.0229% |
| L4+ | 几何衰减 | < 0.002% |

**平均邻接块字节数**（`M=16`，全部 ≤31 层求和）：

```
E[adj_block] = 4 · Σ_{l=0..31} P(level=l) · (33 + 17·l)
              = 4 · 34.13 ≈ 136.5 B/节点
```

即平均每个节点常驻邻接块 ≈ **136.5 字节**（含 1 字节 count + 实际邻居 ID）。

**为什么用 arena 而非 per-node `vector<vector<u32>>`**：

- 旧方案：每个 `vector<u32>` 一个堆头（`libstdc++` 通常 24 B）+ `malloc` 圆整（`jemalloc/glibc` 16~32 B 头部）→ 每节点 40~56 B 额外开销。
- 新方案（`adj_slabs`）：仅 `adj_slabs.back().get() + adj_slab_used` 取址，零额外 malloc 头；旧 slab 永不搬迁，地址稳定不变量维持（与旧 `vector.data()` 一致）；分配序 = 插入序，邻接块在 slab 内连续（提升 `copy_neighbors` 局部性）。
- 实测：约消除 93.75% L0-only 节点的 malloc。

**arena 内存账**：1 MiB slab 内 256 Ki u32 槽。L0 占 33 槽 → 每 slab 装 256 Ki / 33 ≈ 7 754 个 L0 节点的邻接块；64 Ki 节点/chunk → 每 chunk 约需 8~9 个 slab = 8~9 MiB（远小于旧方案碎片）。

## 5. 每节点字节数（精确推导）

设 `D = cfg_.dim`、`M = 16`、`kChunkSize = 65536`。三模式：

### 5.1 默认 f32+int8（`inmem_int8=false` 且有 VNNI+kDot）

```
bytes/节点 = 4·D (vecs)  + D (qcodes) + 4 (qscales) + 4 (qsums)
           + 8 (ords) + 1 (levels) + 8 (adj ptr) + 4 (locks)
           + 136.5 (adjacency)
           = 5·D + 165.5
```

### 5.2 f32-only（无 VNNI 或 `metric=kL2`）

`needs_qcodes_` 为假 → `qcodes/qscales/qsums` 容量 0（不分配）。

```
bytes/节点 = 4·D (vecs) + 8 (ords) + 1 (levels) + 8 (adj ptr) + 4 (locks)
           + 136.5 (adjacency)
           = 4·D + 157.5
```

### 5.3 int8-only（`inmem_int8=true`）

`needs_vecs=false` → `vecs` 容量 0；`needs_qcodes_=true` → qcodes/qscales/qsums 全分配。

```
bytes/节点 = D (qcodes) + 4 (qscales) + 4 (qsums)
           + 8 (ords) + 1 (levels) + 8 (adj ptr) + 4 (locks)
           + 136.5 (adjacency)
           = D + 165.5
```

## 6. chunk 元数据开销

每个 `NodeChunk` 实例自身的结构体成员（含 `std::vector` 各 24 B 三件套 指向/大小/容量，`std::unique_ptr` 8 B，计数器 8 B 等），加上 `chunks_[1024]` 数组：

| 项 | 大小 | 说明 |
|---|---|---|
| `NodeChunk` 实例结构体本身（vector 句柄 + unique_ptr + 计数器） | 约 250~300 B | `sizeof(NodeChunk)` 量级，可忽略 |
| `chunks_[1024]`（`std::array<std::atomic<NodeChunk*>, kMaxChunks>`） | **8 KiB 常驻** | 1024 × 8 B（指针） |
| 每个 chunk 的 slab arena（按需增长） | 1 MiB × slab 数 | 已计入 §5 的邻接块部分 |
| `checkpoint_count_` / `entry_meta_` / `count_` / `max_inserted_ord_` / `writer_active_` | < 100 B | `HnswIndex` 成员（`hnsw.hpp`） |
| `instance_id_` / `rng_` / `inv_log_m_` / `dist_` / `int8_dot_` / `needs_qcodes_` | < 100 B | 同上 |
| `vecs_mmap_*` / `vec_file_` / `qc_file_` / `payload_gen_` | < 200 B | 同上 |

**每 chunk 的实际常驻成本 = arena（已计入邻接块）+ 槽位数组（已计入每节点字节数）+ 结构体自身（≈300 B）**。1M 节点 = 16 chunks → 结构体自身 ≈ 4.8 KiB，可忽略。

## 7. 1M 节点内存占用（按当前 struct 布局重新推导）

按 `1 000 000` 节点、对应 `ceil(1M / 65536) = 16` chunks = `1 048 576` 槽位计算（不过度 round-up，含 4.86% 槽位过分配）。`M=16` 默认参数。

### 7.1 默认 f32+int8（VNNI + kDot）

公式：`5·D + 165.5` 字节/节点，乘以 1 000 000（邻接块按实际节点数，槽位过分配仅影响 `vecs/qcodes/...` 的按 chunk 预分配部分）：

| dim `D` | 每节点字节 | 1M 节点占用 | 备注 |
|---|---|---|---|
| 128 | `5·128 + 165.5 = 805.5` B | **0.75 GiB** | 小模型（如 MiniLM） |
| 384 | `5·384 + 165.5 = 2085.5` B | **1.94 GiB** | 通用 embedding 主流档位 |
| 768 | `5·768 + 165.5 = 4005.5` B | **3.73 GiB** | BERT-large / bge-base |
| 1536 | `5·1536 + 165.5 = 7845.5` B | **7.31 GiB** | bge-large / m3e-large |
| **2560** | `5·2560 + 165.5 = 12965.5` B | **12.08 GiB** | **qwen3-embedding** 部署目标 |

### 7.2 f32-only（无 VNNI 或 `kL2`）

公式：`4·D + 157.5` 字节/节点：

| dim `D` | 每节点字节 | 1M 节点占用 |
|---|---|---|
| 128 | 669.5 B | **0.62 GiB** |
| 384 | 1693.5 B | **1.58 GiB** |
| 768 | 3229.5 B | **3.01 GiB** |
| 1536 | 6301.5 B | **5.87 GiB** |
| 2560 | 10397.5 B | **9.68 GiB** |

### 7.3 int8-only（`inmem_int8=true`）

公式：`D + 165.5` 字节/节点：

| dim `D` | 每节点字节 | 1M 节点占用 | 相对默认降幅 |
|---|---|---|---|
| 128 | 293.5 B | **0.27 GiB** | -64% |
| 384 | 549.5 B | **0.51 GiB** | -74% |
| 768 | 933.5 B | **0.87 GiB** | -77% |
| 1536 | 1701.5 B | **1.59 GiB** | -78% |
| 2560 | 2725.5 B | **2.54 GiB** | **-79%** |

## 8. 三种降内存方向

1. **int8-only 模式**（`HnswConfig::inmem_int8 = true`）：见上表，最大降幅 ~79%。约束：仅 `kDot` 度量（`HnswIndex` 构造时 `assert(!(cfg_.inmem_int8 && cfg_.metric != HnswMetric::kDot))`）。详见 `hnsw-int8-only-design-zh.md`。

2. **BCVS V2 mmap 外存化**：`HnswIndex::load_vec_payload`（`hnsw.cpp`）将前 `checkpoint_count_` 条 f32 向量由 mmap 只读覆盖（page cache 可回收），不计入堆常驻。`HnswIndex::vec_of` 按 id 分支：`< checkpoint_count_` 走 mmap、`≥` 走 `NodeChunk::vecs`（hot chunk）。D=768 全部已 checkpoint 时堆常驻 ≈ 0.95 GiB（qcodes + 邻接 + 元数据）+ mmap 3 GiB（可回收）。

3. **f32-only 路径**：无需 VNNI/kDot 场景下，`HnswIndex::needs_qcodes_ = false` → `qcodes/qscales/qsums` 不分配，省 `D + 8` 字节/节点。D=768 下 ≈ 0.72 GiB 节省（相对默认 f32+int8 的 3.73 GiB）。

## 9. 关键符号索引

| 概念 | 代码位置 |
|---|---|
| `NodeChunk` 结构定义（含 9 个字段） | `include/bitcask/hnsw.hpp` 的 `HnswIndex::NodeChunk` |
| 字段分配条件（按 `needs_vecs` / `needs_qcodes_`） | `src/vector/hnsw.cpp` 的 `HnswIndex::NodeChunk::NodeChunk` 构造函数 |
| 邻接块 arena 分配 | `NodeChunk::alloc_adj`（`hnsw.hpp` 声明，`hnsw.cpp` 定义） |
| 邻接块容量与层内偏移 | `HnswIndex::layer_cap` / `HnswIndex::layer_off`（`hnsw.hpp`） |
| mmap vs hot chunk 路由 | `HnswIndex::vec_of`（`hnsw.hpp`） |
| `HnswConfig`（M / dim / inmem_int8 / metric / ef_construction / seed） | `include/bitcask/hnsw.hpp` 的 `HnswConfig` |
| 层级采样公式 | `HnswIndex::insert` 内（`src/vector/hnsw.cpp`） |
| chunk 目录定容 | `HnswIndex::chunks_`（`std::array<std::atomic<NodeChunk*>, kMaxChunks>`，`hnsw.hpp`） |
| per-node seqlock | `NodeChunk::locks`（`hnsw.hpp`） |
| int8 量化副本访问 | `HnswIndex::qcodes_of` / `HnswIndex::qscale_of` / `HnswIndex::qsum_of`（`hnsw.hpp`） |

## 10. 推导说明

本节数字全部基于：

- 当前 `NodeChunk` 结构（V7 实现，2026-07 状态）9 字段布局。
- M=16 默认值下的邻接块几何分布（`P(level=l) = (1/M^l)(1 - 1/M)`，截断到 level ≤ 31）。
- `std::atomic<std::uint32_t>` 在 x86-64 Linux GCC/Clang 下 4 字节（与 `uint32_t` 同）。
- 指针 8 字节（x86-64 LP64 数据模型）。
- 不含 `std::vector`/`std::unique_ptr` 句柄本身（每 chunk 一次、合计 < 5 KiB），不含 slab arena 中尚未使用的尾段（按需增长，已计入 §5 的 136.5 字节/节点 邻接块均值）。
- int8 量化副本访问器（`qcodes_of` 等）只在 `needs_qcodes_` 为真时存在（见 §3 表）。

如需对其他维度（1280 / 1792 / 3072 等）做同样推导，按 §5 公式代入即可：

```
默认 f32+int8: bytes/node = 5·D + 165.5
f32-only:     bytes/node = 4·D + 157.5
int8-only:    bytes/node = D   + 165.5
1M 节点 = 上述 × 10⁶
```