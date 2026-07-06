# HNSW 向量检索设计（V3 定稿）

> 前置阅读：[`vector-db-design-zh.md`](vector-db-design-zh.md)（可行性探索与总体定位）、[`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)（checkpoint 体系与 A4 合取式门）、[`hnsw-overview-zh.md`](hnsw-overview-zh.md)（算法全景）、[`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md)（图论基础）、[`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md)（图生命周期与持久化）、[`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)（int8 量化）
> 参考文献：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs,"* arXiv:1603.09320 (2016), TPAMI 2018

本文按「§理论 → §实现」两段式编排：理论段复刻 «Malkov 2018» 的核心算法；实现段对照 `include/bitcask/hnsw.hpp` 与 `src/vector/hnsw.cpp` 的工程决策。**持久化、merge 重建、生命周期**等专项主题以 [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) 为权威，本稿仅给交叉引用。

---

# §理论 算法骨架

## 1. 边界与基本选择（设计定稿）

**1.1 引擎只收向量、不算向量**

embedding 由调用方提供（可选 `embedder` 接外部模型服务），C++ 引擎不含任何 ML runtime。依据：

- DocValue v3 `vector` 段已是 source of truth，merge/恢复不依赖模型。
- BM25 内置因分词廉价确定；embedding 是重推理，性质不同。
- 引擎不引入百 MB 级推理依赖（同 tbbmalloc 决策一脉）。

**1.2 维度：库内恒定、初始化显式配置**

```cpp
struct HnswConfig {
    std::uint16_t    dim = 0;                            // 创建时指定；0 = 本集合无向量
    HnswMetric       metric = HnswMetric::kDot;          // kDot (cosine 上游归一化) / kL2
    std::uint32_t    M = 16;                             // 上层邻居容量；L0 = 2M
    std::uint32_t    ef_construction = 200;
    std::uint64_t    seed;                               // 层数抽样种子（测试可复现）
    bool             inmem_int8 = false;                 // P5：int8-only 模式
};
```

写入 `bitcask.meta` 扩展节；重开校验，不符 → `kModeMismatch`；**显式配置，不做首写推断**（脏数据不应有定义集合 schema 的权力）。per-record 的 `[Dim:varint]` 保留作自描述 + 损坏哨兵，策略上必等于 `meta.dim`。

**1.3 cosine = 「写入时归一化 + 内积」**

归一化是引擎内廉价数值操作；`meta` 记 `cosine_normalized` 标志，查询向量同样入口归一化。本模块只暴露两种度量 `HnswMetric::kDot` 与 `HnswMetric::kL2`（`hnsw.hpp` 枚举 `HnswMetric`）。

**1.4 多模型 / 多维度的扩展位**

多字段向量（`field → HNSW 图`，沿 S8.6 `fields_` 同构）——**V3 仅做默认字段单图**，接口留扩展位。

**部署目标实测**（2026-06-12，本机 i9-13900H 混合核，qwen3-embedding，OpenAI 兼容 `/v1/embeddings`）：

| 维度 | 来源 | 用途 | 上限规模（f32） | 上限规模（int8-only） |
|------|------|------|---------------|----------------------|
| 384 | 通用 sentence embedding | 召回质量与速度的折衷 | 1M | >1M |
| 768 | 主流大模型 embedding | 质量优 | ~500k | 1M |
| **2560** | qwen3-embedding | 部署目标 | **~300k**（≈3GB） | **1M**（≈1GB） |

2560 = 8×320，AVX2 主循环 32 宽整除无尾循环；实测输入上限 32K token，**输出已 L2 归一化（实测 norm=1.0）**——引擎写入侧归一化对其幂等，保留不变（兜其它客户端/模型漂移）。32K 上限的文本截断是调用方 embedder 的职责，引擎不感知。

## 2. 论文算法对照

### 2.1 INSERT（论文 Algorithm 1）

```
INSERT(hnsw, q, M, Mmax, efConstruction, mL):
  W ← ∅
  ep ← get enter point for hnsw
  L ← level of ep
  l ← ⌊-ln(unif(0..1)) ∙ mL⌋                  // 新节点层数
  for lc ← L … l+1
      W ← SEARCH-LAYER(q, ep, ef=1, lc)        // Phase 1: 高层贪心
      ep ← get the nearest element from W to q
  for lc ← min(L, l) … 0
      W ← SEARCH-LAYER(q, ep, efConstruction, lc)   // Phase 2: 每层宽搜
      neighbors ← SELECT-NEIGHBORS(q, W, M, lc)     // Alg.4 HEURISTIC
      add bidirectional connections
      for each e ∈ neighbors
          if |eConn| > Mmax                          // Mmax=2M on lc=0 else M
              eNewConn ← SELECT-NEIGHBORS(e, eConn, Mmax, lc)
              set neighbourhood(e) at layer lc to eNewConn
      ep ← W
  if l > L
      set enter point for hnsw to q
```

### 2.2 SEARCH-LAYER（论文 Algorithm 2）

```
SEARCH-LAYER(q, ep, ef, lc):
  v ← ep                              // visited 集合
  C ← ep                              // candidates（min-heap by dist to q）
  W ← ep                              // dynamic list of ef closest（max-heap）
  while |C| > 0
      c ← extract nearest element from C to q
      f ← get furthest element from W to q
      if distance(c, q) > distance(f, q)
          break                        // 收敛
      for each e ∈ neighbourhood(c) at layer lc
          if e ∉ v
              v ← v ⋃ e
              f ← get furthest element from W to q
              if distance(e, q) < distance(f, q) or |W| < ef
                  C ← C ⋃ e
                  W ← W ⋃ e
              if |W| > ef
                  remove furthest element from W to q
  return W
```

> **理论证明见 [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) §6**：期望步数 = `1/(1 - exp(-mL))`，期望层数 = `O(log N)`，总期望复杂度 `O(log N)`。

### 2.3 SELECT-NEIGHBORS-HEURISTIC（论文 Algorithm 4）

候选按到 query 的距离升序逐个考察，仅当**比任一已选邻居都更近**时入选——避免聚簇数据上邻居全聚同一方向。本实现 `HnswIndex::select_neighbors`（f32 路径）与 `HnswIndex::select_neighbors_int8`（int8 路径）均复刻该逻辑；不启用 `extendCandidates`/`keepPrunedConnections`（标准数据集足够）。

### 2.4 K-NN-SEARCH（论文 Algorithm 5）

完整查询 = 高层贪心下降 + L0 束搜 + top-k 截取：

```
SEARCH-KNN(hnsw, q, K, ef):
  // Phase 1: 高层贪心
  cur ← entry point
  for layer ← max_level downto 1:
      cur ← SEARCH-LAYER(q, cur, ef=1, layer)
  // Phase 2: L0 束搜
  W ← SEARCH-LAYER(q, cur, ef=ef, layer=0)
  return K nearest from W
```

---

# §实现 工程化

## 3. 公共 API

`include/bitcask/hnsw.hpp` 的 `bitcask::vec::HnswIndex` 公共接口：

```cpp
class HnswIndex {
public:
    explicit HnswIndex(const HnswConfig& cfg);
    ~HnswIndex();

    // 插入（仅单写者线程）。前置：vec.size() == dim；ord 全局单调。
    // 水位幂等：ord <= max_inserted_ord_ 时丢弃，崩溃回放重叠区安全。
    void insert(std::uint64_t ord, std::span<const float> vec);

    struct Hit { std::uint64_t ord; float score; };

    // 查询 top-k。线程安全（多读者，可与单写者 insert 并发）。
    // ef >= k（内部取 max）。live 非空时结果侧过滤（死节点仍参与导航）。
    [[nodiscard]] std::vector<Hit> search(
        std::span<const float> query, std::size_t k, std::size_t ef,
        const std::function<bool(std::uint64_t)>* live = nullptr) const;

    [[nodiscard]] std::size_t  size() const noexcept;          // 已发布节点数
    [[nodiscard]] bool         empty() const noexcept;
    [[nodiscard]] std::uint64_t max_inserted_ord() const noexcept;
    [[nodiscard]] const HnswConfig& config() const noexcept;

    // V3.5:重建用只读访问（merge 物理清死，VectorPlugin 重建入口）
    [[nodiscard]] std::uint64_t node_ord(std::uint32_t id) const;
    [[nodiscard]] std::span<const float> node_vec(std::uint32_t id) const;

    // S13-P8:merge 期结构化拷贝活子图（O(N+E) memcpy 级，无距离计算）
    [[nodiscard]] std::shared_ptr<HnswIndex>
    clone_live(const std::function<bool(std::uint64_t)>& is_live) const;

    // V7:BCVS v2 快照（search.ckpt 的 kHnsw 段 + search.vec mmap + search.qc8）
    [[nodiscard]] bool save_vec_payload(std::string_view path) const;
    [[nodiscard]] bool load_vec_payload(std::string_view path);
    [[nodiscard]] bool save_qc_payload(std::string_view path) const;
    [[nodiscard]] bool load_qc_payload(std::string_view path);
    [[nodiscard]] bool qc_payload_pending() const noexcept;

    [[nodiscard]] bool serialize(std::vector<std::uint8_t>& out) const;
    [[nodiscard]] bool deserialize(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool save(std::string_view base_path) const;   // 测试用便捷 wrapper
    [[nodiscard]] bool load(std::string_view base_path);
};
```

## 4. 数据结构与内存布局

### 4.1 NodeChunk 定容分块

**设计约束**：内部 `id: u32` 插入序紧凑分配；构造时定容；生命周期内地址永不变（读者并发协议依赖此不变量）。

`include/bitcask/hnsw.hpp` 的 `NodeChunk` 字段对照（一个 chunk = `kChunkSize = 1<<16 = 65536` 个节点）：

| 字段 | 类型 | 大小 | 用途 |
|------|------|------|------|
| `vecs` | `std::vector<float>` | `kChunkSize * dim` × 4B | f32 常驻向量（仅 hot chunk 分配；mmap checkpoint 路径 `needs_vecs=false` 容量 0） |
| `qcodes` | `std::vector<std::int8_t>` | `kChunkSize * dim` × 1B | int8 对称量化副本（VNNI 检索用） |
| `qscales` | `std::vector<float>` | `kChunkSize` × 4B | 每向量一个 scale = `max|v[i]|` |
| `qsums` | `std::vector<std::int32_t>` | `kChunkSize` × 4B | 每向量一个 `Σcodes`（VNNI 偏置补偿） |
| `ords` | `std::vector<std::uint64_t>` | `kChunkSize` × 8B | 内部 `id → ord` 翻译（结果回传 + 删除水位） |
| `levels` | `std::uint8_t` × N | `kChunkSize` × 1B | 每个节点的最高层 |
| `adj` | `std::vector<std::uint32_t*>` | `kChunkSize` × 8B | 每节点邻接块首指针（指向 bump arena，永不搬迁） |
| `adj_slabs` | `std::vector<std::unique_ptr<std::uint32_t[]>>` | 增长中 | bump arena slab 池（`kAdjSlabWords = 1<<18 = 256K u32 = 1MB`/slab） |
| `locks` | `std::unique_ptr<std::atomic<std::uint32_t>[]>` | `kChunkSize` × 4B | per-node 序号锁（偶数=稳定，奇数=更新中） |

> 详细字段语义见 [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) §8。

### 4.2 邻接块内布局

每节点邻接块在 chunk 的 `adj_slabs` 内一次性按层数分配，**层内布局 = `[count | ids...]`**：

```
L0: count | id₁ ... id_{2M}     ← 容量 2M
L1: count | id₁ ... id_M       ← 容量 M
...
Ll: count | id₁ ... id_M
```

`layer_off(layer)` 在 `hnsw.hpp` 的 `HnswIndex::layer_off` 给出：L0 偏移 0；`l>0` 偏移 `(1 + 2M) + (l-1)*(1 + M)`。总槽位 = `(1 + 2M) + levels[id] * (1 + M)`。M=16 单节点最大 `(1+32) + 31*(1+16) = 560 u32`，远小于 `kAdjSlabWords`。

L0 容量 `2M` 是 «Malkov 2018» §4 的明确建议（dense layer 给多容量减少 recall 退化）；上层 `M` 已足够（hub 节点度数高但层稀疏）。

> 邻接分配的工程取舍见 [`hnsw-graph-construction-zh.md`](hnsw-graph-construction-zh.md)；规模内存账见 [`hnsw-memory-footprint-zh.md`](hnsw-memory-footprint-zh.md)。

## 5. 并发协议（V3.3，单写者 + 多读者）

> **设计纪律**：写者路径仅 `IndexPool` 单 worker；查询路径可任意线程并发；**多写者不支持**（`insert` 内有 `debug assert` 声明，`HnswIndex::writer_active_` 原子守卫）。这与 `SearchLayer` 整体单写者约束一致。

### 5.1 chunk 目录与发布序

```cpp
std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_{};   // 1024 项 → 64M 节点上限
std::atomic<std::uint32_t>                     count_{0};    // 发布水位
std::atomic<std::uint64_t>                     entry_meta_{0}; // 高 32 位 = max_level+1，低 32 位 = entry id
std::atomic<std::uint64_t>                     max_inserted_ord_{...};
std::mt19937_64                                 rng_;         // 仅写者使用
```

**必须保持裸指针 + `atomic<NodeChunk*>`**——这是无锁发布协议的核心；`shared_ptr` 的原子引用计数开销不可接受（每次 `search` 都 load）。

**insert 发布序**：

```
1. 写满本节点数据：vecs[id] = vec, qcodes[id] = codes, ords[id] = ord, levels[id] = level
2. alloc_adj(总槽位数) → adj[id] = slab 指针
3. count_.store(id+1, release)            ← 节点"出生"
4. 反向连边（可能在 count 之后对读者可见）
```

**search 读者协议**：

```
1. n = count_.load(acquire)              ← 本次查询的快照水位
2. entry = entry_meta_.load(acquire) → 取低 32 位
3. 所有 copy_neighbors 返回的 nid，若 nid ≥ n → 跳过
4. visited 数组按 n 定界（n 之后的位置本就未初始化）
```

**关键不变量**：

- `count_` 是读者可见性的**唯一**分界：节点数据写入先于 `count_.store`；反向边追加可在 `count_` 之后对读者可见，但 `nid ≥ n` 过滤保证读者永不读未出生节点。
- `entry_meta_` 的发布 happens-after `count_` 的相应位：search 入口先 `load entry_meta_`（acquire）再 `load count_`，保证 entry id 必 < 本地 `count`。
- **写者自身**也取 `n_bound = id`（排除自己）：反向边在低层先行发布后，写者自己更低层 `search_layer` 可能经它走回新节点，把自己选成自己的邻居；以 `id` 为界一并剪掉。

### 5.2 per-node 序号锁（V3.3, S13-P7）

旧设计是 1 字节自旋锁；新版改为 32 位序号锁（`NodeChunk::locks`）：

```
写者改邻接:
  ++locks[id]                              // 奇数 = 更新中
  atomic_store_relaxed 数据字
  ++locks[id]                              // 偶数 = 稳定

读者 copy_neighbors(id, layer, out):
  s0 = locks[id].load(acquire)
  if s0 % 2 == 1: 重读                     // 写者更新中
  atomic_load_relaxed 拷 [count][ids] 到 out
  s1 = locks[id].load(acquire)
  if s0 != s1: 丢弃重试                    // torn 读
  return out_cnt
```

**为什么从自旋锁换序号锁**：HNSW 流量高度偏向 hub 节点——旧自旋锁的 `exchange` 在读者侧是写操作，多核共享同一缓存行时严重 ping-pong。序号锁把读者路径降为纯读，写者单线程 `±1` 发布协议足够；`atomic_ref` relaxed 走数据字，UB-free 且 TSan 干净。

**临界区量级**：纯追加/拷出 ~百 ns；**超容收缩在持锁状态下做距离计算**（微秒级）——读者争同一把锁，TSan 插桩下 20k×16d 单写 4 读无可观测停顿。锁外预选/arena 化留 V3.x。

### 5.3 visited 表（thread_local 版本化数组）

```
struct VisitedTable {
    std::vector<std::uint32_t> marks;     // 按 node id 索引
    std::uint32_t              epoch = 0;
    std::uint64_t              owner = 0;  // HnswIndex 全局自增实例 id（防 ABA）
};
thread_local VisitedTable t_visited;
```

每次 `search_layer` 开头：

```
vt.epoch += 1
if vt.epoch == 0:                              // 回绕
    vt.marks.assign(marks.size(), 0)
    vt.epoch = 1
if vt.owner != instance_id_:                   // 实例切换（多图交替查询）
    vt.marks.assign(marks.size(), 0)
    vt.epoch = 1
    vt.owner = instance_id_
ep = vt.epoch
marks[nid] == ep ⇒ 已访问
```

`owner` 用全局自增实例 id 而非 `this` 指针——避免指针 `delete/new` 复用让陈旧 marks 与新实例 epoch 假性匹配。多实例被同线程交替查询时退化为每次清零，正确性不受影响；单集合单图的常态下零开销。

### 5.4 entry_meta_ 合并原子

`std::atomic<std::uint64_t> entry_meta_` 编码 `(max_level + 1) << 32 | entry_id`：

- `0` 表示空图。
- `insert` 完整连边后才 `store`；`search` 开头 `load(acquire)` 取低 32 位作为 entry。
- 一次 load 拿到完整入口（避免「先 load level 再 load id」的两次原子读之间的撕裂）。

## 6. 距离内核与度量分发

构造时按 `HnswConfig::metric` 分发一次函数指针：

```cpp
DistFn dist_;                       // f32 距离（按 metric+ISA 分发一次）
int8::Int8DotFn int8_dot_;          // int8 粗筛内核；无 VNNI 时 nullptr
bool needs_qcodes_;                 // = inmem_int8 || (int8_dot_ && kDot)
```

**实现选型**：取「运行时 n 通用内核」而非 per-dim 特化——32 宽主循环对 384/2560 均整除，特化无利可图。

**V3.8 优化**（4 路独立累加器 + 候选向量软件预取）：

- 单链 FMA 延迟 ~4cyc 卡在 1 FMA/4cyc，4 路顶到 2 加载/cyc 的访存口上限——内核余量 ~4×
- `copy_neighbors` 后预取遍把未访问邻居向量首 256B 拉向 L1，大图冷 DRAM 取数与距离计算重叠
- 实测（384d, median of 3）：查询 100k/ef64 **483 → 322µs**（−33%）、插入 100k **936 → 1221/s**（+30%）
- 本机（i9-13900H 混合核）无 AVX-512；AVX-VNNI 留给 int8（`vpdpbusd`，32 元素/迭代 + 内存流量 4×↓）

**int8 路径**（VNNI）：仅在 `kDot` + 主机支持 VNNI + `needs_qcodes_=true` 时启用；`dist_id_int8` 走 `int8::Int8DotFn`，与 `dist_id` 同"越小越近"约定（VNNI 返回正的内积，`kDot` 取负）。`kL2` 不支持 int8 路径（上游 open 接线拒绝）。

**int8-only 模式**（`inmem_int8=true`）：`NodeChunk::vecs` 容量 0，全程 int8 寻路，无 f32 精排（详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)）。

## 7. 写入与查询路径

```
put_doc(含 vector) → DocValue 编码(vector 段) → data file (source of truth)
                    → IndexTask → worker:
                                    Index.put_doc + bm25 add_doc
                                    + hnsw_.insert(ord, vec)   ← V3 新增

search_vector(qvec, k, efSearch?) → cosine 归一化 → HNSW 搜索 (live 过滤)
                                  → SearchHit { key, ord, score=相似度 }

search_hybrid(query, qvec, k)     → BM25 top-K' ∥ HNSW top-K'
                                  → RRF(k=60) 融合 → top-k
```

- **RRF**：`score = Σ 1/(60 + rank_i)`，`K' = max(k×4, 64)`（两路各取）；无需分数归一化，与 `vector-db-design` 既定一致。
- **过滤式向量检索**（bool 条件 + 向量）：采用图内过滤方案，公共接口 `search_vector(..., const meta::MetaFilter* filter = nullptr)`；`filter` 与 `is_live` 组合成 HNSW 遍历回调，被拒节点从图遍历源头即不入候选、无需 overfetch。

### 7.1 search() 三阶段调度

`HnswIndex::search` 实现论文 Algorithm 5 + Phase 3 f32 精排：

```
search(query, k, ef):
  n = count_.load(acquire)                          // 快照水位
  entry = entry_meta_.load(acquire) & 0xFFFFFFFF
  cur = entry

  // Phase 1: 高层贪心下降
  for layer = max_level downto 1:
      cur = greedy_closest(query, cur, layer, n)

  // Phase 2: L0 束搜（可选 int8 粗筛）
  results = search_layer(query, cur, ef, 0, n, ...)

  // Phase 3: f32 精排
  partial_sort(results[0..k*3] by f32 distance)
  return top-k (live 过滤)
```

**Phase 1 / Phase 2 均可走 int8 路径**（VNNI 粗筛），**Phase 3 必走 f32 精排**——量化误差补偿。`inmem_int8=true` 时 Phase 3 退化为 int8 直读（无 f32 反量化）。

### 7.2 insert() 实现要点

`HnswIndex::insert` 实现论文 Algorithm 1，复用 `thread_local scratch / found / pool / qv` 缓冲消除每插入的栈分配：

```
insert(ord, vec):
  # 水位幂等
  if ord <= max_inserted_ord_.load(relaxed): return

  # 层数采样
  u = uniform_real_distribution(0, 1)(rng_)
  if u < 1e-12: u = 1e-12                     # 防 log(0)
  level = floor(-log(u) * inv_log_m_)          # mL = 1/ln(M)
  if level > 31: level = 31

  # 量化（一次 f32 → int8，存进 qcodes/qscales/qsums）
  quantize(vec, &qcodes[id], &qscale[id], &qsum[id])

  # 写者自身可见边界
  n_bound = id

  # Phase 1: 高层贪心
  for lc = max_level downto level+1:
      cur = greedy_closest(q, cur, lc, n_bound)

  # Phase 2: 每层 search_layer + select_neighbors + 双向连边
  for lc = min(max_level, level) downto 0:
      W = search_layer(q, cur, ef_construction, lc, n_bound, ...)
      M_cap = (lc == 0) ? 2M : M
      neighbors = select_neighbors(q, W, M_cap)
      add_bidirectional(neighbors, id, lc)
      for e in neighbors:
          if |e.adj[lc]| > M_cap:
              e_new = select_neighbors(q, e.adj[lc], M_cap)
              shrink_neighbors(e, lc, e_new)

  # entry 提升（仅在 level > max_level 时）
  if level > max_level:
      entry_meta_.store(((level+1) << 32) | id, release)

  max_inserted_ord_.store(ord, relaxed)
  count_.store(id+1, release)
```

**int8 路径**（`inmem_int8=true`）：所有距离计算走 `dist_id_int8_node(a, b)`（两节点量化副本），`select_neighbors_int8` 同样适用。

## 8. 持久化与恢复

持久化（BVH2 v2 段 + search.vec/search.qc8 双文件）、crc/watermark、covers 门、merge 重建、克隆活子图等由 [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) 完整负责。本节仅给交叉引用与本稿独有的设计要点。

**API**：

| 方法 | 含义 |
|------|------|
| `save_vec_payload(path)` | 把 `vecs_[0..count_)` 写入 `search.vec`（BCVP 格式：48B header + 4KB 页 CRC 表 + 页对齐数据；S14-2 起走追加路径，`vec_file_` 状态决定增量追加 vs 全量重写） |
| `load_vec_payload(path)` | `MAP_SHARED` mmap 只读 + `madvise(RANDOM)`；S14-2 前缀契约：接受 `header.count ≥ n`，只要求文件持有 `[0, n)` 前缀字节；version 1/2 均接受 |
| `save_qc_payload(path)` | 同 `save_vec_payload` 语义，但写 int8 码字（`search.qc8`）——S14-8 外置，BVH2 v3 段不再内嵌 |
| `load_qc_payload(path)` | mmap 加载码字 |
| `qc_payload_pending()` | v3 反序列化后、`load_qc_payload` 前为 true（v2 内嵌码字 / 无码字配置 → false） |
| `serialize(out)` | v2/v3 header → buf（供 `search.ckpt` `kHnsw` 段嵌入）；含 qcodes/qscales/qsums 直存 + has_payload 标志 + payload 元信息（dim/count/watermark） |
| `deserialize(bytes)` | v2/v3 header buf → 内存结构；不 mmap payload；调用方随后 `load_vec_payload()` |
| `save(base_path)` / `load(base_path)` | 测试便捷 wrapper：前者 = `save_vec_payload` + `serialize` → fwrite；后者 = fread → `deserialize` + `load_vec_payload` |

**S14-2 前缀不变契约**：追加只写 `offset ≥ 旧 count` 的数据尾区域——`ckpt` 声称的 `[0, n)` 前缀在任何 torn append 下保持完好；配合「先 `.vec` 后 `.ckpt` 原子发布」顺序，崩溃后要么用旧 `n`（尾部垃圾被忽略/下次覆盖）要么用新 `n`（数据已 `fdatasync`），方向恒安全。

**S14-8 payload 代号（generation nonce）**：v3 段头与 `.vec`/`.qc8` 头共同携带 `payload_gen`——rebuild 全量重写 payload 后若走 `.prev` 回退，旧图配新 payload（node id 已重映射）而单纯的「前缀 count ≥ n」会错误接受；代号不匹配即拒载（`gen==0` 的 legacy 文件跳过校验，首次 rebuild 后自动进入保护）。

**merge 重建**：调用 `HnswIndex::clone_live(is_live)` 做活子图结构化拷贝（O(N+E) memcpy 级，无距离计算）；死邻过滤的召回补偿是「一跳路径收缩」（借道死邻的活邻居补边）。reducer 提交 `IndexOp::RebuildHnsw`，与 worker 后续 put 任务同线程串行，无写写并发。

## 9. 删除语义（软删）

`HnswIndex` 不感知删除；上层经 `live` 回调（`std::function<bool(uint64_t)>`，由 `Index.live_` 位图驱动）在 `search` 结果侧滤死：

```cpp
[[nodiscard]] std::vector<Hit> search(
    std::span<const float> query, std::size_t k, std::size_t ef,
    const std::function<bool(std::uint64_t)>* live = nullptr) const;
```

死节点**仍参与图导航**（保持图连通性与导航质量），仅结果集过滤；`HnswIndex::clone_live`（merge 重建入口）做物理清除。这是 hnswlib / Lucene 的标准做法，死点比例高时靠 merge 收敛。

**已知边界**：结果侧过滤意味着 ef 候选内活者 < k 时返回不足 k——死文档占比高的邻域调用方需加大 ef；根治靠 merge 重建物理清除。

## 10. 实施阶段表（按落地版本）

| 步 | 内容 | 验证 |
|---|---|---|
| V3.1 | meta VectorConfig + DocValue vector 段读写打通（`put_doc`/`get` 透传） | 格式 round-trip 测试 + 黄金 fixture |
| V3.2 | HNSW 核心（单线程 insert/search，距离内核分发） | **召回对拍** vs 暴力 KNN：低维（32d/10k）recall@10 ≥ 0.95@ef64 / 0.99@ef256；高维纯随机（384d）按 ef=64/128/256/512 标定 0.824/0.960/0.996/1.000——距离集中使其成为最坏形态，真实 embedding 流形数据远易于此 |
| V3.3 | 并发化（per-node 序号锁 + 发布式增长）+ IndexPool 接线 | N 读 × 1 写并发测试；TSan 全插桩全绿 |
| V3.4 | 软删过滤 + LiveChecker 接入。机制随 V3.3 已在位（`Index.live_` 位图即 LiveChecker；`search_vector` 注入 `is_live` 回调；HNSW 结果侧滤死），V3.4 为语义证明：覆写测试（旧向量不可达，key 仅经新向量出现一次）+ 死区导航测试（删掉查询近邻 150/300 形成死壳，k=10 仍凑满、零死文档泄入、与活集暴力真值重合 ≥9/10） | 删除/覆写/死区三类可见性测试 ✅ |
| V3.5 | 持久化（BCVS 完整图快照 + covers 门并入 A4）+ merge 重建。`hnsw_` 改 `atomic<shared_ptr>`，merge 经 IndexPool 提交 RebuildHnsw 由 worker 重建换图（单写者保持）。**开库收益实测**（10k×384d, tmpfs）：快照 reopen 82ms vs 删 hnsw.snap 全量 fold 3770ms（≈46×） | A4 同款三件套（快照/全量等价 + 快照孤本可检索实证、陈旧尾部回放、位翻转回退）+ MergeRebuildEvictsDead（50→25 节点物理清死）+ ConcurrentSearchDuringRebuild；plain/TSan/ASan ctest 378/378（TSan 零报告）|
| V3.6 | search_hybrid RRF。`SearchLayer::search_hybrid`（两路 K'=max(k×4,64)、RRF k=60、rank 从 1 起；平局 → ord 小者；单路退化/双空报错语义定稿） | plain/TSan/ASan ctest 381/381（TSan 零报告）|
| V3.7 | 基准定稿（384d/M16/efC200，median of 3，`hnsw_bench.cpp` + `BM_Cask_SearchHybrid`，已入 `baseline.json`）：查询 10k/ef64 145µs、10k/ef256 463µs、**100k/ef64 483µs ✅（红线 <1ms）**、100k/ef256 1.67ms；hybrid 端到端 270µs（1 万条语料）；插入 10k 档 3.00k/s ✅、**100k 档 936/s ❌（红线 >2k/s 未达）**——归因：154MB 向量集远超 L3，增量插入近乎全打 DRAM（内存受限）；残差路线：① efC 降档、② 邻接 arena 化 + 向量预取、③ int8 量化（4× 带宽缩减）。100k 全量构建 ~107s 为一次性成本，此后 BCVS 快照 82ms 级开库 | 查询红线 ✅；插入红线 10k ✅ / 100k ❌（如实记录）|
| V3.8 | 4 路独立累加器 + 候选向量软件预取（无 ISA 特化，运行时通用内核；AVX-VNNI 留 int8 路径） | 查询 100k/ef64 483→322µs（−33%）、插入 100k 936→1221/s（+30%）|

## 11. 明确不做（V3 边界）

- 进程内 embedding 推理（调用方 embedder 解决）。
- 多字段多图（接口留位，V3.x）。
- 多写者并发插入（全引擎统一约束）。

## 12. 交叉引用

| 主题 | 文档 |
|------|------|
| 算法全景、跳表类比、采样公式 | [`hnsw-overview-zh.md`](hnsw-overview-zh.md) |
| 图论基础、O(log N) 推导、NodeChunk 字段对照、内存带宽分析 | [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) |
| 邻接块分配策略、bump arena 设计取舍 | [`hnsw-graph-construction-zh.md`](hnsw-graph-construction-zh.md) |
| 持久化（BVH2 段、search.vec、search.qc8、payload_gen、S14-2 前缀契约） | [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) |
| 各部署规模（384d/768d/2560d）内存账、chunk 数估算 | [`hnsw-memory-footprint-zh.md`](hnsw-memory-footprint-zh.md) |
| int8-only 模式（`inmem_int8=true`）全程 int8 寻路 | [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) |
| VNNI 内核指令、AVX-VNNI 边界 | [`int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md) |
| 整体可行性、混合检索定位 | [`vector-db-design-zh.md`](vector-db-design-zh.md) |
| checkpoint 体系、A4 合取式门 | [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md) |