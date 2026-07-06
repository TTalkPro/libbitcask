# HNSW 多层图原理：数据结构、搜索复杂度与量化必要性

> 前置阅读：[`hnsw-overview-zh.md`](hnsw-overview-zh.md)（轻量概览）、[`hnsw-design-zh.md`](hnsw-design-zh.md)（设计 + 实现）、[`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md)（图生命周期与持久化）、[`int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md)（量化内核实现）
> 参考文献：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs,"* arXiv:1603.09320 (2016), TPAMI 2018 (DOI: 10.1109/TPAMI.2018.2889473)
> 状态：已实现（理论梳理文档）

本文按「§理论 → §实现」两段式编排：理论段从 «Malkov 2018» 论文复刻核心算法、数据结构与复杂度推导；实现段对照 `include/bitcask/hnsw.hpp` 中 `NodeChunk` / `HnswIndex` 的实际字段与并发协议。

---

# §理论 图论基础

## 1. Navigable Small World 图（NSW）

«Malkov 2018» 在 §2 给出 NSW 的直观描述：把图构造为一个**长程 hub + 局部邻接** 的混合结构，使任意两节点之间的路径长度期望是 `O(log N)`，而搜索过程可以**纯贪心**完成（每跳走最近邻居）。NSW 的 polylogarithmic 复杂度由「小世界图 + 贪心游走」共同保证——图本身只需 `O(N log N)` 条边，远小于 KD-tree 的 `O(N log N)` 但单跳信息量更大。

HNSW 的关键扩展（论文 §3）：把单层 NSW 升为**多层概率图**——

> *"Hierarchical NSW incrementally builds a multi-layer structure consisting from hierarchical set of proximity graphs (layers) for nested subsets of the stored elements. The maximum layer in which an element is present is selected randomly with an exponentially decaying probability distribution."*

直觉等价于：把 NSW 的「一层」换成「顶层稀疏 → 底层稠密」的金字塔。优势：

1. **搜索路径长度对数化**：高层只保留少量 hub 节点，每个跳步跨越大段距离——把单层 NSW 的 `O(log² N)` 降至 `O(log N)`。
2. **避免预洗牌**：单层 NSW 在增量构建时需要预洗牌输入以避免早期节点的 hub 偏差；HNSW 用**层数随机化**取代洗牌，真正支持 online insert。
3. **可扩展的 layer 局部性**：每层是一个独立的小图，layer 0（最密层）包含全部节点，layer `l > 0` 是 layer `l-1` 的随机稀疏子集。

## 2. 层级分配：指数衰减采样

«Malkov 2018» §3 + Algorithm 1 第 4 行明确给出新节点的层数采样公式：

```
l ← ⌊-ln(unif(0..1)) ∙ mL⌋
```

其中：

- `unif(0..1)` 是 `[0, 1)` 上均匀随机数；
- `mL = 1/ln(M)` 是层归一化常数（`M` 为每节点最大邻居数）；
- 取 `mL = 1/ln(M)` 等价于跳表参数 `p = 1/M`，即平均每节点在两层之间的「重叠比」为常数。

由此得到精确的层分布（论文 §4.2.1）：

```
P(level ≥ l) = exp(-l / mL) = M^(-l)
```

| 层 `l` | P(节点 ≥ 此层) | M=16 时数值 |
|--------|---------------|-------------|
| 0 | 100% | 1.000 |
| 1 | 1/M | 6.25% |
| 2 | 1/M² | 0.39% |
| 3 | 1/M³ | 0.024% |

最高层节点数 ≈ `N / M^(max_level)`，由此得期望层数 `E[max_level] = O(log_M N) = O(log N)`。

> **实现对照**：`HnswConfig::M = 16` → `inv_log_m_ = 1/log(M) ≈ 0.360674`；层数抽样在 `HnswIndex::insert` 内执行（`std::uniform_real_distribution<double>(0.0, 1.0)(rng_)` → `floor(-log(u) * inv_log_m_)`，上限 31 防溢出）。本实现沿用论文原公式，不做截断或偏差修正。

## 3. 插入算法（论文 Algorithm 1）

«Malkov 2018» Algorithm 1（`INSERT(hnsw, q, M, Mmax, efConstruction, mL)`）伪代码复刻：

```
W ← ∅
ep ← get enter point for hnsw
L ← level of ep                              // 当前最高层
l ← ⌊-ln(unif(0..1)) ∙ mL⌋                  // 新节点层数
for lc ← L … l+1
    W ← SEARCH-LAYER(q, ep, ef=1, lc)        // Phase 1：贪心下降（高层只用 1 个最近邻）
    ep ← get the nearest element from W to q
for lc ← min(L, l) … 0
    W ← SEARCH-LAYER(q, ep, efConstruction, lc)   // Phase 2：宽搜
    neighbors ← SELECT-NEIGHBORS(q, W, M, lc)      // Alg.4 HEURISTIC
    add bidirectional connections from neighbors to q at layer lc
    for each e ∈ neighbors
        eConn ← neighbourhood(e) at layer lc
        if |eConn| > Mmax                       // Mmax=2M when lc=0 else M
            eNewConn ← SELECT-NEIGHBORS(e, eConn, Mmax, lc)
            set neighbourhood(e) at layer lc to eNewConn
    ep ← W
if l > L
    set enter point for hnsw to q
```

关键阶段：

- **Phase 1（高层贪心）**：用 `ef=1` 调用 `SEARCH-LAYER`——单步贪心（每层只看最近的一个）。这一步纯粹做"高速公路"导航，把入口引到目标区域。
- **Phase 2（每层宽搜 + 连边）**：从 `min(L, l)` 起，每层用 `efConstruction` 搜出候选集 `W`，再用 `SELECT-NEIGHBORS`（论文 Algorithm 4）筛出 `M` 个**空间多样性**邻居，做双向连边。超容时调用同一启发式收缩旧节点的邻居表。
- **entry 提升**：仅当新节点层数超过当前最高层才换 `entry_point`——绝大多数插入不会触发。

> **实现对照**：`HnswIndex::insert(ord, vec)` 实现上述两阶段 + 连边 + entry 提升；`HnswIndex::select_neighbors` 复刻 Algorithm 4 的"按距离升序逐个入选，仅当比已选邻居都更近时保留"。

## 4. 搜索算法（论文 Algorithm 2 + Algorithm 5）

### 4.1 SEARCH-LAYER

«Malkov 2018» Algorithm 2（`SEARCH-LAYER(q, ep, ef, lc)`）伪代码复刻：

```
v ← ep                              // visited 集合
C ← ep                              // candidates（min-heap by dist to q）
W ← ep                              // dynamic list of ef closest（max-heap）
while |C| > 0
    c ← extract nearest element from C to q
    f ← get furthest element from W to q
    if distance(c, q) > distance(f, q)
        break                        // 收敛：剩余候选都比 W 中最差者更远
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

双集合结构：

- `C`（candidates）—— 待扩展的边界，按到 `q` 的距离升序弹出。
- `W`（dynamic list）—— 已发现的最近邻表，容量 ≤ `ef`，按到 `q` 的距离降序排列便于踢最远者。

**收敛条件**：当下一个待扩展候选 `c` 比 `W` 中最差者更远时，`W` 已稳定，可直接返回。这是 HNSW 束搜能 `O(ef × M)` 步结束的核心。

### 4.2 K-NN-SEARCH（完整查询）

论文 Algorithm 5（K-NN-SEARCH）即 `HnswIndex::search`：

```
SEARCH-KNN(hnsw, q, K, ef):
  W ← SEARCH-LAYER(q, ep, ef=efConstruction_layer, lc=0)
  return K nearest from W
```

但工程实现把高层 `SEARCH-LAYER`（`ef=1` 退化为纯贪心）和 L0 `SEARCH-LAYER`（`ef=ef` 宽搜）拆成两步走，对应本实现 `HnswIndex::search` 的三阶段调度：

1. **Phase 1（高层贪心）**：从顶层起，每层调 `greedy_closest(q, start, layer)`（即 `SEARCH-LAYER` 的 `ef=1` 退化）。
2. **Phase 2（L0 束搜）**：调 `search_layer(q, cur, ef, 0, ...)`。
3. **Phase 3（f32 精排）**：若启用了 int8 路径（`HnswConfig::inmem_int8` 或 `HnswMetric::kDot` 且有 VNNI），对 top-`k×3` 候选重算 f32 距离补偿量化误差。

> **量化必要性**：Phase 1+2 都可走 int8 粗筛；Phase 3 是 f32 精排。详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) 与 [`int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md)。

## 5. 邻居筛选算法（论文 Algorithm 4 / RobustPrune）

«Malkov 2018» §4.2.2 比较了两种策略：

- **Strategy 1 / Algorithm 3**（简单）：返回 `M` 个到 `q` 最近的候选——朴素 `top-K` 排序。
- **Strategy 2 / Algorithm 4**（多样性 / HEURISTIC）：逐候选考察，仅当**比已选邻居都更近**才入选，保证空间多样性。

论文原文结论（§4.2.2 末尾）：

> *"In all of the considered cases, use of the heuristic for proximity graph neighbors selection (alg. 4) leads to a higher or similar search performance compared to the naïve connection to the nearest neighbors (alg. 3). The effect is the most prominent for low dimensional data, at high recall for mid-dimensional data and for the case of highly clustered data."*

Algorithm 4 伪代码（论文原文复刻）：

```
SELECT-NEIGHBORS-HEURISTIC(q, C, M, lc, extendCandidates, keepPrunedConnections):
  R ← ∅
  W ← C
  if extendCandidates
      for each e ∈ C
          for each eadj ∈ neighbourhood(e) at layer lc
              if eadj ∉ W
                  W ← W ⋃ eadj
  Wd ← ∅
  while |W| > 0 and |R| < M
      e ← extract nearest element from W to q
      if e is closer to q compared to any element from R
          R ← R ⋃ e
      else
          Wd ← Wd ⋃ e
  if keepPrunedConnections
      while |Wd| > 0 and |R| < M
          R ← R ⋃ extract nearest element from Wd to q
  return R
```

核心不变量（第 9-14 行）：**入选的候选彼此之间到 `q` 的距离大于候选到 `q` 的距离**——数学上等价于「候选在 `q` 周围呈径向分布」，避免邻居全聚在同一簇。

> **实现对照**：`HnswIndex::select_neighbors` 与 `HnswIndex::select_neighbors_int8` 复刻该逻辑；本实现不启用 `extendCandidates`/`keepPrunedConnections`（标准数据集已足够）。

## 6. 复杂度论证：期望 O(log N)

«Malkov 2018» §4.2.1 给出严格证明（在 Delaunay 图假设下；实际 HNSW 用近似 Delaunay，论文论证迁移到经验性能）：

> *"Suppose we have found the closest element on some layer (this is guaranteed by having the Delaunay graph) and then descended to the next layer. One can show that the average number of steps before we find the closest element in the layer is bounded by a constant."*

推导链：

1. **每层内搜索步数为常数**：层采样概率 `p = exp(-mL)`，即「下一跳仍在同层」的概率；剩余「步数」的几何分布给出期望 `S = 1/(1 - exp(-mL))`。取 `mL = 1/ln(M)`，对所有常用 `M`（8-64）该值为 2-5 之间的常数，**与数据集大小无关**。
2. **层数期望为 O(log N)**：因 `P(max_level ≥ l) = M^(-l)`，期望层数 `E[max_level] = 1/(M-1) ≈ O(log_M N) = O(log N)`。
3. **总跳数 = 层数 × 每层常数 = O(log N)**。

论文还说明（§4.2.1 末尾）：

> *"And since the expectation of the maximum layer index by the construction scales as O(log(N)), the overall complexity scaling is O(log(N)), in agreement with the simulations on low dimensional datasets."*

> **本实现实测**：参考 [`hnsw-design-zh.md`](hnsw-design-zh.md) §「实施阶段」——100k×384d、M=16、ef=64，median 查询延迟 483µs（`BM_Cask_SearchHybrid`）；1M 向量集中仅访问 ~0.6% 节点（≈6000 个，与理论推导量级一致）。

## 7. 与距离度量的关系

«Malkov 2018» 明确算法是 **metric-agnostic**：

> *"The algorithm is applicable to general metric spaces, where similarity is defined by a distance function satisfying the triangle inequality."*

论文示例用 Euclidean L2 与 cosine distance；本实现支持：

- `HnswMetric::kDot`（内积，cosine 由上游归一化后等价）
- `HnswMetric::kL2`（平方欧氏距离，不开 int8 路径）

距离函数由 `HnswConfig` 在构造时分发为函数指针 `DistFn dist_`；int8 路径独立分发为 `int8::Int8DotFn int8_dot_`（无 VNNI 时为 `nullptr`，回落到纯 f32）。详见 [`hnsw-design-zh.md`](hnsw-design-zh.md) §「距离内核」。

---

# §实现 数据结构与代码对照

## 8. NodeChunk 内存布局

**设计约束**：插入序紧凑分配的内部 `id: u32`、构造时定容、生命周期内地址永不变（读者并发协议依赖此不变量）。

`include/bitcask/hnsw.hpp` 的 `NodeChunk` 字段对照（一个 chunk = `kChunkSize = 1<<16 = 65536` 个节点）：

| 字段 | 类型 | 大小 | 用途 |
|------|------|------|------|
| `vecs` | `std::vector<float>` | `kChunkSize * dim` × 4B | f32 常驻向量（仅 hot chunk 分配；mmap checkpoint 路径 `needs_vecs=false` 容量 0） |
| `qcodes` | `std::vector<std::int8_t>` | `kChunkSize * dim` × 1B | int8 对称量化副本（VNNI 检索用，4× 带宽缩减） |
| `qscales` | `std::vector<float>` | `kChunkSize` × 4B | 每向量一个 scale = `max |v[i]|`（int8 反量化系数） |
| `qsums` | `std::vector<std::int32_t>` | `kChunkSize` × 4B | 每向量一个 `Σcodes`（VNNI 偏置补偿） |
| `ords` | `std::vector<std::uint64_t>` | `kChunkSize` × 8B | 内部 `id → ord` 翻译（结果回传 + 删除水位） |
| `levels` | `std::vector<std::uint8_t>` | `kChunkSize` × 1B | 每个节点的最高层（决定邻接块大小） |
| `adj` | `std::vector<std::uint32_t*>` | `kChunkSize` × 8B | 每节点邻接块首指针（指向 bump arena，永不搬迁） |
| `adj_slabs` | `std::vector<std::unique_ptr<std::uint32_t[]>>` | 增长中 | bump arena slab 池（`kAdjSlabWords = 1<<18 = 256K u32 = 1MB`/slab） |
| `adj_slab_used` | `std::size_t` | 8B | 当前 slab 已用字数（bump pointer） |
| `locks` | `std::unique_ptr<std::atomic<std::uint32_t>[]>` | `kChunkSize` × 4B | per-node 序号锁（偶数=稳定，奇数=写者更新中） |

`NodeChunk::alloc_adj(n)` 从 `adj_slabs` 后端 bump `n` 个零初始化 `uint32_t`；slab 构造即全零，bump 区从不复用，无需再次清零。

**为什么是 bump arena 而不是 per-node `new[]`**：旧设计每节点一次 `new[]`，对 L0-only 节点而言 ~93.75% 的分配都浪费在 malloc 开销 + 碎片。arena 化后：

1. **零拷贝**：bump pointer O(1)，无 `malloc` 系统调用。
2. **插入序连续**：`adj_slabs` 内邻接块按插入序紧密排列——`copy_neighbors` 局部性更好，L1/L2 命中率提升。
3. **地址稳定不变量**：旧 slab 永不移动，迁移读者协议无需修改。

> 详细设计取舍见 [`hnsw-graph-construction-zh.md`](hnsw-graph-construction-zh.md)（Phase 2a 邻接分配专项）。

## 9. 邻接块内布局

每个节点的邻接块从 chunk 的 `adj_slabs` 一次性按层数分配，**层内布局 = `[count | ids...]`**（`count` 在前，`ids` 紧随其后）；跨层偏移由 `layer_off(layer)` 计算：

```
L0: count | id₁ id₂ ... id_{2M}    ← 容量 2M = 32（M=16）
L1: count | id₁ id₂ ... id_M      ← 容量 M
...
Ll: count | id₁ id₂ ... id_M      ← 节点最高层，层数由 levels[id] 决定
```

`layer_off(layer)` 在 `hnsw.hpp` 的 `HnswIndex::layer_off` 给出：

```
L0: offset 0
Ll (l>0): offset (1 + 2M) + (l-1) * (1 + M)
```

**总槽位** = `(1 + 2M) + levels * (1 + M)` 个 `uint32_t`。M=16 时单节点最大 `(1+32) + 31*(1+16) = 560 u32 = 2240 B`，远小于 `kAdjSlabWords = 1<<18 = 262144 u32`（1MB）——`NodeChunk::alloc_adj` 的 `assert(n <= kAdjSlabWords)` 兜底。

L0 容量 `2M` 而非 `M` 是 «Malkov 2018» §4 的明确建议：

> *"Simulations suggest that 2M is a good choice for Mmax0."*

L0 是最密层、节点全在，给更多容量可减少 recall 退化；上层 `M` 已足够（hub 节点度数高但层稀疏）。

## 10. chunk 目录与并发发布

`HnswIndex` 顶部成员（`hnsw.hpp` 私有区）：

```cpp
std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_{};        // 1024 项
std::atomic<std::uint32_t>                     count_{0};         // 发布水位
std::atomic<std::uint64_t>                     entry_meta_{0};   // 高32位=max_level+1，低32位=entry id
std::atomic<std::uint64_t>                     max_inserted_ord_{...}; // 崩溃回放水位
std::mt19937_64                                 rng_;            // 仅写者使用
```

`kMaxChunks = 1024` → 上限 `1024 × 65536 = 64M` 节点。**必须保持裸指针 + `atomic<NodeChunk*>`**：这是无锁发布协议的核心——`shared_ptr` 的原子引用计数开销不可接受（每次 `search` 都 load）。

### 10.1 count_ 发布序

```
insert(id) 写者:
  ① 写满本节点数据：vecs[id] = vec, qcodes[id] = codes, ords[id] = ord, levels[id] = level
  ② alloc_adj(总槽位数) → adj[id] = 指针（slab 增长，已对读者可见）
  ③ count_.store(id+1, release)             ← 节点"出生"
  ④ 再做连边（双向边可能在 count 之后对读者可见）
```

读者协议：

```
search() 读者:
  ① n = count_.load(acquire)                ← 本次查询的"快照水位"
  ② entry = entry_meta_.load(acquire) → 取低32位
  ③ 所有 copy_neighbors 返回的 nid，若 nid ≥ n → 跳过
  ④ visited 数组按 n 定界（n 之后的位置本就未初始化）
```

**关键不变量**：

- `count_` 是读者可见性的**唯一**分界。节点数据写入先于 `count_.store`；反向边追加可在 `count_` 之后对读者可见（写者此时还未完成邻居表的反向插入），但 `nid ≥ n` 的过滤保证读者永不读未出生节点。
- `entry_meta_` 的发布 happens-after `count_` 的相应位：search 入口先 `load entry_meta_`（acquire）再 `load count_`，保证 entry id 必 < 本地 `count`。

### 10.2 per-node 序号锁（V3.3）

旧设计是 1 字节自旋锁；新版改为 32 位序号锁（`NodeChunk::locks`，S13-P7）：

```
写者改邻接:
  ++locks[id]                                    // 奇数 = 更新中
  atomic_store_relaxed 数据字
  ++locks[id]                                    // 偶数 = 稳定

读者 copy_neighbors(id, layer, out):
  s0 = locks[id].load(acquire)
  if s0 % 2 == 1: 重读                           // 写者更新中
  atomic_load_relaxed 拷 [count][ids] 到 out
  s1 = locks[id].load(acquire)
  if s0 != s1: 丢弃重试                          // torn 读
  return out_cnt
```

**为什么从自旋锁换序号锁**：HNSW 流量高度偏向 hub 节点（旧 `writer_active_` 自旋锁的 `exchange` 在读者侧是写操作，多核共享同一缓存行时严重 ping-pong）。序号锁把读者路径降为纯读——写者单线程，`±1` 发布协议足够，读者双读序号一致才采信。`atomic_ref` relaxed 走数据字，UB-free 且 TSan 干净。

### 10.3 visited 表（thread_local 版本化数组）

```
struct VisitedTable {
    std::vector<std::uint32_t> marks;   // 按 node id 索引
    std::uint32_t              epoch = 0;
    std::uint64_t              owner = 0;   // HnswIndex 全局自增实例 id（防 ABA）
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

### 10.4 entry_meta_ 合并原子

`std::atomic<std::uint64_t> entry_meta_` 编码 `(max_level + 1) << 32 | entry_id`：

- `0` 表示空图。
- `insert` 完整连边后才 `store`；`search` 开头 `load(acquire)` 取低 32 位作为 entry。
- 一次 load 拿到完整入口（避免「先 load level 再 load id」的两次原子读之间的撕裂）。

## 11. 节点访问路径

`HnswIndex` 的内联访问器把"chunk 目录 → slot 偏移 → 标量"封装为单条语句：

```cpp
const float*      vec_of(id)        // mmap < checkpoint_count_ 走 mmap；否则走 chunk
const std::int8_t* qcodes_of(id)
float             qscale_of(id)
std::int32_t      qsum_of(id)
std::uint64_t     ord_of(id)
std::uint32_t     layer_cap(layer)  // layer == 0 ? 2M : M
std::size_t       layer_off(layer)  // 见 §9 公式
```

`dist_id(query, id)` 走 `dist_(query, vec_of(id), dim)`，是 `HnswConfig::metric` 构造时分发的函数指针（无 ISA 分支开销）；int8 路径 `dist_id_int8(query_codes, query_scale, query_sum, id)` 调用 `int8::Int8DotFn`（无 VNNI 时为 nullptr，回退 f32）。

## 12. 内存带宽与 int8 必要性

HNSW 图遍历的本质是**指针追逐（pointer chasing）**：邻居节点在内存中随机散布——图拓扑不保证空间局部性，CPU prefetcher 无法预测下一跳。

```
dist_id(node_id):
  chunks_[id >> 16].load(acquire)             // 随机 NodeChunk 地址
  chunk->qcodes.data() + (id & 0xFFFF) * dim   // 随机偏移
  读取 dim 个 int8（VNNI 32 元素/迭代）
```

| 路径 | 每节点读取 | 占缓存行 | L3 (32MB) 容纳 | L1 (48KB) 容纳 |
|------|-----------|---------|---------------|---------------|
| f32 (dim=384) | dim × 4B = 1536 B | 24 行 | ~14k 节点 | ~21 节点 |
| int8 (dim=384) | dim × 1B = **384 B** | **6 行** | **~44k 节点** | **~66 节点** |

int8 把 L3 cliff 从 14k 推到 **44k**——**3.1× 延迟收益**（与 `BM_Cask_SearchHybrid` 实测相符）。计算时间对比：int8 VNNI 点积（dim=384）约 50ns，DRAM miss 约 80-100ns——**CPU 在等内存而非做计算**，所以压缩访存是首要收益。

**两阶段检索的精度补偿**：

```
Phase 1 (int8 粗筛): 图遍历，拿 ef 个候选      ← 追求速度
Phase 2 (f32 精排):  仅对 k×3 候选算 f32 距离  ← 追求精度
```

Phase 2 只算 ~30 个 f32 距离（~5µs），不影响整体延迟，但补偿量化误差。实测 recall@10 > 95%（`hnsw-design-zh.md` 实施阶段 V3.2 实测：384d 随机数据 ef=64/128/256/512 收敛曲线 0.824 / 0.960 / 0.996 / 1.000）。

量化误差被 HNSW 本身的算法近似误差所主导——即使 f32 精确距离，HNSW 也只返回近似最近邻。int8 引入的额外误差在此背景下可忽略。详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) 与 [`int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md)。

## 13. 与 Phase 2a 文档的分工

本文件聚焦「理论 + 数据结构 + 代码字段对照」；以下主题由 Phase 2a 完成的专项文档负责，避免重复：

| 主题 | 专项文档 |
|------|---------|
| 邻接块分配策略、bump arena 设计取舍、`assert` 兜底 | [`hnsw-graph-construction-zh.md`](hnsw-graph-construction-zh.md) |
| 持久化（BCVS/BVH2 段、search.ckpt/search.vec/search.qc8 三文件）、crash 回放、merge 重建 | [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) |
| 各部署规模（384d/768d/2560d）的内存账、chunk 数估算 | [`hnsw-memory-footprint-zh.md`](hnsw-memory-footprint-zh.md) |
| int8-only 模式（`inmem_int8=true`）的全程 int8 寻路、量化副本回收 | [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) |

## 14. 相关符号索引

| 符号 | 位置 | 含义 |
|------|------|------|
| `HnswIndex` | `include/bitcask/hnsw.hpp` | 主索引类（公共接口） |
| `NodeChunk` | `include/bitcask/hnsw.hpp` 私有区 | chunk 内节点定容容器 |
| `HnswConfig` | `include/bitcask/hnsw.hpp` | dim / metric / M / ef_construction / seed / inmem_int8 |
| `kChunkBits` / `kChunkSize` / `kChunkMask` | `HnswIndex` 静态成员 | chunk 几何参数 |
| `kMaxChunks` | `HnswIndex` 静态成员 | 目录上限（64M 节点） |
| `kAdjSlabWords` | `HnswIndex` 静态成员 | bump arena slab 大小 |
| `chunks_` / `count_` / `entry_meta_` / `max_inserted_ord_` | `HnswIndex` 私有成员 | 并发原子 |
| `inv_log_m_` | `HnswIndex` 私有成员 | `mL = 1/ln(M)` |
| `insert` / `search` / `search_layer` / `greedy_closest` / `copy_neighbors` / `select_neighbors` | `HnswIndex` 成员函数 | 论文 Alg.1-4 的工程复刻 |
| `dist_id` / `dist_id_int8` / `dist_id_int8_node` | `HnswIndex` 内联 | 距离内核入口 |
| `int8::Int8DotFn` | `include/bitcask/detail/int8_kernels.hpp` | VNNI 内核函数指针 |
| `layer_off` / `layer_cap` | `HnswIndex` 内联 | 邻接块内层偏移与容量 |
| `vec_of` / `qcodes_of` / `qscale_of` / `qsum_of` / `ord_of` | `HnswIndex` 内联 | 节点字段访问器 |