# 分段索引（Segment Index）：LSN/docid 解耦 + 并行构建 + merge 回收

> 前置阅读：
> - [`ord-recycling-design-zh.md`](ord-recycling-design-zh.md)（ord 三角色、per-write 硬约束、方案 A/B）
> - [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)（当前 base + delta 链持久化）
> - [`merge-policy-zh.md`](merge-policy-zh.md)（merge 触发与执行）
> - [`plugin-arch-split-design-zh.md`](plugin-arch-split-design-zh.md)（宿主/插件扇出、reducer 单写者）
>
> 状态：**设计草案（未实现）**。对应 TASK.md S27 梯队；取代 S26 梯队 ① term-sharded 多 reducer。
>
> 一句话：把当前「单一全局在线索引 + base/delta 链」演进为「多个不可变段（segment）+
> 后台 merge」。docid 随之自然下沉为**段内本地序号**，`ord` 回归纯粹的**全局 LSN**，
> delta 链整类代码与其 bug 类（见 §2.3）被段模型消解。

---

## 1. 动机：三个已知痛点收敛到同一个架构

| 痛点 | 现状 | 出处 |
|---|---|---|
| **ord 死内存不回收** | Index 的 per-ord 数组随写入无界增长，死 slot 占内存；方案 A（seq/ord 解耦回收）在单一全局索引里要改 17+ 调用点、PostingList 改按 seq 存储，代价高，**未做** | `ord-recycling-design-zh.md` §5 |
| **索引吞吐 ≪ KV 写吞吐** | 单 reducer 串行插倒排是天花板；term-sharded 多 reducer（①）踩 per-field 幂等水位 / 跨 shard 原子性 / 全局统计竞争，坑深 | 本轮会话；TASK.md S26 |
| **delta 链复杂且脆** | base + delta + delta 链，`apply_delta`/`walk_chain`/`from_ord` 不变量微妙，S26-B 的误报 assert 即其副作用 | `recovery-unified-checkpoint-design-zh.md`；TASK.md S26-B |

三者看似无关，但**段模型一次性解掉**：段被 drop 即回收死内存（痛点1）；段内单线程、段间独立
→ 并行构建无需分片一个共享表（痛点2）；段不可变、无 delta（痛点3）。这不是巧合——它正是
Lucene / Tantivy / Vespa 的共同答案（`ord-recycling-design-zh.md` §4.2 已指出 per-write ord
「与 segment-based 模型完全一致」）。

## 2. 概念模型：三个身份严格分离

### 2.1 identity / LSN / docid

成熟引擎从不把这三样合一，本设计遵循同一纪律：

| 概念 | 定义 | 作用域 | libbitcask 载体 |
|---|---|---|---|
| **identity（身份）** | 用户 key / 外部 `_id` | 全局，稳定 | `key`（不变） |
| **LSN（日志序号）** | 写入序列号；定序 / MVCC / 恢复 / 幂等水位 | 全局，单调，永不回收 | `ord`（`alloc_ord()`=`next_ord_.fetch_add(1)`，keydir.cpp:366，语义不变） |
| **docid（文档序号）** | posting list / liveDocs / docvalue 的下标 | **段内**，0-based，dense，随段 drop 而回收 | **新增**：段内本地整数 |

对比 `ord-recycling-design-zh.md` 的「方案 A」：方案 A 想在**单一全局索引**里引入 `seq`（LSN）
把 `ord`（下标）解耦出来回收——但 PostingList 得改按 seq 存储，牵连 17+ 处（§5.3）。
**段模型让这件事免费发生**：docid 天生段内本地，段 drop 即回收，无需在全局索引里做 seq/ord
双轨；LSN 就是现有的 `ord`，一字不改。

### 2.2 段（Segment）的定义

一个**不可变、自包含**的索引单元，覆盖任意一批文档：

```
Segment {
  seg_id           : 全局唯一段号（单调分配，仅命名，不参与排序）
  doc_count        : 本段文档数 N_s（docid ∈ [0, N_s)）
  inverted[field]  : 每字段一份倒排（term → PostingList，posting 存 docid，非 ord）
  hnsw?            : 可选向量图（节点按 docid）
  doc_store        : docid → { key, ord(LSN), DocLoc, tstamp, doc_len, meta }
  live_docs        : 位图，docid → 是否存活（删除只翻位，不动 posting）
  stats            : 本段 BM25 统计（N_s、Σdoc_len、per-field df 快照）
  base_lsn/hi_lsn  : 本段覆盖的 LSN 区间（恢复对齐用）
}
```

关键性质：**段一旦 flush 落盘就永不修改**（除 `live_docs` 位图与——若引入——docvalue 更新）。
新文档进新段；更新 = 旧段标死 + 新段新增；死内容靠 merge 物理回收。这与现有
`InvertedIndex` 的 base 序列化（`serialize_default`/`serialize_fields`，text_plugin.cpp:899
`save_component_base`）**格式几乎复用**——段就是「一个 base」。

### 2.3 段如何消解 delta 链

现状增量持久化 = `save_component_delta` 写 `bm25.ckpt.d<seq>`，load 时 `walk_chain` 按
`from_ord` 单调重放 `apply_delta`。这套不变量微妙：`from_ord`（全局链 coverage）与
`max_indexed_ord_`（per-field posting 水位）天然发散，S26-B 的误报 assert 即由此而来。

段模型里**没有 delta**：每次 flush = 一个新的不可变段；恢复 = 「列出活跃段 + 各自整体 load」；
死内容靠 merge。于是 `apply_delta` / `walk_chain` / `from_ord` / per-field 水位 / kDeltaInfo
链校验——**整类代码和整类 bug 一并退役**。段是 delta 链「做对了」的形态。

## 3. 架构

### 3.1 段生命周期

```
                 put_doc/put               flush(size/docs/mem 阈值)
   [写入] ───────────────────▶ [Building 内存段] ─────────────────▶ [Flushed 不可变段(盘)]
                                     │                                      │
                                     │ 单线程构建（段内）                    │ 进入活跃段集（manifest）
                                     ▼                                      ▼
                              (Stage 4: N 个并行 builder)          [Merging] ──merge──▶ [新大段]
                                                                          │
                                                                   旧段 [Dropped] (死内存回收)
```

- **Building**：内存态倒排（现 `InvertedIndex` 直接复用），单线程写入，无跨线程共享。
- **Flushed**：序列化为不可变段文件（复用 base 格式 + 段头）。可被查询。
- **Merging**：后台把若干小段合并成大段，物理删死 docid、重编 docid、合并统计。
- **Dropped**：被 merge 吃掉的旧段引用计数归零后删除文件、释放内存 → **死内存回收**。

### 3.2 活跃段集（manifest）

现 `index.manifest`（`ManifestEntry{generation, chain_seq, chain_wm}` per-component）演进为
**活跃段清单**：一个原子提交点（tmp+rename，沿用现协议），列出当前所有活跃 `seg_id` +
各段覆盖的 LSN 区间 + merge 代际。crash 恢复 = 读 manifest → load 各活跃段 → fold LSN 尾巴
（manifest 最高 LSN 到 keydir 尾）补齐窗口。**单 commit point 语义与现协议一致**，只是
「组件 base+chain」换成「段列表」。

### 3.3 复用 vs 新建

| 现有能力 | 段模型中的角色 | 改动量 |
|---|---|---|
| `InvertedIndex` base 序列化/反序列化 | 段的落盘格式（加段头：seg_id/doc_count/live_docs/stats） | 小（加壳） |
| merge / compaction 框架（`run_merge`、merge-policy） | 提升为**段级 merge**（tiered 策略选段、rebuild、drop） | 中（选段+归并逻辑） |
| docmap 存活过滤（`is_live`） | 每段 `live_docs` 位图 | 中（下沉到段） |
| analyzer / PostingList / WAND / 查询算子 | **段内原样复用**（posting 存 docid 而非 ord） | 小 |
| `index.manifest` 原子提交 | 活跃段清单 | 小（结构换血） |
| `ord`/`alloc_ord`/keydir | **保持不变**（LSN 角色） | 零 |
| **新建** | 段管理器、docid↔key/LSN 段内 doc_store、**多段查询归并**、BM25 跨段统计、（Stage4）并行 builder + refresh 可见性 | 大 |

### 3.4 段本地 doc_store 与全局 resolver（已定）

现全局 `Index`（index.hpp，已实装「方案 B」分块数组）一份数据按**全局 ord**扛 4 个职责，
段化把其中 3 个下沉到段、只留 1 个在全局：

| 职责 | 现载体 | 段化后 |
|---|---|---|
| **R1 身份解析** key→最新版本 | `ext2ord_`（key→ord） | **全局 resolver**（唯一保留的全局可变态） |
| **R2 结果水合** →{loc,tstamp,doc_len,meta,key} | `chunks_[].slots/ord2ext`、`meta_blobs_` | **段本地**（不可变） |
| **R3 存活过滤** →live | `live_[]` 平坦位图 | **段本地**（可变位图） |
| **R4 打分热路径** SIMD gather doc_len/live | `doc_lens_[]`/`live_[]` 平坦 SoA | **段本地**（平坦，SIMD 保留） |

**封口段 doc_store**（docid dense `0..N_s`，flush 后不可变，除 `live_docs`）：

```
SealedSegment.doc_store {
  N_s          : u32
  slots[N_s]   : DocSlot{DocLoc(offset/file_id/total_sz), tstamp, doc_len}   # R2
  keys[N_s]    : string（偏移+池）    # docid→外部 key（R2 水合）
  lsn[N_s]     : u64                  # docid→ord(LSN)  ← MVCC / RRF 版本键
  doc_lens[N_s]: u32 (SoA)           # R4：段内连续无洞，SIMD gather
  meta[N_s]    : blob（惰性）
  live_docs[N_s]: bitmap             # R3：可变，删除翻位；SIMD fill_is_live
  stats        : {N_s, Σdoc_len,...} # G-on-the-fly 段本地统计（§4）
}
```

要点：
- **封口段全平坦、定长、可 mmap → chunk 退役**。分块只为「全局 ord 稀疏且无界」而生；段封口后
  大小已知、docid 无洞，SIMD gather 反而更干净（零稀疏跳转）。R4 热路径（`fill_is_live`/
  `fill_doc_lens`）改按**段内 docid** gather 段本地 SoA，AVX2 快路径保留且省掉跨 `unique_ptr<Chunk>`
  边界判断。
- **仅 `live_docs` 封口后可变**（删除/覆盖翻位），其余冻结。
- **Building 段**受 flush 阈值封顶 → 普通 amortized `vector` 即可（连分块都不必），flush 时冻结成上面布局。
- **红利**：方案 B 的 `compact_chunks`/chunk 计数/惰性释放整套 → 被「merge 时整段 drop」取代，
  docmap 内存管理一并简化。

**全局 resolver（R1，唯一留在全局的可变态）**：`ext2ord_` 演进为 `key → (seg_id, docid)`，指向该 key
当前存活版本。两案取 **B1（推荐）**：
- **B1**：全局 hashmap `key→(seg_id,docid)`。insert / 覆盖 repoint+标旧死 / **merge 对存活文档 repoint**。
  最简、直接复用 `ext2ord_` 语义、点查 O(1)。成本同今日 `ext2ord_` + merge 期界于段大小的 repoint。
- **B2（Lucene term-delete，后续再评）**：不维护全局映射，覆盖/删除记「按 key 删」，各段用自身 key→docid
  惰性翻位。省全局 map + merge 无需 repoint，但删除 O(段数) 且每段要存 key→docid。

三条路径：
```
点查 key:   resolver[key] → (seg,docid) → seg.slots[docid]                    # R1→R2
搜索:       各段命中 docid → seg.keys/seg.lsn → seg.live_docs 过滤(段内 SIMD) → 跨段 top-k 归并
覆盖 key:   新文档进 Building 段拿新 docid；resolver 旧 (seg,docid) → 旧段 live_docs[docid]=0；resolver repoint
merge:      存活 docid 搬进新段重编 → 逐条更新 resolver → 原子换 manifest → 旧段引用归零后 drop
```

**RRF 去重键**：doc_store 保留 `lsn[docid]`。因 §9.6 暂定「向量留全局、只段化 BM25」，文本/向量两路
无共同 `(seg_id,docid)` → 沿用**全局 LSN(ord)** 作 RRF 共同去重键（与现 `HybridSearcher` 零差异，
对齐 `ord-recycling` §6）。将来向量也段化，再评估切 `(seg_id,docid)`。

### 3.5 多段查询：线程模型与 k 路归并（已定）

**复用锚点**：现 `search_fields`（text_plugin.cpp:610）已在做「跨多个 `InvertedIndex` 归并 top-k」
（各字段索引各查 → `acc[ord]+=score` → `partial_sort`）。段归并同构，但两处差异：

| | search_fields（现） | 多段（新） |
|---|---|---|
| 一个 doc 出现在… | 多字段（同 ord）→ 分数**求和** | 恰好一个段（docid 唯一）→ **并集，不求和** |
| idf 来源 | 各字段索引**独立**（字段=不同语料，正确） | 同字段各段**共享** → G-on-the-fly（§4） |

故有**两层归并**：段内跨段（同字段，并集，共享 idf）= `search_text` 走；跨字段（求和，独立 idf，
不变）= `search_fields` 在段归并之上再套。段归并是**内层**，插在现逻辑之下。

**线程模型：串行逐段（对标 Lucene 默认），且独立于 IndexPool。**
- **不复用 IndexPool 的 map worker**：那是写入流水线、受背压钳制；查询只读高并发，混用会把读延迟
  耦合到写背压。读写隔离是硬线。
- **Stage 2 = 串行逐段**：段数由 tiered merge 压小（通常 <一二十），串行延迟 = Σ 各段，够用。
  并行逐段（降尾延迟）+ 原子共享阈值后置。

**串行的红利：跨段 WAND 阈值传播。** 串行迭代可把「当前全局第 k 名分数」作下一段 WAND 的阈值下限：
```
threshold = -inf
for seg in segments:                                 # 串行
    hits_s = seg.search_wand(terms, k, floor=threshold, shared_idf)
    merge hits_s → global top-k heap
    threshold = heap.kth_score()                     # 抬高→下一段跳更多块
```
段越查到后面阈值越高、跳过越多，跨段剪枝几乎免费（Lucene 同款）。并行会丢掉它（每段从 0 起），
需补原子共享阈值（`MaxScoreAccumulator`），故后置。

**k 路归并**：两阶段 G-on-the-fly——① 扇出收 `Σdf/ΣN` 算全局 idf；② 串行逐段 `search_wand(floor,
shared_idf)` 返 ≤k 局部有序，喂进**大小 k 的 min-heap**（现 `InvertedIndex::search` 内部即用
`priority_queue<greater>` 截 top-k，inverted.cpp:167，同款复用）；③ 水合 `(seg,docid)` →
`seg.keys/seg.lsn/score`（替代现 `ord_to_ext`）。

**存活/去重与 RRF**：各段 `live_docs` 段内 SIMD 过滤（同 `LiveChecker`）；覆盖写近实时窗口内同 key
可能「旧段未翻位 + 新段已入」短暂双现，严格可归并按 key 去重取最高 lsn，通常直接接受。**RRF/
HybridSearcher 不变**：文本 hit 仍以全局 lsn(=ord) 为键返回 → RRF 按 ord 并桶零改动（§3.4）。

## 4. 查询路径（Stage 2 重点，另文深挖）

多段后，一次查询**两阶段**：先跨段聚合出**一份**全局统计算 idf，再用它给每段打分、归并：

```
search(q, k):
  # 阶段 1：G-on-the-fly —— 跨段聚合词统计，算一次全局 idf
  for term in q.terms:
    df = Σ_seg seg.inverted[field].df(term)      # 各段 term-dict 顺手取 df 求和
  N, sum_dl = Σ_seg (seg.N, seg.sum_doc_len)     # 段头统计求和
  idf = BM25.idf(df, N)                          # 全局，唯一

  # 阶段 2：用同一 idf 给每段打分（可并行），再跨段归并 top-k
  for seg in active_segments:                    # 可并行
    hits_s = seg.search(q, k, shared=idf, avgdl=sum_dl/N)
    map docid → (key, ord) via seg.doc_store; filter seg.live_docs
  return merge_topk(all hits_s, k)
```

**BM25 跨段统计：已定 = G-on-the-fly（对标 ES/Lucene 单节点/段级）。**

ES/Lucene 在**段之间**（= 我们的段，同一 shard 内）用的正是这个：查询 rewrite 时把各段本地
`docFreq` 求和得到 shard-global df（`TermStates.build` 遍历各段累加），算**一个** idf，再用它
给所有段打分 → **零 idf 漂移**，质量等于单一大索引。而**不**维护一张单独的全局 df 表（我们
先前担心的 flush/merge/删除增量维护耦合，ES 根本不付）。P（per-segment idf 各打各的）漂移、
非 ES；G-维护表成本高、亦非 ES——两者都不取。ES 的 `dfs_query_then_fetch` 是**跨 shard**
（分布式）那一级的事，本设计单节点不涉及（见 §9 未来分布式备注）。

落地前提（均满足/低成本）：
1. 段 term-dict 廉价暴露 per-term df —— `PostingList` 的 df/list 长度已有 ✓；
2. 段头存 `N_s`、`Σdoc_len`（§2.2 已列）→ 阶段 1 求和即得全局 N/avgdl；
3. 查询额外成本 = 每 term 每段一次 df 查，而该 term 的 posting list 本就要在阶段 2 打开 →
   顺带取 df，近乎免费。

**接受的近似**：段 df **含尚未 merge 掉的已删文档**（删除只翻 `live_docs` 位、不减 df），
故 df 略高估，**merge 时自愈**（物理删死 doc → df 归正）。纯追加/低删除负载几乎无影响——
Lucene 同样直接接受。

其余待定（Stage 2 深挖）：k 路归并的堆结构与短路；段并行查询的线程模型（复用 IndexPool 的
map worker？还是查询独立池）；段数上限与 merge 触发的耦合；`HybridSearcher` RRF 去重键从
ord 改为 `(seg_id, docid)`（`ord-recycling-design-zh.md` §6：RRF 必须以文档版本为键——段内
docid + seg_id 唯一确定一个版本）。

## 5. 写入路径

### 5.1 Stage 3（单写者，先落地）

现单 reducer 不变，但 `apply_job` 目标从「全局 InvertedIndex」改为「当前 Building 段」。
checkpoint = flush 当前段成不可变段（而非写 delta）。docid 在段内 build 时按加入顺序 0,1,2…
分配；`ord`(LSN) 仍全局分配、存进段 doc_store。**此步即可删掉 delta 链代码。**

### 5.2 Stage 4（并行 builder，DWPT）

多个 Building 段，各自段内单线程（`InvertedIndex` 本就单写者安全），文档按写入分配给不同
builder（round-robin 或 per-writer-thread）。flush 各自成段。**吞吐红利在此落地**——而此时
查询归并（Stage 2）、段 merge、liveness（Stage 3）均已就绪。

关键：并行 builder **不共享任何可变态**、**不需要跨线程 ord 定序**——这正是段模型相对
term-sharded（①）的根本优势：把 per-shard 水位 / 跨 shard 原子性 / 全局统计竞争这些坑
**用架构绕开**，而非硬解。可见性放松为 refresh 语义（段发布后才可查），本就是近实时索引的
标准模型。

## 6. 诚实的难点

1. ~~BM25 跨段统计~~ —— **已定：G-on-the-fly（§4，对标 ES 段级）**，零漂移、不维护全局表。
2. **查询扇出成本**——查 M 段比查 1 索引贵（M 次表查 + M 路归并）。靠 tiered merge 压小 M。
3. **向量/HNSW 每段建图 + merge 期合并图**——公认贵活（Lucene HNSW 亦然）。可先让向量走
   「单全局 HNSW、留在 reducer 串行」，段化只覆盖 BM25；向量段化作为独立后续。
4. **merge 期一致性**——rebuild 段期间，查询要看到「旧段仍活 + 新段就绪」的原子切换（manifest
   rename 提交点保证），死段引用计数归零才删（沿用现 shared_ptr 锚定 + merge 协议）。
5. **段数膨胀 vs merge 频率**——tiered 策略权衡：段多则查询慢，merge 勤则写放大高。

## 7. 分阶段落地

| Stage | 内容 | 独立价值 | 风险 |
|---|---|---|---|
| **1** | 概念解耦：doc_store 内 docid 与 ord(LSN) 分离的映射层（可先在单索引内做） | 打地基 | 低 |
| **2** | 段抽象上盘面 + **多段读**（查询归并 + BM25 统计策略），哪怕只有 1-2 段 | 验证查询侧最难的一块 | 中 |
| **3** | 段累积替换 delta 链：checkpoint flush 新段 + 后台 merge。仍单写者 | **删 delta 链 + 死内存回收** | 中 |
| **4** | 并行 builder（DWPT） | **吞吐红利** | 中高 |

排序刻意把**吞吐提升放最后**：前三步各自有独立价值（Stage 3 光删 delta 链就值），且每步都
不碰 ① 的那些坑。可逐步合入、随时回退。

## 8. 迁移与兼容

- **格式**：段文件复用 `bm25.ckpt` base 段格式 + 段头；旧 `bm25.ckpt`/`.d*` 链在首次 open 时
  一次性读入合成「初始段」，之后不再写 delta（一次性迁移，类似 S17-5 legacy 迁移路径）。
- **manifest**：新增段清单结构；旧 `index.manifest` 识别为 legacy → 触发迁移。
- **API**：查询/写入公开面不变（多段归并对上层透明）；`SearchHit.ord` 语义保持（LSN），
  内部 `(seg_id, docid)` 不外泄。

## 9. 开放问题（Stage 2 深挖清单）

1. ~~BM25 跨段统计~~ —— **已定：G-on-the-fly（§4，对标 ES 单节点/段级）**。
2. ~~段本地 doc_store 结构~~ —— **已定（§3.4）**：封口段平坦定长（chunk 退役）承载 R2/R3/R4；
   全局 resolver `key→(seg_id,docid)`（B1）担 R1；保留 `lsn[docid]` 供 RRF。
3. ~~多段查询的线程模型 / k 路归并~~ —— **已定（§3.5）**：串行逐段 + 全局第 k 名阈值传播 +
   大小 k 的 min-heap 并集归并；查询独立于 IndexPool；段归并作 `search_fields` 内层。并行后置。
4. ~~RRF/混合检索去重键~~ —— **已定（§3.4/§3.5）**：BM25 段化、向量留全局期间沿用全局 LSN(ord)
   并桶，`HybridSearcher` 零改；向量段化后再评估 `(seg_id,docid)`。
5. 段头格式与 CRC 隔离：沿用 `SearchCheckpoint` 段级 CRC 容器？
6. 向量段化的取舍：BM25 先段化、HNSW 暂留全局，何时段化向量？
7. （未来分布式，非本设计）跨 shard idf：默认 per-shard（P）、可选 `dfs_query_then_fetch`（G）。

> **Stage 2 设计基线已定**：BM25 跨段统计（§4，G-on-the-fly）+ 段本地 doc_store（§3.4）+
> 多段查询归并（§3.5）三块闭合。剩余开放项 §9.5/§9.6 属段落盘格式与向量段化，随 Stage 2/3
> 实现细化。
>
> **实现从 Stage 1（S27-1）起步**：先把 **LSN 与 docid 概念拆分**——建立 `Lsn`（全局单调、
> 恢复/MVCC/幂等水位）与 `DocId`（倒排/数组下标，将来段内本地、merge 可重编）的类型与接口
> 区分，**当前二者数值仍相等**（docid==lsn==ord），零行为变更。目的：为后续「docid 段内本地化 +
> merge 重编码」把耦合点显式化、类型化，避免届时盲扫 20 处 ord 不变量（见
> `ord-recycling-design-zh.md` §3）。
