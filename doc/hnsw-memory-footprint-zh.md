# HNSW 内存占用分析（1M 向量）

> 前置阅读：`hnsw-design-zh.md`（HNSW 基础设计）、`hnsw-external-storage-design-zh.md`（外存预留点）、`hnsw-int8-only-design-zh.md`（int8-only 模式）
> 状态：分析文档（基于当前 V7 实现，对照 `src/vector/hnsw.cpp` / `include/bitcask/hnsw.hpp`）
> 结论速览：典型 768 维默认配置下，100 万向量约占 **4 GiB**；int8-only 约 **1 GiB**；mmap 外存化则堆常驻约 1 GiB + 文件 3 GiB（可回收）。

## 1. 分块结构

节点按 `kChunkSize = 1 << 16 = 65536` 一块管理（`hnsw.hpp:147-150`）。

100 万条 → `ceil(1,000,000 / 65536) = 16` 块 = **1,048,576 个槽位**。
定长数组按槽位数（而非实际节点数）分配，最后一块约 48,576 槽未使用（约 4.9% 过分配）。

## 2. NodeChunk 每槽位内存布局

`NodeChunk` 构造函数（`src/vector/hnsw.cpp:295-307`）对**每个槽位必定分配**以下成员。
关键点：**int8 量化副本 `qcodes` 永远会分配**，即使不是 `inmem_int8` 模式（`hnsw.cpp:301`）。

| 成员 | 类型 | 字节/节点 | 说明 |
|---|---|---|---|
| `vecs`（f32 本体） | `float[dim]` | **4·D** | 仅 `needs_vecs = !inmem_int8` 时分配 |
| `qcodes`（int8 量化） | `int8[dim]` | **D** | **永远分配** |
| `qscales` | `float` | 4 | 每向量量化 scale |
| `qsums` | `int32` | 4 | VNNI 偏置补偿 |
| `ords` | `uint64` | 8 | ordinal / 文档序号 |
| `levels` | `uint8` | 1 | 节点层级（≤31） |
| `locks` | `atomic<uint8>` | 1 | per-node 自旋锁 |
| `adj` 外层 | `std::vector`（24B） | 24 | 邻接块外层 vector 句柄 |
| `adj` 内层（仅实插入节点） | `uint32[1 + 2M + level·(1+M)]` | 约 150（平均） | 见 §3 |

> D = 向量维度（`HnswConfig::dim`，`uint16_t`，无默认值，初始化时设定）。

## 3. 邻接表（adjacency）

分配逻辑（`hnsw.cpp:707-710`）：

```cpp
const std::size_t slots =
    (1 + cfg_.M * 2) + static_cast<std::size_t>(level) * (1 + cfg_.M);
c->adj[slot].resize(slots);  // 定容，.data() 地址此后永不搬迁
```

- M = 16（默认），L0 容量 = `1 + 2M = 33` 槽，每个上层 = `1 + M = 17` 槽，每槽 4 字节（`uint32`）。
- 层级分布几何衰减：`P(level ≥ 1) ≈ 1/(M-1) ≈ 1/15`。约 **93.75%** 节点只有 L0（33×4 = 132B）。
- 平均 ≈ 34 槽 ×4 + glibc malloc 开销（~16B）≈ **150B/节点**。
- 内层 vector 仅对实际插入的节点（1M）分配，不按 1,048,576 槽过分配。

## 4. 内存计算公式（默认 f32+int8，M=16）

```
定长数组 = 1,048,576 槽 × (5·D + 42) B      ← vecs(4D) + qcodes(D) + qscales(4) + qsums(4) + ords(8) + levels(1) + locks(1) + adj外层(24)
邻接内层 = 1,000,000 节点 × ~150 B
```

维度相关部分实际是 **5D**（f32 本体 4D + int8 副本 D 重复存储）。

## 5. 不同维度的内存需求（纯内存构建，无 checkpoint）

| 维度 D | f32 本体 | 默认（f32+int8） | int8-only 模式 |
|---|---|---|---|
| 128  | ~0.51 GiB | **~0.81 GiB** | ~0.31 GiB |
| 384  | ~1.53 GiB | **~2.06 GiB** | ~0.56 GiB |
| 768  | ~3.07 GiB | **~3.93 GiB** | ~0.93 GiB |
| 1536 | ~6.14 GiB | **~7.68 GiB** | ~1.68 GiB |

## 6. 三个降内存方向

1. **量化副本重复分配**：默认模式下 f32 本体（4D）与 int8 副本（D）同时常驻。代码自身注释指出 `int8_dot_ == nullptr` 时这段是浪费（`hnsw.cpp:693-695`）。若不用 VNNI 粗筛，砍掉 `qcodes` 可省约 20%（5D → 4D）。

2. **int8-only 模式**（`HnswConfig::inmem_int8 = true`）：不存 f32 本体（`needs_vecs = false`，`vecs` 容量 0），仅存量化副本。D=768 时约 3.9 GiB → **约 0.93 GiB**，降 4×。约束：仅 `kDot` 度量，精度略降。

3. **BCVS V2 mmap 外存化**：从 checkpoint 加载的前 `checkpoint_count_` 条 f32 向量由 mmap 只读覆盖（page cache，可回收），不计入堆常驻（`hnsw.hpp:185-193` 的 `vec_of`，`316-325`）。D=768 全部已 checkpoint 时，堆常驻约 **0.95 GiB**（qcodes + 邻接 + 元数据）+ mmap 3 GiB（可回收）。

## 7. 关键代码位置

- `NodeChunk` 定义：`include/bitcask/hnsw.hpp:153-178`
- `NodeChunk` 构造（分配点）：`src/vector/hnsw.cpp:295-307`
- 邻接块分配：`src/vector/hnsw.cpp:707-710`
- 层级抽样：`src/vector/hnsw.cpp:667-671`
- 向量路由（mmap vs hot chunk）：`include/bitcask/hnsw.hpp:185-193`
- `HnswConfig`（M / dim / inmem_int8）：`include/bitcask/hnsw.hpp:63-73`
