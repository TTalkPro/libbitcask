# 向量双引擎选型：DiskANN vs IVF-RaBitQ（磁盘档第二引擎预研）

> 状态：**预研 / 选型结论已定 + 规划 v2（2026-07-13），未立项**。
> 部署前提（v2 修订）：用户机器 8-16G 内存，**本库可用 1-6G**，且须与 BM25
> （keydir/doc_table 常驻 + S30 mmap 段页缓存）共享该预算。
> 已定四点方向：① **HNSW 为主引擎**（优化线见
> [`s29-11-hnsw-deep-opt-design-zh.md`](s29-11-hnsw-deep-opt-design-zh.md)）；
> ② **ckpt 方案必须改**——不能依赖 close 才收敛，崩溃恢复代价须有界（§6.2）；
> ③ IVF-RaBitQ/DiskANN 做**完全独立的插件**，建库时配置、存入 meta（§4）；
> ④ 提供 **HNSW ↔ 磁盘引擎的盘上内容转换工具**（§6.4）。
> 本文记录磁盘档引擎候选对比（DiskANN / IVF-RaBitQ / SPANN）、PQ 与 RaBitQ
> 量化知识、**v1 选 IVF-RaBitQ、DiskANN 留作 v2 条件触发**的结论，以及按
> 上述四点重排的分期（§7）。
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

可行性矩阵（1M 向量，int8-only 已开；列为**向量子系统可用预算**——总预算
1-6G 扣除 BM25 常驻后的份额，推导见 §6.1）：

| dim | 稳态堆 | ×2 重建峰值 | 向量预算 1G | 向量预算 4G |
|---|---|---|---|---|
| 384 | 0.51 GB | 1.0 GB | ⚠️ 重建时压线 | ✅ |
| 768 | 0.87 GB | 1.7 GB | ❌ 重建 OOM | ✅ |
| 1536 | 1.59 GB | 3.2 GB | ❌ | ⚠️ |
| 2560 (qwen3) | 2.54 GB | 5.1 GB | ❌ | ❌ |

### 1.2 HNSW 优化线（第一引擎，与本文正交）

- **P0 纯配置**：`vector_inmem_int8` + `vector_quantized` + MRL 截断
  `vector_dim`（qwen3 是 Matryoshka 模型，2560→1024 召回损失很小）；
- **P1 工程**：`.qc8` mmap 化（`qcodes_of` 按 `id < checkpoint_count_` 路由，
  与 `vec_of` 同型；BCQ8 定长 stride 天然可按 id 寻址）→ 堆降到 ~165 B/节点，
  内存不足退化成慢而非 OOM；`clone_live` 改 `needs_vecs=false` + f32 从旧
  mmap 流式写新 `.vec`，消 2× 峰值；
- **P2 按需**：RAM 驻 RaBitQ 导航码（1 bit/维，见 §3.3）替代 int8 导航，
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

### 3.2 IVF（倒排文件）——聚类分桶流派

```
建索引：k-means 把 N 个向量聚成 nlist 个簇（如 4√N 个）
        每簇一条 posting list，存该簇成员
查  询：query 先和 nlist 个质心比距离 → 只扫最近的 nprobe 个簇
        （nprobe/nlist ≈ 扫描比例，典型只扫 1-5% 的数据）
```

本质是"用聚类把 O(N) 暴力扫剪成 O(N × 几%)"。**关键性质：posting list 是
连续存储 → 磁盘读是少量大顺序读**——这是它对 DiskANN（图遍历 = 大量 4K
随机读）的根本差异，也是它能架在 SATA/云盘甚至对象存储上的原因。

弱点：query 落在簇边界时召回崩（靠加大 nprobe 补偿，平滑旋钮）；数据分布
漂移后质心需重训（posting 追加本身廉价，重训才是增量的痛点）。

与量化组合的典型形态（LanceDB）：质心 + 压缩码驻内存或热层，posting list
顺序躺盘上，粗筛用压缩码（PQ 查表或 RaBitQ 位运算），终排用原始向量 refine。

### 3.3 RaBitQ（2024，NUS）——PQ 的零训练替代品

```
随机正交旋转向量 → 每维取符号位（1 bit/维）→ 存少量校正标量（范数等）
距离 = 位运算(XOR/popcount) + 校正公式 → 无偏估计量，带可证误差界
```

对比 PQ 的三个优势：

1. **零训练**：旋转矩阵是随机的，没有 codebook，也就没有数据漂移后
   codebook 失配的问题（PQ 增量场景的隐性成本）；
2. **同压缩比下误差界更优**，且是带证明的无偏估计（PQ 只有经验误差）；
3. **距离计算是 popcount**（比 PQ 查表还快，SIMD 极友好；内核 ~200 行，
   与本库 int8 内核基建同型的三级 runtime dispatch）。

32× 压缩（f32→1 bit/维）；Extended RaBitQ 可调 2-8 bit/维换精度。
采用者：LanceDB IVF-RQ、Milvus 2.6、Elasticsearch BBQ（衍生自 RaBitQ）。
（与 PQ 的码宽/甜区对比见 §3.5 末。）

### 3.4 SPANN（微软，NeurIPS 2021）——质心图 + 磁盘倒排流派（已排除）

SPANN 与 DiskANN 同出微软，是内部竞争路线：

```
┌──── RAM ────────────────┐   ┌──── 磁盘 ─────────────────┐
│ 大量质心（~N 的 10-16%）  │   │ 每质心一条 posting list     │
│ + SPTAG 图索引导航质心    │   │ （全精度向量，顺序存储）      │
└─────────────────────────┘   └───────────────────────────┘
查询：RAM 内图搜索找最近的几个质心 → 顺序读回这几条 posting → 精算
```

与 IVF 同属聚类分桶心智，但两个关键改进：

1. **质心数量极多**（不是 4√N 而是 N 的一成量级），每条 posting 很短且
   长度均衡 → 单次查询只需读少数几条、每条一次顺序 IO；
2. **边界向量多副本**（closure assignment）：落在簇边界的向量同时塞进多个
   相邻 posting → 解决 IVF 最大的痛点（簇边界召回崩），代价是磁盘膨胀
   ~1.5-2×。

后续 **SPFresh**（SOSP 2023）解决增量更新（posting 的轻量分裂/合并协议）。
Chroma 云端、turbopuffer 都是 SPANN 系——"顺序读 posting"的 IO 模式可以
直接架在**对象存储**（S3）上，随机 4K 读的 DiskANN 做不到。

**本库排除理由**：质心向量驻留（N 的一成 × dim）在高维下吃 RAM，与 2-4G
前提正面冲突（100M×1024d 的 10% 质心 ≈ 数十 GB 量级，量化后仍 GB 级）。

### 3.4b 三流派横向对比（10M-100M、单机 NVMe 视角）

| | DiskANN | SPANN | IVF + PQ/RaBitQ |
|---|---|---|---|
| RAM 驻留 | PQ 码 32-64B/向量（**最省**，教科书口径；v1 详设的反转见 §5.0） | 质心向量 + 图（N 的一成×dim，**最费**，高维下 2-4G 放不下） | 质心 + 可选压缩码（居中；码下盘后仅质心，见 §5.1） |
| 磁盘 IO 模式 | 每跳 4K 随机读 ×几十次（**必须 NVMe**） | 几次大顺序读（对象存储也行） | 几次大顺序读 |
| 召回@同延迟 | **最优**（95-98%） | 好（90%+，靠副本补边界） | 最弱（85-95%，nprobe 补） |
| 增量更新 | FreshDiskANN（复杂） | SPFresh（中等） | posting 追加易、**质心重训难** |
| 工程量 | 最大（图构建+beam IO+PQ） | 中（聚类+副本+图导航质心） | **最小**（k-means + 顺序扫，无图） |
| 磁盘膨胀 | 1×（+4K 对齐填充，见 §5.3） | 1.5-2×（副本） | 1× |

### 3.5 PQ 码宽（决策变量释义）

**码宽 = PQ 压缩后每条向量占的字节数 = 参数 M（子空间个数）**。"PQ32" 即
32 字节/向量。它是怎么来的：

```
原向量 (D=1024 维, f32, 4096 字节)
    │  切成 M 段（M=32 → 每段 32 维）
    ▼
[ 段1: 32维 ] [ 段2: 32维 ] ... [ 段32: 32维 ]
    │  每段在自己的 codebook（k-means 训练的 256 个质心）里找最近质心
    ▼
[ id₁: 1字节 ] [ id₂: 1字节 ] ... [ id₃₂: 1字节 ]   ← 共 32 字节
```

每段 256 个质心正好用 1 字节编号，所以码宽 = M 字节。查询时距离不解压，
用 ADC 查表：query 先对每段预算 256 个"query 子段 ↔ 质心"距离表
（M×256 个 float），之后每条候选的距离 = M 次查表求和。

| 码宽 | 每段维数(D=1024) | 压缩比 | 100M 向量 RAM | 误差 |
|---|---|---|---|---|
| PQ64 | 16 | 64× | 6.4 GB | 最小 |
| PQ32 | 32 | 128× | 3.2 GB | 小 |
| PQ24 | ~43 | ~171× | 2.4 GB | 中 |
| PQ16 | 64 | 256× | 1.6 GB | 大 |

**为什么码宽是 DiskANN 选型的核心变量**——它是 RAM↔召回 的直接旋钮：

- **段越少（码窄）**：每段要用固定的 256 个质心覆盖更高维的子空间（PQ16 时
  一段管 64 维），量化误差急剧上升 → 近似距离更不准；
- 在 DiskANN 里，PQ 距离只用来决定 **beam search 展开哪些候选**（导航），
  最终排序由 SSD 上的全精度（本库方案：int8）精排兜底——所以窄码不直接毒害
  终排，但会让导航走错路、漏掉真近邻，表现为**召回下降，或被迫加大 beam
  width 补偿（= 更多 SSD 随机读、更高延迟）**；
- 本库场景的具体读法：100M×PQ32 = 3.2 GB 超出 4G 机器预算 → 被逼到
  PQ16/24 才放得下，但每段管的维数翻倍，导航质量降多少必须由召回 harness
  实测——这正是 §5.3 表中 DiskANN 召回一栏标"窄码打折"的原因。

两个相关补充：

1. **OPQ**（Optimized PQ）：训练前先学一个旋转矩阵，把信息量在 M 个段之间
   摊匀（否则某些段方差大、256 质心不够用）。同码宽下召回更好，是窄码的
   标配补偿手段，代价是多一步训练。
2. **对比 RaBitQ 的"码宽"**：RaBitQ 的旋钮是 bit 数/维而非段数——1 bit/维
   时 1024d → 128 字节 + 几个校正标量，比 PQ32 宽得多（2560d 时 320B vs
   32B）。所以**极限压缩（十亿级全量码进 RAM）仍不可替代 PQ**；RaBitQ 的
   甜区是"中等压缩 + 零训练 + popcount 快"——这也是为什么 DiskANN 生态用
   PQ，而 IVF 新贵（LanceDB / Milvus 2.6 / ES BBQ）用 RaBitQ。

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

### 4.1 引擎形态决策（v2 已定）：完全独立的兄弟插件

**决策：IVF-RaBitQ / DiskANN 做与 `VectorPlugin` 平级的独立 `CaskPlugin`
实现**（如 `vec::IvfPlugin`），不做 VectorPlugin 内的策略对象。理由：

- 两类引擎的 delta/base 语义**真实分叉**：HNSW 的 delta 是"插入日志→重放
  重建图"、base 是全图序列化；IVF 的 delta 是"posting 追加段"、base 是
  posting 重写——塞进同一个策略接口会让 flush/恢复语义漏抽象；
- 宿主层本就以 `CaskPlugin` 为唯一契约（§4 开头），兄弟插件是架构的自然形态
  ——Milvus/Redis 的多索引类型同样是平级注册而非策略注入；
- 引擎在**建库时一次性选定**（见下），运行期不存在切换，策略对象的动态性
  没有用武之地。

代价（接受）：VectorPlugin 里引擎无关的域逻辑需抽成共享件而非继承复用——
抽取清单：`normalize_for_write`（本就是纯函数）、delta 插入日志结构
（`delta_ords_/delta_data_` 平行数组 + serialize/apply，抽成
`vec::DeltaLog`）、`ckpt::ChainState`（已共享）、int8 量化器（已共享）、
召回 harness（M0 新建，两引擎共用）。

### 4.2 建库时选定 + meta 持久化

- `CaskOptions::vector_engine`（enum：`kHnsw` / `kIvfRq` / 预留 `kDiskann`）
  → 写入 `bitcask.meta`（`meta::MetaConfig` 新字段）；
- 重开校验：与 `vector_dim` / `vector_metric` / `vector_quantized` /
  `vector_inmem_int8` 同组 mode_mismatch 检查（`cask.cpp` 现有检查点扩一项）
  ——**引擎不可运行期切换**，唯一切换路径是 §6.4 离线转换工具；
- Cask open 按 meta 实例化对应插件（工厂一处分支）；manifest 组件身份 =
  `name()`（"hnsw" / "ivfrq"，本就按算法命名的持久化身份）；
- 两插件同守 `CaskPlugin` 幂等重放 / 水位 / merge 协议——宿主除工厂外零改动。

---

## 5. v1 详设对比：IVF-RaBitQ vs DiskANN

约束：独立插件接线（§4）之后、10M-100M、向量 RAM 预算 ≤1G（§6.1 总账，
与 BM25 共享 1-6G）、NVMe、metric=kDot（归一化）。

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
- **落地勘误（2026-07-13，S32-M3 v1）**：实现为 int8 码字单遍扫（粗筛 =
  精排一遍过），RaBitQ 1-bit 码区在 BIV1 格式留位（flags bit0）未启用——
  是否引入由召回 harness 出数决定（TASK.md M3.5）。v1 磁盘 = N×(dim+16)；
  基线发现：紧簇合成语料的簇内边际低于 int8 噪声底，跨引擎召回对账需
  harness 语料 v2（M3.5-①）；
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

## 6. v2 规划新增工作面（2026-07-13 部署前提修订后）

### 6.1 内存预算总账（1-6G，BM25 同租户）

总预算 1-6G 内，向量子系统不是唯一住户：

| 住户 | 常驻性质 | 量级 |
|---|---|---|
| keydir + doc_table | 硬驻留 | ~50-80 B/key：10M docs ≈ 0.5-0.8G；**100M ≈ 5-8G** |
| BM25 building 段 + term cache | 硬驻留（预算封口可控，S30） | 百 MB 级可配 |
| BM25 封口段 | mmap 页缓存（可回收） | 弹性 |
| 向量引擎 | 见下 | 目标 ≤1G 硬驻留 |

**⚠️ 100M 档的独立风险**：keydir 自身 5-8G 就撑爆总预算——这是与向量无关的
KV 层大轴（keydir 出内存/分库），若 100M 是认真目标须**另行立项**，本文
向量规划以"keydir 已另案解决或文档数 ≤ 数千万"为前提。

向量侧经验预算：**硬驻留 ≤1G，页缓存弹性共享**。各档位归宿：

- **HNSW（P1 后）**：堆 ~165 B/节点 + qc8 页缓存（N×(D+8)）——舒适区
  N×D ≲ 向量可分到的页缓存；1024d 下 ~2-4M 向量；
- **IVF-RaBitQ**：硬驻留 ~300 MB（质心 + delta 窗口），nprobe 扫描吃页缓存
  但是顺序读、可回收——与 BM25 mmap 段的互扰面是页缓存竞争，退化形态是
  慢而非 OOM。

### 6.2 ckpt 改进：崩溃恢复代价必须有界（已定方向 ②）

**现状（代码核实 2026-07-13）**：自动 checkpoint 已存在（S31.5：每
`auto_checkpoint_min_docs=64K` ord 增量异步提交，默认开）——"只在 close
落盘"已不成立；**但向量插件每次 auto flush 只写 delta**（`.d` = ord + f32
插入日志），base（BVH2 header + `.vec`/`.qc8`）仅在 rebuild / 链长满
（`max_delta_chain=64`，与文本共用，`search_config.hpp`）/ close 时收敛。

**崩溃恢复代价 = base 反序列化（结构拷贝，快）+ 链重放 = 重新建图**：
最坏 64 链 × 64K ≈ **4.2M 条向量重插**（每条一次 ef_construction 搜索）
——高维/无 VNNI 机器上小时级；且 `.d` 内联 f32，链窗口磁盘冗余 ≈ 64×64K×4D。
durability 无问题（data file 即 WAL），痛点是**恢复时间无界**。

改进方案：

1. **向量 base rebase 改双门槛**：链覆盖 ord 增量 ≥ `vector_rebase_min_docs`
   （默认 256K）**或** 链长 ≥ 上限（保底）→ 恢复重放窗口恒 ≤ 256K + 64K 条
   （分钟级）；
2. base 成本已被 S14-2/S14-8 摊薄：`.vec`/`.qc8` 是**追加**（前缀不变契约 +
   身份收养），全量重写的只剩 BVH2 header（≈165 B/节点）：1M ≈ 165MB 顺序写
   秒级、3M ≈ 0.5GB——周期化可承受；
3. `max_delta_chain` 向量侧独立配置（向量 delta 内联 f32，天然比文本重）；
4. **不做**邻接增量落盘（反向边可变，`hnsw.hpp` 头注已判"邻接可变无法
   外置"）；若未来 5M+ 节点 header 重写成痛点，再评估按 chunk 脏标记局部
   重写；
5. 同款双门槛平移给 IVF 插件（其 base = posting 段重写，天然更便宜；delta
   本就是 posting 追加，恢复重放是**assign 而非建图**，代价低一个量级）。

验收：kill -9 注入 × 恢复时间上界测试（重放 ≤ 窗口）；恢复时长纳入 bench
基线。

### 6.3 独立插件接线

见 §4.1/§4.2（形态决策 + meta 持久化 + 工厂）。要点重申：建库时选定、
meta mode_mismatch 校验、运行期不可切换。

### 6.4 引擎转换工具（已定方向 ④）

`tools/vec_engine_migrate.cpp`（先例：`tools/migrate_le.cpp`）。
CLI：`--dir <db> --to hnsw|ivfrq|diskann [--dry-run]`。

**原理：data file 是向量权威**（DocValue 向量段），两引擎的索引文件全是
派生数据 ⇒ 转换 = **离线重建目标索引**，不做索引格式互转（无需两引擎互懂
对方格式，DiskANN 加入时零改动扩展）：

```
file lock 独占 → 读 meta → 流式 fold data files（复用恢复扫描器）
→ 构建目标引擎组件文件（tmp 名）→ 写新 meta（vector_engine=目标）
→ rename 原子发布 → 旧引擎组件文件保留一代（回滚 = 改回 meta）
```

- **方向性差异**：`→ivfrq` 流式外部分桶，小内存可转 100M；`→hnsw` 需全图
  驻留建图，仅 ≤ 数 M 可行——工具按 N×D 与可用内存**预检**，超限拒绝并
  提示；
- **量化联动**：`vector_quantized` 库直接搬 int8 码字，零再量化漂移（两
  引擎共用量化器的设计红利）；
- 工具运行期间库必须关闭（file lock 排他，与在线 merge 互斥）。

## 7. 结论与分期（v2）

**磁盘档 v1 选 IVF-RaBitQ**——不是折衷而是本约束下的正解：DiskANN 仅存的
两项优势（延迟、召回上限）恰是约束里最有弹余的（10-30ms 对 10M-100M
离线/RAG 场景通常可接受），而其三项硬伤（PQ 码 RAM、建库、增量重建）全部
撞在小内存前提上。

```
M0    引擎接线基建：vector_engine meta 字段 + 插件工厂 + 共享域件抽取
      （归一化 / DeltaLog / 链状态 / int8 量化器）+ 召回 harness
      （S29-11 §2，两引擎共用）
M1    ckpt 改进（§6.2）——独立小批次，不依赖 M0，可最先做：
      崩溃恢复窗口从最坏 4.2M 条收到 ≤320K 条
M2    HNSW 线 P0/P1（qc8 mmap 化 + clone_live 峰值治理）——主引擎
      舒适区推到 ~2-4M（1024d）
M3    IvfPlugin v1（IVF-RaBitQ，~3k LOC，独立插件）；RaBitQ popcount
      内核顺手供 HNSW 线 P2（导航码）
M4    转换工具 vec_engine_migrate（依赖 M3；~0.5k LOC）
M5    条件触发：DiskannPlugin——出现「p99 <10ms 且 ≥95% 召回 @ 50M+」
      硬指标才立项；届时 harness / int8 精排 / delta 窗口 / 转换工具
      框架全部复用，边际成本远低于直上
```

已定选型取舍备忘：

- 引擎 = 建库时一次性选定（meta），运行期不可切换；切换唯一路径 = §6.4
  离线工具；
- 精排精度统一 int8（两候选同），f32 不进磁盘引擎（体积 4×、收益 ~1pt）；
- delta 窗口统一复用 HnswIndex(inmem_int8) 有界实例——FreshDiskANN 最难的
  "RAM 侧临时索引"本库已有；
- SPANN 排除（质心 RAM 与小内存前提冲突）；纯 mmap HNSW 大图排除（图遍历
  随机 page fault 不可控，见 quant 文档 §10）；
- 100M 档的 keydir 内存（5-8G）是独立于向量的 KV 层大轴，须另行立项
  （§6.1）。

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
