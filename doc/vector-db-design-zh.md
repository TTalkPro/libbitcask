# 向量库设计：在 Bitcask 引擎上原生扩展（BM25 + Embedding）

本文是 libbitcask 在 C++ Bitcask 引擎之上构建**混合检索向量库**的最终设计——
按当前代码（Phase 1 / 2a / 2b 已落地）描述 as-built 状态，不保留早期蓝图
阶段的「待定/探索」标签。配合 [`cpp-arch.md`](cpp-arch.md)（实现地图）、
[`format-zh.md`](format-zh.md)（磁盘格式）、[`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)（checkpoint 协议）阅读。

> **定位**：不保留 legacy Bitcask 兼容，把 KV 引擎升级成一个**单域**
> 的向量引擎：BM25 倒排（稀疏检索）＋ HNSW（稠密 ANN）＋ RRF 混合融合。

## 0. 已定决策（设计记录）

| 项 | 决策 | 推荐 / 备选 / 决定 / 理由 |
|---|---|---|
| 兼容性 | 放弃 legacy Erlang 字节兼容，flag-day 切小端 | **推荐** flag-day 切 LE（x86/ARM64 原生零转换，mmap 零拷贝）。**备选** 保留双字节序（设计 / 测试 / 迁移成本三倍，且与 mmap 不兼容）。**决定** flag-day，旧大端目录走 `tools/migrate_le`。**理由**：LE-only 让格式契约确定，x86/ARM64 上零 byte-swap；mmap 直读不需要 endian-aware 字段反序列化。 |
| 单域引擎 | 把 KV/索引统一到一份 data file + 一套恢复路径 | **推荐** 单域：消除跨域 GC 耦合 / 恢复漏删 / 双调度器。**备选** 双域（KV 一套、向量库一套）：分文件 + 分恢复路径 + 分 GC。**决定** 单域。**理由**：HybridSearcher 需要两域「同一文档同一 ord 同一 ext_id」，跨域会迫使 ord↔key 双向翻译无处不在；单域下三栈共享 Index/ord。 |
| 文档模型 | 一条文档 = 一条 kDoc record，text+vector+meta 打包进 value | **推荐** 单 record 自包含。**备选** text record + vector record 两份（同 ext_id 双写）。**决定** 单 record。**理由**：append-only 模型「改=写新+旧变垃圾」自然延伸到 vector；少一次写、少一个 ord 分配、merge 不用协调两份同 ext_id 的 record；回放单 record 即重建一切。 |
| ord | 引擎单调分配（per-write），永不复用 | **推荐** 单调 u64。**备选** 复用 slot（同 LSM segment compaction remap）。**决定** 单调不复用。**理由**：HNSW 节点号、倒排 posting 下标、恢复回放「ord > W 尾巴」全部依赖 ord 在崩溃恢复下依然单调；不复用导致 ABA + 需要 remap posting/HNSW，开销远大于数组稀疏 8B/槽。 |
| 字节序 | 全盘小端（header 整数、向量 f32、checkpoint 容器） | **推荐** 固定小端。**备选** 平台原生（endian-aware 反序列化）。**决定** 固定小端。**理由**：x86/ARM64 都是 LE，「固定 LE == 平台原生」→ memcpy 零转换；mmap 不需做 endian 修正；格式契约确定。 |
| 稀疏索引 | BM25 倒排，分词 + DAAT + Block-Max WAND | **推荐** Block-Max WAND（top-k 剪枝）。**备选** 朴素 DAAT 全扫。**决定** BMW 必备：百万级倒排朴素扫全表不可能。**理由**：posting 数 ∝ 文档 × term，热词 = 数万 posting → 不剪枝不可用。 |
| ngram 分析 | 默认字符 bi/tri-gram，中英混合无依赖 | **推荐** 字符 ngram。**备选** Jieba 中文分词。**决定** 字符 ngram 为默认，Jieba 为可选注入（`JiebaAnalyzer`，open-time 配置）。**理由**：CJK 无空格、混合文本（"iPhone 15 Pro"）字符 ngram 最稳；Jieba 精度更高但 vendored 依赖重，开放给有需要的部署。 |
| 稠密索引 | HNSW 单图常驻内存，per-node 细粒度锁 | **推荐** 单图常驻 + 单写者/多读者。**备选** 多段 + buffer（LSM 式）。**决定** 单图，**接口/段格式预留多段**（V7 BCVS 双文件 + payload 代号）。**理由**：≤1M 召回最好、查询最简；HNSW 近似检索不需要 MVCC 一致性快照；自迁移协议让并发查询无需 keydir 锁；超百万再走多段。 |
| 度量 | cosine（写入端归一化）、dot、L2 | **推荐** cosine normalized（库内归一化，HNSW 仅见 kDot）。**备选** dot / L2 直存。**决定** 三种全支持，但 cosine 由写入端归一化（存储即归一化值）；HNSW 只见 kDot/kL2 两种原始度量。**理由**：cosine 是检索最常用度量；上游归一化让 HNSW 无需每 query 再归一化、距离单调可比。 |
| 量化 | int8 对称量化（`kFlagVecQuantized` + `inmem_int8`） | **推荐** int8 对称量化（P3a 落盘 + P5b 内存）。**备选** f32 常驻 / PQ。**决定** int8 对称：4× 内存缩减、VNNI 加速、两档可独立开。**理由**：int8 VNNI 在 x86 是免费加速；对称量化按 `max|v[i]|` 缩放足够好；PQ/scalar 留给超大规模。 |
| 接口面 | `put_doc` + `search_text/vector/hybrid` + 批量版 | **推荐** `Searcher` 类型化门面（新代码） + `Cask::search_*` 薄委托（源兼容）。**决定** 两层并存。**理由**：门面保证 read-your-writes（`drain_plugins`）；门面方法签名 `expected<...>` 强类型错误，翻译在 Cask 边界。 |
| 混合融合 | RRF（Reciprocal Rank Fusion），常数 c=60 | **推荐** RRF（无权重）。**备选** 加权融合（`w_text * bm25_score + w_vec * vec_score`）。**决定** RRF 固定 c=60，无权重旋钮。**理由**：BM25 / cosine 分数尺度不可比，加权融合需要归一化 → 阈值敏感；RRF 只用 rank，跨域鲁棒，论文推荐 c=60。 |
| K' (overfetch) | `max(k×4, 64)` | **推荐** 4× 起点。**备选** 2× / 8× / 全部。**决定** `max(k×4, 64)`。**理由**：4× 是 RRF overfetch 的常见甜点；下界 64 保小 k 时两路至少有合理候选池。 |
| 恢复协议 | 单 watermark 自门 + per-component ckpt + manifest commit | **推荐** per-component 独立 ckpt + 80B manifest 唯一 commit 点。**备选** 单 ckpt 文件分多段（早期 P14e 形态）。**决定** per-component + manifest。**理由**：损坏隔离到组件（docmap 坏 ≠ bm25 坏 ≠ vec 坏），单段坏单组件重 fold；manifest 是 80B + CRC + tmp+rename，比多段 ckpt 原子性更确定。 |
| 索引模式 vs 纯 KV | `enable_search=true`（+ `vector_dim>0` 可选）开启索引模式 | **推荐** 模式分离。**备选** 永久索引模式（强制所有库都建索引）。**决定** 模式分离，纯 KV 库零开销。**理由**：纯 KV 用 hint fold 即可恢复，索引模式需要 fold + 组件 load，差异显著。 |
| 升级路径 | `Cask::upgrade(dirname, search_config)` 离线 KV→索引模式 | **推荐** 离线一次性升级（不取 write.lock / merge.lock）。**备选** 在线热升级（atomic 切 meta + 异步建索引）。**决定** 离线升级，文档级契约「目录必须离线」。**理由**：在线热升级引入双格式过渡期与查询态转换，复杂度爆炸；离线一次性 fold 全库 = 分钟级，明确可控。 |

## 1. 总体架构（KV + BM25 + HNSW 三栈）

```
┌─────────────────────────────────────────────────────────────┐
│ 查询门面  Cask::search_* (薄委托)  /  Searcher<text|vec>    │
│           HybridSearcher  ── RRF(60) ── text + vec 双路融合  │
├─────────────────────────────────────────────────────────────┤
│ 索引模式三栈                                                │
│  ┌─ TextPlugin ─────────────────────────────────────────┐  │
│  │ Analyzer (ngram / Jieba) → InvertedIndex             │  │
│  │   bm25 k1/b, Block-Max WAND, SIMD tf_norm kernel     │  │
│  │   per-shard tbb::concurrent_hash_map, CoW posting    │  │
│  ├─ VectorPlugin ────────────────────────────────────────┤  │
│  │ normalize_for_write → HnswIndex                       │  │
│  │   M=16, ef_construction=200, cosine normalized, int8 │  │
│  │   single writer / multi reader (atomic<NodeChunk*>)  │  │
│  ├─ DocMap (Index) ─────────────────────────────────────┤  │
│  │ ext2ord / slots / ord2ext / live / doc_lens / meta   │  │
│  │ 宿主服务,三栈共享身份表,实现 DocTable 窄接口        │  │
│  └───────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│ 数据层 (单一 append WAL)                                    │
│   *.bitcask.data  ── typed record (kDoc / kTombstone)      │
│   *.bitcask.hint  ── 元数据快速重建                          │
│   bitcask.meta    ── mode + 向量配置 + CRC                  │
│   field.schema    ── 字段名 ↔ id 注册表                      │
│   kv.keydir.ckpt  ── keydir 快照 (single section)          │
│   docmap.ckpt     ── 组件 base + .prev + .d<seq> 链        │
│   bm25.ckpt       ── 组件 base + .prev + .d<seq> 链        │
│   vec.ckpt        ── 组件 base + .prev + .d<seq> 链        │
│   index.manifest  ── 80B 三组件统一 commit point             │
│   search.vec      ── HNSW f32 payload (mmap)               │
│   search.qc8      ── HNSW int8 量化码字 (int8-only)        │
│   search.ckpt     ── [legacy] pre-S17 单文件迁移源           │
└─────────────────────────────────────────────────────────────┘
```

**关键原则**：

- **只有一个 data file / 一个 epoch / 一个恢复路径**——三栈共享 KV 权威日志，
  各栈的索引是日志的派生缓存。
- **ord 是 per-write 单调序号**，跨栈统一：HNSW 节点号、倒排 posting 下标、
  docmap slot、keydir LWW 顺序——同一数值在所有栈含义一致。
- **DocMap（`index::Index`）是宿主服务**，三栈都借 `DocTable` /
  `DocLenWriter` / `CompactionStats` 三个窄接口访问，插件不持身份表，
  保证查询面只读纪律。
- **per-component ckpt 损坏隔离**：docmap 段坏 ≠ bm25 段坏 ≠ vec 段坏，
  `manifest::min_chain_watermark()` 取最小值作为 fold 起点候选，单组件
  坏退该组件链头 fold。
- **`index.manifest` 是唯一 commit point**（80B + CRC + tmp+rename +
  目录 fsync），组件数据先于 manifest 落盘（`fdatasync` 屏障）。

## 2. 数据模型与 record 格式

### 2.1 原则：一条文档 = 一条 record

data file 沿用 Bitcask「**一条 record 一个 key、一次写一条**」的根本模型，
写入 / merge / 恢复的核心假设全部不变。**一次 `put_doc` 只 append 一条
record**，文本、向量、meta 打包进同一个 value：

```
Key   = ext_id                       业务主键(调用方指定)
Value = 序列化的 {vector, text, meta, fields} (DocValue v3)
```

`get(ext_id)` 一次 pread 取回整条文档（含文本 + 向量）。倒排索引和 HNSW
**不**往 data file 写任何额外 record——它们是内存派生结构，由 per-component
ckpt 承担持久化（见 §5）。

### 2.2 record 布局（typed record，V1）

`include/bitcask/format.hpp` 定义的 typed record 布局：

```
[0..3]    CRC32     (覆盖 Type..Value 区段)
[4]       Type      u8   (RecordType: kDoc / kTombstone)
[5..8]    Tstamp    u32 小端
[9..16]   Ord       u64 小端 (引擎单调分配的写入序号,per-write,永不复用)
[17..18]  KeySz     u16 小端 (key == ext_id)
[19..22]  ValueSz   u32 小端 (kDoc 时是 DocValue v3;kTombstone 时通常为 0)
[23..]    Key | Value
总长 = kHeaderSize + KeySz + ValueSz
```

| Type | 含义 | Value 内容 |
|---|---|---|
| `kDoc` (0) | 一条文档 | DocValue v3 打包的 `{vector, text, meta, fields}`（BM25 / HNSW 重建源） |
| `kTombstone` (1) | 删除标记 | value 通常为空，target 由 `Key=ext_id + Ord` 确定 |

> `Type` 区分 record 种类，**不**用来把文档拆成多条；向量不是单独 record。

### 2.3 ext_id 与 ord

- **ext_id**：调用方在 `put_doc` 时指定的文档主键，任意字节（如 `"doc-12345"` /
  URL / UUID）；用户用它增删查。即 data record 的 `Key`。
- **ord**：引擎分配的紧凑整数（u64），供倒排 posting 下标、HNSW 节点号、
  docmap slot 复用——ord 是**密集单调整数**，所有 `ord→X` 结构用数组而非
  hashmap（O(1) 下标 + 省内存）。

> **ord 是 per-write（每次写一个），不是 per-document（每文档一个）。**
> 引擎每 append 一条 record 就单调 `next_ord++` 分配一个新 ord——它本质是
> 「第几次写」的版本号。这一点对自洽很关键：
>
> - **更新**同一 `ext_id` ⇒ 写一条更大 ord 的新 record，**旧 ord 被软删**；
>   这是 append-only 的「改 = 追加新版本 + 旧版本变垃圾」，旧 ord 下次
>   merge 清理。
> - **`ext_id → ord` 映射指向当前最新 ord**；`ord → ext_id` 把检索结果翻译回主键。
> - 因 ord 随写入单调递增，§6 恢复的「回放 `ord > W` 尾巴」恰好覆盖「上次
>   ckpt 之后写的全部 record」，严格成立。

引擎维护 `ext_id → 最新 ord` 与 `ord → ext_id` 映射；两者皆为派生状态，
恢复时扫 record（Key=ext_id、header 带 ord）即可重建。

### 2.4 DocValue v3 value 打包布局

`include/bitcask/format.hpp` 中的 `kDocValueVersion = 3` 自描述打包：

```
DocValue v3:
┌───────┬────────┬──────────────┬──────────────┬──────────────┬──────────────┬─────────────┐
│ Ver   │ Flags  │ vector 段     │ text 段      │ meta 段      │ fields 段    │ ExpiryAt?   │
│ u8    │ u8     │ (可选)        │ (可选)       │ (可选)       │ (可选)       │ (可选)      │
└───────┴────────┴──────────────┴──────────────┴──────────────┴──────────────┴─────────────┘

flags 位:  bit0=has_vector  bit1=has_text  bit2=has_meta
           bit3=vec_quantized  bit4=has_fields  bit5=has_expiry

vector 段 (has_vector):
  [Dim:varint]
  [若 vec_quantized: SchemeVer:u8 + scale:f32 + int8 × Dim]   # P3a int8 对称量化
  [否则: f32 × Dim 小端]

text   段 (has_text):    [Len:varint][ utf8 字节 ]
meta   段 (has_meta):    [Len:varint][ 序列化字节 (msgpack/CBOR) ]
fields 段 (has_fields):  [FieldCount:varint] × { [FieldId:varint][ValLen:varint][value] }
ExpiryAt (has_expiry):   [ExpiryAt:u32 LE] 绝对 unix 秒,0 = 永不
```

设计要点：

- **`ver = 3`**：布局版本号，decode 只接受 v3；演进靠 bump ver。
- **`flags`**：文档可纯文本 / 纯向量 / 两者皆有，按位开关，缺段不占空间。
- **向量段放最前**：HNSW 重建只读 `2+varint(dim)` 字节即可 O(1) 切片出向量，
  BM25 重建按 varint 算向量段长度跳过，直达 text。两条偏路径都 O(1) 定位。
- **`dim` 冗余存一份**：collection 配置虽有 dim，record 自带 dim 使其自描述、
  对配置漂移/多 collection 鲁棒。
- **`vec_quantized` 位**：P3a int8 对称量化，方案进 SchemeVer；按 `max|v[i]|`
  缩放，重建 `v̂[i] = codes[i] * scale / 127`。
- **`fields` 段存 FieldId 而非字段名**：字段名 ↔ id 映射由 `field.schema`
  注册表维护，避免每条 record 重复内联字段名；decode 只还原 id，上层用
  schema 译回名字。
- **per-key TTL（`kFlagHasExpiry`）**：置位时 value 末尾追加 `ExpiryAt:u32 LE`；
  旧读端（不识别本位）按位忽略尾部字节 → 静默降级为永不过期。

#### 字节序规则（x86 + ARM64 均零成本）

| 字段 | 字节序 | 理由 |
|---|---|---|
| header 整数（`dim`/`len`/`ord`/`tstamp`） | **小端** | 与 `format.hpp` 既有契约一致 |
| 向量 `f32` 数组 | **固定小端** | x86/ARM64 都是 LE，原生零 byte-swap；格式仍良定义 |
| checkpoint 容器（ckpt 头/段载荷/页脚） | **小端** | mmap 零拷贝读 |

- 把磁盘格式定义为「f32 固定小端」（而非「平台原生」），契约确定。
  x86 与 AArch64（Linux/Apple Silicon/Graviton/Android 均 LE）上
  「固定 LE == 平台原生」→ memcpy 零转换；仅真正的大端主机（s390x 等，非目标）
  读时才 byte-swap，但格式始终良定义。
- **对齐无忧**：向量从 value memcpy 进对齐的内存数组后再算距离，value 内未对齐
  偏移不会被直接当 f32 解引用；memcpy 在 x86/ARM 上都安全处理未对齐。

## 3. 内存索引结构（三栈 + DocMap）

### 3.1 DocMap（宿主服务）：`index::Index`

`include/bitcask/index.hpp` 中 `Index` 是 legacy KeyDir 的演化版：主映射
从「key → location」改为「ext_id → 最新 ord」+ 一组「ord 下标的数组」
（`slots` / `ord2ext` / `live` / `doc_lens` / `meta_blobs`）。

```cpp
class Index : public bm25::DocTable,         // ord ↔ ext + live + doc_len
              public bm25::DocLenWriter,     // reducer 回填 doc_len
              public bm25::CompactionStats;  // 节流统计
public:
    // ord 分配
    std::uint64_t alloc_ord();

    // 写
    void put_doc(std::string_view ext_id, std::uint64_t ord, const DocSlot& slot);
    bool remove(std::string_view ext_id, std::uint64_t tomb_ord);
    void set_doc_len(std::uint64_t ord, std::uint32_t len) override;  // DocLenWriter
    void set_meta(std::uint64_t ord, std::span<const std::byte> blob);

    // 读
    std::optional<DocHit>          get(std::string_view ext_id) const;
    std::optional<std::string>     ord_to_ext(std::uint64_t ord) const override;
    bool                           is_live(std::uint64_t ord) const override;
    std::uint32_t                  doc_len(std::uint64_t ord) const override;
    std::vector<std::byte>         meta_blob(std::uint64_t ord) const;
    bool                           eval_meta(std::uint64_t ord,
                                            const meta::MetaFilter&) const override;

    // 批量（SIMD-gather 友好）
    void fill_is_live(std::span<const std::uint64_t> ords,
                      std::span<char> out) const override;
    void fill_doc_lens(std::span<const std::uint64_t> ords,
                       std::span<std::uint32_t> out) const override;

    // chunk 回收（merge 后调）
    std::uint64_t compact_chunks();

    // docmap sidecar 序列化（嵌入 search.ckpt kDocmap 段）
    bool serialize_docmap(std::vector<std::uint8_t>& out,
                          std::uint64_t covers_next_ord) const;
    std::optional<std::uint64_t>
    deserialize_docmap(std::span<const std::uint8_t> bytes);
private:
    std::unordered_map<std::string, std::uint64_t,
                       StringHash, std::equal_to<>> ext2ord_;   // ext_id → 最新 ord

    std::vector<std::unique_ptr<Chunk>> chunks_;                // chunk 64K ords
    std::vector<std::uint8_t>           live_;                   // 平坦(SIMD gather)
    std::vector<std::uint32_t>          doc_lens_;               // SoA(SIMD gather)
    std::vector<std::vector<std::byte>> meta_blobs_;            // V5 惰性化
    std::uint64_t next_ord_, live_docs_, retired_since_compact_, chunks_alloc_, chunks_freed_;
    mutable std::shared_mutex mutex_;
    std::atomic<bool> dirty_{true};
    std::vector<std::pair<std::string, std::uint64_t>> removals_;  // delta 窗口
};

struct DocSlot {
    DocLoc        loc;        // offset / file_id / total_sz
    std::uint32_t tstamp;
    std::uint32_t doc_len;    // BM25 token 数
};
```

存储布局：

- `ext2ord_`（`unordered_map<string, u64>`）：ext_id → 最新 ord，单点查 + 覆盖判断。
- `chunks_[ci]`（每 chunk 64K ords）：`slots[si]` 24B × 64K = 1.5MB，
  `ord2ext[si]` ~32B（SSO）× 64K = 2MB；`live_count == 0` 的 chunk merge 后释放。
- `live_` / `doc_lens_` / `meta_blobs_`：**平坦**（按 ord 下标）保持 SIMD gather
  友好；`meta_blobs_` 惰性化（首个非空 set_meta 前恒 empty）。
- ord 永不复用 ⇒ `chunks_` / `ord2ext` 长度 = 历史总写入数，更新多了出现空洞 →
  **merge 时 compact_chunks 释放空 chunk**（Tiered Arrays 方案 B，详见
  [`ord-recycling-design-zh.md`](ord-recycling-design-zh.md)）。

并发模型：

- `shared_mutex`：`get/eval_meta` 持 shared_lock；`put_doc/remove/set_*` 持 unique_lock。
- 三栈（TextPlugin / VectorPlugin / HybridSearcher）通过 `DocTable` /
  `DocLenWriter` / `CompactionStats` 三个**只读或窄写接口**借用，插件
  不持 `Index&`，保查询面只读纪律（S16-3）。

### 3.2 文本索引：`TextPlugin` + `InvertedIndex`

`include/bitcask/text_plugin.hpp` / `include/bitcask/inverted.hpp`。

**TextPlugin 顶层**：

```cpp
class TextPlugin final : public plugin::CaskPlugin {
public:
    // 单字段/多字段倒排(per-field InvertedIndex) + analyzer + 查询缓存 + 高亮
    // 借用 DocTable& (live/doc_len/翻译),DocLenWriter& (回填),CompactionStats& (节流)
    plugin::PluginStatus open(const plugin::OpenContext& ctx) override;
    void on_put(const plugin::PutEvent&, plugin::PreparedPtr) override;
    void on_delete(const plugin::DeleteEvent&) override;
    plugin::FlushResult  flush(const plugin::FlushRequest&) override;
    void                 on_merge_commit(const plugin::MergeCommitEvent&) override;

    std::vector<search::SearchHit>
    search_text  (std::string_view query, std::size_t k,
                  const Bm25Params* p = nullptr,
                  const meta::MetaFilter* f = nullptr) const;
    std::vector<search::SearchHit>
    search_phrase(..., std::size_t k, ...) const;
    std::vector<search::SearchHit>
    search_near  (..., std::uint32_t slop, std::size_t k, ...) const;
    std::vector<search::SearchHit>
    bool_search  (..., std::size_t k, ...) const;
    std::vector<search::SearchHit>
    bool_search_tree(const QueryNode& root, ..., std::size_t k, ...) const;
    std::vector<search::SearchHit>
    search_fuzzy (..., std::uint32_t max_edit_distance, std::size_t k, ...) const;
    std::vector<search::SearchHit>
    search_wildcard(const std::string& pattern, std::size_t k, ...) const;
    ScoreExplanation
    explain(const std::vector<std::string>& terms, std::uint64_t ord, ...) const;
};
```

**InvertedIndex 内部结构**：

```cpp
struct PostingList {                            // S22-M6 SoA
    std::vector<std::uint64_t> ords;            // 严格升序无重复
    std::vector<std::uint32_t> tfs;
    std::vector<std::uint32_t> dls;             // 索引时 doc_len(WAND min_dl)
    std::vector<std::uint32_t> pos_data;        // positions 扁平存储(可选)
    std::vector<std::uint64_t> pos_off;         // 惰性物化(空 = 未启用)
    std::vector<PostingBlock>  blocks;          // Block-Max WAND 跳跃索引
    std::uint32_t max_tf = 0;
};

class InvertedIndex {
public:
    void add_doc   (std::uint64_t ord, const TermPositions& term_data);
    void remove_doc(std::uint32_t doc_len,
                    const std::unordered_map<std::string, std::uint32_t>& term_freqs);

    std::vector<SearchResult>
    search(const std::vector<std::string>& query_terms, std::size_t k,
           const LiveChecker& live, const Bm25Params* p = nullptr) const;

    // 链 delta 协议(S14-4)
    void                 serialize(std::vector<std::byte>& out) const;
    void                 serialize_delta(std::vector<std::byte>& out, std::uint64_t from_ord) const;
    bool                 apply_delta(std::span<const std::byte> bytes);
    std::optional<...>   deserialize(std::span<const std::byte> bytes);

    std::size_t compact(const LiveChecker& live, double dead_ratio = 0.5);

private:
    static constexpr std::size_t kShardCount = 64;          // 64 分片,term hash 分桶
    std::array<Shard, kShardCount> shards_;                 // 每 shard 一张 tbb::concurrent_hash_map
    std::atomic<std::uint64_t> live_doc_count_, sum_doc_len_, max_indexed_ord_;
    Bm25Params params_;
    bool index_positions_;
};
```

关键设计点：

- **analyzer**：NFKC 归一 → CJK 字符 bi/tri-gram / 拉丁空白切分+小写 → （可选停用词）。
  写与查用同一条链；Jieba 为可选注入。
- **评分**（BM25+）：
  `tf_norm = tf*(k1+1) / (tf + k1*(1-b) + k1*b*dl/avgdl)`
  `contrib = idf * (tf_norm + delta)`，`delta = 1.0`（BM25+ 扩展，缓解长文档过度惩罚）。
  `idf = log(1 + (N - df + 0.5) / (df + 0.5))`（Lucene 标准）。
- **Block-Max WAND**：每 128 条 posting 一块，块内维护 `max_tf` + `min_dl`（v5
  impacts）作分数上界；查询时对 term posting 按 `idf * max_tf_norm` 排序，
  贪心推进 cursor，剪掉无望 doc。阈值 `kWandThreshold = 1024` 才走 WAND，
  小集合走朴素 DAAT（避免 WAND setup 成本）。
- **SIMD tf_norm 内核**（`include/bitcask/bm25_kernels.hpp`）：
  AVX-512F (16 lanes)、AVX2+FMA (8 lanes)、scalar fallback，运行时按
  `__builtin_cpu_supports` 一次性探测并缓存。
- **并发**：每 shard `tbb::concurrent_hash_map<std::string, shared_ptr<PostingList>>`
  自带桶级锁，CoW 发布协议——读 phrase/near 持引用零拷贝；写者
  `use_count == 1` 原地改、`> 1` 克隆替换。query 路径无锁读。
- **df 漂移**：posting 残留死点使 df 偏大；查询时 `live_checker.fill_is_live`
  批量过滤（一次 shared_lock + 数组直读，SIMD gather 友好）。merge / 自动
  compaction 重算 df（`auto_compact_dead_ratio` 阈值触发，reducer 线程内
  串行）。
- **持久化**：基线 `bm25.ckpt`（`kBm25Default` + `kBm25Fields` 段）+
  delta 链 `.d<seq>`（`kBm25DefaultDelta` / `kBm25FieldsDelta` 段，每个
  delta 含 `kDeltaInfo` 校验三元组 `base_gen / prev_wm / seq`）；链走读
  `bitcask::search::walk_chain` 统一模板。

### 3.3 向量索引：`VectorPlugin` + `HnswIndex`

`include/bitcask/vector_plugin.hpp` / `include/bitcask/hnsw.hpp`。

**VectorPlugin 顶层**：

```cpp
class VectorPlugin final : public plugin::CaskPlugin {
public:
    // dim>0 构造 HnswIndex;cosine → 写入端归一化,HNSW 只见 kDot
    std::expected<std::span<const float>, const char*>
    normalize_for_write(std::span<const float> input,
                        std::vector<float>& norm_buf) const;

    // 单写者(Reducer)插图
    void insert(std::uint64_t ord, std::span<const float> v);

    // 多读者查询
    std::vector<search::SearchHit>
    search(std::span<const float> query, std::size_t k, std::size_t ef,
           const meta::MetaFilter* filter = nullptr) const;

    // merge 物理清死
    void on_merge_commit(const plugin::MergeCommitEvent&) override;

    plugin::PluginStatus  open(const plugin::OpenContext&) override;
    plugin::FlushResult   flush(const plugin::FlushRequest&) override;
};
```

**HnswIndex 核心配置（`include/bitcask/hnsw.hpp::HnswConfig`）**：

```cpp
enum class HnswMetric : std::uint8_t {
    kDot = 0,    // cosine_normalized 上游已归一化 → 走内积
    kL2  = 1,    // 平方欧氏距离
};

struct HnswConfig {
    std::uint16_t dim = 0;
    HnswMetric    metric = HnswMetric::kDot;
    std::uint32_t M = 16;                // 上层邻居容量;L0 层 = 2M
    std::uint32_t ef_construction = 200; // 建图搜索宽度
    std::uint64_t seed = 0x5EEDF00Dull;  // 层数抽样种子(测试可复现)
    // P5: int8-only 内存模式 → NodeChunk 不存 f32(vecs 容量 0),
    // 建图/查询全程 int8,向量内存 ~−80%。仅 kDot;
    // kL2 不支持(上游 open 接线拒绝)。
    bool          inmem_int8 = false;
};
```

**默认参数与边界**：

- `M = 16`，`ef_construction = 200`，`mL = 1 / ln(M)`（层数抽样参数）。
- cosine → 写入端归一化（`VectorPlugin::normalize_for_write`），存储即归一化值；
  HNSW 内只见 `kDot`（内积等价 cosine） / `kL2` 两种度量。
- `dim` 库内恒定（`CaskOptions::vector_dim`，与 `bitcask.meta` 校验一致），
  HNSW 只收向量不算向量。
- 单图常驻内存：`kChunkBits = 16` → 65536 节点/chunk；chunk 内 `vecs / ords /
  levels / adj` 指针 + per-node 自旋锁全部**定容预分配**，地址稳定。
- `kMaxChunks = 1024` → 上限 64M 节点；`kAdjSlabWords = 256K u32` →
  1 MB per-chunk arena。

**并发协议（V3.3 单写者 + 多读者）**：

- **写者**：reducer 线程单写，`writer_active_` debug assert 守卫。
  写满本节点数据 → `count_.store(id+1, release)` 发布 → 再做连边。
- **per-node 自旋锁 / seqlock**：写者改邻接持该节点锁；读者读邻居前
  持同一把锁把 `[count][ids]` 拷到栈缓冲再放锁。临界区 ~百 ns。
  S13-P7 起改为 per-node seqlock（`atomic<uint32_t>` 序号）——读者双读
  序号一致才采信；写者是单线程无需互斥，只需发布协议。
- **`entry_point/max_level`**：合并单 `atomic<u64>`，高 32 位 = level+1
  （0 = 空图），低 32 位 = entry id；insert 完整连边后才更新。
- **删节点不感知**：上层经 live 过滤回调在结果侧滤死，死节点留作图内
  路标，merge 重建时物理清除。
- **水位幂等**：`max_inserted_ord_`（atomic u64）记录已插最大 ord；
  `insert(ord ≤ 水位)` 直接丢弃，崩溃回放重叠区安全。

**距离计算**：

- `dist_id(q, id)`：f32 内积 / L2，AVX2 (x86) / NEON (ARM) 加速。
- `dist_id_int8(query_codes, query_scale, query_sum, id)`：P5 int8 两阶段
  检索的粗筛路径——`int8_dot_` 内核（VNNI），与 f32 路径并行不互相干扰；
  QVector 的 sum_codes 由 quantize() 预算好供 VNNI 偏置补偿。
- `kDot` 度量下取重建内积的负（与 f32 路径同「越小越近」约定）。

**持久化（V7 BCVS v2 + 量化外置）**：

- `serialize` → BCVS v2 header 字节流（嵌入 `vec.ckpt` 的 `kHnsw` 段），
  含 qcodes/qscales/qsums 直存 + `has_payload` 标志 + payload 元信息
  （dim/count/watermark）+ `payload_gen`（代号防 `.prev` 错配）。
- `save_vec_payload` → `search.vec`（f32 BCVP：48B header + 每 4KB 页
  CRC32 表 + 页对齐 vecs 数据区，tmp+rename 原子）。S14-2 起支持增量追加
  路径——身份匹配（dev/ino）只写 `[vec_file_.count, count_)` 增量 + 原地
  重写 64B header；前缀不变契约保证 torn append 安全。
- `save_qc_payload` → `search.qc8`（int8 量化码字，BCQ8 v1）；同前缀契约
  + 身份收养 + 追加机制。
- `inmem_int8` 模式下 `has_payload = false`，不产生 `search.vec`（无常驻 f32）；
  `kL2` / 无 VNNI 时 qcodes 不分配、不产生 `search.qc8`。

### 3.4 DocMap ↔ 三栈的分工

§3.1 的 DocMap **本身不做检索**——它只是「按 ord 存每篇文档边角料」的
一组**侧表**。把「查询词 → 候选文档」连起来的是 §3.2 的**倒排索引**，
把「query 向量 → 候选 ord」连起来的是 §3.3 的 **HNSW**。三者分工：

```
倒排索引 (§3.2)          HNSW (§3.3)             DocMap 侧表 (§3.1)
term → [(ord,tf)...]   node_id ↔ ord             ord → {doc_len, ext_id, live, location, meta}
       ↑                      ↑                          ↑
  "查询词命中哪些 ord"  "近邻向量是哪些 ord"    "给定 ord,O(1) 查它的信息"
```

**为什么 `ord→X` 用数组、`ext_id→ord` 用 hashmap**：ord 是密集连续整数
（0,1,2…），数组下标即 ord，O(1) 且省内存；ext_id 是任意字符串，只能哈希。

#### 一次 `search_text("降息", k)` 如何用到三栈

```
1. analyzer 切词："降息" → ngram ["降息"]
2. 查倒排：inverted["降息"] → [(7,tf=1),(42,tf=2)]         # 仅产出候选 ord
3. 逐候选打分（用 DocMap 取数据）:
     if live[ord]==0: continue                              # fill_is_live 批量过滤
     dl  = doc_lens_[ord]                                   # fill_doc_lens 批量取
     idf = log(1 + (N - df + 0.5)/(df + 0.5));
     score[ord] += idf * (tf*(k1+1)/(tf + k1*(1-b) + k1*b*dl/avgdl) + delta)
4. top-k 堆选最高分的 ord
5. 翻译：ext_id = ord2ext[ord]                              # DocTable::ord_to_ext
6. （可选）回显原文：slots[ord]→(file_id,offset)→pread kDoc
```

| 步骤 | 用到 | 作用 |
|---|---|---|
| 2 | 倒排索引 (§3.2) | term → 候选 ord |
| 3 | `live[ord]` / `doc_lens_[ord]` / `live_doc_count`,`sum_doc_len` | 过滤死点 + BM25 打分 |
| 5 | `ord2ext[ord]` | ord → 返回给用户的 ext_id |
| 6 | `slots[ord]` location | pread 原文 |

> 一句话：**倒排索引负责「词 → 一堆 ord」；HNSW 负责「query 向量 → 一堆 ord」；
> DocMap 负责「给定 ord, O(1) 查 doc_len / live / ext_id / 磁盘位置」。**
> 检索 = 倒排 / HNSW 出候选 ord，再用 DocMap 的 ord 下标数组取打分数据与 ext_id。

#### 一次 `search_vector(query_vec, k)` 如何用到三栈

```
1. VectorPlugin::search(query_vec, k, ef, filter):
     live callback = [&DocMap](ord) -> filter 命中 + live[ord]
     HnswIndex::search(query, k, ef, &live_callback)
2. HNSW 内部:
     入口 entry → 顶层贪心下降 → 底层 WAND-style search_layer →
     结果侧过滤 live_callback 不通过的节点
3. 翻译: ext_id = DocMap::ord_to_ext(ord)
4. 返回 SearchHit { ext_id, ord, score }   # kDot 内积 / kL2 -平方距离
```

## 4. 写路径

### 4.1 提交点原则

> **append 到 data file = 提交点；所有索引状态都是「持久日志 + 墓碑」的确定性
> 函数，可由回放重建。**

由此而来的两条铁律：

1. **先持久 append，后更新索引**——绝不能反过来。否则索引可能引用一条没落盘的
   record，崩溃后检索会返回不存在的文档。
2. **一致性恒成立，持久性看 sync 策略**：索引是内存态，append 未 fsync 前崩溃，
   record 和索引更新（都在内存 / OS buffer）一起丢，恢复从存活日志重建 →
   索引始终与日志一致；「这条写入是否扛得住崩溃」才取决于 fsync。

### 4.2 异步索引 MapReduce 流水线

Cask 的索引路径不阻塞写者：`put_doc` → 写 data file + hint file + keydir
（在 `write_mu_` 串行段内完成）→ **释放 write_mu_ 后**经 `IndexPool` 提交
到后台 reducer 流水线。

```
write_mu_ 内:                  写者锁外（SubmitIndexTask）:       后台 reducer:
┌─────────────────────────┐    ┌──────────────────────────┐    ┌──────────────────────┐
│ alloc_ord               │    │ IndexPool (有界队列,     │    │ N 个 map worker 并行  │
│ encode DocValue v3      │ →  │  capacity 10240,         │ →  │   plugin->prepare()   │
│ write_and_keydir        │    │   满则背压写者)          │    │   (TextPlugin 分析)  │
│   - data file append    │    │                          │    │ per-lane reorder buf │
│   - hint file append    │    │ IndexPool 由 KeyDir      │    │   (按 ord 排序)      │
│   - keydir put          │    │ Registry 共享,N map      │    │ 单 reducer 串行 apply│
│ docmap->put_doc         │    │ worker + 1 reducer,      │    │   docmap_(已 apply)  │
│ flush_index? (wait)     │    │ 与库数无关               │    │   plugins_->on_put   │
└─────────────────────────┘    └──────────────────────────┘    └──────────────────────┘
                                                                ↑ 单写者 I3 协议
```

- **IndexPool**：TBB-backed 有界队列 + N+1 工作者线程；满则 push 阻塞写者，
  自然限速，N = `hardware_concurrency()`（G1 真数据并行）。
- **per-lane reorder buffer**：map worker 产出按 ord 排序后入 reducer 静止点，
  保证 ord 全局单调的应用序（concurrent_hash_map 遍历安全前提）。
- **reducer 单写者**：docmap 由 reducer 持 unique_lock apply（put_doc/
  set_doc_len/set_meta），保证 meta 与定位/live 同写入原子点。
- **背压**：池满时阻塞写者；H1 改后释放 `write_mu_` 后才提交（背压只阻塞
  本写者，不冻其他写路径）。
- **OrSkipGuard**：写路径 alloc_ord 后出错必须补 Skip，否则 reducer
  next_apply_ord 永久空洞，后续 flush/merge/close 在 cv 上永久阻塞。

### 4.3 `put_doc(key, doc)` 路径

```
write_mu_ 内:
  1. alloc_ord() → ord
  2. encode DocValue v3 (vector + text + meta + fields + 端到端归一化/量化)
  3. write_and_keydir(key, encoded, tstamp, ord) → 提交 data file + hint + keydir put
  4. docmap_->put_doc(ext_id, ord, DocSlot{loc, tstamp, doc_len=0})
     （doc_len=0 占位,token 数 BM25 侧分析后回填）
  5. prepare_index_task(task) → IndexTask (含 ord, doc 含 text+vec, ...) 经 SubmitIndexTask
write_mu_ 外（reducer 链）:
  6. docmap 已 apply（步骤 4）
  7. TextPlugin::on_put:  analyzer 切词 → DocMap::set_doc_len(ord, doc_len) → InvertedIndex::add_doc
  8. VectorPlugin::on_put: if (vec 非空) HnswIndex::insert(ord, vec)（单写者）
```

### 4.4 update 时的软删与计数

update 不写墓碑（同 ext_id 的更大 ord 已隐含覆盖），但要正确处置 `old_ord`：

| 动作 | insert | update | 说明 |
|---|---|---|---|
| `live[old_ord]` | — | **clear（软删）** | 旧版本退出检索 |
| `ext2ord[ext_id]` | 设为 ord | 改指 ord | 始终指向最新 |
| HNSW | 新增节点 | `markDelete(old_ord)`（在 live callback 侧过滤） | 旧节点留作路由中转 |
| 旧 posting | — | 不动（靠 live 过滤） | merge 时 purge |
| `live_doc_count`(N) | **+1** | **+0** | 一个 ext_id 始终算一篇活文档 |
| `sum_doc_len` | +new_len | **−old_len +new_len** | old_len = `doc_lens_[old_ord]` |
| `fstats` 死字节 | — | 旧 record 的 total_sz 转死字节 | 驱动 merge |
| `retired_since_compact` | — | +1 | S12-2 自动 compaction 节流输入 |

`N+0` 这点最容易写错：更新同一文档**不该**让文档总数虚增——否则 IDF/avgdl 全偏。

### 4.5 `remove(ext_id)` 写路径

```
1. old_ord = docmap->ext2ord[ext_id]; 不存在 → 返回 false
2. append 一条 kTombstone record (key=ext_id, ord=next_ord++) = 提交点
3. docmap_->remove(ext_id, tomb_ord):
     live.clear(old_ord); ext2ord.erase(ext_id);
     live_docs_-1; sum_doc_len -= doc_lens_[old_ord];
     retired_since_compact_++; fstats 旧 record 转死字节;
     removals_ 入账 (S14-4 delta 窗口)
4. 索引侧：墓碑不广播 on_delete（HNSW 软删经 live filter; 倒排统计扣减
   仅在 reducer remove_doc 处走一次,V2 实际不删 posting 行,靠 live 过滤）
```

### 4.6 崩溃点分析

| 崩溃位置 | 后果 | 恢复 |
|---|---|---|
| `alloc_ord` 后、写 data 前 | 内存态丢失，磁盘无此 record | `next_ord` 恢复时 = 磁盘最大 ord + 1，幽灵 ord 自动消失，无空洞 |
| data 已写、keydir 未 apply | 日志有、keydir 没跟上 | fold `ord > W` 重建（§6） |
| data 已写、index task 还没 reduer apply | 日志有、索引没跟上 | fold 重放该 ord（HNSW `max_inserted_ord_` / InvertedIndex `max_indexed_ord_` 水位幂等丢弃重叠区） |
| data 已写 OS buffer 未 fsync | record 与索引更新一起丢 | 二者皆失 → 一致；该写入「未持久」符合 sync 语义 |

## 5. 持久化（per-component ckpt + manifest + payload）

恢复分两块、**成本天差地别**：侧表（便宜，全量重建） vs 派生索引 BM25/HNSW
（昂贵，靠 ckpt + 回放避免从头重算）。当前实现是 **per-component ckpt +
manifest commit + payload 外置**——详细协议见
[`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)。

### 5.1 文件总览（索引模式目录）

| 文件 | 用途 | 命名常量 |
|---|---|---|
| `<tstamp>.bitcask.data` / `.hint` | 唯一 WAL（KV + DocValue 索引 payload） | — |
| `bitcask.meta` | 目录配置 v3（magic + version + mode + 向量配置 + CRC） | — |
| `field.schema` | 字段名 ↔ id 注册表（多字段路径） | — |
| `kv.keydir.ckpt` | keydir 快照（SearchCheckpoint 容器，单段） | `kKeydirSnapName` |
| `docmap.ckpt` / `.prev` / `.d<seq>` | docmap 组件 base + delta 链 | `kDocmapCkptName` |
| `bm25.ckpt` / `.prev` / `.d<seq>` | bm25 组件 base + delta 链 | `kBm25CkptName` |
| `vec.ckpt` / `.prev` / `.d<seq>` | hnsw 组件 base + delta 链 | `kVecCkptName` |
| `index.manifest` | 三组件 ckpt 统一 commit point（80 字节 + CRC + magic 头尾） | `kManifestName` |
| `search.vec` | hnsw 外部 payload（f32 mmap，BCVP 格式） | — |
| `search.qc8` | hnsw int8 量化码字（BCQ8 v1，可选） | — |
| `search.ckpt` / `.prev` / `.d<seq>` | **[legacy]** pre-S17 单文件 ckpt，S17-5 后仅作迁移源 | `kSearchCkptName` |

后缀契约：`.ckpt` = 可重建的检查点；`.prev` = 上一代回退目标；`.d<seq>` =
delta 链文件。所有派生文件可删——删后 open 走全量 fold 重建。

### 5.2 SearchCheckpoint 容器（统一 ckpt / delta 文件格式）

`include/bitcask/search_checkpoint.hpp::SearchCheckpoint` 是所有
`*.ckpt` / `*.d<seq>` 文件的统一容器，自描述、分段、**每段独立 CRC**，
页脚目录定位：

```
== 头部 (16 B) ==
  [0..3]    magic "BCSC"
  [4..7]    version u32   (kCkptVersion=1 或 kCkptVersion2=2)
  [8..15]   watermark u64 (本快照覆盖到的 next_ord 上界)

== 段载荷区 ==
  各段 payload 顺序拼接(无内联段头;位置/校验由页脚目录给出)

== 页脚 ==
  directory(dirLen 字节):
    sectionCount u32
    每段: type u16 | flags u16 | offset u64 | len u64 | crc32 u32
        (crc 覆盖该段 payload)
  footerCrc u32   CRC 覆盖 directory 字节
  dirLen    u32   directory 字节长度
  trailer   "BCSC" (4 ASCII)
```

- `write` 走 `tmp + fdatasync + rename` 原子写。
- `read` 整文件读入后从尾倒走 footer 校验，逐段 CRC 写入 `LoadedSection::crc_ok`。
- `read_selected` 支持按段类型过滤读取——段级 dirty-bit 前移用（干净段
  零拷贝搬入新 ckpt，脏段重序列化时不重读）。

**段类型一览**（`include/bitcask/search_checkpoint.hpp::CkptSectionType`）：

| 类型 | 数值 | 用途 |
|---|---|---|
| `kDocmap` | 1 | docmap base 段（v2 gap+vbyte 行编码） |
| `kBm25Default` | 2 | bm25 默认域 `InvertedIndex` |
| `kBm25Fields` | 3 | bm25 多字段 |
| `kHnsw` | 4 | hnsw 段（V7 header，f32 payload 外置） |
| `kMeta` | 5 | 可选加速缓存 |
| `kTerms` | 6 | 可选加速缓存（terms-cache，替代旧 bm25 WAL） |
| `kBm25DefaultDelta` | 7 | bm25 默认域 delta |
| `kBm25FieldsDelta` | 8 | bm25 多字段 delta |
| `kDeltaInfo` | 9 | 链校验三元组（`base_gen u64` / `prev_wm u64` / `seq u32`） |
| `kDocmapDelta` | 10 | docmap delta（v1 定宽；保留兼容读） |
| `kHnswDelta` | 11 | hnsw 插入日志 |
| `kKeydirDelta` | 12 | keydir 元数据（`"BKMD"`：标量/fstats/字节水位） |
| `kDocmapDeltaV2` | 13 | docmap delta v2（gap+vbyte 行编码） |

### 5.3 index.manifest commit 协议

`include/bitcask/index_manifest.hpp` 定义 per-component 协议的**唯一 commit
point**：

```
[magic "BCMF"(4)] [version u32=1] [component_count u32=3]
per component [0=docmap, 1=bm25, 2=vec]:
  base_watermark u64 | chain_seq u32 | chain_watermark u64
[footer_crc32 u32] [trailer "BCMF"(4)]
```

总尺寸 80 字节（`kManifestSize = 12 + 3 × 20 + 8`），全小端。

- `write_manifest` 走 `tmp + fdatasync + rename + 目录 fsync`；损坏退全量
  fold——80 字节 + CRC + 原子 rename 足够可靠，无 `*.manifest.prev`。
- `Manifest::min_chain_watermark()` 返回所有组件 `chain_watermark` 的最小值
  ——fold 起点候选。
- **崩溃窗口处理**：组件先于 manifest 落盘（`fdatasync` 屏障保证）——
  - 已写组件 / 未写 manifest → manifest 仍指旧代，链走读退回
    `chain_seq=0` 起点、缺文件即停。
  - 已写 manifest / 未写 keydir 快照 → keydir 快照在下一次 close /
    merge 末尾兜底（best-effort）。

### 5.4 paired save 语义

`Cask::save_checkpoint_paired`（`src/cask/cask.cpp`）是所有索引模式 ckpt
保存的**统一入口**（手动 `checkpoint()` / 自动 `maybe_submit_auto_checkpoint()`
/ 收尾 / merge 收尾 / ①post-recovery 均经此）。写序不变量
`keydir_covered ≤ search_covered` 集中在一处维护。

**决策路径**：

1. 脏掩码组装：`docmap_->dirty()` / `text_->dirty()` / `vec_plugin_->dirty()`
2. 全局判据 `global_base = !any_dirty || ckpt_rebase_needed_`
   （close / compact / rebuild / legacy / merge 走 base；脏但无 rebase 走 delta）
3. 链上限：`docmap_cap = (max_delta_chain > 0 && docmap_chain_.chain_seq
   >= max_delta_chain)`——docmap 走 base，插件自查各自上限
4. 走 base 还是 delta 由每组件自决：docmap 走 `save_docmap_base` /
   `save_docmap_delta`；bm25 / vec 经 `plugin::flush` 自决

**docmap 侧**（`include/bitcask/docmap_ckpt.hpp`）：

- `save_docmap_base`：`rename(base, base.prev)` + 写新 base +
  `remove_chain_files` 清链文件 + `Index::begin_delta_window(watermark)` +
  `clear_removals` + `clear_dirty` 收尾。
- `save_docmap_delta`：写 `<base>.d<seq>`，含 `kDeltaInfo` + `kDocmapDeltaV2`
  段（窗口 live 行 + 删除日志按 ord 升序交错）+ 可选 `kKeydirDelta` 段
  （keydir 半边元数据内联进 delta 文件，delta 路径不写独立 keydir 快照）。

**keydir 快照成对**：`global_base` 走时（docmap base 已落成）→ 调
`write_keydir_snapshot(*wms)` 写 `kv.keydir.ckpt`。Delta 路径不写
keydir 快照——keydir 元数据已内联进 docmap delta 的 `kKeydirDelta` 段。

**链走读**：`bitcask::search::walk_chain`（`include/bitcask/ckpt_chain.hpp`）
统一管理 `.d1, .d2, ...` 链——每个 `.d<seq>` 调 `SearchCheckpoint::read`；
校验 `kDeltaInfo` 段三元组 `(base_gen / prev_wm / seq)` 必须与基准世代 /
当前 coverage / 当前 seq 一致；通过则调 caller 的 `apply(LoadedCheckpoint)`。

### 5.5 写入触发点

- `Cask::close()`：`write_keydir_snapshot()` + paired save
- `Cask::merge()`：merge 末尾 `compact_chunks` + `force_ckpt_rebase` +
  paired save（merge 恒 rebase → 走 base + 全量快照）+ `write_keydir_snapshot`
  兜底
- `Cask::checkpoint()`：手动 checkpoint API，paired save
- `Cask::maybe_submit_auto_checkpoint()`：周期自动触发（`auto_checkpoint_min_docs`
  阈值，reducer 线程 RunFn，fire-and-forget），保持 pending + in-flight 互斥
- `Cask::recover` 末尾 ①：post-recovery paired save，把刚 fold 出的成果
  落盘（避免下次崩溃再白付一次 fold）

### 5.6 校验与回退（兜底）

- 段或清单 CRC 校验失败 / `index.manifest` 缺失损坏 → **回退到全量 fold**：
  读所有 live `kDoc` 的 value，切词 + 逐个插 HNSW。慢但永远正确。这是
  安全网。
- 即正确性只依赖日志 + hint；ckpt 纯属加速，丢了不影响数据。

### 5.7 恢复成本与节奏旋钮

| 路径 | 成本 | 何时 |
|---|---|---|
| 侧表重建（hint fold） | O(记录数)，顺序读 hint，快 | 总是 |
| ckpt 加载 + fold 尾巴 | 加载 O(索引大小) + fold「ckpt 水位以来」× 重建成本 | 正常 |
| 全量 fold（5.6） | O(N × HNSW 插入)，1M 量级可能数分钟 | 仅 ckpt 损坏时 |

⇒ **paired save（merge / 自动 ckpt）节奏是恢复时长的旋钮**：越勤，
fold 尾巴越短、恢复越快，但 save 开销越高。

```
<dir>/
├── <tstamp>.bitcask.data           # typed record: kDoc (text+vector+meta+fields) / kTombstone
├── <tstamp>.bitcask.hint           # 元数据快速重建
├── bitcask.meta                    # mode + 向量配置 + CRC
├── field.schema                    # 字段名注册表
├── kv.keydir.ckpt                  # keydir 快照(单段,SearchCheckpoint 容器)
├── docmap.ckpt / .prev / .d<seq>   # docmap 组件 base + delta 链
├── bm25.ckpt / .prev / .d<seq>     # bm25 组件 base + delta 链
├── vec.ckpt / .prev / .d<seq>      # hnsw 组件 base + delta 链
├── index.manifest                  # 三组件 commit point(80B + CRC,原子 rename)
├── search.vec                      # HNSW f32 payload(mmap,BCVP,可追加)
├── search.qc8                      # HNSW int8 量化码字(BCQ8 v1,可追加)
└── search.ckpt / .prev / .d<seq>   # [legacy] pre-S17 单文件 ckpt(S17-5 后仅迁移源)
```

## 6. 一致性与恢复

### 6.1 单 watermark 自门模型

`Cask::load_recovery_snapshots`（`src/cask/cask_recovery.cpp`）流程：

1. **keydir 快照先载**：`keydir_->load_snapshot(dirname_/kKeydirSnapName)`。
   返回 `RecoverySnapshots::snap_wms`（per-file 字节水位）与
   `snap_loaded = true`。BCKS v2 校验失败 → `snap_wms = {}`。
2. **legacy 一次性迁移**：`!has_manifest && has_old_ckpt` 时
   `migrate_legacy_search_ckpt()`——读旧 `search.ckpt` 段集、改写为
   per-component 文件族 + `index.manifest` + 删旧文件（`.prev` / `.d<seq>`
   / `.vec` / `.qc8`）。
3. **manifest 读**：`bitcask::read_manifest(mpath)`。读不到 → 全量 fold。
4. **docmap 组件直载**：`index::load_docmap` 以 `entry.base_watermark` 校验
   base，失败退 `.prev`；成功后链重放（`DocmapReplayHook` 透传
   `kKeydirDelta` 段到宿主 keydir LWW put 与标量/fstats/字节水位更新）。
5. **插件 open**：经 `plugin::OpenContext` 注入器注入 `dir` / `host` /
   `committed_{base,chain}_watermark` / `committed_chain_seq`；所有路径
   （含 manifest 缺失 / 迁移失败的全量 fold 早退）都必须调用——零提示
   时插件自降级（`watermark 0` + rebase 置位 → 首次 flush 全量 base）。
6. **健康判据**：每组件 `plugin->watermark() == entry.chain_watermark` 即
   健康。所有组件健康 → 清 `ckpt_rebase_needed_`。
7. **快路径门**：`search_ok = all_components_ok && recovery.snap_loaded`。
   任一组件 `.prev` 回退 → 字节水位不可信 → 退全量 fold。

### 6.2 fold 起点自门

`Cask::load_keydir_from_disk` 走 `fold_start(fid) = snap_loaded ? wm_of(fid) : 0`。

对每个 sealed data 文件 `fold_one(e)`：

- 有 hint 且无 search 模式且无 snap_loaded → 走 hint fold（单遍校验 + fold，
  trailer CRC 不过时回调零次零污染）
- 否则走 data fold：`DataFile::fold(..., start_offset = fold_start, ...)`
- search 模式：每条 record 经 `decode_doc_value` → 攒批到 `recover_batch`
  （`kRecoverBatch = 1024`）→ 满批调 `flush_recover`：批内
  `tbb::parallel_for` 跑 `plugin::prepare`（分析并行），然后 fold 序
  串行 apply `docmap_->put_doc` + `plugins_[pi]->on_put`——与活写路径
  `reduce_index_entry` 同构。
- 墓碑前必 flush 攒批（保「文档↔墓碑」相对序）。
- search 模式墓碑重放：仅宿主 `docmap_->remove`（**不广播 `on_delete`**——
  恢复期不扣减倒排统计，统计基线随 ckpt 恢复）。

调度：search 模式或单文件 → 串行 fold；纯 KV 库多文件 → 按硬件并发数
并行 fold（`JoiningPool` RAII 防 `terminate`）。

### 6.3 崩溃恢复时序图

```
open(dirname)
   │
   ├─ read meta (bitcask.meta v3)         ←── MetaConfig
   │
   ├─ keydir_->load_snapshot
   │     └─ BCKS v2 校验失败 → snap_wms = {} (snap_loaded = false)
   │
   ├─ has_manifest ?
   │     ├─ yes: read_manifest
   │     │       └─ CRC 失败 → 全量 fold
   │     └─ no + has_old_ckpt: migrate_legacy_search_ckpt
   │             └─ 失败 → open_plugins(空 manifest) + 全量 fold
   │
   ├─ per-component load:
   │     ├─ docmap::load_docmap ──> DocmapReplayHook → keydir 半边
   │     └─ plugin->open (per-component chain state 注入)
   │
   ├─ watermark 对齐：
   │     all_components_ok && snap_loaded → fold_start = keydir_wm(fid)
   │     else                              → fold_start = 0
   │
   ├─ fold_one(e) × N                     ←── fold 主体
   │     ├─ data fold (search 模式 攒批并行 prepare + 串行 apply)
   │     └─ hint fold (纯 KV 无 search / 无 snapshot 时)
   │
   └─ ① post-recovery paired save        ←── 把刚 fold 出的成果落盘
         (search mode + docs ≥ 1000 时立即回存 ckpt)
```

### 6.4 关键不变量

1. **paired save 写序**：`global_base` 路径下 docmap base 落成后写
   `kv.keydir.ckpt`；delta 路径下 keydir 元数据内联进 `kKeydirDelta` 段。
   两路径均维持 `keydir_covered ≤ search_covered`。
2. **commit point 单向性**：`index.manifest` 是三组件 ckpt 唯一提交点；
   组件数据先于 manifest 落盘（`fdatasync` 屏障）——断电后 manifest 已
   提交但组件页丢失 → CRC 坏 → 整组件退全量 fold，不会出现「manifest
   OK 但组件坏」的混合态。
3. **回退完备性**：`*.ckpt` 均带 `.prev`；链走读 `chain_seq == 0` 表示
   零已提交 delta，孤儿 `.d1`（crash 在「先写 delta 后提交 manifest」窗口）
   被有界模式忽略，无界模式才会扫盘重放。
4. **fold 起点自门**：`fold_start = snap_loaded ? keydir_wm(fid) : 0`。
   `search_covered ≤ keydir_covered` 不变量保证 `[keydir_covered, end)`
   覆盖搜索所需；各索引按自身 ord 水位自门丢弃重叠区，幂等安全。
5. **链校验三元组**：每个 `.d<seq>` 的 `kDeltaInfo` 段声明
   `(base_gen, prev_wm, seq)`，必须与基准世代 / 当前 coverage / 当前
   seq 一致——保证链是 `1..N` 连续、单调覆盖。
6. **崩溃任意点安全**：ckpt 偏旧或部分写（tmp 未 rename）→ 对应块退回旧态
   / 空态，watermark 下移，fold 补齐。无「门失败 → 全量 fold」悬崖。
7. **墓碑前必 flush**：search 模式恢复 fold 时遇到 `kTombstone` → 先
   flush 攒批（保「文档↔墓碑」相对序），再 `docmap_->remove`。墓碑不
   广播 `on_delete`——历史语义保留。

## 7. 查询路径（三接口 + Searcher 门面）

### 7.1 三接口语义

```cpp
// BM25 词袋
search_text  (query, k, filter = nullptr, offset = 0)
  → TextPlugin::search_text 内核：ngram 切词 + DAAT + BMW top-k + filter 后过滤

// HNSW ANN
search_vector(query_vec, k, ef = 0, filter = nullptr)
  → VectorPlugin::search 内核：live callback + HNSW search_layer
    ef=0 → max(k, 64)

// RRF 混合
search_hybrid(text_query, vec_query, k, filter = nullptr)
  → HybridSearcher::search: text_/vec_ 各取 K'=max(k×4,64), RRF 融合
```

**批量版**（`search_text_batch` / `search_vector_batch` / `search_hybrid_batch`）：
N 条独立查询并发跑进程级共享 Search 池（TBB task_arena），保序返回。
单条查询内部仍串行（WAND 顺序依赖、HNSW 图遍历）；并发发生在查询之间。
一次 `drain_plugins()` 读屏障覆盖全批（read-your-writes）。

### 7.2 HybridSearcher — RRF 融合（`src/search/hybrid_searcher.cpp`）

```cpp
class HybridSearcher {
public:
    HybridSearcher(const text::TextPlugin& text, const vec::VectorPlugin& vec);
    std::expected<std::vector<SearchHit>, SearchError>
    search(std::string_view text_query, std::span<const float> vec_query,
           std::size_t k, const meta::MetaFilter* filter = nullptr) const;
private:
    const text::TextPlugin&  text_;
    const vec::VectorPlugin& vec_;
};
```

**算法（Reciprocal Rank Fusion, Cormack/Clarke/Buettcher 2009）**：

```
K' = max(k × 4, 64)                            // 两路各超采样 K'

text_hits = (text_query 非空) text.search_text(text_query, K', filter)
vec_hits  = (vec_query  非空) vec.search(vec_query, K', ef=0, filter)

对每路 leg, i = 0..leg.size()-1:
  acc[leg[i].ord].score += 1.0 / (60.0 + (i + 1))    // c = 60,常数

确定性平局序: RRF 分相等 → ord 小者在前
```

- **c = 60** 是论文推荐常数（无权重旋钮、不归一化分数）。
- **空路退化**：text 空 → 纯向量路径；vec 空 → 纯文本路径；两路皆空
  → `SearchError::kEmptyHybridQuery`。
- **filter 同时作用于两路**：text 后过滤（overfetch K' 后过滤），vec 折
  HNSW live callback（无需 overfetch）；仅双路都通过的文档进 RRF 融合。
- **维度不符** → `SearchError::kVectorDimMismatch`。

### 7.3 Searcher 门面（`include/bitcask/searcher.hpp`）

新代码推荐走类型化门面：`text::Searcher` / `vec::Searcher` /
`search::CaskHybridSearcher`。它们持 `Cask&` + 插件引用，查询前自动
`cask.drain_plugins()` 读屏障（read-your-writes：submitted ⇒ applied），
再直调插件内核；错误经 `Cask::search_error_fault` 统一翻译。

`Cask::search_*` 系列保留为薄委托（源兼容），新代码走门面：

```cpp
auto* tp = cask->text_plugin();          // 未启用搜索时为 nullptr
bitcask::text::Searcher ts(*cask, *tp);
auto hits = ts.search_text("hello", 10);

auto* vp = cask->vector_plugin();
bitcask::vec::Searcher vs(*cask, *vp);
auto vhits = vs.search(query_vec, 5);

bitcask::search::CaskHybridSearcher hs(*cask, *cask->hybrid_searcher());
auto hyb = hs.search("hello", query_vec, 10);
```

### 7.4 metadata filter

`meta::MetaFilter`（`include/bitcask/meta_filter.hpp`）支持结构化 KV
过滤（如 `meta.lang = "zh" AND meta.year > 2020`）。索引模式 + filter 非空时：

- **text 路径**：overfetch K' 后逐 ord `docmap_->eval_meta(ord, filter)`
  过滤，截断到 k——S13-P8 实现，`eval_meta` 锁内求值（shared_lock）省
  `meta_blob` 锁内拷贝。
- **vec 路径**：折进 HNSW `live` callback，与 `is_live` 组合成
  `live AND eval_meta`——无需 overfetch。
- **hybrid 路径**：两路各按上述处理，仅双路都通过的文档进 RRF 融合。

## 8. 删除与回收（merge + 自动 compaction）

### 8.1 删除 = 写墓碑 + 索引侧软删

- data file 写一条 `kTombstone` record（log 一等公民）。
- DocMap 侧：清 `live[old_ord]`、erase `ext2ord[ext_id]`、`live_docs_--`、
  `sum_doc_len -= doc_lens_[old_ord]`、`retired_since_compact_++`、`removals_`
  入账。
- HNSW 侧：本模块不感知删除；上层经 live 过滤回调在结果侧滤死，死节点
  留作图内路标，merge 重建时物理清除。
- InvertedIndex 侧：V2 实际不删 posting 行（靠 live 过滤），但
  `remove_doc` 减少 `live_doc_count_ / sum_doc_len_` 以保持统计准确。

### 8.2 单一 merge（`Cask::merge`）

单次 pass 完成：

1. 重写存活 data record（沿用现有 merge）
2. purge 倒排里已删 ord，重算 `df / N / avgdl`
3. HNSW `clone_live`（S13-P8）——结构化拷贝活子图：保留原图层数与邻接
   结构，只做 id 重映射 + 死邻过滤（一跳路径收缩）；`inmem_int8` 模式
   直接拷 qcodes/scale/sum，消掉反量化→再量化往返
4. `docmap_->compact_chunks()` 释放 live_count == 0 的 chunk
5. `force_ckpt_rebase()` 联动两插件自持标志 → paired save 走 base + 全量快照
6. `write_keydir_snapshot` 兜底

`merge::PolicyOptions` 的 `decide()` 触发信号：**死字节率**（现有）∪
**删除率**（倒排 / HNSW 膨胀）∪ **TTL 过期**（`expiry_secs`）。

因为单域，无跨域 pin、无回收顺序协调问题。

### 8.3 S12-2 自动 compaction

`SearchLayerConfig::auto_compact_dead_ratio` 阈值开启后，reducer 线程
在每次 put_doc 后累计 `retired_since_compact_`；达节流阈值
（`max(1024, live/2)`）时对死占比 ≥ 本值的 posting list 触发一次
`InvertedIndex::compact()`——与 `add_doc` 同线程串行，无并发窗口。

效果：posting list 内存随 churn 有界，不再依赖 merge 回收。代价：触发时
短暂扫描压实，延迟后续文档的**索引可见性**（非 durability——数据已落
data file）。

### 8.4 后台 merge 与 live writer 并发

`bitcask.write.lock`（live writer 持有）与 `bitcask.merge.lock`（merger
持有）独立，周期性 merge_worker 与主 writer 互不阻塞：

- merge 写自有输出文件（`keydir->increment_file_id()`），active writer
  文件通过 `active_file_id_` 探测并从候选中排除。
- keydir 重定位是条件 CAS（`newest_put = false`），收尾对 stuck 文件
  跳过 unlink 兜底。
- get 对 merge unlink 窗口有一次重查重试。
- 索引模式的 merge 收尾 HNSW 重建经 `run_serialized`（RunFn 通道）投递
  reducer 静止点，单写者约束保持。

## 9. 索引模式 vs 纯 KV 模式

| 维度 | 纯 KV 模式 | 索引模式 |
|---|---|---|
| `enable_search` | false | true（+ `search_config`） |
| `vector_dim` | 0 / 不配置 | >0 启用向量（可选） |
| 索引组件 | 无 | TextPlugin + VectorPlugin + HybridSearcher |
| docmap | 不需要 | 必需（宿主服务，ord 主键） |
| IndexPool | 不需要 | 由 KeyDirRegistry 共享 |
| 持久化 ckpt | 无 | per-component + manifest + payload |
| 恢复 | hint fold（O(记录数)，快） | ckpt 加载 + fold 尾巴（O(索引大小) + 重放） |
| API | `put/get/remove/merge/iter/scan` | 上 + `put_doc/search_text/vector/hybrid` |
| 线程开销 | 1 把 `write_mu_` + 读无锁 | 上 + reducer 后台流水线 |
| `status().hnsw_nodes` | 0 | HNSW 图节点数 |
| `has_search()` | false | true |

**两种模式互斥选项**：

- 纯 KV 库（`enable_search = false`）：恢复走 hint 快路径，不创建任何
  索引插件；`status().hnsw_nodes = 0`。
- 索引模式库（`enable_search = true`）：必须配 `search_config`；
  `vector_dim > 0` 才启用向量（HNSW）；否则走 BM25-only。

## 10. 升级路径：`Cask::upgrade()`

```cpp
[[nodiscard]] static std::expected<std::unique_ptr<Cask>, CaskFault>
Cask::upgrade(std::string_view dirname,
              const search::SearchLayerConfig& search_config);
```

**离线**将 KV 模式目录升级为索引模式：

- **前提**：目录存在且当前为 KV 模式；**目录必须离线**（无活跃 writer/merger）。
- **流程**：
  1. 读 `bitcask.meta` 验证 mode == `kKv`（不是 `kIndex`）
  2. 写新 `bitcask.meta`（mode = `kIndex`）
  3. 创建搜索插件（Text/Vector）+ HybridSearcher
  4. 创建 KeyDir + `load_keydir_from_disk`（全量 fold 重建索引）
  5. `mark_ready` → 返回只读索引模式 Cask
- **线程安全**：产生独立的 Cask 对象；不获取 `write_mu_` / `merge.lock`。
- **后续使用**：调用方可 `close()` 升级返回的 Cask 后，再用
  `Cask::open(dirname, {enable_search=true, read_write=true})` 正常打开
  读写模式使用。
- **失败模式**：meta 不存在 / mode 已是索引 / meta 读失败 / fold 失败 →
  对应 `CaskError`（`kIo` / `kModeMismatch` / ...）。

## 11. 公共 API 摘要

```cpp
// CaskOptions 关键字段
struct CaskOptions {
    bool          read_write           = false;
    std::uint64_t max_file_size        = 2 GiB;
    std::size_t   max_read_handles     = 0;       // 自动: RLIMIT_NOFILE/2
    bool          o_sync               = false;
    std::uint32_t sync_every_n         = 0;       // 组提交阈值
    std::uint32_t auto_checkpoint_min_docs = 0;   // 自动 ckpt 阈值
    std::uint32_t expiry_secs          = 0;
    bool          merge_only           = false;
    std::uint8_t  tombstone_version    = 0;

    // 搜索模式
    bool          enable_search        = false;
    std::optional<search::SearchLayerConfig> search_config;
    std::uint16_t vector_dim           = 0;
    bool          vector_quantized     = false;   // P3a: 落盘 int8
    bool          vector_inmem_int8    = false;   // P5b: HNSW int8-only 内存
    meta::VectorMetric vector_metric   = meta::VectorMetric::kCosineNormalized;

    std::shared_ptr<const text::SynonymMap> synonym_map;  // open-time 不可变
    std::function<void(LogLevel, std::string_view)> log_fn;
};

// 写入
put(key, value, tstamp = 0, expiry_at = 0)
put_doc(key, {text, meta, vector, fields, expiry_at}, tstamp = 0)
put_batch(items, tstamp = 0)
remove(key, tstamp = 0)
sync()
checkpoint()                        // 手动 paired save
merge(files = {}, now_sec = 0)      // 周期性 GC + 物理清死
backup(dst_dir)                     // 不停机备份

// 读取
get(key) → GetResultView / GetResult (zero-copy)
parallel_scan(n_threads, fn, key_prefix)
make_iter() → CaskIter

// 检索
search_text(query, k, filter = nullptr, offset = 0)
search_phrase(query, k, offset = 0)
search_near(query, slop, k)
bool_search(query, k, offset = 0)
bool_search_tree(query_tree, k)
search_fields(query, k)
search_fuzzy(query, k, max_edit_distance)
search_wildcard(pattern, k)
search_vector(query_vec, k, ef = 0, filter = nullptr)
search_hybrid(text_query, vec_query, k, filter = nullptr)
search_text_highlight(query, k, opts = {})

// 批量检索（共享 Search 池，inter-query 并发）
search_text_batch(queries, k, filter)
search_vector_batch(queries, k, ef, filter)
search_hybrid_batch(items, k, filter)

// 元
status()
is_empty_estimate()
is_frozen()
needs_merge(now_sec = 0)
has_search()
drain_plugins()                     // 读屏障（read-your-writes）
text_plugin() / vector_plugin() / hybrid_searcher()
docmap()

// 离线升级
upgrade(dirname, search_config)     // KV → 索引模式
```

## 12. 已知约束 / 后续

- **百万级目标**：HNSW 单图常驻内存 ≤1M 召回最好；超百万走多段 + buffer
  （接口/段格式预留，V7 BCVS 双文件 + payload 代号 + `.prev` 回退已就位）。
- **merge 整体重建**对 ≤1M 可接受，但仍是 stop-the-world 风险点——
  HNSW 重建应在副本上建、原子换图，避免阻塞在线查询；当前 S13-P8
  `clone_live` 已实现结构化拷贝活子图（O(节点+边) memcpy 级拷贝），
  但仍走 reducer 静止点。
- **删除率高时召回/排序在两次 merge 间漂移**（死点累积）——靠删除率触发
  merge + S12-2 自动 compaction 缓解。
- **metadata filter 当前是查询后过滤**——高效实现（filter-while-search）
  留到属性索引成熟后；`eval_meta` 锁内求值已规避 `meta_blob` 拷贝代价。
- **量化（PQ/scalar affine）、IVF、外存**：百万级用不上，接口预留（P3a
  int8 对称量化 + P5b int8-only 内存已落地）；超规模再做。
- **search.ckpt legacy 格式**：`legacy_ckpt.{hpp,cpp}` 是 pre-S17 单文件
  ckpt 的 load-only 读取器，唯一生产用途是
  `Cask::migrate_legacy_search_ckpt`。旧格式支持整体退役时本模块删除。
- **kv.keydir.ckpt 是单独文件**：keydir 快照仍走独立的 `SearchCheckpoint`
  容器（单段 `kKeydir`），与三组件 ckpt 平行；paired save 协调两者
  写序维持 `keydir_covered ≤ search_covered`。
- **`search.vec` / `search.qc8` 是 HNSW payload 外置文件**：与
  `vec.ckpt` 平行（kv.keydir.ckpt 类比），前缀不变契约 + 身份收养 +
  追加机制；备份时一并 hardlink。