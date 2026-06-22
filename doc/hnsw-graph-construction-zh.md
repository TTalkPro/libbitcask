# HNSW 建图算法：邻接表是怎么"算"出来的

> 前置阅读：`hnsw-design-zh.md`（基础设计）、`hnsw-graph-theory-zh.md`（数据结构与搜索复杂度）、`hnsw-memory-footprint-zh.md`（邻接表内存占用）
> 参考文献：Malkov & Yashunin, "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs," [arXiv:1603.09320](https://arxiv.org/abs/1603.09320)
> 状态：已实现（对照 `src/vector/hnsw.cpp`）
> 本文回答：HNSW 如何决定每个点连哪些边，以及"相近向量"如何被组织进图——而非被划进区域。

## 0. 先纠正两个常见误区

**误区 1：HNSW 没有"邻接矩阵"。**
邻接矩阵是 N×N 稠密表，100 万节点 = 10¹² 格，存不下。HNSW 存的是**稀疏邻接表**：每节点最多 `M`（上层）或 `2M`（L0）个邻居 ID，本实现里是 `uint32` 数组（`hnsw.cpp:707-710`），平均每节点才 ~34 个邻居。

**误区 2：HNSW 不"把相近向量划分到一个区域"。**
"分区/分桶"是 **IVF / 聚类（k-means）** 的思路——先切空间，查询只扫命中的桶。HNSW 不切空间，而是**把相近的点直接用边连成一张"可导航小世界图（NSW）"**，靠图上的**贪心游走**找近邻。

```
IVF/聚类:  切空间成桶 → 查命中桶          (分区)
HNSW:       连成可导航图 → 贪心游走找近邻   (连边) ← 本实现
```

所以正确的问题是：_它如何决定每个点连哪些边_。

## 1. 总览：逐点插入、增量建图

入口 `HnswIndex::insert(ord, vec)`（`hnsw.cpp:636`）。单写者协议（`hnsw.cpp:640`）。每插入一个点 `id`，做四步：①随机分层 → ②上层贪心下降找入口 → ③每层束搜索找候选 → ④启发式裁边 + 双向连边 + 超容收缩。

## 2. ① 随机分配层级（`hnsw.cpp:667-671`）

```cpp
double u = uniform(0,1);
auto level = static_cast<std::uint32_t>(-std::log(u) * inv_log_m_);  // inv_log_m_ = 1/ln(M)
if (level > 31) level = 31;  // 截断防极端
```

指数衰减：`P(level ≥ 1) ≈ 1/M = 1/16`。即约 **93.75% 的点只在 L0**，极少数爬到高层。
高层点稀疏 → 充当"长途高速公路"，让贪心搜索能大步跳到目标大致区域。

## 3. ② 上层贪心下降找入口（`greedy_closest`，`hnsw.cpp:375`）

从全局入口点出发，在每个高层（`max_level` 降到 `level+1`）上**只往"离我更近的邻居"走**，走到无法更近为止，把局部最优当下一层起点：

```cpp
const float d = dist_id(q, nid);
if (d < cur_d) { cur_d = d; cur = nid; improved = true; }  // 贪心：谁近去谁
```

意义：先在高层大步逼近，再到低层精修——这就是"分层"的价值。

## 4. ③ 每层束搜索找候选（`search_layer`，`hnsw.cpp:401`）

在第 `l` 层用 `efConstruction = 200` 的束搜索（beam search）收集一批离 `id` 最近的候选。
双优先队列：小顶堆 `cands` 出最近候选扩展，大顶堆 `top` 维护当前 top-ef（满了踢最远）。

```cpp
if (d > top.top().first && top.size() >= ef) break;   // 收敛：候选都比 top 里最远的还远
...
if (top.size() < ef || nd < top.top().first) {
    cands.push({nd, nid}); top.push({nd, nid});
    if (top.size() > ef) top.pop();
}
```

`ef` 越大候选越全、图质量越高，但建图越慢。访问标记用 thread_local 版本化数组（`vt.epoch`）避免每次清零。

## 5. ④ 启发式裁边 —— 核心（`select_neighbors`，`hnsw.cpp:570`）

拿到一堆候选后，**不是简单选最近的 M 个**，而是按论文 Algorithm 4 启发式选 M 个：

```cpp
for (const auto& [d, id] : cands) {               // 候选按离 q 距离升序
    bool ok = true;
    const float* v = vec_of(id);
    for (const auto& [pd, pid] : picked) {
        if (dist_(v, vec_of(pid), cfg_.dim) < d)  // 离已选某点 比 离 q 还近？
            { ok = false; break; }                // 是 → 丢弃（方向冗余）
    }
    if (ok) picked.push_back({d, id});            // 否 → 保留（方向新）
}
```

**直觉**：一个候选，如果它离"已选中的某个邻居"比离"我自己"还近，说明它和那个邻居挤在同一簇、方向冗余，丢掉。
这样选出的 M 条边**朝不同方向分散**。

**为什么至关重要**：若只选最近的 M 个，聚簇数据里一个点会把边全用在同一团里，图被切成孤岛、跨簇走不通、召回崩。启发式裁边强制每点保留**几条指向不同方向（甚至跨簇）的边**，图才保持全局连通、可导航。

不足 M 时用剩余最近者补齐（论文 keepPruned 变体，`hnsw.cpp:591-601`）。

> int8-only 模式有等价实现 `select_neighbors_int8`（`hnsw.cpp:607`），只把距离换成量化副本上的 `dist_id_int8_node`，启发式逻辑一致。

## 6. ⑤ 双向连边 + 超容收缩（`hnsw.cpp:767-815`）

HNSW 是**无向图**，连边要双向写，且度数有界：

- **正向边**（`hnsw.cpp:768-776`）：把选中的 M 个写进 `id` 自己本层的邻接块。本节点已发布，持自身自旋锁写。
- **反向边**（`hnsw.cpp:779-787`）：把 `id` 加进每个邻居的邻接表，持邻居锁写。
- **超容收缩**（`hnsw.cpp:788-813`）：若某邻居边数超过容量 `cap`（L0=2M，上层=M），把它的旧邻居 + 新点 `id` 放一起，**以该邻居为查询点重跑启发式选 `cap` 条**：

```cpp
if (nb[0] < cap) { nb[++nb[0]] = id; }   // 还有空位，直接加
else {
    // 旧邻居 ∪ 新点，以 nid 为 query 重选 cap 条（方向去冗余）
    ... pool ...; std::sort(pool); select_neighbors(vec_of(nid), pool, cap);
    // 覆盖写回 nb[]
}
```

保证：度数有界（内存可控，见 `hnsw-memory-footprint-zh.md`）+ 保留的仍是方向分散的边。
临界区为微秒级（持锁做距离计算），读者只在 `copy_neighbors` 短暂争同一把锁。

## 7. 层提升（`hnsw.cpp:818-822`）

若新点层级超过当前最高层，**完整连边后**才更新 `entry_meta_`（release）。保证读者拿到的入口恒为已可达节点，无半连接可见。

## 8. 一句话总结

HNSW 的邻接表不是"算一个矩阵"，而是**每插入一个点，在图上贪心找到近邻候选，再用"方向去冗余"的启发式挑出 ~M 条分散的边双向连上**。
"相近的向量"不是被划进同一区域，而是**被直接连成邻居**；查询时从顶层入口贪心游走，几十跳逼近目标——近邻边保局部精度，启发式保留的长程边保跨簇跳转。

## 9. 关键代码位置

| 步骤 | 函数 | 位置 |
|---|---|---|
| 插入主流程 | `HnswIndex::insert` | `hnsw.cpp:636` |
| 随机分层 | （inline） | `hnsw.cpp:667-671` |
| 上层贪心下降 | `greedy_closest` | `hnsw.cpp:375` |
| 每层束搜索 | `search_layer` | `hnsw.cpp:401` |
| 启发式裁边 | `select_neighbors` | `hnsw.cpp:570` |
| 双向连边 + 收缩 | （insert 内） | `hnsw.cpp:767-815` |
| 层提升 | （insert 内） | `hnsw.cpp:818-822` |
| 邻接块容量/偏移 | `layer_cap` / `layer_off` | `hnsw.hpp:211-220` |
