# HNSW 算法全景

HNSW 近似最近邻（ANN）索引的算法原理、参数语义与本实现的工程优化点速览。配合以下文档阅读：

- [`hnsw-design-zh.md`](hnsw-design-zh.md) — 本实现的并发协议、持久化、实施阶段表
- [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) — 图论基础（O(log N) 推导 / RobustPrune）
- [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) — 生命周期与持久化（Phase 2a 已完成）
- [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) — int8 量化与 VNNI 内核

权威头文件：`include/bitcask/hnsw.hpp`（`bitcask::vec::HnswIndex`）。

---

## 一、全称

**H**ierarchical **N**avigable **S**mall **W**orld graphs — **分层可导航小世界图**

> 论文：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World Graphs"*, arXiv:1603.09320 (2016), TPAMI (2018)。本实现的算法参考写在 `hnsw.hpp` 头注释（"算法参考文献"小节），由 `select_neighbors()` 内部 `HnswIndex::select_neighbors` 即论文 Algorithm 4（SELECT-NEIGHBORS-HEURISTIC）的工程复刻。

名字拆解三个概念：

| 概念 | 含义 |
|------|------|
| **Small World（小世界）** | 图的直径很短——任意两节点间只需少量跳数可达（六度分隔的图论版） |
| **Navigable（可导航）** | 贪心搜索能沿着邻居链高效逼近目标（每跳距离单调下降） |
| **Hierarchical（分层）** | 多层图结构，上层稀疏（少数节点做长距离跳跃），下层密集（全部节点做精细搜索） |

---

## §理论 核心算法

### 2.1 分层结构（灵感来自跳表）

«Malkov 2018» 把 HNSW 直接对应到概率跳表结构：

> *"Hierarchical NSW algorithm can be seen as an extension of the probabilistic skip list structure with proximity graphs instead of the linked lists."* — arXiv:1603.09320 §3

```
Layer 2:    ●─────────────────●──────────────●        ← 最稀疏（长距离跳跃）
            │                 │              │
Layer 1:    ●─────●─────●─────●──────●───────●─────●
            │     │     │     │      │       │     │
Layer 0:    ●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─● ← 全部节点（精细搜索）
```

- **每个节点随机分配到一个最大层数** `l`，出现在 `[0, l]` 所有层。
- **层数采样公式**（论文 Algorithm 1 第 4 行）：`l = floor(-ln(uniform(0,1)) × mL)`，其中 `mL = 1/ln(M)`。
- 指数衰减 → 高层节点极少；几何分布使层数期望为 `O(log N)`，对应概率 `P(level ≥ l) = exp(-l / mL) = M^(-l)`。
- **L0 包含全部节点**，上层仅保留部分节点。

### 2.2 搜索流程（对应论文 Algorithm 5 K-NN-SEARCH）

```
search(query, k, ef):
  ① 从顶层 entry_point 出发
  ② 逐层贪心下降（layer = top → 1）：
       每层只跟踪最近的 1 个节点作为下一层入口          ← 论文 Alg.2 + ef=1 退化为贪心
  ③ 到达 L0（最密层）时展开搜索宽度：
       维护 ef 大小的动态候选集                         ← 论文 Alg.2（SEARCH-LAYER, ef=ef）
       双集合：候选集合 C（closest）+ 结果集合 D（ef 个最近）
  ④ 从结果集合取 top-k 返回
```

- `ef`（exploration factor）控制精度与速度的权衡：越大 → 召回率越高 → 越慢。
- `ef ≥ k`（`HnswIndex::search` 内部取 `max(ef, k)`）。
- 死节点仍参与导航（图内路径路标），结果侧用 `live` 回调过滤——参见 [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) 与 `hnsw.hpp` 顶部"删除"小节。

### 2.3 插入流程（对应论文 Algorithm 1 INSERT）

```
insert(ord, vec):
  ① 随机采样新节点层数 l = floor(-ln(uniform(0,1)) × mL)
  ② 从顶层贪心下降到 l+1 层（每层 ef=1 跟踪最近的 1 个）   ← 论文 Alg.1 L5–L7
  ③ 从 l 层往下，每层：
       search_layer 找 efConstruction 个最近候选            ← 论文 Alg.1 L8–L9
       select_neighbors 启发式选 M 个                       ← 论文 Alg.4（HEURISTIC）
       双向连边（新节点 → 邻居，邻居 → 新节点）
       超容时调用 select_neighbors 收缩邻居（Alg.1 L12–L16）
  ④ 如果 l > 当前 max_level → 更新 entry_point             ← 论文 Alg.1 L18
```

**邻居选择启发式**（`HnswIndex::select_neighbors` 复刻论文 Algorithm 4）：

> 候选按到 query 的距离升序逐个考察：仅当候选到 query 的距离小于其到任一已选邻居的距离时入选——避免聚簇数据上邻居全挤在同一方向。

这是 HNSW 相对 NSW 的关键改进，保证**多样性连接**，不同方向的邻居都能保留，贪心搜索不会陷入死角。`extendCandidates`/`keepPrunedConnections` 在本实现中未启用（标准聚类数据集足够）。

### 2.4 关键参数

| 参数 | 本实现默认 | 含义 |
|------|-----------|------|
| `M` | 16（`HnswConfig::M`） | 每节点邻居容量（上层 `M`，L0 层 `2M`） |
| `ef_construction` | 200（`HnswConfig::ef_construction`） | 建图时每层的搜索宽度（论文 Alg.1 输入） |
| `ef` | 调用方指定 | 查询时 L0 的搜索宽度（`≥ k`） |
| `mL` | `1/ln(M)`（`inv_log_m_`） | 层数生成的指数衰减参数 |

`M=16` 时 `mL ≈ 0.36`，层间节点比例约 `1/M = 6.25%`（Malkov 公式 `P(level ≥ l) = M^(-l)`），最高层节点数 ≈ `O(log N)`。

---

## §实现 本实现的工程优化速览

本实现不是论文的直译，有大量工程级优化。完整设计与并发论证见 [`hnsw-design-zh.md`](hnsw-design-zh.md)，量化与 VNNI 见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)。以下为要点速览。

### 5.1 chunk 分段 + 无锁发布协议

`HnswIndex` 用 `chunks_: std::array<std::atomic<NodeChunk*>, kMaxChunks>` 作为目录（`kMaxChunks=1024` → 64M 节点上限）；每个 `NodeChunk` 定容预分配 `vecs/ords/levels/adj/locks`，地址永不变。

- **单写者 + 多读者**：`insert` 由 `IndexPool` 单 worker 串行执行；`search` 多线程并发（与 `live` 回调一致）。
- **per-node 序号锁**（`NodeChunk::locks`，见 `hnsw.hpp` 顶部"并发协议"小节）：写者包住邻接更新；读者双读序号一致才采信。
- **`entry_meta_` 合并原子**（`std::atomic<std::uint64_t>`）：高 32 位 = `max_level+1`（0 表示空图），低 32 位 = `entry id`——一次 load 拿到完整入口。
- **`count_` 发布序**：先写满节点数据 → `count_.store(id+1, release)` → 再连边。读者以 load 的 `count` 为可见边界。

### 5.2 int8 量化 + VNNI 两阶段检索

```
查询时:
  ① int8 粗筛（上层 + L0 search_layer）  ← VNNI 指令 4× 带宽缩减
  ② f32 精排（仅 top-k 附近）             ← 全精度 rerank
```

可选 `HnswConfig::inmem_int8 = true`：`NodeChunk::vecs` 容量 0，全 int8 寻路，向量内存减约 80%。详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) 与 [`simd-vnni-internals-zh.md`](simd-vnni-internals-zh.md)。

### 5.3 水位幂等 + BCVS 双文件持久化

- `max_inserted_ord_` 水位：重复 `insert` 同 ord 直接丢弃，崩溃回放安全。
- 持久化：`BVH2 v2` 段（qcodes/邻接/ords/levels + entry/count）嵌入统一 `search.ckpt`；f32 向量字节流单独写 `search.vec`（BCVP，mmap 只读 + 每页 CRC）；int8-only 模式跳过 `.vec`。
- API：`HnswIndex::serialize/deserialize`、`save_vec_payload/load_vec_payload`、`save_qc_payload/load_qc_payload`（int8 码字外置 → `search.qc8`）。详见 [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md)。

---

## 三、优势与劣势

### 优势

| 优势 | 说明 |
|------|------|
| **对数级搜索复杂度** | `O(log N)` 期望（详见 [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) §2），而非暴力 `O(N)` 或 KD-tree 的 `O(N^(1-1/d))` |
| **高召回率** | `ef` 调大可逼近精确搜索；典型 `ef=200` 召回率 >95% |
| **增量构建** | 随时 `insert()`，不需要像 LSH / PQ 那样预训练 |
| **任意距离度量** | 只需提供距离函数。本实现支持 `HnswMetric::kDot`（内积，cosine 由上游归一化）和 `HnswMetric::kL2`（平方欧氏） |
| **低延迟** | 百万级数据集查询 <1ms（`BM_Cask_SearchHybrid` baseline） |

### 劣势

| 劣势 | 说明 |
|------|------|
| **内存占用大** | 图结构 + 向量常驻内存；本实现用 chunk 分段（每 chunk 65536 节点定容）缓解 |
| **构建慢** | 每节点插入需 `O(M × efConstruction × log N)` 次距离计算 |
| **近似而非精确** | ANN ≠ NN，不保证返回真正的最近邻 |
| **参数敏感** | `M` / `ef_construction` / `ef` 需按数据特性调参 |
| **删除困难** | 软删除：`HnswIndex` 无 `mark_deleted` API——死节点经 `live` 回调在结果侧过滤，仍参与图导航；物理清除靠 merge 重建（`HnswIndex::clone_live`） |
| **高维退化** | 超高维（>1000）下距离区分度下降，但仍远好于 KD-tree |

---

## 四、一句话总结

> HNSW 用**分层跳表式图结构**把 ANN 搜索降到 `O(log N)`：上层稀疏图做"长途飞行"快速定位区域，底层密集图做"精细步行"逼近最近邻。代价是常驻内存和构建时间。本实现通过 chunk 分段无锁发布 + int8 量化 VNNI 把延迟和内存压到工程可接受范围。