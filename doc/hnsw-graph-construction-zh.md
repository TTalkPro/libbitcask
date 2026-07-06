# HNSW 建图算法：邻接表是怎么"算"出来的

> 前置阅读：`hnsw-overview-zh.md`（HNSW 算法综述）、`hnsw-design-zh.md`（V3 基础设计）、`hnsw-memory-footprint-zh.md`（邻接表内存占用）
> 参考文献：Malkov & Yashunin, "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs," [arXiv:1603.09320](https://arxiv.org/abs/1603.09320)
> 实现入口：`HnswIndex::insert`，定义于 `include/bitcask/hnsw.hpp` 的 `HnswIndex` 类，函数体在 `src/vector/hnsw.cpp`
> 本文回答：HNSW 如何决定每个点连哪些边，以及"相近向量"如何被组织进图——而非被划进区域。

## 0. 先纠正两个常见误区

**误区 1：HNSW 没有"邻接矩阵"。**
邻接矩阵是 N×N 稠密表，100 万节点 = 10¹² 格，存不下。HNSW 存的是**稀疏邻接表**：每节点最多 `M`（上层）或 `2M`（L0）个邻居 ID，本实现里是 `uint32` 数组（见 `NodeChunk::alloc_adj`，`hnsw.hpp` 的 `NodeChunk` 结构内），平均每节点约 34 个邻居。

**误区 2：HNSW 不"把相近向量划分到一个区域"。**
"分区/分桶"是 **IVF / 聚类（k-means）** 的思路——先切空间，查询只扫命中的桶。HNSW 不切空间，而是**把相近的点直接用边连成一张"可导航小世界图（NSW）"**，靠图上的**贪心游走**找近邻。

```
IVF/聚类:  切空间成桶 → 查命中桶          (分区)
HNSW:      连成可导航图 → 贪心游走找近邻   (连边) ← 本实现
```

所以正确的问题是：_它如何决定每个点连哪些边_。

## 1. 总览：逐点插入、增量建图

入口 `HnswIndex::insert(ord, vec)`（声明 `hnsw.hpp`，实现 `hnsw.cpp` 的 `HnswIndex::insert`）。单写者协议（见 `writer_active_` 守卫，`hnsw.cpp` 内 `insert` 函数体）。

每插入一个点 `id`，做四步：
1. 随机分层
2. 上层贪心下降找入口
3. 每层束搜索（`ef_construction` 宽）找候选
4. 启发式裁边 + 双向连边 + 超容收缩

## 2. 配置参数（`HnswConfig`）

定义于 `include/bitcask/hnsw.hpp` 的 `HnswConfig` 结构体：

| 字段 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `dim` | `uint16_t` | 无 | 向量维度（库内恒定） |
| `metric` | `HnswMetric` | `kDot` | `kDot`（内积，cosine 已在写入端归一化）或 `kL2`（平方欧氏） |
| `M` | `uint32_t` | `16` | 上层邻居容量；L0 容量 = `2M` |
| `ef_construction` | `uint32_t` | `200` | 构建期每层搜索的束宽 |
| `seed` | `uint64_t` | `0x5EEDF00D` | 层数采样种子（测试可复现） |
| `inmem_int8` | `bool` | `false` | P5：int8-only 模式（详见 `hnsw-int8-only-design-zh.md`） |

`M` 与 `ef_construction` 是图质量与建图成本的**唯一杠杆**：

- `M` 越大 → 每节点邻接越多 → 图越稠密 → 召回越高 + 内存越大 + 插入越慢。本实现固定 `M=16`（与 hnswlib 惯例一致）。
- `ef_construction` 越大 → 插入时束搜索探索越多 → 选出的邻居越全 → 图质量越高 + 插入越慢。默认 `200`，与论文 `efConstruction=100..200` 同档；高维部署可上调到 `400` 换取召回。

层数采样公式依赖 `mL = 1/ln(M)`，构造时一次性算好存 `inv_log_m_`（`HnswIndex` 构造函数）。

## 3. ① 随机分配层级

公式（`HnswIndex::insert` 体内的 level 采样）：

```cpp
double u = uniform(0,1);
if (u < 1e-12) u = 1e-12;       // 截断：u=0 → log 爆炸
auto level = static_cast<std::uint32_t>(-std::log(u) * inv_log_m_);
if (level > 31) level = 31;      // 截断防极端（高位少见）
```

其中 `inv_log_m_ = 1.0 / log(M)`（构造时预算，`HnswIndex` 构造函数）。这是一个**指数衰减分布**：

| 层 | M=16 时占比 | 角色 |
|---|---|---|
| L0 | 100% | 全量节点，密图精筛 |
| L1 | ~36% | 区域导航 |
| L2 | ~13% | 大区导航 |
| L3 | ~5% | 全局入口层 |
| L4+ | 递减 | 极少数"超长程导航点" |

> 注：占比指 `P(level ≥ l) = M^(-l)`。L0 包含所有节点（含更高层节点），L1 包含约 `1/M = 6.25%` 节点，依次类推。

高层点稀疏 → 充当"长途高速公路"，让贪心搜索能大步跳到目标大致区域。

## 4. ② 上层贪心下降找入口

从全局入口点出发，在每个高层（`max_level` 降到 `level+1`）上**只往"离我更近的邻居"走**，走到无法更近为止，把局部最优当下一层起点。函数：`HnswIndex::greedy_closest`（声明 `hnsw.hpp`，实现 `hnsw.cpp`）。

```cpp
while (improved) {
    improved = false;
    for (邻居 nid in cur 的 layer 层) {
        if (nid >= n) continue;                       // 协议：本地 count 之外跳过
        const float d = dist_id(q, nid);
        if (d < cur_d) { cur_d = d; cur = nid; improved = true; }
    }
}
return cur;
```

`dist_id` 内部走构造时按 metric+ISA 分发的距离函数（`HnswIndex::dist_`）。int8-only 模式下改用 `HnswIndex::greedy_closest_int8` 与 `HnswIndex::dist_id_int8`，全程 int8 距离。

意义：先在高层大步逼近，再到低层精修——这就是"分层"的价值。

## 5. ③ 每层束搜索找候选

函数：`HnswIndex::search_layer`（声明 `hnsw.hpp`，实现 `hnsw.cpp`）。

在第 `l` 层用 `ef_construction` 的束搜索（beam search）收集一批离 `id` 最近的候选。结构：

- **小顶堆 `cands`**（`std::priority_queue<Cand, std::vector<Cand>, std::greater<>>`）：出最近候选扩展。
- **大顶堆 `top`**（`std::priority_queue<Cand, std::vector<Cand>, std::less<Cand>>`）：维护当前 top-`ef`，满了踢最远。

```cpp
while (!cands.empty()) {
    auto [d, id] = cands.top();
    if (d > top.top().first && top.size() >= ef) break;   // 收敛
    cands.pop();
    // 拷出邻居的 layer 层 → scratch
    const auto cnt = copy_neighbors(id, layer, scratch);
    for (nid in scratch, nid < n) {
        if (visited[nid] == epoch) continue;
        visited[nid] = epoch;
        const float nd = dist_id(q, nid);
        if (top.size() < ef || nd < top.top().first) {
            cands.push({nd, nid});
            top.push({nd, nid});
            if (top.size() > ef) top.pop();
        }
    }
}
```

收敛条件：候选最远都比 `top` 顶还远，且 `top` 已满 `ef`，扩展无意义。

`ef_construction` 与最终图质量的关系：

| `ef_construction` | 期望效果 |
|---|---|
| 50 | 建图快；高维下召回明显下降（候选不足） |
| 100 | 常规 384d 召回 OK，建图中等 |
| **200**（默认） | 大多数维度召回饱和（≥ 0.99 vs `ef=∞`）；推荐起点 |
| 400 | 高维（≥1024d）或召回优先部署的微调上限；建图慢 ~2× |

经验：`ef_construction = 2·k` 是经验下限；再大主要换建图时间，召回饱和。

int8-only 与 f32+int8 路径的束搜索版本是 `HnswIndex::search_layer_int8`，结构与上同，只换 `dist_id` → `dist_id_int8`。VNNI 粗筛版的两阶段检索（int8 粗筛 → f32 精排）见 `HnswIndex::search` 主体；int8-only 取消精排阶段。

访问标记用 thread_local 版本化数组（`VisitedTable t_visited`，`hnsw.cpp` 内）。每线程一份 `{marks, epoch, owner}`：

- `owner` = HNSW 实例的全局自增 id（非 `this` 指针，避免 delete/new 复用导致陈旧 marks 与新实例 epoch 假性匹配）。
- `owner` 切换整组清零；同实例 `epoch` 自增免清零，回绕时清一次。
- 正确性：每次 search 开头取 `n` 快照，邻居 id `≥ n` 一律跳过（不在其本地可见边界内）。

## 6. ④ 启发式裁边 —— 核心（Algorithm 4）

函数：`HnswIndex::select_neighbors`（声明 `hnsw.hpp`，实现 `hnsw.cpp`）。

拿到一堆候选后，**不是简单选最近的 M 个**，而是按论文 Algorithm 4 启发式选 M 个：

```cpp
for (const auto& [d, id] : cands) {                 // cands 已按离 q 距离升序
    bool ok = true;
    const float* v = vec_of(id);
    for (const auto& [pd, pid] : picked) {
        if (dist_(v, vec_of(pid), cfg_.dim) < d)    // 离已选某点 比 离 q 还近？
            { ok = false; break; }                  // 是 → 丢弃（方向冗余）
    }
    if (ok) picked.push_back({d, id});              // 否 → 保留（方向新）
}
if (picked.size() < m) { /* 不足 m 用剩余最近者补齐（keepPruned 变体） */ }
```

**直觉**：一个候选，如果它离"已选中的某个邻居"比离"我自己（query）"还近，说明它和那个邻居挤在同一簇、方向冗余，丢掉。这样选出的 M 条边**朝不同方向分散**。

**为什么至关重要**：若只选最近的 M 个，聚簇数据里一个点会把边全用在同一团里，图被切成孤岛、跨簇走不通、召回崩。启发式裁边强制每点保留**几条指向不同方向（甚至跨簇）的边**，图才保持全局连通、可导航。

int8-only 版为 `HnswIndex::select_neighbors_int8`：候选与已选的距离比较改走 `HnswIndex::dist_id_int8_node`（两个量化副本之间的 int8 距离），启发式逻辑不变。

## 7. ⑤ 双向连边 + 超容收缩

HNSW 是**无向图**，连边要双向写，且度数有界（`layer_cap` 限 L0=`2M`、上层=`M`；`hnsw.hpp` 内 `HnswIndex::layer_cap`）。

### 7.1 正向边

把选中的 M 个写进 `id` 自己本层的邻接块。本节点已发布，持自身锁写：

```cpp
auto& my_seq = c->locks[slot];
seq_write_begin(my_seq);                             // seq → 奇
std::uint32_t* my = c->adj[slot] + layer_off(lay);
for (const auto& [d, nid] : picked) {
    adj_store(my + 1 + cnt, nid);                    // 用 atomic_ref relaxed 写
    ++cnt;
}
adj_store(my, cnt);                                  // 更新 count
seq_write_end(my_seq);                               // seq → 偶（release 发布）
```

锁是 per-node seqlock（`std::atomic<std::uint32_t>` 序号，`hnsw.hpp` 中 `NodeChunk::locks` 字段；写者侧 helper `seq_write_begin`/`seq_write_end`，`hnsw.cpp` 内）：写者奇→偶号包住 adj 更新，读者双读序号一致才采信（torn 读被重试丢弃）。并发查询时 hub 节点的锁缓存行不再乒乓——seqlock 的读侧是纯读，无写共享行。

### 7.2 反向边 + 超容收缩

把 `id` 加进每个邻居的邻接表，持邻居锁写。若该邻居本层边数已达上限，则**超容收缩**：

```cpp
auto& nseq = nc->locks[nslot];
seq_write_begin(nseq);
std::uint32_t* nb = nc->adj[nslot] + layer_off(lay);
const std::uint32_t cap = layer_cap(lay);            // L0=2M=32, 上层=M=16
const std::uint32_t ncnt = adj_load(nb);
if (ncnt < cap) {
    // 容量未满：直接追加
    adj_store(nb + 1 + ncnt, id);
    adj_store(nb, ncnt + 1);
} else {
    // 容量满：旧邻居 ∪ 新候选 并集 → 以 nid 为 query → 重选 cap 条
    pool.clear(); pool.reserve(cap + 1);
    for (i=1..ncnt) pool.push_back({dist(nid, pool_nb), pool_nb});
    pool.push_back({dist(nid, id), id});
    std::sort(pool.begin(), pool.end());
    select_neighbors(vec_of(nid), pool, cap);        // 重启发式
    for (i=0..pool.size()) adj_store(nb + 1 + i, pool[i].id);
    adj_store(nb, pool.size());
}
seq_write_end(nseq);
```

收缩路径在持锁状态下做距离计算（微秒级临界区），读者只在 `HnswIndex::copy_neighbors` 短暂争同一把锁。临界区长度受 `cap` 限（M=16 时 ≤32 次距离），实测可接受。

## 8. 层提升

若新点层级超过当前最高层，**完整连边后**才更新 `entry_meta_`（`std::atomic<std::uint64_t>`，高 32 位 = `level+1`，低 32 位 = `id`；声明于 `hnsw.hpp` 的 `HnswIndex` 私有区）：

```cpp
if (static_cast<std::int32_t>(level) > max_level) {
    entry_meta_.store(
        (static_cast<std::uint64_t>(level + 1) << 32) | id,
        std::memory_order_release);
}
```

**保证**：读者拿到的入口恒为已可达节点，无半连接可见。

## 9. 并发协议（单写者 + 多读者）

实现约束（`HnswIndex::insert` 的 `writer_active_` 守卫）：多写者**不支持**——debug assert 在 NDEBUG 下消失，但成员无条件存在以保 TU 间布局一致。

**写者发布序**（必须严格）：

1. 写满本节点数据：`vecs`/`qcodes`/`qscale`/`qsum`/`ords`/`levels` + 邻接块零初始化。
2. `count_.store(id + 1, release)`：从此读者可见本节点。
3. 连边：持目标节点 seqlock 改其 adj。
4. 最后（若需）`entry_meta_.store(..., release)`。

**读者协议**：

- 每次 search/rebuild 开头取一次 `entry_meta_`（acquire）再 `count_`（acquire）——entry 发布 happens-after 其 count 发布。
- 邻居列表里的 id 若 `≥ 本地 count`，一律跳过（并发插入的新节点尚未对本读者发布，等价"晚一拍可见"）。
- visited 数组按本地 count 定界 → 安全。

`HnswIndex::copy_neighbors` 读侧：seqlock 偶数 → relaxed 拷 adj → acquire fence → 复读 seqlock 一致才采信，否则重读。

`HnswIndex::clone_live`（merge 重建用）保留原图的层数与邻接结构，只做 id 重映射 + 死邻过滤 + 一跳路径收缩补边——O(节点+边) 的 memcpy 级拷贝，无距离计算（详见 `hnsw-lifecycle-zh.md`）。

## 10. 一句话总结

HNSW 的邻接表不是"算一个矩阵"，而是**每插入一个点，在图上贪心找到近邻候选，再用"方向去冗余"的启发式挑出 ~M 条分散的边双向连上**。
"相近的向量"不是被划进同一区域，而是**被直接连成邻居**；查询时从顶层入口贪心游走，几十跳逼近目标——近邻边保局部精度，启发式保留的长程边保跨簇跳转。

## 11. 关键符号索引

| 概念 | 代码位置 |
|---|---|
| `HnswConfig`（M / ef_construction / seed / dim / metric） | `include/bitcask/hnsw.hpp` 的 `HnswConfig` |
| 层数采样公式 `-ln(U) * inv_log_m_` | `HnswIndex::insert` 内（`src/vector/hnsw.cpp`） |
| 上层贪心下降 f32 版 / int8 版 | `HnswIndex::greedy_closest` / `HnswIndex::greedy_closest_int8`（`hnsw.cpp`） |
| 每层束搜索 f32 版 / int8 版 | `HnswIndex::search_layer` / `HnswIndex::search_layer_int8`（`hnsw.cpp`） |
| 启发式裁边 f32 版 / int8 版 | `HnswIndex::select_neighbors` / `HnswIndex::select_neighbors_int8`（`hnsw.cpp`） |
| 邻接块容量 / 层内偏移 | `HnswIndex::layer_cap` / `HnswIndex::layer_off`（`hnsw.hpp`） |
| 双向连边 + 超容收缩 | `HnswIndex::insert` 内循环（`hnsw.cpp`） |
| 单写者守卫 | `writer_active_` 与 `HnswIndex::insert` 内 debug assert |
| 节点发布水位 | `HnswIndex::count_`（`std::atomic<std::uint32_t>`，`hnsw.hpp`） |
| 入口 + max_level 合并单 atomic | `HnswIndex::entry_meta_`（`std::atomic<std::uint64_t>`，`hnsw.hpp`） |
| per-node seqlock | `NodeChunk::locks`（`hnsw.hpp`）；helpers 在 `hnsw.cpp` 匿名命名空间 |
| 重建活子图（merge 期 COW 语义） | `HnswIndex::clone_live`（`hnsw.hpp` 声明，`hnsw.cpp` 实现） |