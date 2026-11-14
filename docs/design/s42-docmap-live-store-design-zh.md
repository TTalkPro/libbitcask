# S42: DocMap Live-Only Store 设计

## 1. 结论

把全局 DocMap 从“按历史 ord/LSN 展开的数组”改成“只保存当前 live row 的 map”，方向是合理的。它能消除当前最坏的内存模式：同一个 key 反复覆盖写时，`live_`、`doc_lens_`、chunk 内 `slots`/`ord2ext` 随总写入次数增长，而不是随 live docs 增长。

但有一个关键修正：不能用普通 unordered map 的天然遍历顺序直接替代现有 `for_each_live()` / `for_each_live_in()`。docmap base checkpoint 的 vbyte gap 编码、delta checkpoint 的窗口行，以及现有测试都依赖按 ord 升序输出。因此本设计采用：

```text
live-only hash map 作为主存储
+ checkpoint 时提取 row refs 后排序
+ delta 窗口 journal 避免每次 delta checkpoint 扫全量 live docs
```

同时放弃全局 DocMap 层的 SIMD `fill_is_live` / `fill_doc_lens` 热路径。这个代价可以接受，因为 BM25 主查询路径已经使用分段索引和段内 dense DocId；全局 DocMap 不再是评分循环的数组后端，而主要负责 KV 点查、ord/ext 翻译、meta 过滤、搜索结果物化，以及 HNSW/IVF 的 live callback。

## 2. 问题陈述

当前 `index::Index` 的内存形态是：

```cpp
std::unordered_map<std::string, std::uint64_t> ext2ord_;
std::vector<std::unique_ptr<Chunk>> chunks_;
std::vector<std::uint8_t>  live_;
std::vector<std::uint32_t> doc_lens_;
std::vector<std::vector<std::byte>> meta_blobs_;
```

其中：

```text
Chunk = DocSlot[65536] + string_view[65536]
      = 32B * 65536 + 16B * 65536
      = 3 MiB
live_[ord]     = 1B / historical ord
doc_lens_[ord] = 4B / historical ord
```

LSN/ord 全局单调且永不复用。覆盖同一个 key 也会分配新的 ord，并把旧 ord 变成软删。`compact_chunks()` 只能释放整个 64K chunk，而且要求该 chunk 的 `live_count == 0`；平坦数组本身不会收缩。

因此极端场景是：

```text
live_docs == 1
同一个 key 覆盖写入 N 次
DocMap 常驻内存约 O(N)
```

每个历史 ord 的 chunk 成本约为 `48B`，加上 `live_` 和 `doc_lens_` 后约为 `53B`。一百万次热点覆盖写就会留下约 `50MiB+` 的 DocMap 常驻结构，即使只有一条文档仍然 live。

## 3. 目标

1. DocMap 稳态内存从 `O(total writes / historical ords)` 降到 `O(live docs)`。
2. 覆盖写后立即释放旧版本的逻辑数据；不需要等待 merge 或 `compact_chunks()`。
3. 保持磁盘 docmap checkpoint 格式不变，避免引入新的恢复链格式。
4. 保持 `put_doc()`、`remove()`、`get()`、`ord_to_ext()`、`is_live()`、`doc_len()`、meta 相关接口的对外语义不变。
5. 保持 `SearchHit.ord` 的语义不变：它仍然是全局 LSN，不是全局 DocMap 的数组下标。
6. 保持 S40 的 key 单实例不变量：同一个 ext_id 的字符串存储仍然归属稳定的 node-based map 节点。

## 4. 非目标

1. 不改变 BM25 分段结构。
2. 不改变段内本地 dense DocId 的编码方式。
3. 不改变 HNSW 内部 dense node id 的结构。
4. 不改变 IVF record 的紧凑顺序流结构。
5. 不追求全局 DocMap 层的 SIMD gather 性能。
6. 本设计不要求 ord 复用，也不引入 ord 世代压缩。

## 5. 现状事实

### 5.1 BM25 已经分段化

BM25 查询使用 `TextPlugin::collect_default_segment_views()`。每个 segment 内部使用本地 dense DocId，并保存：

```text
key_at(local_docid)
lsn_at(local_docid)
```

segment merge 时使用 `docid_map[old_docid] = new_docid` 做段内重编码。评分和 live 过滤发生在段内本地 id 空间，不再依赖全局 DocMap 的平坦数组。

### 5.2 HNSW 不用 LSN 寻址

HNSW 内部 node id 是 dense `uint32` 插入序。LSN 保存在 `ords` 数组里，只作为外部翻译值使用。

### 5.3 IVF 不用 LSN 寻址

IVF record 是紧凑顺序流，ord 是 record 内字段，不是数组下标。

### 5.4 全局 DocMap 是剩余的按 LSN 展开点

因此本设计只处理 `bitcask::index::Index`，也就是 Cask 内的 `docmap_`。

## 6. 新数据结构

### 6.1 主结构

```cpp
struct LiveRow {
    std::string_view ext;   // 指向 ext2ord_ 的 node key，地址稳定
    DocSlot slot;
};

std::unordered_map<std::string, Lsn, StringHash, std::equal_to<>>
    ext2ord_;

ankerl::unordered_dense::map<Lsn, LiveRow>
    live_;

ankerl::unordered_dense::map<Lsn, std::vector<std::byte>>
    meta_;

ankerl::unordered_dense::set<Lsn>
    delta_live_;

std::vector<std::pair<std::string, Lsn>>
    removals_;
```

要点：

1. `ext2ord_` 继续使用 node-based `std::unordered_map`。S40 依赖 key 地址稳定，`LiveRow.ext` 只保存指向该 node key 的 `string_view`。
2. `live_` 使用项目已有依赖 `ankerl::unordered_dense::map`。它只保存当前 live 的 ord。
3. `LiveRow` 不保存 ord 本身。ord 是 map key，避免每行额外 8B。
4. `meta_` 单独保存，避免无 meta 部署为每个 live doc 支付 `std::vector` header。
5. `delta_live_` 是当前 checkpoint 窗口内仍 live 的 ord journal，用于 delta checkpoint。
6. `removals_` 沿用现有删除日志机制。

不再保留：

```cpp
std::vector<std::unique_ptr<Chunk>> chunks_;
std::vector<std::uint8_t> live_;
std::vector<std::uint32_t> doc_lens_;
std::vector<std::vector<std::byte>> meta_blobs_;
```

### 6.2 为什么不能直接无序遍历

现有 base checkpoint 使用 ord gap + vbyte 编码。虽然 gap 用无符号回绕计算时乱序仍可解码，但写入端必须保持升序才能获得稳定压缩率，现有恢复端和测试语义也假定行按 ord 升序重放。

delta checkpoint 更强：`for_each_live_in(from, watermark)` 必须提取当前窗口内仍 live 的行，并与 removals 按 ord 交错重放。“删后重写”场景要求删除先于同 key 的新行。

因此新的 checkpoint 写入流程必须显式排序。

## 7. 核心操作语义

### 7.1 `alloc_ord()`

保持不变：

```cpp
return next_ord_++;
```

LSN 仍然全局单调、永不复用。

### 7.2 新 key 写入

`put_doc(ext_id, ord, slot)`：

1. 更新 `next_ord_ = max(next_ord_, ord + 1)`。
2. `ext2ord_` 插入新 node `(ext_id, ord)`。
3. `live_[ord] = LiveRow{string_view(node.key), slot}`。
4. 如果 `ord >= delta_window_wm_`，把 `ord` 插入 `delta_live_`。
5. `live_docs_ += 1`。
6. 置 dirty。

### 7.3 覆盖写

覆盖写是本设计的主要收益点：

1. 在 `ext2ord_` 找到旧 ord。
2. `live_.erase(old_ord)`。
3. `meta_.erase(old_ord)`。
4. `delta_live_.erase(old_ord)`。
5. 更新 `ext2ord_[ext_id] = new_ord`。
6. 插入新的 `live_[new_ord]`。
7. 插入 `delta_live_`。
8. 只有旧版本实际 live 时才递增 `retired_since_compact_`。
9. 置 dirty。

旧 ord 的 slot、ext back-pointer、meta、delta journal 都在写路径同步移除。热点 key 反复覆盖写不会积累死 row，也不会扩大 chunk 数组。

### 7.4 删除

`remove(ext_id, tomb_ord)`：

1. 更新 `next_ord_`。
2. 找到当前 ord 后 `live_.erase(cur_ord)`。
3. `meta_.erase(cur_ord)`。
4. `delta_live_.erase(cur_ord)`。
5. 从 `ext2ord_` extract 节点。
6. 如果 `tomb_ord >= delta_window_wm_`，按现有语义把 removal 追加到 `removals_`。
7. 更新 `live_docs_`、`retired_since_compact_`、dirty。

必须先 erase `live_` 中的 row，再 extract `ext2ord_` 节点，避免 `LiveRow.ext` 悬垂。

即使 key 不存在，removals 的入账语义保持现状：只要 tomb ord 在当前窗口内就要记录，因为 keydir 侧重放仍可能依赖该墓碑。

### 7.5 `set_doc_len()`

```cpp
auto it = live_.find(ord);
if (it == live_.end()) return;
it->second.slot.doc_len = len;
```

不再同步写平坦 `doc_lens_`。

### 7.6 `set_meta()`

1. 只有 `live_.contains(ord)` 时才写入或清除 `meta_[ord]`。
2. blob 为空时 erase `meta_[ord]`。
3. blob 非空时 assign/copy。
4. 置 dirty。

新结构与旧行为兼容：死 ord 没有可读 meta；`eval_meta()` 对无 meta 或死 ord 都返回 false。

## 8. Checkpoint 与恢复

### 8.1 Base checkpoint

现有磁盘格式不变。序列化时在 shared lock 内收集 row refs：

```cpp
struct RowRef {
    Lsn ord;
    std::string_view ext;
    const DocSlot* slot;
};
```

流程：

1. 遍历 `live_`，收集 `RowRef`。
2. 按 `ord` 升序排序。
3. 按现有 `serialize_docmap()` 格式写入。
4. 遍历排序后的 live rows 写入 meta section。
5. 写入成功后 `begin_delta_window(watermark)` 并清空 removals/dirty。

排序期间仍持有 shared lock。字符串不会失效，因为写操作需要 unique lock。这个流程避免把所有 ext 字符串再拷贝一份，只增加 `32B/live` 左右的临时 row ref。

### 8.2 Delta checkpoint

delta 不扫描全量 `live_`，只处理 `delta_live_`：

1. 遍历 `delta_live_`。
2. 过滤 `from <= ord < watermark` 且当前仍 live。
3. 收集 `RowRef`。
4. 按 ord 排序。
5. 用现有 `kDocmapDeltaV3` 行编码写盘。
6. 与现有 `removals_snapshot()` 按 ord 交错。
7. meta section 也只遍历当前窗口内 live 的 meta rows，并排序后写盘。
8. 写入成功后推进窗口并清空 journal/removals/dirty。

`begin_delta_window(watermark)` 时清空 `delta_live_`。覆盖写在窗口内反复发生时，旧 ord 会从 journal 中移除，新 ord 只保留一条，因此热点覆盖写不会让 delta 文件随写入次数线性膨胀。

### 8.3 Recovery 重放

checkpoint 载入继续调用现有：

```text
put_doc(ext, ord, slot)
remove(ext, tomb)
set_meta(ord, blob)
```

格式不变，因此旧 checkpoint 可以直接被新实现读取，新 checkpoint 也可以被旧实现读取。

恢复阶段优化：在 checkpoint replay 期间抑制 `delta_live_` 的常规增长。base replay 会插入大量 live row，但没有必要同时为每个 ord 维护 delta journal；成功载入后会推进窗口并清空 journal，失败则退回 fold 重建。该抑制只影响内存 journal，不影响磁盘重放语义。

### 8.4 Migration 影响：不需要磁盘 migration

1. **被替换的结构从未落盘**。chunks / `live_` / `doc_lens_` / `meta_blobs_` 全是内存表示；
   持久化面只有 docmap sidecar（`BCIS` v3）、`kDocmapDeltaV3` 与 meta section，本设计逐字节不变。
2. **恢复入口不变**。deserialize / apply 仍走 `put_doc/remove/set_meta`；新结构接受任意 ord
   （hash key，无扩容、无数组下标前提），旧链无需转换即可载入。`next_ord_` 水位语义不变，
   与 WAL/data 的衔接点不变。
3. **回滚双向兼容**。新实现写出的仍是 v3/V3，且 delta 行集合与旧 `for_each_live_in(from, wm)`
   语义等价（窗口内当前 live），旧实现可读新链。不 bump 版本号、无 flag-day。
4. **migrate 工具面积不含 docmap**。`src/fileops/migrate.cpp` 只覆盖 data record / meta /
   field schema / 端序 / u32→u64 纪元；本届不新增 migration 分支。
5. **唯一的重建场景是安全降级而非 migration**：若写盘纪律被破坏（升序、removals 交错、
   meta 在行后、tstamp 定宽 8B），旧读端拒收整链 → 退 fold 全量重建。fold 是既有路径，
   不产生格式转换。S42-2 的 golden 对拍就是这条纪律的守门。
6. 行为微差不构成兼容问题：旧 `set_meta` 对死 ord 会扩容但不亮 live，新实现 no-op；
   两者可观察语义等价（死 ord 的 meta 恒不可读、不入 checkpoint），且重放保证行先于 meta。

## 9. 读取接口

### 9.1 KV 点查

```cpp
get(ext_id):
    ext2ord_.find(ext_id)
        -> ord
    live_.find(ord)
        -> slot
    return DocHit{slot, ord}
```

也可以后续优化成 `ext2ord_` 的 value 直接携带 `DocSlot`，`live_` 只保存指向 ext node 的 back-pointer，从而点查一次 hash lookup 完成。第一版先采用更直观的双 map 结构，便于迁移。

### 9.2 ord 翻译

```cpp
ord_to_ext(ord):
    live_.find(ord)
    return std::string(row.ext)
```

死 ord 不存在，因此天然返回 `nullopt`。

### 9.3 live / doc_len

```cpp
is_live(ord):
    return live_.contains(ord);

doc_len(ord):
    live_.find(ord)->slot.doc_len
```

### 9.4 Meta

```cpp
meta_blob(ord):
    meta_.find(ord)

eval_meta(ord, filter):
    live_.find(ord)
    meta_.find(ord)
    filter.evaluate(blob)
```

`eval_meta()` 继续在 shared lock 内完成，避免锁外引用并发写释放的 blob。

## 10. SIMD 热路径取舍

移除：

```cpp
fill_is_live_avx2()
fill_doc_lens_avx2()
```

对全局 DocMap 的批量接口退化为一次 shared lock 内逐 candidate hash lookup：

```cpp
fill_is_live(ords, out):
    shared_lock
    for ord in ords:
        out[i] = live_.contains(ord)

fill_doc_lens(ords, out):
    shared_lock
    for ord in ords:
        out[i] = live_.find(ord)->slot.doc_len
```

这不是零成本。随机 hash lookup 的 cache local 性和吞吐都不如 AVX2 gather 平坦数组。但该代价只作用于仍使用全局 ord 的路径，例如部分搜索结果物化、meta 过滤、HNSW/IVF live callback。

BM25 主评分路径已经不依赖这里的 SIMD：

```text
segment-local dense DocId
+ segment-local live checker
+ segment-local doc length
```

因此为了消除 O(total writes) 内存，放弃全局 DocMap SIMD 是合理的取舍。

## 11. 遍历兼容

新增内部批量方法：

```cpp
snapshot_live_row_refs(from, to) -> vector<RowRef>
snapshot_meta_row_refs(from, to) -> vector<MetaRowRef>
```

两者都在锁内收集并在锁内或同一 checkpoint 流程中按 ord 排序。

现有 `for_each_live()` / `for_each_live_in()` 为兼容保留。如果仍有外部调用方依赖升序，则临时实现为收集后排序再回调；只为内部 checkpoint 服务的路径改用新的 snapshot API，避免每次全量遍历都复制/排序。

长期可以把无序遍历版本标记为 deprecated，避免新增调用方误以为它仍按 ord 有序。

## 12. 并发模型

第一版保持现有粗粒度锁：

```text
读路径: std::shared_lock
写路径: std::unique_lock
checkpoint: shared_lock + snapshot/sort/serialize
```

不变量：

1. `LiveRow.ext` 只指向 `ext2ord_` 的 node key。
2. 覆盖写不移动 ext node，只更新其 value。
3. 删除时必须先 erase live row，再 extract ext node。
4. 不保存 `ext2ord_` 或 `live_` 的 iterator 到锁外。
5. `meta_` 的 blob 指针不逃逸出 shared lock。

后续如果点查成为瓶颈，可以再做分片锁或 optimistic hash table，但这不是本设计的必要条件。

## 13. 内存模型

### 13.1 热点覆盖写

旧结构：

```text
每次写入分配新 ord
live_/doc_lens_ 增加 5B/ord
每 64K ord 分配约 3MiB chunk
compact_chunks() 只能释放完全为空的 chunk
```

新结构：

```text
每次覆盖写 erase old live row
插入 new live row
delta_live_ 同步替换
稳态只剩 ext2ord_ + live_ 中的一条记录
```

内存从：

```text
O(total writes)
```

变为：

```text
O(live docs)
```

### 13.2 稳定大 live set

对低 churn、百万级不同 key 都 live 的场景，live-only map 不一定比几乎满的连续 chunk 更省。粗略估算：

```text
旧结构每 historical ord 约 53B + ext2ord hash 项
新结构每 live doc 约 100B-160B
```

具体取决于 key 长度、allocator 开销和 hash map load factor。因此这不是单纯的内存压缩设计，而是把最坏情况从历史写入量解耦。对高频覆盖写收益巨大，对低 churn 大全量集可能略有增加。

可选后续优化：

1. `ext2ord_` value 直接携带 `DocSlot`，减少点查路径和间接层。
2. 使用自定义 key arena + open addressing 降低 per-entry allocator 开销。
3. checkpoint 成功后按需要 shrink hash map scratch capacity。
4. 提供编译期或运行时 profile 选择 flat/live-only，但第一版不建议维护双实现太久。

## 14. `compact_chunks()` 兼容

`compact_chunks()` 不再管理 chunk。为了减少调用方改动，第一版保留 API：

```cpp
return 0;
```

`IndexInfo` 中的 `chunks_allocated` / `chunks_freed` 也保留，用于诊断兼容，但新实现中不再增长。

`retired_since_compact()` 继续统计覆盖写和删除退休的版本数。它仍可用于后台 compaction 节流；只是 DocMap 内存不再等待该 compaction 释放。

## 15. 需要保留的测试语义

1. `put_doc()` 覆盖旧版本后，旧 ord 的 `is_live()` 必须为 false。
2. `ord_to_ext(old_ord)` 必须为 `nullopt`。
3. `remove()` 后 `get(ext_id)` 必须为 `nullopt`。
4. checkpoint 保存/加载后 live set、slot、meta、ord 翻译完全一致。
5. delta checkpoint 的 rows/removals 按 ord 升序交错重放。
6. “删后重写”必须先应用删除，再应用新行。
7. `for_each_live_in()` 现有调用方兼容语义不回退。
8. 恢复中途失败不能污染正式状态；失败路径退 fold 重建。

## 16. 测试计划

### 16.1 单元测试

迁移并扩展 `tests/index_test.cpp`：

1. 新 key / 覆盖写 / 删除。
2. 同窗口多次覆盖写后 delta checkpoint 只包含当前 live row。
3. 删除后同 key 重写的 ord 交错重放。
4. meta 设置、覆盖写清除旧 meta、恢复后过滤结果一致。
5. checkpoint base + delta 链。
6. checkpoint 损坏退 fold。
7. `for_each_live()` 兼容顺序。
8. 64 位 ord 边界不使用数组下标。

### 16.2 随机一致性测试

生成随机操作流：

```text
put / overwrite / remove / reinsert / set_doc_len / set_meta / checkpoint
```

同时维护一个简单 reference model，比较：

```text
get(ext)
ord_of(ext)
ord_to_ext(ord)
is_live(ord)
doc_len(ord)
meta_blob(ord)
checkpoint replay 后全量状态
```

### 16.3 内存测试

关键回归指标：

```text
1 个 live key，覆盖写入 1M 次
RSS / DocMap 常驻内存必须近似恒定
```

对照旧实现预期会随 ord chunk 增长约 `50MiB+`。

### 16.4 Benchmark

必须覆盖以下场景：

1. 热点 key overwrite。
2. 大规模不同 key 写入。
3. 低 churn 大 live set 点查。
4. BM25 分段查询 P99。
5. HNSW 带 live callback 搜索。
6. IVF 带 meta/live 过滤搜索。
7. base checkpoint 吞吐与持续时间。
8. delta checkpoint 吞吐与文件大小。
9. 开库 recovery 时间。

通过标准不是所有路径都快，而是：

```text
热点 overwrite RSS 显著下降
BM25 主查询 P99 无明显回退
checkpoint/recovery 在可接受范围内
```

## 17. Migration / Rollout

### Phase 1: 结构落地

1. 引入 live-only 内部结构。
2. checkpoint 使用 sorted snapshot。
3. delta 使用 journal。
4. 保留 `compact_chunks()` 和诊断字段。
5. 补齐单元测试和 shadow 测试。

### Phase 2: 默认开启

1. 默认使用 live-only DocMap。
2. 观察 RSS、查询 P99、checkpoint/recovery 指标。
3. 必要时提供短期配置开关回退。

### Phase 3: 清理

1. 删除 chunk、flat arrays 和 AVX2 gather kernel。
2. 清理不再需要的 `fill_*` SIMD 依赖。
3. 简化 `IndexInfo` 或将 chunk 字段标记 obsolete。

## 18. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| checkpoint 遍历顺序改变 | 恢复结果不稳定或测试失败 | 所有 checkpoint snapshot 显式按 ord 排序 |
| `LiveRow.ext` 悬垂 | use-after-free | 继续使用 node-based ext map；删除时先 erase live row |
| 全局批量过滤变慢 | HNSW/IVF/结果物化 P99 回退 | benchmark gate；必要时为这些路径做批量引用缓存或专用快照 |
| checkpoint 排序增加 CPU | 保存耗时增加 | delta 只排序窗口 journal；base 只在保存时排序 |
| replay journal 放大内存 | 开库峰值增加 | replay 期间抑制 delta journal |
| 低 churn 大全量集内存略增 | 非目标场景劣化 | benchmark 决策；后续使用紧凑 arena/open addressing 优化 |
| 误保留 iterator/string_view | rehash 后悬垂 | 只保存 ext node key 的地址，不保存容器 iterator |
| removals 在超长窗口内增长 | 删除频繁场景内存增长 | 沿用 checkpoint 窗口清空；必要时按 removals 数量提前 fold |

## 19. 最终评审意见

这个设计值得做。它解决的不是普通搜索索引的数组规模问题，而是全局 DocMap 仍按历史 LSN 展开导致的内存泄漏式增长。

核心设计约束有三条：

1. live-only map 是正确的稳态结构；
2. checkpoint 必须显式恢复升序语义；
3. delta journal 必须在覆盖写时淘汰旧 ord，否则热点写仍会把窗口 journal 写爆。

只要 benchmark 确认 BM25 分段查询和向量 live callback 没有明显回退，就可以按本设计实施。第一版不建议引入复杂有序索引来维持遍历顺序；checkpoint 时排序是最小且正确的方案。
