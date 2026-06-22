# Merge 触发策略与 HNSW 死节点回收

> 前置阅读：`hnsw-lifecycle-zh.md`（HNSW 图生命周期）、`format-zh.md`（磁盘格式）
> 状态：已实现

## 1. 概述

bitcask 的 merge **不会自动触发**——没有后台定时器，没有写入量阈值自动启动。
调用方必须显式调用 `bitcask_needs_merge()` 检查，再调 `bitcask_merge()` 执行。

merge 是唯一物理回收 HNSW 死节点（已 delete 的文档向量）的时机。在此之前，
死节点通过搜索时的 `is_live` callback 软过滤。

## 2. Delete → HNSW：软删

### 2.1 delete 时发生了什么

```
bitcask_delete(cask, key, 0, &fault)
  └→ Cask::remove(key)
       ├→ KeyDir: 写 tombstone entry（ord 分配不变）
       └→ IndexPool worker:
            ├→ SearchLayer::on_delete(key, ord)
            │    ├→ BM25 倒排索引：移除词项统计（立即）
            │    ├→ index_.remove(key, tomb_ord)：标记 ord 为死（立即）
            │    ├→ doc_texts_.erase(ord)：清 LRU 原文（立即）
            │    └→ cache_.invalidate_terms(...)：失效受影响词项的搜索缓存（立即）
            └→ HNSW：❌ 什么都不做
```

向量节点留在 HNSW 图中，但对应的 ord 在 index 中被标记为死。

### 2.2 为什么不增量删除 HNSW 节点

HNSW 删除节点会导致邻接表悬空边——需要级联修复邻居连接，
复杂度 O(dead_count × M × ef_construction)。全量重建虽然 O(N log N)，
但只在 merge（低频批处理）时执行，且能重排图结构获得更优拓扑。

### 2.3 搜索时：live callback 过滤

```cpp
// search_layer.cpp:200-211（无 filter 分支）
live = [this](std::uint64_t ord) -> bool {
    return index_.is_live(ord);  // 死 ord → false
};
auto raw = hnsw->search(q, k, ef, &live);   // 死节点不入候选集
```

搜索结果正确，但死节点仍占用图空间和遍历时间。

## 3. Merge 触发条件：两段式决策

### 3.1 第一阶段：Trigger（任一满足就启动整次 merge）

默认值均来自 `include/bitcask/merge_policy.hpp` 的 `PolicyOptions` 结构体。

| 条件 | 默认值 | 字段 | 含义 |
|------|--------|------|------|
| `frag_merge_trigger` | 60 (%) | `PolicyOptions::frag_merge_trigger` | 某文件碎片率 ≥ 60% |
| `dead_bytes_merge_trigger` | 512 MB | `PolicyOptions::dead_bytes_merge_trigger` | 某文件死字节 ≥ 512MB |
| `deletion_rate_trigger` | 0（禁用）| `PolicyOptions::deletion_rate_trigger` | 全局删除率 ≥ N% |
| 过期文件 | `expiry_secs` 配置时 | `PolicyOptions::expiry_secs` | 文件全部 entry 过期 |

碎片率 = `(1 - live_keys / total_keys) × 100`，即被覆写/删除的 key 占比。
死字节 = `total_bytes - live_bytes`。

一个 trigger 都没命中 → `needs_merge` 返回 `false`，不 merge。

### 3.2 第二阶段：Per-file 阈值（Trigger 命中后挑文件）

| 条件 | 默认值 | 含义 |
|------|--------|------|
| `frag_threshold` | 40 (%) | 碎片率 ≥ 40% |
| `dead_bytes_threshold` | 128 MB | 死字节 ≥ 128MB |
| `small_file_threshold` | 10 MB | 文件 < 10MB |
| 过期 | `expiry_secs` 配置时 | 文件全部 entry 过期 |

任一满足，该文件入选合并候选列表。

### 3.3 `deletion_rate_trigger`（V4 新增）

专门为索引模式设计。当 `(total_ords - live_docs) / total_ords × 100 ≥ N` 时
触发 merge——即使 data file 的碎片率没到阈值。

```c
/* 配置示例：20% 文档被删就触发 merge */
bitcask_options_t opts; bitcask_options_init(&opts);
opts.read_write = 1;
opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;
/* deletion_rate_trigger 在 PolicyOptions 中配置 */
bitcask_open(Dir, &opts, &cask, NULL);
```

如果 trigger 成立但无文件通过 per-file 阈值，所有非活跃文件全部入选
（否则触发信号成立了但没文件可并）。

默认 `0` = 禁用，纯 KV 行为完全不变。

### 3.4 决策流程

```
needs_merge(Ref)
  └→ KeyDir::info() 拿 fstats 快照
  └→ merge_policy::decide(fstats, opts, now, dead_doc_rate)
       1. 任一文件命中 trigger？  ── No ──→ false
       2. 扫描所有文件，挑命中 per-file 阈值的
       3. 返回 {true, {Files, Expired}}
```

### 3.5 代码位置

- 策略纯函数：`src/merge/merge_policy.cpp` + `include/bitcask/merge_policy.hpp`
  （`decide(summary, opts, now_sec, dead_doc_rate=0)`）
- 调用入口：`Cask::needs_merge()` in `cask.cpp`，从 Index 计算 `dead_doc_rate`
- 默认值：`include/bitcask/merge_policy.hpp` 的 `PolicyOptions` 内联初始化

## 4. Merge 执行：HNSW 物理清死

### 4.1 rebuild_hnsw()

merge 的 Phase 2 调用 `rebuild_hnsw()`（`search_layer.cpp:55-66`）：

```cpp
void SearchLayer::rebuild_hnsw() {
    auto fresh = std::make_shared<HnswIndex>(old->config());
    for (id = 0 .. old->size()-1) {
        ord = old->node_ord(id);
        if (!index_.is_live(ord)) continue;  // 死节点不插入新图
        fresh->insert(ord, old->node_vec(id));
    }
    hnsw_.store(std::move(fresh));  // 原子换入
}
```

- 新图只含活节点（零死节点）
- 重建期间并发查询走旧图（含死节点，语义同软删）
- 换入后旧图由在途读者 `shared_ptr` 续命（无锁回收）
- `rebuild_hnsw()` 本身**不落盘**——重建后由 merge 流程的 `save_search_ckpt`
  统一持久化（图头入 `search.ckpt` 的 hnsw 段，f32 向量入 `search.vec`，
  见 [`format-zh.md` §10](format-zh.md)）。merge 中重建实际提交给 IndexPool
  worker 执行（维持单写者），`flush()` 阻塞等其完成

### 4.2 完整 merge 流程

实际流程见 `cask.cpp::Cask::merge` 顶部的「V4 Merge Pipeline Ordering Contract」
注释，顺序严格不可乱：

```
bitcask_merge(cask, &fault)
  └→ Cask::merge(files)        // files 为空先 needs_merge 决定
       Phase 1 — Data compaction:
         1. run_merge()              重写活 record 到新文件，CAS 更新 KeyDir
       Phase 2 — Index maintenance（search_ 存在时）:
         2. write_keydir_snapshot()  捕获 ord 水位
         3. IndexPool::flush()       排干在途索引任务
         4. compact(0.2)             阈值压实死 posting（不重读、不重分词；
                                     定位由 run_merge 的 on_relocate 已更新）
         5. compact_index_chunks()   释放全死 DocSlot chunk
         6. rebuild_hnsw + flush     提交 worker 同步重建 HNSW（物理清死，vector_dim>0）
         7. save_search_ckpt()       统一落盘 search.ckpt（docmap/bm25/hnsw 段）+ search.vec
       Phase 3 — Cleanup:
         8. erase read_files_ 缓存 + unlink 旧 data/hint
         9. trim_fstats
        10. write_keydir_snapshot()  最终状态快照
```

> 注意：merge **不**调用 `rebuild_index()`（全量重读 + 重分词）——倒排以稳定 ord
> 为键、与文件位置无关，run_merge 只通过 `on_relocate` 更新存储定位，故只需
> `compact()` 按阈值压实死 posting，省掉全量 NLP 重算。

## 5. 对运维的建议

| 场景 | 建议 |
|------|------|
| 大量 delete 后向量搜索变慢 | 调 `bitcask_merge(cask, &fault)`，重建 HNSW |
| 持续写入 + 偶尔删除 | 设 `deletion_rate_trigger` 为 10-20，轮询 `needs_merge` |
| 纯 KV（无向量/搜索） | 默认策略足够，碎片率和死字节驱动 |
| 低延迟要求 | 在低峰期手动 merge，避免影响在线查询 |

## 6. 相关文件索引

| 文件 | 内容 |
|------|------|
| `src/merge/merge_policy.cpp` | `decide()` — 两段式决策纯函数 |
| `include/bitcask/merge_policy.hpp` | `PolicyOptions` — 默认值 + 参数定义 |
| `src/search/search_layer.cpp:405` | `on_delete()` — BM25 清理（HNSW 不动） |
| `src/search/search_layer.cpp:200-211` | search_vector live callback |
| `src/search/search_layer.cpp:55-66` | `rebuild_hnsw()` — 物理清死 |
| `src/cask/cask.cpp` | `needs_merge()` + `merge()` 调用链（V4 Pipeline Contract 注释） |
| `include/bitcask/merge_policy.hpp` | `PolicyOptions` 默认阈值（frag_merge_trigger 等） |
| `doc/hnsw-lifecycle-zh.md` | HNSW 图完整生命周期（构建/持久化/恢复） |
