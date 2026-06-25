# HNSW 算法全景

HNSW 近似最近邻（ANN）索引的算法原理、优势劣势，以及本实现的工程优化。配合以下文档阅读：

- [`hnsw-design-zh.md`](hnsw-design-zh.md) — 本实现的设计细节（并发协议 / 持久化 / 实施表）
- [`hnsw-graph-theory-zh.md`](hnsw-graph-theory-zh.md) — 图论基础
- [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md) — 生命周期管理
- [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) — int8 量化设计

权威头文件：`include/bitcask/hnsw.hpp`（`bitcask::vec::HnswIndex`）。

---

## 一、全称

**H**ierarchical **N**avigable **S**mall **W**orld graphs — **分层可导航小世界图**

> 论文：Malkov & Yashunin, *"Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World Graphs"*, arXiv:1603.09320 (2016), TPAMI (2018)。本实现的算法参考写在 `hnsw.hpp:3-7`。

名字拆解三个概念：

| 概念 | 含义 |
|------|------|
| **Small World（小世界）** | 图的直径很短——类似"六度分隔"，任意两节点间只需少量跳数可达 |
| **Navigable（可导航）** | 贪心搜索能沿着邻居链高效逼近目标（每跳距离单调下降） |
| **Hierarchical（分层）** | 多层图结构，上层稀疏（少数节点做长距离跳跃），下层密集（全部节点做精细搜索） |

---

## 二、核心算法原理

### 2.1 分层结构（灵感来自跳表 skip-list）

```
Layer 2:    ●─────────────────●──────────────●        ← 最稀疏（长距离跳跃）
            │                 │              │
Layer 1:    ●─────●─────●─────●──────●───────●─────●  ← 中间层
            │     │     │     │      │       │     │
Layer 0:    ●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─●─● ← 全部节点（精细搜索）
```

- **每个节点随机分配到一个最大层数** `l`，出现在 `[0, l]` 所有层。
- 层数采样公式：`l = floor(-ln(uniform(0,1)) × mL)`，其中 `mL = 1/ln(M)`（`hnsw.hpp:7,283`）。
- 指数衰减 → 高层节点极少（层级几何分布 `mL=1/ln(M)`，`M=16` 时 P(level≥1)≈1/M≈6.25%，
  即约 93.75% 节点只在 L0，逐层约按 1/M 衰减）。
- **第 0 层（L0）包含全部节点**；邻居容量 `2M`；上层各 `M` 个（`hnsw.hpp:66,198-199`）。

### 2.2 搜索流程（`search()`，`hnsw.hpp:94-96`）

```
search(query, k, ef):
  ① 从顶层 entry_point 出发
  ② 逐层贪心下降（layer = top → 1）:
       每层只找最近的 1 个节点作为下一层入口       ← greedy_closest()
       （上层稀疏，O(log N) 步逼近目标区域）
  ③ 到达 L0（最密层）时展开搜索宽度:
       维护 ef 大小的候选优先队列                   ← search_layer()
       贪心扩展邻居，直到队列稳定
  ④ 从候选队列中取 top-k 返回
```

- `ef`（exploration factor）控制精度与速度的权衡：`ef` 越大 → 召回率越高 → 越慢。
- `ef ≥ k`（内部取 `max(ef, k)`）。
- 死节点仍参与导航（贪心路径上的路标），结果侧用 `live` 回调过滤（`hnsw.hpp:39-40,93-96`）。

### 2.3 插入流程（`insert()`，`hnsw.hpp:86`）

```
insert(ord, vec):
  ① 随机采样新节点层数 l
  ② 从顶层贪心下降到 l+1 层（每层只跟踪最近的 1 个）
  ③ 从 l 层往下，每层:
       search_layer 找 ef 个最近候选
       select_neighbors 启发式选 M 个             ← 论文 Algorithm 4
       双向连边（新节点 → 邻居，邻居 → 新节点）
       超容时收缩（删最远邻居）
  ④ 如果 l > 当前 max_level → 更新 entry_point
```

**邻居选择启发式**（`select_neighbors`，`hnsw.hpp:267-272`）：

> 候选若离 query 比离任一已选邻居更近才保留——避免聚簇数据上邻居全挤在同一方向。

这是 HNSW 相比 NSW 的关键改进——保证图的**多样性连接**，不同方向的邻居都能保留，使贪心搜索不会陷入死角。

### 2.4 关键参数

| 参数 | 本实现默认 | 含义 |
|------|-----------|------|
| `M` | 16 | 每节点邻居容量（上层 `M`，L0 层 `2M`） |
| `ef_construction` | 200 | 建图时的搜索宽度 |
| `ef` | 调用方指定 | 查询时的搜索宽度（`≥ k`） |
| `mL` | `1/ln(M)` | 层数生成的指数衰减参数 |

---

## 三、优势

| 优势 | 说明 |
|------|------|
| **对数级搜索复杂度** | `O(log N)` 而非暴力 `O(N)` 或 KD-tree 的 `O(N^(1-1/d))` |
| **高召回率** | `ef` 调大可逼近精确搜索；典型 `ef=200` 召回率 >95% |
| **增量构建** | 随时 `insert()`，不需要像 LSH / PQ 那样预训练 |
| **任意距离度量** | 只需提供距离函数。本实现支持 `kDot`（内积，cosine 由上游归一化）和 `kL2`（平方欧氏）（`hnsw.hpp:58-61`） |
| **低延迟** | 百万级数据集查询 <1ms |

---

## 四、劣势

| 劣势 | 说明 |
|------|------|
| **内存占用大** | 图结构 + 向量常驻内存。本实现用 chunk 分段缓解（每 chunk 65536 节点定容预分配，`hnsw.hpp:141-168`） |
| **构建慢** | 每节点插入需 `O(M × log N)` 次距离计算 |
| **近似而非精确** | ANN ≠ NN，不保证返回真正的最近邻 |
| **参数敏感** | `M` / `ef_construction` / `ef` 需按数据特性调参 |
| **删除困难** | 软删除：HnswIndex 无 `mark_deleted` API——死节点经查询时的 `live` 回调（`hnsw.hpp:93-97`）在结果侧过滤，仍参与图导航；物理清除靠 merge 重建（`rebuild_hnsw`） |
| **高维退化** | 超高维（>1000）下距离区分度下降，但远好于 KD-tree |

---

## 五、本实现的工程优化

本实现不是论文的直译，有大量工程级优化。完整设计见 [`hnsw-design-zh.md`](hnsw-design-zh.md)，以下为要点速览。

### 5.1 chunk 分段 + 无锁发布协议（`hnsw.hpp:22-36,140-168`）

```
chunks_: std::array<std::atomic<NodeChunk*>, 1024>   ← 64M 节点上限
  每个 NodeChunk 定容预分配 vecs/ords/levels/adj/locks
  写者 store-release 安装 chunk；读者 load-acquire 读取
```

- **单写者 + 多读者**：insert 由 IndexPool 单 worker 串行执行；search 多线程并发。
- **per-node 自旋锁**（1 字节）：写者改邻接时持锁；读者拷贝到栈缓冲后放锁。
- **entry_meta_ 合并原子**：高 32 位 = max_level+1，低 32 位 = entry id — 一次 load 拿到完整入口。
- **count_ 发布序**：先写满节点数据 → `count_.store(id+1, release)` → 再连边 → 读者以 load 的 count 为可见边界。

### 5.2 int8 量化 + VNNI 两阶段检索（`hnsw.hpp:69-72,156-161,180-233`）

```
查询时:
  ① int8 粗筛（上层 + L0 search_layer）  ← VNNI 指令 4× 带宽缩减
  ② f32 精排（仅 top-k 附近）             ← 全精度 rerank

可选 inmem_int8=true:
  NodeChunk 不存常驻 f32（vecs 容量 0）
  全程 int8，向量内存 ~−80%
```

详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md) 与 [`simd-vnni-internals-zh.md`](simd-vnni-internals-zh.md)。

### 5.3 水位幂等 + 快照持久化

- `max_inserted_ord_` 水位：重复 `insert` 同 ord 直接丢弃，崩溃回放安全。
- 持久化（V7 两文件）：BVH2 v2 段（qcodes/邻接/ords/levels + entry/count）嵌入统一
  `search.ckpt`，f32 向量字节流单独写 `search.vec`（BCVP，mmap 只读 + 每页 CRC）。
  `serialize`/`deserialize` + `save_vec_payload`/`load_vec_payload`（`hnsw.hpp`）。
  （早期单文件 `hnsw.snap` BCVS v1 `[magic][ver][payload][crc]` 已被取代；`save()/load()`
  仅存留为测试包装。）见 [`hnsw-lifecycle-zh.md`](hnsw-lifecycle-zh.md)。

---

## 六、一句话总结

> HNSW 用**分层跳表式图结构**把 ANN 搜索降到 `O(log N)`：上层稀疏图做"长途飞行"快速定位区域，底层密集图做"精细步行"逼近最近邻。代价是常驻内存和构建时间。本实现通过 chunk 分段无锁发布 + int8 量化 VNNI 把延迟和内存压到工程可接受范围。
