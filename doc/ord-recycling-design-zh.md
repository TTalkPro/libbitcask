# ord 回收复用：可行性与方案（实现现状稿）

> 前置阅读：`vector-db-design-zh.md`（ord 模型）、`concurrency-zh.md`（锁与不变量）、
> `recovery-unified-checkpoint-design-zh.md`（快照恢复流程）。
>
> 本文聚焦三个问题：ord 单调递增下 Index 数组是否无界？ord 不可回收的真正硬约束
> 是什么？方案 B（分块数组 + merge 释放全死 chunk）如何落地？RRF 融合的去重键
> 为什么必须继续以 ord 为单位？答案均以当前代码（`include/bitcask/index.hpp`、
> `src/keydir/index.cpp`、`src/keydir/docmap_ckpt.cpp`、`src/search/hybrid_searcher.cpp`）
> 为准描述。

## 1. 问题陈述

### 1.1 真正的痛点不是 ord 溢出

`next_ord_` 是 `std::uint64_t`。即使持续按每秒 10⁵ 个 ord 的速率分配，约需 5.8 × 10¹³
年才溢出（远超宇宙年龄）——这点没有任何现实工作负载能突破。所以「ord 会不会耗尽」
不是问题，**问题在于 Index 的 per-ord 数组持续增长、死 slot 占内存**。

### 1.2 Index 内 ord 的三个角色

| # | 角色 | 代表字段 | 假设 | 回收后后果 |
|---|------|----------|------|-----------|
| R1 | **数组下标** | `chunks_[ord/kChunkOrds]->slots[ord%kChunkOrds]`、`live_[ord]`、`doc_lens_[ord]`、`meta_blobs_[ord]` | ord → 固定内存偏移 | 新文档读到旧 slot 的 stale 数据 |
| R2 | **幂等水位** | InvertedIndex 的 `max_indexed_ord_` 拒绝旧序、HNSW 的 `max_inserted_ord_` 同理 | ord 永远递增，小 ord = 已处理 | 回收 ord 被水位拦截 → **静默丢弃** |
| R3 | **唯一排序键** | `PostingList::items` 严格升序无重复；`IntersectInput` SIMD 求交；HybridSearcher RRF 并桶（`acc.try_emplace(leg[i].ord)`） | ord 全局唯一且单调 | 旧死文档与新文档碰撞 → **搜索结果错误** |

### 1.3 一个自然的疑问：回收 ord 不行吗？

理论上可以引入 `seq`（全局单调的写入序列号）来解耦，让 `ord` 退化为可回收的
数组下标。但 §3 的硬约束会拦下这条路径——这就是「方案 A」未做的真正理由。
工程上的现实选择是方案 B：**ord 不回收，但 Index 的 per-ord 数组按 chunk 切分，
全死 chunk 在 merge 之后释放**。这是当前实现的全部机制。

## 2. Index 的存储结构（方案 B 落地形态）

### 2.1 字段总览

`bitcask::index::Index`（`include/bitcask/index.hpp`）的核心数据成员：

| 字段 | 类型 | 角色 | 形态 |
|------|------|------|------|
| `ext2ord_` | `std::unordered_map<std::string, std::uint64_t, StringHash, std::equal_to<>>` | ext_id → 最新 ord | hashmap |
| `chunks_` | `std::vector<std::unique_ptr<Chunk>>` | slots / ord2ext 数组（按 ord 下标） | **分块** |
| `live_` | `std::vector<std::uint8_t>` | ord 存活位图 | **平坦** |
| `doc_lens_` | `std::vector<std::uint32_t>` | SoA 副本，给 SIMD gather 读 | **平坦** |
| `meta_blobs_` | `std::vector<std::vector<std::byte>>` | per-ord meta blob | **平坦 + 惰性** |
| `next_ord_` | `std::uint64_t` | 下一个待分配 ord | 标量 |
| `live_docs_` | `std::uint64_t` | 当前存活文档数（= `ext2ord_.size()`） | 标量 |
| `retired_since_compact_` | `std::uint64_t` | 自上次压实起的退休版本数 | 标量 |
| `chunks_alloc_` / `chunks_freed_` | `std::uint64_t` | 累计分配 / 释放的 chunk 数（内省用） | 标量 |

`Chunk` 结构（`include/bitcask/index.hpp`）：

```cpp
static constexpr std::size_t kChunkOrds = 65536;  // 每 chunk 64K 个 ord

struct Chunk {
    std::array<DocSlot,     kChunkOrds> slots;    // 24B × 64K ≈ 1.5 MiB
    std::array<std::string, kChunkOrds> ord2ext;  // ~32B × 64K ≈ 2.0 MiB（SSO 字符串）
    std::uint32_t           live_count = 0;       // chunk 内存活 ord 数；== 0 可释放
};
```

`DocSlot` 是 24 字节紧凑布局（`offset` 前置消 padding，自身不再常驻 ord）：
`DocLoc{offset:u64, file_id:u32, total_sz:u32}` + `tstamp:u32` + `doc_len:u32`。

### 2.2 为什么不全分块？

`live_` 与 `doc_lens_` 必须是**平坦**的，否则 `Index::fill_is_live` /
`Index::fill_doc_lens` 的 AVX2 `vpgatherdq` / `vpgatherqd` 快速路径（`src/keydir/index.cpp`
中 `fill_is_live_inbounds_avx2`）无法工作——SIMD gather 的源必须是连续内存，
不能跨 `unique_ptr<Chunk>` 解引用。SIMD 的代价是这两个数组必须常驻；它们一个 1
字节、一个 4 字节，是为换取热路径 zero-branch / auto-vectorize 付出的固定开销。
这部分权衡体现为「slots/ord2ext 分块 + live/doc_lens 平坦」的混合形态。

### 2.3 ord → chunk / slot 索引

`Index::ensure_capacity_locked`（`src/keydir/index.cpp`）保证写入前数组就位：

```cpp
void Index::ensure_capacity_locked(std::uint64_t ord) {
    const std::size_t want = static_cast<std::size_t>(ord) + 1;
    if (live_.size() < want) {
        live_.resize(want, false);
        doc_lens_.resize(want, 0);
        if (!meta_blobs_.empty()) meta_blobs_.resize(want);  // S21-1：惰性启用后才跟平
    }
    const std::size_t ci = static_cast<std::size_t>(ord) / kChunkOrds;
    if (chunks_.size() <= ci) {
        chunks_.resize(ci + 1);
    }
    if (!chunks_[ci]) {
        chunks_[ci] = std::make_unique<Chunk>();
        ++chunks_alloc_;
    }
}
```

要点：
- `chunks_` 是 `unique_ptr` 数组，**只在需要时分配**——前几个 chunk 不会被预分配
  撑大 `vector` 自身（这与方案 A 的「一次性分配 O(N) 平坦数组」形成对比）。
- `live_` / `doc_lens_` 跟平增长，不可回收。
- `meta_blobs_` 首个非空 `set_meta` 前恒空（`S21-1` 惰性化），启用后才与 `live_`
  同宽。

### 2.4 写入路径：ord 永不复用

`Index::put_doc` 在覆盖写时**不回收旧 ord**：

```cpp
if (auto it = ext2ord_.find(ext_id); it != ext2ord_.end()) {
    const std::uint64_t old_ord = it->second;
    if (old_ord < live_.size() && live_[old_ord]) {
        live_[old_ord] = false;                                  // 旧 ord 软删
        if (chunks_[oc]) --chunks_[oc]->live_count;              // 旧 chunk 计数 -1
        ++retired_since_compact_;                                // 压实节流计数
    }
    it->second = ord;                                            // 新 ord 接位
}
chunk->slots[si]    = slot;
chunk->ord2ext[si].assign(ext_id);
++chunk->live_count;
live_[ord]      = true;
doc_lens_[ord]  = slot.doc_len;
```

旧 `ord` 标死后，`slots_` / `ord2ext_` 的内容原地保留（直到 merge 后 `compact_chunks`
把全死 chunk 整个释放）。这是方案 B 的核心：**死 slot 占位，但内存按 chunk 回收，
不是按 slot 回收**。

## 3. ord 不可变不变量（20 处）

下表是「回收 ord 会被打破」的全部硬约束。任意一条破坏 → 搜索语义错乱。
这是方案 A（seq + 回收）未做的根因。

| # | 组件 | 不变量 | 位置 | 回收后破坏方式 |
|---|------|--------|------|---------------|
| 1 | HNSW | `ord <= max_inserted_ord_` 拒绝插入 | `InvertedIndex` / `HnswIndex` 水位字段 | 回收 ord ≤ 水位 → 向量被静默丢弃 |
| 2 | HNSW 快照 | ord 在快照中严格递增 | `HnswIndex::serialize` | 快照加载失败（整体拒绝） |
| 3 | HNSW | `NodeChunk::ords[slot] == ord` | HNSW 节点存储 | 并发读者看到 stale ord |
| 4 | Inverted | `PostingList::items` 按 ord 升序无重复 | `InvertedIndex::add_doc` | 回收 ord 与死 ord 碰撞 → 错误文档命中 |
| 5 | Inverted | `add_doc`：ord > `max_indexed_ord_` | Inverted 水位字段 | 回收 ord ≤ 水位 → postings 静默丢弃 |
| 6 | Inverted | `PostingList::find()` 按 ord 二分 | `InvertedIndex::lookup` | 返回死文档的 stale index |
| 7 | Inverted | `PostingBlock` 的 `base_ord` / `end_ord` 范围 | WAND 块元数据 | WAND 块跳过误判 |
| 8 | Inverted WAL | WAL 条目存原始 ord | Inverted WAL append | 重放回收 ord 触发水位拦截 |
| 9 | Inverted load | `max_indexed_ord_` 从最大 posting ord 重建 | 倒排快照载入 | 快照后回收 ord 被重放拒绝 |
| 10 | Index | 所有数组 `[ord]` 寻址 | `Index` 全部读路径 | 回收 ord ≥ 数组长度 → 越界 |
| 11 | Index | `put_doc`：旧 ord 软删，新 ord 占 slot | `Index::put_doc` | slot 未清 → stale 数据 |
| 12 | Index | `for_each_live`：0..size 扫描 `live_` | `Index::for_each_live` | 回收 ord 重复出现 → 双重计数 |
| 13 | Index | `fill_is_live` / `fill_doc_lens` SIMD 假设 in-bounds | `Index::fill_*` AVX2 快速路径 | 慢路径处理越界；快路径静默出错 |
| 14 | KeyDir | `alloc_ord` 返回下一个未用值（无回收机制） | `KeyDir::alloc_ord` | 无回收机制存在 |
| 15 | KeyDir | `SingleEntry::ord` 是属性而非索引 | `KeyDir::SingleEntry` | 风险较低（仅 tie-breaking） |
| 16 | Merge | ord 通过 merge 保持不变（不重编号） | merge 管线 | 方案 A 下需重编号（外加 seq 翻译） |
| 17 | Search | RRF 用 ord 作为并桶键 | `HybridSearcher::search` 的 `acc.try_emplace(leg[i].ord)` | 死 + 回收 ord 碰撞 → 错误融合分数 |
| 18 | 交集 | 输入必须严格升序无重复 | `IntersectInput` SIMD 求交 | 回收 ord → 重复 → 错误交集结果 |
| 19 | LiveChecker | `is_live(ord)` / `doc_len(ord)` | `LiveChecker` 接口 | 回收 ord 有 stale slot → 返回错误值 |
| 20 | Format | "per-write, 永不复用"（格式头注释） | `format::kOrdOffset` 注释 | 磁盘格式契约违反 |

第 17 条（即 RRF 的并桶键）是本设计稿专门点出的——见 §6。

## 4. ord 为什么是 per-write 分配（而非 per-key）？

### 4.1 三个硬约束

- **约束 1：`PostingList::add_doc` 是纯追加**
  实现是 `pl.items.push_back({ord, tf, ...})`，依赖 ord 永远递增来维持升序
  不变量。如果同一 key 复用 ord：
  - 水位 `ord <= max_indexed_ord_` 直接拒绝，更新进不了索引；
  - 即使绕过水位，旧 posting 需 O(n) 删除、新 posting 需 O(n) 插入。

- **约束 2：HNSW 无法原地更新向量**
  HNSW 节点存的是 ord 对应的向量。更新文档意味着向量改变，但 HNSW 不支持
  原地向量替换——删旧节点会破坏图连通性。当前方案：旧节点标记死
  （`is_live(ord)` 过滤），merge 时整体重建。

- **约束 3：幂等水位依赖 ord 单调性**
  HNSW / InvertedIndex 都用水位 `ord <= max_X_ord_ → 拒绝`，保证崩溃恢复时
  WAL 重放不重复。同 ord 二次写入会被水位直接拦截。

### 4.2 ord = 搜索索引的文档版本标识

```
KV 层（KeyDir）:    key → file_loc, tstamp         ← 可以原地替换，tstamp 决定胜负
搜索层（Index）:    ord → postings, HNSW node      ← 无法原地更新，只能追加新版本 + 软删旧版本
```

如果只有 KV 没有搜索层，理论上可以复用 ord。但一旦有倒排索引 + HNSW，每次更新
就必须是一个新 ord。**ord per-write 是搜索引擎的硬约束，不是设计缺陷。**
这点与 Lucene / Tantivy 的 segment-based 模型完全一致——见 §7 的业界对比。

## 5. 方案 A：seq + merge-gated ord 回收（**未做**）

### 5.1 核心思路

引入独立单调递增的 `seq`（写入序列号）承担幂等水位职责，解耦 `ord` 使其可回收：

- `seq`：全局单调递增、永不回收，用于 HNSW / Inverted 的幂等水位
- `ord`：可回收的数组下标，仅用于 Index 层的数组索引
- 回收窗口：ord 在 delete 时进入 `pending_free_`，merge 后升级为 `available_free_`，
  此后 `alloc_ord` 优先从 free list 分配

### 5.2 致命问题：PostingList 升序不变量

`PostingList::add_doc` 是纯追加，依赖 ord 永远递增。merge-gated 回收后：

```
Merge N 完成后:
  "cat" posting list = [ord=1, ord=7, ord=10]      ← rebuild 只保留活文档
  available_free_    = [2, 4, 5, 6, 8, 9]          ← 待回收 ord

新文档到来 → alloc_ord() 返回 ord=2 (回收)
  add_doc(ord=2, {"cat": ...})
    pl.items.push_back({ord=2, ...})
    → "cat" posting list = [1, 7, 10, 2]           ← 不升序！
```

连锁破坏 5 个下游消费者：
- `PostingList::find()` 二分查找 → 返回错误位置
- WAND `PostingBlock` 块跳过 → `base_ord/end_ord` 范围假设失效
- `intersect_u64` SIMD 求交 → 前置条件直接违反
- `fill_is_live` AVX2 快速路径 → `ords.back() < bound` 优化静默出错
- `HybridSearcher` RRF 并桶 → 同 ord 合并到同一桶，分数累加错位

### 5.3 修复：PostingList 改为按 seq 存储

PostingList 必须存储 **seq**（全局单调）而非 ord。连锁反应：

- `Posting.ord` → `Posting.seq`
- 搜索结果返回 seq → 需要 `seq2ord_[seq]` 转换才能访问 `live_[]` / `ord2ext_[]`
- `fill_is_live(seqs)` 需要批量 seq→ord 转换后再查 `live_[]`
- `compact_flags` 的 live 数组按 ord 索引 → 需要 seq→ord 映射
- 17 个 `snapshot_flat` + `fill_is_live` 调用点全部需要改造

### 5.4 改动清单

| 步骤 | 改动 | 规模 | 风险 |
|------|------|------|------|
| 1 | KeyDir 加 `next_seq_` 原子，`SingleEntry` 加 seq 字段 | Quick | 低 |
| 2 | 数据文件记录格式加 seq（`format.hpp::kOrdOffset` 旁） | Quick | 低 |
| 3 | HNSW 水位 → `max_inserted_seq_`，节点存 seq | Short | 低 |
| 4 | InvertedIndex 水位 → `max_indexed_seq_` | Short | 低 |
| 5 | WAL `kWalEntryAddDoc` 条目加 seq 字段 | Short | 低 |
| 6 | **PostingList 改为按 seq 存储** | **Large** | **最高** |
| 7 | Index 加 `seq2ord_[]` 数组 | Short | 中 |
| 8 | 17 个 `fill_is_live` 调用点加 seq→ord 转换 | Large | 高 |
| 9 | `compact_flags` live 数组改为 seq 索引 | Medium | 中 |
| 10 | free list（`pending_free_` / `available_free_`） | Short | 中 |
| 11 | merge 管线最后一步：pending → available | Short | 高 |
| 12 | 所有快照格式更新 | Short | 低 |

**步骤 6–9 是风险集中区**——直接改造搜索热路径。

### 5.5 与 generational index（slotmap）的对比

`gen_idx = (slot << 20) | generation`：

| 方面 | seq 方案 | slotmap |
|------|----------|---------|
| PostingList 排序 | 按 seq 存储（全局单调）✓ | gen_idx 非全局单调 → 仍有问题 |
| Live 检查 | `live_[ord]` 数组 | 比较 generation → 可省 `live_` 数组 |
| 水位比较 | 简单 `u64` 比较 | 需比较 `(slot, gen)` 对 |
| 内存 | seq 额外 8B/doc | generation 打包进同一 u64（0 额外） |
| 改造面 | 20 个不变量 | 20 个不变量 + ord 类型从 u64 改为打包 gen_idx |

slotmap 对本代码基更差，因为 `gen_idx` 非全局单调，PostingList 问题无法解决。
seq 方案严格优于 slotmap。

## 6. HybridSearcher 的 RRF 去重——为什么以 ord 为键

### 6.1 实现

`src/search/hybrid_searcher.cpp` 的 RRF 融合：

```cpp
std::unordered_map<std::uint64_t, Fused> acc;
acc.reserve(text_hits.size() + vec_hits.size());
auto fold_leg = [&acc](std::vector<SearchHit>& leg) {
    for (std::size_t i = 0; i < leg.size(); ++i) {
        auto [it, fresh] = acc.try_emplace(leg[i].ord);    // ← ord 作为并桶键
        if (fresh) it->second.hit = std::move(leg[i]);
        it->second.score += 1.0 / (60.0 + static_cast<double>(i + 1));
    }
};
fold_leg(text_hits);
fold_leg(vec_hits);
```

确定性平局序：RRF 分相等 → ord 小者在前（测试锁此行为）。

### 6.2 为什么是 ord 而不是 ext_id

| 候选键 | 优点 | 缺点 |
|--------|------|------|
| **`ord`**（当前选择） | 单调递增，hash 分布均匀，PB 友好 | ord 回收 → 同文档两路命中落到不同桶，分数累加失败 |
| `ext_id` | 物理文档身份，与回收无关 | string 哈希贵；多字段副本（多字段独立 ord）可能同 key 不同 ord |
| `(file_id, offset)` | 物理位置，永不复用 | 太重，hash 不友好 |

选 `ord` 的隐含假设：**两路命中的「同一物理文档」必然有同一 ord**。这要求
- PostingList 与 HNSW 给同一文档产出同一 ord（成立：两路都用 `Index::put_doc` 登记同一 ord）
- ord 不回收（成立：方案 A 不做）
- 同一文档在两路中的 hit 携带同一 ord 字段（成立：`SearchHit.ord` 直接由查询结果带出）

如果走方案 A，RRF 必须改为按 `(file_id, offset)` 或 `ext_id` 并桶——这是方案 A
会带来的连锁改动之一。

### 6.3 多字段文档的处理

`SearchHit` 来自 BM25 与 HNSW 两路。同一 `ext_id` 在两路中通常产出同一 ord
（因为 `put_doc` 给单一文档分配单一 ord）。但 `fields` 段拆分后 BM25 可能为同一
文档产多个 ord（每个独立字段一次 `add_doc`）——这部分由 `HybridSearcher` 上层的
去重策略保证，本节不展开。

## 7. merge 期间的死内存回收

### 7.1 时序

```
Delete 时间点:
  Index::remove  → live_[ord] = false，slots/ord2ext 内容原地保留
  Merge 管线（plugin 各自提交）:
    TextPlugin::on_merge_commit → compact(0.2) 压实倒排死 posting
    VectorPlugin::on_merge_commit → rebuild 重建 HNSW 图（跳过死节点）
  Merge 末尾（宿主）:
    Index::compact_chunks → 释放 live_count == 0 的 chunks_ entry
    save_search_ckpt_paired → 落 search.ckpt（DocMap base + delta）
```

死 slot 在 merge 之前一直占位。merge 期间插件先做各自索引的死 posting / 死
节点回收（这部分与 Index 的 chunk 释放独立），merge 末尾由宿主调
`Index::compact_chunks` 释放 DocMap 层的全死 chunk。

### 7.2 `Index::compact_chunks` 实现

`src/keydir/index.cpp`：

```cpp
std::uint64_t Index::compact_chunks() {
    std::unique_lock lk(mutex_);
    std::uint64_t freed = 0;
    for (auto& chunk_ptr : chunks_) {
        if (chunk_ptr && chunk_ptr->live_count == 0) {
            chunk_ptr.reset();         // unique_ptr 释放 Chunk（含 slots / ord2ext）
            ++freed;
        }
    }
    chunks_freed_ += freed;
    if (freed > 0) dirty_.store(true, std::memory_order_relaxed);
    return freed;
}
```

调用点（`src/cask/cask.cpp` 的 merge 收尾 RunFn）：

```cpp
t.fn = [this, search_ckpt, wms = merge_snap_wms,
         wm = keydir_->peek_next_ord()] {
    docmap_->compact_chunks();          // ← DocMap 全死 chunk 释放
    force_ckpt_rebase();
    if (!save_search_ckpt_paired(search_ckpt, wm, wms, {})) {
        log_warn("search checkpoint save failed after merge "
                 "(will rebuild on next open)");
    }
};
```

如果 `chunks_[ci]` 被释放，下次访问该 ord 的 reader 会走 `live_[ord]==false`
分支返回「已删」，不会触碰已被释放的 `Chunk`。但 `chunks_` 的 `vector` 长度
（`chunks_.size()`）不缩——`chunks_` 自身的 `unique_ptr` 数组是连续内存，开销极小
（指针数组），无需压缩。

### 7.3 `live_` / `doc_lens_` 不回收

`Index::compact_chunks` 不动 `live_` 与 `doc_lens_`。理由：

- `live_[ord]` 仍被 `for_each_live` 顺序扫描（顺序访问命中 prefetcher），
  全释放会留下稀疏位图；
- `doc_lens_` 是 SIMD `vpgatherqd` 快速路径的源，**必须是连续内存**；
- 二者体量小（ord 数 × 1B / 4B），百万 ord 也只 1MB / 4MB。

唯一回收路径是把 `next_ord_` 归零的「全量 fold」——但 fold 意味着重启级操作，
日常不触发。

### 7.4 ext_id 保持稳定

merge **不重编号**——同一文档的 ext_id 与 ord 在 merge 前后保持一致（merge
只搬移 record，不重写 docmap 槽）。这是 ord 模型的核心承诺，也是方案 A 改造
面广的原因之一。

## 8. 倒排列表膨胀与 compaction 机制

### 8.1 死 posting 的生命周期

`InvertedIndex::remove_doc` 只扣全局统计（`live_doc_count_--`、
`sum_doc_len_ -= doc_len`），**不从 posting list 里物理删除 posting**。死 posting
留在 `items[]` 中，经历：

```
add_doc(ord=0, "cat")    → "cat" PL: [Posting{ord=0, tf=1}]
put("foo") 更新          → ord=0 标死
  ↓ 查询时:
    snapshot_flat() 拷出所有 posting（含死）→
    fill_is_live() 批量过滤 → 只对 live 评分
  ↓ compact():
    dead_ratio ≥ 阈值时 compact_flags() 物理移除死 posting
  ↓ merge 时也调 compact(0.2) 压实死 posting；不再全量 rebuild_index
```

### 8.2 查询时的死 posting 过滤

所有搜索路径使用**批量 `fill_is_live`**（P2.1 优化）：

- `score_bow_topk` — `fill_is_live(fp.ords, live)` → 评分后 `if (live[i])` 筛选
- `search_wand` — `fill_is_live(tp.fp.ords, tp.live)` → pivot 检查
- `search_phrase` — `fill_is_live(first_ords, first_live)` → 候选检查
- `bool_search` BMW — 块级惰性 fill，`ensure_block()`
- `bool_search` must/should — 每 term 批量 fill

设计选择：死 posting 也参与 SIMD 评分（branchless），结果不用——无分支 SIMD 全量评分
+ 后置过滤，比逐 posting 分支检查更快。

### 8.3 compaction 机制

| 机制 | 触发方式 | 清理效果 |
|------|---------|---------|
| `InvertedIndex::compact` | 手动 API **+ merge 管线**（`dead_ratio=0.2`） | dead_ratio ≥ 阈值时移除死 posting |
| `TextPlugin::rebuild_index` | **仅定义存在**，merge 不调用（旧全量重建路径） | 全量重读 + 重分词，无死 posting |
| 查询时过滤 | 每次 search | 不回收内存，只保证正确性 |
| `Index::compact_chunks` | merge 末尾 RunFn | 释放 DocMap 全死 chunk |

merge 管线已包含 `compact(0.2)`；不再依赖全量 `rebuild_index`。无后台线程
自动触发——若用户既不手动 `compact()` 又不 merge，posting list 会随写入膨胀。

### 8.4 `fill_is_live` 成本

`Index::fill_is_live`（`src/keydir/index.cpp`）—— 批量取 live 状态：

- **AVX2 快速路径**：`vpgatherdq` 一次 gather 4 个 ord，整列一次 `shared_lock`
- **慢路径**：逐 ord `live_arr[ords[i]]` 数组下标
- **每 ord 成本**：~2 cycles（无 SIMD），~0.5 cycles（AVX2 gather）
- **主要开销**：`shared_lock` 获取（每 term 每查询一次，非每 posting）

### 8.5 `for_each_live` 与 `for_each_live_in`

`Index::for_each_live` 顺序扫描 `0..live_.size()`，对 `live_[ord]==true` 的位置
回调 `(ord, ext_id, slot)`。全死 chunk 也被扫描，但只跳过 `unique_ptr` 解引用
（`chunks_[ci]` 为 `nullptr` 时直接跳）——释放后访问仍安全（live_ 检查在先）。

`for_each_live_in(from, to)` 是范围版，专供 docmap delta 行提取用（`from`
来自上次 base 水位）。语义与全量版一致，锁语义也一致。

## 9. 查询接口

`Index` 实现 `bm25::DocTable`（只读身份表接口，`include/bitcask/doc_table.hpp`），
暴露给查询面的接口：

| 接口 | 用途 | 实现位置 |
|------|------|----------|
| `ord_to_ext(ord)` | ord → ext_id（命中翻译） | `Index::ord_to_ext` |
| `eval_meta(ord, filter)` | meta 过滤锁内求值 | `Index::eval_meta` |
| `ord_of(ext_id)` | ext_id → ord（explain 等 key→ord 反查） | `Index::ord_of` |
| `is_live(ord)` | LiveChecker | `Index::is_live` |
| `doc_len(ord)` | LiveChecker | `Index::doc_len` |
| `fill_is_live(ords, out)` | P2.1 批量 LiveChecker（SIMD） | `Index::fill_is_live` |
| `fill_doc_lens(ords, out)` | P2.1 批量 LiveChecker（SIMD） | `Index::fill_doc_lens` |
| `meta_blob(ord)` | per-ord meta blob（锁内拷贝返回） | `Index::meta_blob` |
| `get(ext_id)` | 取 ext_id 当前存活文档的 DocHit | `Index::get` |
| `live_docs()` | CompactionStats | `Index::live_docs` |
| `retired_since_compact()` | CompactionStats | `Index::retired_since_compact` |
| `reset_retired_since_compact()` | CompactionStats | `Index::reset_retired_since_compact` |
| `set_doc_len(ord, len)` | DocLenWriter（仅 reducer 上下文） | `Index::set_doc_len` |

写接口：

| 接口 | 用途 |
|------|------|
| `alloc_ord()` | 拿下一个 ord（unique_lock） |
| `put_doc(ext_id, ord, slot)` | 登记一条文档 |
| `remove(ext_id, tomb_ord)` | 软删（unique_lock） |
| `set_meta(ord, blob)` | meta blob 写入 |
| `compact_chunks()` | 释放全死 chunk |
| `serialize_docmap(out, wm)` | sidecar 序列化（V2） |
| `deserialize_docmap(buf)` | sidecar 反序列化（V1/V2 双收） |

`bm25::DocTable` 的 `ord_to_ext` 是 S16-3 起的关键查询面接口——
`TextPlugin` 与 `VectorPlugin` 的查询代码、HNSW live-callback、`materialize_hits`
均通过它消费 docmap，不再直摸 `Index` 具体类型。这是 P4 双插件拆分（Text/Vector）
的前置。

## 10. Checkpoint 持久化（DocMap base + delta chain）

### 10.1 sidecar（"BCIS"）格式

`Index::serialize_docmap` / `deserialize_docmap`（`src/keydir/index.cpp`）：

| 字段 | 字节 | 含义 |
|------|------|------|
| Magic | 4 | `"BCIS"` |
| Version | 4 | 当前 `kSidecarVersion = 2`（V1 也兼容读） |
| covers_next_ord | 8 | 快照覆盖的 ord 水位（与后续 WAL/data 衔接点） |
| rows（占位 / vbyte） | 8 / 变长 | 活文档行数 |
| 行（×rows） | 变长 | 每行 v2：`[vbyte gap][vbyte klen][ext][vbyte file_id][vbyte offset][vbyte total_sz][tstamp u32][vbyte doc_len]` |
| CRC32 | 4 | 覆盖 `[magic..rows+行尾]` |

V2 vs V1：V2 行编码 gap + vbyte（ord 差分 + 标量 vbyte，tstamp 保持定宽 4B），
固定 34B/行 → 典型 12–15B/行。写端恒写 V2；读端 V1/V2 双收。旧读端遇 V2 →
version 不符拒收 → 组件退 fold（可重建，降级安全）。

### 10.2 base + delta chain

`src/keydir/docmap_ckpt.cpp`：

- **`save_docmap_base`**：写 `docmap.ckpt`（base），原子替换 `.prev`；链坍缩
  （静止点所有已入账 ord < watermark）。收尾：
  `begin_delta_window(watermark)` + `clear_removals()` + `clear_dirty()`。
- **`save_docmap_delta`**：写 `docmap.ckpt.d<seq>`（delta），段布局：
  - `kDeltaInfo`：[base_gen][from][seq] 三元组
  - `kDocmapDeltaV2`：窗口 live 行（`for_each_live_in(from, watermark)`）+ 删除
    日志（`removals_snapshot()`）
  - `kKeydirDelta`：keydir 半边（成对不变量）
- **`load_docmap`**：读 base + walk chain（`sc::walk_chain`）有界重放。链失败
  → fold 全量重建。

### 10.3 删除日志（自记账原则）

- 写路径（`Index::remove`）在 `tomb_ord >= delta_window_wm_` 时把 `(key, tomb_ord)`
  入 `removals_`（链覆盖区内的旧墓碑不入账，防跨文件 stale removal 重放误杀复活文档）。
- `removals_snapshot()` 在静止点快照（拷贝返回，窗口内条目量小）。
- base / delta 落成或载入成功后 `clear_removals()`。
- 链重放中途失败 → `begin_delta_window(0)` + `clear_removals()`（fold 会重新入账）。

### 10.4 dirty 自记账

- `put_doc` / `remove` / `set_doc_len` / `set_meta` / `compact_chunks` 内部置 `dirty_ = true`。
- `dirty()` 读（relaxed 原子）由 docmap 保存方（宿主 `Cask` 或 legacy `SearchLayer`
  路径）调用；保存或载入成功后 `clear_dirty()`。初值 `true`（未知状态一律重序列化）。

### 10.5 与 keydir 的成对不变量

docmap 组件落时（S14-7），若 keydir delta 非空，一并写入 `kKeydirDelta` 段。
这保证 docmap 重放时 keydir 的 LWW 顺序与 docmap 一致。`save_search_ckpt_paired`
封装 base + delta 双文件与 keydir meta 的协调。

## 11. 方案对比

| 维度 | 方案 A（seq + 回收） | 方案 B（分块数组，已落地） |
|------|---------------------|---------------------------|
| 改动文件 | ~15 | 3–4（`index.hpp` / `index.cpp` / `docmap_ckpt.cpp` / `cask.cpp`） |
| 不变量改动 | 20 | 0 |
| PostingList 改造 | 需要（大工程） | 不需要 |
| WAL / 快照格式 | 破坏性变更 | 不变 |
| 工期 | 2–3 周 | 1–2 天（已落地） |
| 风险 | 高（搜索热路径） | 低（存储层隔离） |
| 内存回收效果 | 完美（无空洞） | 依赖块聚集性 |
| 部分-dead 块处理 | 无空洞 | 仍有内存浪费 |
| 适用场景 | 均匀 churn + 内存敏感 | 突发批量删除 |

## 12. 结论：这个设计有必要吗？

### 12.1 ord 空间耗尽？

**不需要。** ord 是 `std::uint64_t`。每秒 10⁵ 个 ord → 约 5.8 × 10¹³ 年才耗尽。
即使 32-bit ord 也够约 12 天连续写入。ord 回收**不是**为了防止空间耗尽。

### 12.2 Index 数组内存增长？

**这是真正的痛点**，但量级取决于工作负载。假设 1 亿文档（高 churn：90% 已删除），
按当前结构估算：

| 字段 | 单 ord 开销 | 总内存（1 亿 ord） | 死 slot 占比 90% |
|------|-------------|--------------------|--------------------|
| `live_`（平坦） | 1B | 100 MB | 仍常驻（不可回收） |
| `doc_lens_`（平坦） | 4B | 400 MB | 仍常驻（不可回收） |
| `slots_`（分块） | 24B × 64K / chunk | 1.5 MB / chunk × 必要数 | **全死 chunk 释放** |
| `ord2ext_`（分块） | ~32B × 64K / chunk | 2.0 MB / chunk × 必要数 | **全死 chunk 释放** |
| `meta_blobs_`（惰性） | 视负载 | 视负载 | 视负载 |

如果 90% 删除是聚集性的（批量过期 / TTL 窗口），大部分 chunk 能完全释放；
如果删除是均匀 churn（每个 64K 窗口都有少量活文档），`live_` / `doc_lens_` 常驻，
chunk 释放效果有限。

### 12.3 方案 B 是否够用？

**对大多数场景够用。** 分块数组的优势是零风险、零不变量改动。如果删除模式
有聚集性（批量过期、TTL 窗口），大部分 chunk 能完全回收。

唯一不够的场景：**持续均匀 churn**（每个 64K 窗口都有少量活文档）。此时方案 B
的 chunk 释放无效，需要方案 A 或在 chunk 内做局部 compaction。

### 12.4 建议

**方案 B 当前阶段：已落地**。`Index` 的 `slots_` / `ord2ext_` 已分块、
merge 后释放全死 chunk（见 §2、§7）。

**方案 A 当前阶段：未做。** 理由：

1. **问题尚未出现**：没有实际工作负载数据显示 Index 数组内存成为瓶颈。
2. **方案 A 成本极高**：改造搜索热路径（PostingList → seq），引入 12+ 步变更，
   2–3 工期，风险集中在唯一的 BM25 搜索链路上。
3. **方案 B 是更好的第一步**：如果未来出现内存问题，先评估方案 B 是否覆盖
   工作负载的删除模式；如果不覆盖（均匀 churn），再评估方案 A。
4. **过早优化**：当前 ord 模型简洁、正确、经过充分测试。引入 seq 增加了一层
   概念复杂度（ord + seq 双 ID 空间），维护成本永久上升。

**触发条件**（满足任一可重新评估）：
- Index 数组内存占用在实际工作负载下超过可用内存的 20%
- 出现需要频繁全量 merge 来回收内存的运维痛点
- 嵌入式部署场景对内存有硬性限制

**当需要做时**：先评估方案 B 的覆盖度，不够再走方案 A（seq + PostingList 改造）。
两条路径不互斥——方案 B 的分块基础设施可以与方案 A 的 seq 回收叠加。

## 13. 决策记录（推荐 / 备选 / 决定 / 理由）

### 决策 D1：Index 数组按 chunk 切分而非全局回收 ord

- **推荐**：方案 B（chunked arrays），slots_/ord2ext_ 按 `kChunkOrds = 65536` 分块，
  `live_` / `doc_lens_` 保持平坦。
- **备选**：方案 A（seq + ord 回收）。
- **决定**：方案 B。
- **理由**：方案 A 触及 20 处不变量（PostingList 升序、WAL 重放水位、HNSW 节点
  存储、HybridSearcher RRF 并桶键等），风险集中在唯一的 BM25 搜索链路上；方案 B
  是存储层隔离，零不变量改动，工程量 1–2 天 vs 方案 A 的 2–3 周。方案 B 的局限
  （均匀 churn 下 chunk 释放无效）当前没有工作负载触发，留待未来按 §12.4 触发
  条件重新评估。

### 决策 D2：`live_` / `doc_lens_` 保持平坦而非分块

- **推荐**：保持平坦。
- **备选**：分块（与 `slots_` / `ord2ext_` 一起）。
- **决定**：保持平坦。
- **理由**：`fill_is_live` / `fill_doc_lens` 的 AVX2 快速路径（`vpgatherdq` /
  `vpgatherqd`）要求源数组是连续内存，跨 `unique_ptr<Chunk>` 解引用会破坏 SIMD
  gather。`live_` 是 1B/ord、`doc_lens_` 是 4B/ord，体量小（百万 ord 也只 1MB /
  4MB），不值得为回收这点内存牺牲热路径性能。

### 决策 D3：ord 不回收，依赖方案 B 的 chunk 释放

- **推荐**：ord 永远单调递增，永不复用。
- **备选**：方案 A 的 free list（pending_free_ → available_free_）。
- **决定**：不回收。
- **理由**：方案 A 需要全局引入 `seq` 双 ID 空间，触发 12+ 步连锁改动（见 §5.4）。
  当前工作负载没有出现内存瓶颈，方案 B 的 chunk 释放已覆盖大多数场景。

### 决策 D4：HybridSearcher RRF 以 ord 为并桶键

- **推荐**：以 `SearchHit.ord` 为键（`acc.try_emplace(leg[i].ord)`）。
- **备选**：以 `ext_id` 或 `(file_id, offset)` 为键。
- **决定**：以 ord 为键。
- **理由**：ord 是单调递增 u64，hash 分布均匀，PB 友好；两路命中的同一物理文档
  必然携带同一 ord（两路都用 `Index::put_doc` 登记同一 ord）。隐含假设是 ord
  不回收（决策 D3 已锁定）。如果未来走方案 A，RRF 必须改为按 `ext_id` 或
  `(file_id, offset)` 并桶——这是方案 A 改造面的一部分。

### 决策 D5：`compact_chunks` 在 merge 末尾由宿主调

- **推荐**：merge 末尾 RunFn 调 `Index::compact_chunks`。
- **备选**：插件自治（`TextPlugin::on_merge_commit` 顺手调）。
- **决定**：宿主调用。
- **理由**：`compact_chunks` 是 DocMap 层的内存回收，与 Inverted / HNSW 的索引层
  回收（compact / rebuild）独立。归宿主调用便于：① 与 `save_search_ckpt_paired`
  的收尾顺序协调（先回收，再落 ckpt，让 ckpt 反映最紧凑状态）；② 与 `force_ckpt_rebase`
  在同一原子点；③ 不依赖任一插件存在（即使没有 TextPlugin，DocMap 也要回收）。

### 决策 D6：sidecar 用 gap + vbyte 行编码（V2）

- **推荐**：V2 行编码（`kSidecarVersion = 2`）：`[vbyte gap][vbyte klen][ext]
  [vbyte file_id][vbyte offset][vbyte total_sz][tstamp u32][vbyte doc_len]`。
- **备选**：V1 定宽编码（34B/行）。
- **决定**：V2。
- **理由**：V2 典型 12–15B/行（V1 是 34B/行），写端体积下降 ~50%。`ord` /
  `tomb` 走 gap 差分（窗口内单调升序 → 差分后典型 1–2B）；`tstamp` 保持定宽 4B
  （unix 秒级时间戳 vbyte 需 5B 反而更大）。gap 用 u64 二补数回绕，正确性**不依赖**
  升序——乱序只损压缩率不损数据。读端 V1/V2 双收（旧读端遇 V2 拒收 → 降级
  fold，安全）。

### 决策 D7：DocMap ckpt 自记账（dirty_/removals_）

- **推荐**：写路径（`put_doc` / `remove` / `set_doc_len` / `set_meta` /
  `compact_chunks`）内部置 `dirty_ = true`；删除日志在 `tomb_ord >= delta_window_wm_`
  时入 `removals_`。
- **备选**：外部记账（保存方维护「哪些 ord 写过」的集合）。
- **决定**：自记账。
- **理由**：写它的人负责记账——减少跨组件同步，让 docmap 保存方（宿主 `Cask`
  或 legacy `SearchLayer` 路径）成为纯消费方。`dirty_` 是 relaxed 原子（与原
  `SearchLayer::dirty_docmap_` 语义一致：写点已被 reducer / 静止点串行化）。
  `removals_` 受 `mutex_` 保护（`remove` 已持 unique_lock）。

## 14. 业界对比：主流搜索引擎如何处理文档更新

### 14.1 Lucene / Elasticsearch

Lucene 的 `updateDocument` 内部是 **delete + insert**：

```java
writer.updateDocument(new Term("id", "doc1"), updatedDocument);
// 内部:
//   1. deleteDocuments(new Term("id", "doc1"))  → 标记旧文档删除
//   2. addDocument(updatedDocument)              → 插入新文档（新 doc ID）
```

- 每次更新获得新 doc ID（segment 内递增），旧 doc 标记为 deleted
- LiveDocs bitset：Lucene 10.4 引入 `SparseLiveDocs`（≤1% 删除时用稀疏 bitset）
  和 `DenseLiveDocs`（>1% 删除时用传统 bitset），查询时 O(1) 过滤
- segment merge：合并时跳过 deleted 文档，物理回收。ES 默认 33% 删除率触发优先 merge

> 引用：[Lucene segment merging](https://dev.to/iprithv/lucene-segment-merging-when-and-why-the-index-rewrites-itself-1k78)
> "Lucene's immutable segment design means documents are never updated in place."

### 14.2 Tantivy（Rust）

与 Lucene 完全相同的模型：

> 引用：[Tantivy ARCHITECTURE.md](https://github.com/quickwit-oss/tantivy/blob/main/ARCHITECTURE.md)
> "On commit, tantivy will find all of the segments with documents matching this existing term
> and remove from alive bitset file... Like all segment files, this file is immutable."

### 14.3 Solr

Solr 对 `docValues="true"` 的数值字段支持**原地更新**（不重建倒排索引），但条件
极严苛：字段必须 `indexed="false"` + `stored="false"` + `docValues="true"` + 数值类型。
对倒排索引中的文本字段，仍然是 delete + insert。

> 引用：[Solr partial updates](https://solr.apache.org/guide/solr/latest/indexing-guide/partial-document-updates.html)

### 14.4 对比总结

| 引擎 | 文档 ID 是否稳定 | 原地更新 posting list | 死文档清理 |
|------|:---:|:---:|-----------|
| **Lucene** | 否（每次更新新 ID） | 否 | LiveDocs bitset + segment merge |
| **Elasticsearch** | 否（继承 Lucene） | 否 | bitset + merge |
| **Tantivy** | 否（每次更新新 ID） | 否 | alive bitset + merge |
| **Solr** | 否（仅 docValues 数值字段可原地） | 否 | bitset + merge |
| **bitcask** | 否（ord per-write） | 否 | `live_[]` bitmap + `compact_chunks` |

**bitcask 的 ord-per-write 模型与 Lucene 完全一致——这就是业界标准做法。**
没有任何主流搜索引擎使用「保持 ID 不变 + 原地更新 posting list」的方案。

### 14.5 为什么不原地更新 posting list？

1. **有序数组原地删除 = O(n)**：需要从中间移除并移动元素
2. **HNSW 无法原地更新向量**：删节点破坏图连通性
3. **不可变数据结构 = 并发友好**：Lucene segment 和 bitcask `PostingList`
   （`shared_ptr` + CoW）都依赖不可变性实现 lock-free 读。原地修改需要阻塞所有读者
4. **幂等水位依赖单调 ID**：崩溃恢复时需要区分新旧版本

## 15. 如果要控制 posting list 膨胀

当前 gap：`compact()` 无自动触发。三种解决方案（**均不需要改 ord 模型**）：

| 方案 | 改动 | 效果 |
|------|------|------|
| **后台 compact 线程** | 定期扫描 dead ratio，超阈值自动 `compact()` | 低延迟回收，适合在线服务 |
| ~~**merge 时加 compact 步**~~（**已实现**） | merge 管线已直接调 `compact(0.2)` | merge 即回收死 posting |
| **写路径触发** | `add_doc` 后检查所在 PL 的 dead ratio，超阈值异步 compact | 精准回收，但增加写延迟 |

## 16. 关键源码索引（按出现顺序）

| 关注点 | 位置 |
|--------|------|
| `Index::Chunk` / `kChunkOrds` | `include/bitcask/index.hpp` |
| `Index::ensure_capacity_locked` | `src/keydir/index.cpp` |
| `Index::alloc_ord` / `put_doc` / `remove` | `src/keydir/index.cpp` |
| `Index::set_doc_len` / `set_meta` | `src/keydir/index.cpp` |
| `Index::ord_to_ext` / `ord_of` / `is_live` / `doc_len` | `src/keydir/index.cpp` |
| `Index::eval_meta` / `meta_blob` | `src/keydir/index.cpp` |
| `Index::fill_is_live` / `fill_doc_lens`（AVX2 路径） | `src/keydir/index.cpp` |
| `Index::compact_chunks` | `src/keydir/index.cpp` |
| `Index::serialize_docmap` / `deserialize_docmap`（sidecar V2） | `src/keydir/index.cpp` |
| `Index::begin_delta_window` / `removals_snapshot` / `clear_removals` | `src/keydir/index.cpp` |
| `bm25::DocTable` / `LiveChecker` / `DocLenWriter` / `CompactionStats` | `include/bitcask/doc_table.hpp` |
| `LiveChecker::is_live` / `doc_len` / `fill_*` | `include/bitcask/live_checker.hpp` |
| `bm25::HybridSearcher::search`（RRF 并桶） | `src/search/hybrid_searcher.cpp` |
| `save_docmap_base` / `save_docmap_delta` / `load_docmap` | `src/keydir/docmap_ckpt.cpp` |
| `apply_docmap_delta_section` / `apply_docmap_delta_section_v2` | `src/keydir/docmap_ckpt.cpp` |
| merge 末尾 `compact_chunks` + `save_search_ckpt_paired` | `src/cask/cask.cpp` 的 `run_merge` 收尾 RunFn |
| `KeyDir::alloc_ord` / `advance_ord` | `src/keydir/keydir.cpp` |
| 磁盘格式 ord 字段定义 | `include/bitcask/format.hpp::kOrdOffset` |
| `bm25::SearchHit`（含 `ord` 字段） | `include/bitcask/search_types.hpp` |