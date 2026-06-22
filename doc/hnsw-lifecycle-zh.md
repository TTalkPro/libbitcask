# HNSW 图生命周期：构建、持久化与恢复

> 前置阅读：`hnsw-design-zh.md`（V3 HNSW 基础设计）、`int8-vnni-v4-zh.md`（V4.2 量化检索）
> 状态：已实现（文档化梳理）

## 1. 概述

HNSW 多层图经历三个阶段：**增量构建**（put 时逐节点插入）、**全量重建**（merge 时物理清死）、**快照持久化**（close/merge 后落盘）。本文档梳理完整生命周期。

## 2. 增量构建（put 路径）

### 2.1 调用链

```
Cask::put_doc(vec)          [cask.cpp]
  → SearchLayer::on_vector() [search_layer.cpp]
    → HnswIndex::insert()    [hnsw.cpp:643]
```

索引任务在 IndexPool worker 线程串行执行（单写者约束）。put 返回前不阻塞等图构建完成——向量已随 DocValue 落入 data 文件（唯一 WAL），插图任务入 IndexPool 队列由 worker 线程异步消费。

### 2.2 insert() 内部流程

`hnsw.cpp:643`

```
insert(ord, vec):
  1. 水位幂等检查：ord <= max_inserted_ord_ → 拒绝（防重复）
  2. 分配 node id，定位 NodeChunk（地址稳定的定容块）
  3. 随机采样层数：
     u ~ Uniform(0, 1)
     level = floor(-ln(u) * inv_log_m_)   // inv_log_m_ = 1/ln(M)，M=16 → mL≈0.36
     cap at 31
  4. 写入：vec f32 + int8 量化副本 + ord + level
  5. 分配邻接表空间：(1 + 2M) + level × (1 + M)
  6. 发布节点：count_.store(id+1, release)
  7. 首节点 → 设 entry_meta_，return
  8. 从 entry point 贪心下降到 level+1 层（仅导航，不连边）
  9. 逐层（min(level, max_level) → 0）：
     a. search_layer() 找 ef_construction 个候选
     b. select_neighbors()（HNSW Algorithm 4）选 M 条
        - 候选按到 query 距离排序
        - 仅保留比所有已选邻居更近于 query 的候选（多样性启发式）
     c. 双向连边；对方超容量(2M)则裁剪最远边
  10. level > max_level → 更新 entry_meta_（新 entry point）
```

### 2.3 层级分布

采样公式 `level = floor(-ln(U) / ln(M))` 实现指数衰减：

| 层 | M=16 时占比 | 角色 |
|---|---|---|
| L0 | 100% | 全量节点，密图精筛 |
| L1 | ~36% | 区域导航 |
| L2 | ~13% | 大区导航 |
| L3 | ~5% | 全局入口层 |
| L4+ | 递减 | 更稀疏 |

检索时从最高层逐层下降——上层稀疏图快速定位目标区域，下层密图精筛候选。

## 3. 全量重建（merge 路径）

### 3.1 触发条件

Merge 物理压缩 data files 后触发图重建。Merge pipeline 中图重建是 Phase 2 的一部分（`Cask::merge`，`cask.cpp:1739`；rebuild 提交点 `:1772-1775`）：

```
Phase 1: 重写活 record → 新 data files + CAS 更新 KeyDir
Phase 2（search_ 存在时）:
  write_keydir_snapshot() + flush 索引队列
  compact()            ← 阈值压实死 posting（不重读、不重分词，非全量 rebuild）
  compact_index_chunks()
  rebuild_hnsw()       ← 提交 IndexPool worker 全量重建 HNSW 图（vector_dim>0）
  save_search_ckpt()   ← 统一落盘 search.ckpt（hnsw 段）+ search.vec
Phase 3: 清理旧文件
```

> 详见 [`merge-policy-zh.md` §4.2](merge-policy-zh.md) 的完整 V4 Pipeline Contract。
> 注意 HNSW 重建在 IndexPool worker 内执行（维持单写者），`flush()` 阻塞等其完成。

### 3.2 rebuild_hnsw() 实现

`search_layer.cpp:55-66`

```cpp
void SearchLayer::rebuild_hnsw() {
    auto old = hnsw_.load(std::memory_order_acquire);
    if (!old) return;
    auto fresh = std::make_shared<vec::HnswIndex>(old->config());
    for (id = 0 .. old->size()-1) {
        ord = old->node_ord(id);
        if (!index_.is_live(ord)) continue;  // 物理清死
        fresh->insert(ord, old->node_vec(id));
    }
    hnsw_.store(std::move(fresh), std::memory_order_release);  // 原子换入
}
```

**语义保证**：
- 重建期间并发查询走旧图（含死节点，语义同 V3.4 软删）
- 换入后旧图由在途读者 `shared_ptr` 续命（无锁回收）
- 新图保证零死节点（merge 承诺）

### 3.3 为什么不增量删除死节点

HNSW 图删除节点会导致邻接表悬空边——需要级联修复邻居连接，复杂度 O(dead_count × M × ef_construction)。全量重建虽然 O(N log N)，但只在 merge（低频批处理）时执行，且能重排图结构获得更优拓扑。

## 4. 持久化

### 4.1 格式：V7 双文件（BVH2 v2 段 + BCVP payload）

HNSW 不再有独立 `hnsw.snap`。V7 起持久化为**两部分**（统一落进搜索 checkpoint，
完整字节布局见 [`format-zh.md` §10](format-zh.md)）：

1. **图头**：作为 `search.ckpt`（`kSearchCkptName`，`cask.cpp:27`）的 **hnsw 段**
   （type 4），magic `"BVH2" = 0x32485642`、version 2。段内含 dim/metric/M/efc/seed/
   count/entry_meta/max_ord，以及每节点 `ord | level | int8 qcodes[dim] | qscale f32
   | qsum i32` + 各层邻接表，末尾段内 crc32。`HnswIndex::serialize`（`hnsw.cpp:1156`）。
2. **向量 payload**：全精度 f32 向量外存到 `search.vec`（magic `"BCVP"`，version 1），
   64B 头 + 每 4KB 页 CRC + 页对齐 f32；只读 `mmap`。`HnswIndex::save_vec_payload`
   （`hnsw.cpp:981`）。`inmem_int8` 模式无常驻 f32 → 不产生 `.vec`。

**写入**：`save_vec_payload(.vec)` → `serialize`（段）→ `SearchCheckpoint::write`，
均 `tmp + rename` 原子。读者协议：先 acquire `count_`/`entry_meta_` 快照，邻接
id ≥ count 一律跳过（防读到半发布状态）。

**注意（V7 变更）**：int8 qcodes **直接持久化在 BVH2 段内**——load 时**不再**从
f32 重新量化（省启动量化 pass）。f32 向量单独存 `search.vec`，按需 mmap。

### 4.2 落盘时机

| 时机 | 调用 | 位置 | 说明 |
|---|---|---|---|
| `close()` | `save_search_ckpt()` | `cask.cpp:694` | worker 已停 → 图静止，watermark = next_ord |
| `merge()` Phase 2 | `save_search_ckpt()` | `cask.cpp:1779` | rebuild 后立即落盘，下次 open 走快照路径 |
| `put()` | ❌ 不落盘 | — | 只更新内存图，data 文件（WAL）保证持久性 |

### 4.3 加载时机

| 时机 | 调用 | 位置 | 说明 |
|---|---|---|---|
| `open()` | `load_search_ckpt()` → hnsw 段 `deserialize` + `load_vec_payload` | `cask.cpp:881`（`load_recovery_snapshots`） | search.ckpt 健康且全段 CRC 通过时走快照路径 |
| `open()`（不健康/段坏） | 全量 fold + `on_vector()` | 回退路径 | 逐条遍历 data files 重建图 |

### 4.4 覆盖性 Gate（统一 watermark 自门）

V7 不再用 per-index 的 `hnsw_covers_next_ord()` 成对门。改为 `search.ckpt`
容器头部单个 **watermark**（= 保存时 `next_ord`）+ **全段 CRC 通过**
（`CkptLoadResult.all_segments_ok`，`search_layer.hpp:256-257`）共同判定：

```
search.ckpt 健康且 all_segments_ok ?
   fold_start(fid) = keydir_wm(fid)   // 各索引按自身 ord 水位自门跳重叠
 : fold_start(fid) = 0                // 全量 fold 兜底
```

各索引（hnsw `insert` 丢 ord≤max_inserted_ord_、bm25 `add_doc` 丢 ord≤水位）
重放幂等——详见 [`recovery-unified-checkpoint-design-zh.md` §4](recovery-unified-checkpoint-design-zh.md)。

### 4.5 Load 的整体拒绝语义

`HnswIndex::deserialize`（`hnsw.cpp:1278`，BVH2 v2 段）+ `load_vec_payload`
（`hnsw.cpp:1062`，.vec）：magic/version 不符、CRC 不匹配、config 不一致
（dim/metric/M）、邻接 id 越界、层级覆盖不完整——**任一违规整图拒绝**，
调用方弃实例回退 fold。绝不半载示人。容器层另有**段级隔离**：单 hnsw 段坏只
重建 hnsw，其余段照常载入。

## 5. 完整生命周期图

```
    put(vec)           put(vec)           put(vec)
      │                  │                  │
      ▼                  ▼                  ▼
  insert(内存)       insert(内存)       insert(内存)
      │                  │                  │
      ·                  ·                  ·
      ·                  ·                  ·
  ┌───┴──────────────────┴──────────────────┘
  │
  ├─ close() ──────────→ save_search_ckpt() ──→ search.ckpt(hnsw 段) + search.vec
  │                                                  │
  │                                        (进程退出)
  │                                                  │
  ├─ open() ──────────→ load_search_ckpt() ←── search.ckpt + search.vec
  │                       (watermark 自门 + 全段 CRC)
  │                         ├ pass → 快照加载（秒级）
  │                         └ fail → 全量 fold（O(N log N)）
  │
  └─ merge() ──→ rebuild_hnsw() ──→ save_search_ckpt() ──→ search.ckpt + search.vec
                   (物理清死)        (重建后落盘)
```

## 6. 相关文件索引

| 文件 | 内容 |
|---|---|
| `src/vector/hnsw.cpp:643` | `insert()` — 增量插入 + 层采样 + 连边 |
| `src/vector/hnsw.cpp:577` | `select_neighbors()` — HNSW Algorithm 4（int8 版 :614） |
| `src/vector/hnsw.cpp:1156` | `serialize()` — BVH2 v2 段序列化 |
| `src/vector/hnsw.cpp:1278` | `deserialize()` — BVH2 v2 段反序列化 + 校验 |
| `src/vector/hnsw.cpp:981` | `save_vec_payload()` — BCVP `.vec` 写 |
| `src/vector/hnsw.cpp:1062` | `load_vec_payload()` — BCVP `.vec` 读 + 校验 |
| `src/search/search_layer.cpp:55-66` | `rebuild_hnsw()` — 全量重建 |
| `src/search/search_layer.cpp:998` / `:1112` | `save_search_ckpt()` / `load_search_ckpt()` — 统一分段 checkpoint |
| `src/cask/cask.cpp:27` | `kSearchCkptName = "search.ckpt"` |
| `src/cask/cask.cpp:694` | `close()` 落盘 |
| `src/cask/cask.cpp:881` | `open()` 加载 + watermark 自门（`load_recovery_snapshots`） |
| `src/cask/cask.cpp:1739` | `merge()` rebuild + 落盘 |
| `include/bitcask/hnsw.hpp:337` | `entry_meta_` 定义 |
