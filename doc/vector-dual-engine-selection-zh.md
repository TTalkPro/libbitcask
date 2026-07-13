# 向量双引擎选型：DiskANN vs IVF-RaBitQ（磁盘档第二引擎预研）

> 状态：**预研 / 选型结论已定，未立项（2026-07-13）**。
> 背景：HNSW 在 2-4GB 内存机器上无法有效运行；确定走**双引擎**路线——
> HNSW 继续做 ≤1M 常驻档（优化线见 [`s29-11-hnsw-deep-opt-design-zh.md`](s29-11-hnsw-deep-opt-design-zh.md)），
> 另加一个磁盘档引擎覆盖 10M-100M 场景，按配置选择。本文记录磁盘档引擎的
> 候选对比（DiskANN / IVF-RaBitQ / SPANN）、PQ 与 RaBitQ 量化知识、以及
> **v1 选 IVF-RaBitQ、DiskANN 留作 v2 条件触发**的结论与依据。
> 前置阅读：[`vector-db-ann-landscape-zh.md`](vector-db-ann-landscape-zh.md)（算法全景）、
> [`vector-ondisk-quant-design-zh.md`](vector-ondisk-quant-design-zh.md)（int8 量化 + PQ 对比 §9 + DiskANN 理论 §10-11）、
> [`hnsw-memory-footprint-zh.md`](hnsw-memory-footprint-zh.md)（HNSW 内存精确推导）。

---

## 1. 背景：为什么需要第二引擎

### 1.1 HNSW 在 2-4G 机器上的硬账（代码验证，2026-07-13）

每节点堆常驻（`hnsw.hpp` NodeChunk，M=16，详细推导见 memory-footprint 文档）：

| 组件 | 字节/节点 | 现状 |
|---|---|---|
| f32 向量 `vecs` | 4·D | 已可出堆：checkpoint 后走 `.vec` mmap（`vec_of` 路由） |
| int8 码字 `qcodes/qscales/qsums` | D + 8 | **纯堆驻留**。`.qc8` 只是启动格式优化——`load_qc_payload` 是 fread+memcpy 进堆（`hnsw.cpp`），非 mmap |
| 邻接块 + 元数据 | ~157.5 | 堆（bump-slab arena） |

三个对小内存机器致命的点：

1. **int8 码字是最后的堆大头**：int8-only 模式下 `D + 165.5` B/节点，高维时
   qcodes 占 ~95%，硬驻留不可回收；
2. **merge 重建峰值 ≈ 2×**：`clone_live` 在旧图存活期间于堆上完整构建新图；
   且默认模式下新图 chunk 以 `needs_vecs=!inmem_int8` 分配**全量 f32**——旧图
   已 mmap 化的 f32 在重建期被拉回堆，直到下次 checkpoint。2G 机器上稳态
   1G 的图在 merge 时即 OOM；
3. chunk 定容 65536 槽：高维时单 chunk 是百 MB 级台阶（D=2560 int8-only
   ≈ 170 MB/chunk）。

可行性矩阵（1M 向量，int8-only 已开）：

| dim | 稳态堆 | ×2 重建峰值 | 2G 机器 | 4G 机器 |
|---|---|---|---|---|
| 384 | 0.51 GB | 1.0 GB | ✅ | ✅ |
| 768 | 0.87 GB | 1.7 GB | ⚠️ 重建时危险 | ✅ |
| 1536 | 1.59 GB | 3.2 GB | ❌ | ⚠️ |
| 2560 (qwen3) | 2.54 GB | 5.1 GB | ❌ | ❌ |

### 1.2 HNSW 优化线（第一引擎，与本文正交）

- **P0 纯配置**：`vector_inmem_int8` + `vector_quantized` + MRL 截断
  `vector_dim`（qwen3 是 Matryoshka 模型，2560→1024 召回损失很小）；
- **P1 工程**：`.qc8` mmap 化（`qcodes_of` 按 `id < checkpoint_count_` 路由，
  与 `vec_of` 同型；BCQ8 定长 stride 天然可按 id 寻址）→ 堆降到 ~165 B/节点，
  内存不足退化成慢而非 OOM；`clone_live` 改 `needs_vecs=false` + f32 从旧
  mmap 流式写新 `.vec`，消 2× 峰值；
- **P2 按需**：RAM 驻 RaBitQ 导航码（1 bit/维，见 §4.3）替代 int8 导航，
  2560d → 320 B/节点。

覆盖 ≤1M；10M-100M 交给第二引擎。

---

## 2. 行业现状：谁在用什么（2026）

**DiskANN 是少数派，HNSW 才是行业默认。**

| 阵营 | 数据库 | 说明 |
|---|---|---|
| 纯 HNSW 默认（多数派） | Qdrant、Weaviate、Elasticsearch/Lucene、Vespa、MongoDB Atlas、Chroma（单机）、pgvector | 内存型 HNSW + 量化（int8/BBQ）压内存 |
| HNSW 默认 + DiskANN 可选 | Milvus、Redis 8.2+（SVS-VAMANA） | DiskANN 只是大规模档位的**可选索引类型** |
| DiskANN 系为主 | Microsoft 自家（Bing、Azure AI Search、Cosmos DB）、**pgvectorscale**（Timescale 的 PG 扩展，StreamingDiskANN） | DiskANN 出自 MSR，微软生态是主要生产用户 |
| 非 DiskANN 的磁盘原生 | LanceDB（IVF-PQ / IVF-RQ=RaBitQ）、Chroma 云端 / turbopuffer（SPANN 系） | 磁盘路线有三个并行流派 |

> 勘误：landscape 文档把 DiskANN 记在 pgvector 0.8+ 名下——严格说 pgvector
> 本体只有 HNSW/IVFFlat，DiskANN 实现在 **pgvectorscale** 独立扩展里。

三个模式：

1. **"内存够就 HNSW"是全行业共识**——DiskANN 采用几乎都发生在"向量数据大到
   RAM 成本不可接受"（亿级+）之后；没有一家在千万级以下默认 DiskANN；
2. **采用 DiskANN 的都是双引擎形态**（Milvus / Redis：HNSW 默认 + 按
   collection 选装）——本库双引擎提案与业界已验证的产品形态一致；
3. **磁盘档内部有流派之争**：DiskANN（图+PQ+beam IO）、IVF-PQ/RaBitQ
   （LanceDB）、SPANN（质心倒排，对象存储友好）。

---

## 3. 三流派原理速查

### 3.1 DiskANN（Vamana 图 + PQ + beam search）

架构与 Vamana/RobustPrune 详解见
[`vector-ondisk-quant-design-zh.md`](vector-ondisk-quant-design-zh.md) §10-11。要点：

- **图**：Vamana 单层 α-图（α≈1.2 保留长程边，替代 HNSW 多层），度上限 R≈64；
- **盘上**：每节点对齐块 `[向量 | 邻接]` 共置——每跳恰好一次 SSD 读（灵魂）；
- **RAM**：每向量一个 PQ 码（32-64B）做导航距离 + beam search（W=4-8 路并发
  随机读）；最终排序用盘上全精度重排；
- **增量**（FreshDiskANN）：盘上主图不可变 + RAM delta 索引 + 后台
  StreamingMerge——与本库 building/sealed/merge 段模型同构。

### 3.2 IVF（倒排文件）+ 量化

```
建索引：k-means 聚 nlist 个簇（≈4√N），每簇一条 posting list
查  询：query 与质心比距离 → 只扫最近 nprobe 个簇（1-5% 数据）
```

本质是"用聚类把 O(N) 暴扫剪成 O(N×几%)"。**关键性质：posting 连续存储 →
磁盘读是少量大顺序读**（vs 图遍历的大量 4K 随机读）。弱点：簇边界召回
（nprobe 补偿）、质心漂移需重训。

### 3.3 RaBitQ（2024，NUS）——PQ 的零训练替代品

```
随机正交旋转 → 每维取符号位（1 bit/维）→ 存少量校正标量（范数等）
距离 = XOR/popcount + 校正公式 → 无偏估计量，带可证误差界
```

对比 PQ 三优势：**零训练**（旋转矩阵随机，无 codebook 漂移）、同压缩比误差界
更优、距离是 popcount（SIMD 极友好，内核 ~200 行，与 int8 内核基建同型三级
分发）。32× 压缩；Extended RaBitQ 可 2-8 bit/维换精度。采用：LanceDB
IVF-RQ、Milvus 2.6、Elasticsearch BBQ（衍生自 RaBitQ）。

### 3.4 SPANN（微软，NeurIPS 2021）——已排除

RAM 驻大量质心（~N 的 10-16%）+ SPTAG 图导航，盘上每质心一条短 posting；
边界向量多副本（closure assignment）补簇边界召回，磁盘膨胀 1.5-2×。
顺序 IO 可架对象存储（Chroma 云端 / turbopuffer 路线）。增量版 SPFresh
（SOSP 2023）。**排除理由：质心驻留高维下吃 RAM，与 2-4G 前提冲突。**

### 3.5 PQ 码宽（决策变量释义）

PQ 把 D 维切 M 段，每段在 256 词 codebook（k-means 训练）里编 1 字节 →
**码宽 = M 字节/向量**。"PQ32" = 32 字节。

| 码宽 | 每段维数(D=1024) | 压缩比 | 100M 向量 RAM | 误差 |
|---|---|---|---|---|
| PQ64 | 16 | 64× | 6.4 GB | 最小 |
| PQ32 | 32 | 128× | 3.2 GB | 小 |
| PQ24 | ~43 | ~171× | 2.4 GB | 中 |
| PQ16 | 64 | 256× | 1.6 GB | 大 |

码宽是 RAM↔召回旋钮：段越少，每段 256 质心要覆盖更高维子空间，误差急升。
DiskANN 里 PQ 距离只决定 beam 展开谁（导航），f32/int8 精排兜底——窄码不直接
毒害终排，但导航走错路 → 召回降或被迫加大 beam（更多 IO）。**OPQ**（训练前
学旋转矩阵把信息量摊匀各段）是窄码标配补偿。对比：RaBitQ 的旋钮是 bit/维
（1024d@1bit = 128B），比 PQ32 宽——**极限压缩（十亿级）仍需 PQ**；RaBitQ
甜区是中等压缩 + 零训练 + popcount 快。

---

## 4. 插件接缝可行性（代码验证）

**宿主层（`plugin_api.hpp`）已引擎无关**：

- `on_put`（reducer 单写者、ord 升序）+ `watermark()` 幂等重放义务——正是
  FreshDiskANN"不可变主图 + 重放补 delta"的恢复模型，契约现成；
- merge 协议（`on_merge_commit` → `run_serialized` 静止点）= StreamingMerge
  的调度骨架；
- 删除 = 结果侧 `DocTable::is_live` 过滤 + merge 物理清理，与墓碑模型对应。

**领域层（`VectorPlugin`）HNSW 硬编码的只有薄薄一层**（`hnsw_` 成员、
`graph()/set_graph`、kHnsw 段）；归一化、delta 插入日志、链状态、`.d` 文件族、
水位记账、`MetaFilter` 组合全部引擎无关可复用。

### M0：AnnEngine 策略接口（双路线共同前置）

在 VectorPlugin 内做策略对象（而非平行写第二个 plugin——那要复制全套记账）：

```cpp
// vec::AnnEngine — VectorPlugin 的图引擎策略（草案）
struct AnnEngine {
    virtual void insert(uint64_t ord, span<const float> v) = 0;   // 单写者
    virtual vector<Hit> search(span<const float> q, size_t k, size_t ef,
                               const function<bool(uint64_t)>* live) const = 0;
    virtual uint64_t max_inserted_ord() const = 0;                // 幂等门
    virtual shared_ptr<AnnEngine> rebuild_live(live_fn) = 0;      // merge 收尾
    virtual bool save_base(dir, wm) = 0;                          // 自管文件族
    virtual bool load_base(dir, wm) = 0;
};
```

`HnswIndex` 公开面逐一映射，抽取是纯重构零行为变化。接线：
`CaskOptions::vector_engine`（"hnsw"/"ivfrq"/"diskann"）→ `bitcask.meta`
持久化 + 重开 mode_mismatch 校验（与 `vector_dim` 同型检查）；manifest 组件
身份用引擎名（`name()` 本就按算法命名）。

---

## 5. v1 详设对比：IVF-RaBitQ vs DiskANN

约束：AnnEngine 之后、10M-100M、2-4G RAM、NVMe、metric=kDot（归一化）。

### 5.0 先修正直觉：v1 的 RAM 赢家是 IVF

教科书里 DiskANN "RAM 最省"的前提是 IVF 的码也放内存；但 v1 里 IVF 的码可以
**躺在 posting 里顺序扫**，RAM 只驻质心：

| RAM 驻留（1024d） | 10M | 100M |
|---|---|---|
| IVF-RaBitQ：int8 质心（nlist≈4√N） | ~12 MB | ~40 MB |
| DiskANN：PQ32 全量码 | 320 MB | **3.2 GB（4G 机器打满）** |
| 共用：delta 窗口（HnswIndex int8-only，20 万条上限） | ~250 MB | ~250 MB |

### 5.1 候选 A：IVF-RaBitQ v1

```
RAM   质心表（nlist × dim int8 + f32 norm；SIMD 暴扫 <1ms @ nlist≤64k）
      delta 窗口：复用 HnswIndex(inmem_int8)，有界

盘    ivf.meta   质心 f32 + 训练元信息 + payload_gen（复用 gen 配对协议）
      ivf.post   按簇连续 posting 段：
                 [簇头: count, crc]
                 [RaBitQ 码区: N_c × (dim/8 + 8B 校正)]   ← 粗筛顺序扫
                 [int8 码区:   N_c × (dim + 8)]           ← 精排就地取
```

- **关键决策：精排 int8 码内联在 posting**（磁盘 +13%），换精排零随机 IO
  （否则每查询 100-200 次 `read_at` 随机读，顺序 IO 优势尽失）。int8 精排
  召回 ~97%+（§6 harness 同型数据）；
- **查询**：质心暴扫 top-nprobe(16-64) → 顺序读 nprobe 段（100M/40k≈2.5k
  条/簇 ×~150B ≈ 400KB/段）→ RaBitQ popcount 粗筛 → 段内 int8 精排 →
  delta 窗口归并 → live 过滤。p99 ≈ 10-30ms；
- **写入/恢复/merge 全部映射现有机制**：`on_put`→delta 窗口（= 现有
  `delta_ords_/delta_data_` 语义）；flush→窗口按质心分配追加 `.d<seq>`
  posting 增量（= 现有组件链 base+.d+fold 原型）；base rebase→posting 段
  重写；质心重训仅在 merge 收尾且漂移超阈（posting 失衡度/簇内距涨幅），走
  `on_merge_commit → run_serialized`；崩溃恢复 = watermark + fold 重放填
  窗口，零新机制；
- **建库（决定性优势）**：采样 k-means（1M 样本 int8 ≈ 1GB）→ 流式扫 data
  file 逐条 assign → 按簇外部分桶 → 拼接。**峰值内存 = 样本+质心+单桶缓冲，
  2-4G 机器直接建 100M**（耗时小时级，受限磁盘带宽）；
- **工程量 ~2.5-3k LOC**：k-means ~400 · RaBitQ 编码+popcount 内核(AVX2
  三级分发) ~500 · posting 格式/建库 ~600 · 查询 ~500 · delta/链接线 ~400 ·
  meta/gen ~300。无并发图结构、无新并发协议（只读段扫描 + 原子换代）。

### 5.2 候选 B：DiskANN v1

```
RAM   PQ codebook（MB 级）+ PQ 码全量（N×M 字节）+ 入口/热点缓存
      delta 窗口：同 A

盘    diskann.graph  节点块 4K 对齐：[int8 向量 dim+8 | 邻接 R×4B]
                     （1024d → ~1.3KB/节点；v1 存 int8 不存 f32，与 A 同
                      精排精度，块小 3×）
      diskann.pq     PQ 码 + codebook（启动载入 RAM）
```

- **查询**：beam search（W=4-8）：PQ-ADC 选扩展候选 → 并发 W 路 4K 随机
  pread（int8 精算 + 邻接）→ 迭代。每查询 50-150 次随机读，p99 ≈ 2-8ms。
  需 IO 并发基建（v1 thread_pool 同步 pread，io_uring 留优化）；
- **建库（决定性劣势）**：Vamana 插入式建图要求对已插节点随机访问——2-4G
  机器只能重叠分片（每片 ≤2M 条 int8）片内建图 + 边合并二次剪枝（DiskANN
  merged build）。编排本身 ~1k LOC，建 100M 十小时级；实际大概率退化为
  "大机器离线建、小机器服务"；
- **增量**：delta 窗口同 A；但 fold 进主图 = **全量重建**（小时级）直到 v2
  StreamingMerge（另 1-2k LOC）；
- **工程量 ~5.5-7.5k LOC** + 三个高风险件：分片建图正确性（召回损失隐蔽）、
  beam IO 调度、PQ 窄码（100M@4G 被逼 PQ16/24+OPQ）召回悬崖。

### 5.3 正面对比表

| 维度 | IVF-RaBitQ v1 | DiskANN v1 | 赢家 |
|---|---|---|---|
| RAM（100M, 1024d） | ~300 MB | ~3.5 GB（PQ32），进 4G 需 PQ16/24+OPQ | **A 压倒性** |
| p99 延迟 | 10-30 ms | 2-8 ms | **B** |
| 召回@10（预估，harness 定案） | 90-95%（nprobe 平滑旋钮） | 95-98%（窄码打折） | **B** |
| 2-4G 机器建库 | ✅ 流式外部分桶直接建 | ❌ 分片编排或离线大机器 | **A 决定性** |
| 增量 fold 成本 | posting 追加（链模型现成） | 全量重建（小时级）直到 v2 | **A** |
| 磁盘类型 | 顺序读，SATA/云盘可跑 | 4K 随机读，NVMe 硬要求 | **A** |
| 磁盘占用（100M,1024d） | ~13 GB | ~130 GB（4K 对齐填充） | **A** |
| 工程量/风险 | ~3k LOC，无新并发协议 | ~6-7.5k LOC，三高风险件 | **A** |
| 基建贴合度 | 链/fold/merge/gen 逐一映射 | 恢复模型好，建库/merge 外来物 | **A** |
| 天花板（延迟敏感十亿级） | 有 | 高（微软十亿级验证） | **B** |

---

## 6. 结论与分期

**v1 选 IVF-RaBitQ**——不是折衷而是本约束下的正解：DiskANN 仅存的两项优势
（延迟、召回上限）恰是约束里最有弹余的（10-30ms 对 10M-100M 离线/RAG 场景
通常可接受），而其三项硬伤（PQ 码 RAM、建库、增量重建）全部撞在 2-4G 前提上。

```
M0    AnnEngine 抽取 + vector_engine 接线 + 召回 harness（S29-11 §2，
      两引擎共用，任何一条线的前置）
M1    HNSW 线 P0/P1（qc8 mmap + clone_live 峰值治理）——覆盖 ≤1M
M2-v1 IVF-RaBitQ 引擎（~3k LOC，一个中批次）；RaBitQ popcount 内核
      顺手供 HNSW 线 P2（导航码）
M2-v2 条件触发：出现「p99 <10ms 且 ≥95% 召回 @ 50M+」硬指标才上
      DiskANN——届时 harness、int8 精排、delta 窗口已在 v1 趟熟，
      边际成本远低于直上
M3    按需：StreamingMerge、PQ16/OPQ（100M 进 4G 时）
```

已定选型取舍备忘：

- 精排精度统一 int8（两候选同），f32 不进磁盘引擎（体积 4×、收益 ~1pt）；
- delta 窗口统一复用 HnswIndex(inmem_int8) 有界实例——FreshDiskANN 最难的
  "RAM 侧临时索引"本库已有；
- SPANN 排除（质心 RAM 与 2-4G 冲突）；纯 mmap HNSW 大图排除（图遍历随机
  page fault 不可控，见 quant 文档 §10）。

## 附：参考文献

- Subramanya 等, *DiskANN: Fast Accurate Billion-point Nearest Neighbor
  Search on a Single Node*, NeurIPS 2019（Vamana / RobustPrune）。
- Singh 等, *FreshDiskANN: A Fast and Accurate Graph-Based ANN Index for
  Streaming Similarity Search*, 2021（增量 / StreamingMerge）。
- Gao, Long, *RaBitQ: Quantizing High-Dimensional Vectors with a
  Theoretical Error Bound for Approximate Nearest Neighbor Search*,
  SIGMOD 2024（RaBitQ；Extended RaBitQ 2025）。
- Jégou 等, *Product Quantization for Nearest Neighbor Search*,
  IEEE TPAMI 2011（PQ / ADC）；Ge 等, *Optimized Product Quantization*,
  CVPR 2013（OPQ）。
- Chen 等, *SPANN: Highly-efficient Billion-scale Approximate Nearest
  Neighbor Search*, NeurIPS 2021；Xu 等, *SPFresh: Incremental In-Place
  Update for Billion-Scale Vector Search*, SOSP 2023。
