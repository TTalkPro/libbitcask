# 向量落盘 int8 量化设计

> **状态**：设计 / 已落地。`kFlagVecQuantized` 量化路径在 `codec.cpp` 中实现，
> §6 召回测量决定默认行为。
> **关联**：[`vector-db-design-zh.md`](vector-db-design-zh.md)（持久化架构）、
> [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)（内存侧 int8-only）、
> [`int8-vnni-v4-zh.md`](int8-vnni-v4-zh.md)（VNNI 内核）。

---

## 第一部分：实现（代码导向）

### §1. 动机

向量集合的 data file 体积被 f32 向量主导：以 `vector_dim = 2560` 为例，
单条文档向量 = `4 × dim` ≈ 10 KB/doc。落盘 int8 把每条向量压到
`varint(dim) + 1 + 4 + dim` 字节（2564B @ 2560 维），**磁盘占用与读 I/O 缩减
约 4×**。BM25 / KV 负载不受影响——仅 kDoc 的向量段发生变化。

### §2. 现有 seam（DocValue v3 已预留）

DocValue v3 格式（`include/bitcask/format.hpp`）在 v3 引入时已为量化预留一
等公民地位，而非外挂 tag：

- `format::kFlagVecQuantized = 0x08`（`format.hpp` 标志位定义）——向量段是
  码字而非裸 f32；**无独立 magic 常量**，量化由该 **flag 位**标识。早期草稿
  提及的独立 `kQuantizedMagic "QCOD"` stub 已废弃且代码中不存在。
- 码字内含 **`format::kQuantizedVersion = 1`**（`format.hpp` SchemeVer 字
  段），代表当前唯一支持的方案：per-vector 对称 int8。未来 affine 或 PQ 等
  新方案通过 bump 此版本号、读端按版本分发实现，旧读端见未知版本拒绝。
- 向量段定序最前：`[Dim:varint][ f32×Dim 或 量化码字 ]`，便于 HNSW O(1) 切
  片重建（无需遍历前面的 text / meta 段）。

向量段码字布局（`format.hpp` `kFlagVecQuantized` 置位时）：

```
[Dim:varint 元素数][SchemeVer:u8 = kQuantizedVersion][scale:f32 LE][int8 × Dim]
```

大小 = `varint(Dim) + 1 + 4 + Dim` 字节。`varint` 取 1 字节时（Dim < 128）
为 6 + Dim 字节，约 f32 体积的 `6/dim`，dim=2560 时 ≈ 0.23%——**对大数据向量
非常划算**。

### §3. 量化方案（与内存侧共用一份量化器）

直接复用 `bitcask::vec::int8::quantize`（`detail/int8_kernels.hpp`）的
per-vector 对称量化，避免双实现造成位级不一致——磁盘码字一旦写入 checkpoint
就要求量化器稳定，重复实现是召回漂移的常见来源。

**量化公式**（`int8_kernels.hpp::quantize`）：

```
scale      = max |v[i]|                 (per-vector, scalar)
inv_scale  = 127.0f / scale
codes[i]   = round(v[i] * inv_scale)    clamp 到 [-127, 127]
重建 v̂[i] = codes[i] * scale / 127
```

- `max_abs == 0.0f` 时回退为 `scale = 1.0f`，全零向量有明确定义的 scale
  （codes 全 0）。
- 训练过程：零——对称 scalar quantization 无需离线 k-means 或 codebook。
- 预计算 `sum_codes = Σ codes[i]` 与 `sq_norm_codes = Σ codes[i]²`，使下游
  每一次 VNNI 距离调用退化为纯算术 op（消除每向量 reduction loop）。

**落盘一致性保证**：round 模式使用 `std::round`（half-away-from-zero），
**不**切换 SIMD `_mm256_cvtps_epi32` 的 round-half-to-even——后者会在 `.5` 边
界产生不同结果，违反 checkpoint 位级不变约定（见 `quantize_into` 注释）。

**为什么先做对称、不做 affine**：
- 对称与内存侧 int8-only 完全一致，磁盘码字可直接喂给 HNSW（in-memory 也是
  int8），**省一次量化往返**；
- affine（min/max 偏移）精度略高，但需要 MRL 截断 + 偏移归一化两条流水线，
  收益对 cosine-归一化嵌入有限（值域已限制到 [-1, 1]）。
- 后续如需 bump `kQuantizedVersion` 即可加入 affine，无需破坏现有盘上数据。

### §4. f32 权威问题

f32 当前在三处是 source of truth，int8-only 落盘会让它们有损：

| 用途 | int8-only 影响 |
|------|---------------|
| HNSW rerank（int8 粗筛 → f32 精排） | **精排失去 f32**：只能 int8 精排，召回略降（int8 cosine 误差 ≈ 1e-3 量级，见 `int8-vnni-v4-zh.md`） |
| 重嵌入 / 迁移 | 原始 f32 不可逆恢复（量化有损） |
| `get` 返回向量 | 返回 dequant 近似 f32（有损） |

**结论与推荐**：

- int8 落盘是**有损**的体积/精度权衡，不能默认开。
- 设计为 **opt-in**：新增 open 选项 `{vector_quantized, true}`（写入侧量化
  落盘）；默认仍 f32（零行为变化、零兼容风险）。
- `get` 对量化文档返回 dequant f32 并在文档注明有损（见
  `codec::doc_vector_f32` 反量化路径）。
- 是否把它设为某类部署的推荐，**取决于 §6 召回测量**。

不做「int8 + 同时存 f32」——那没有体积收益，违背 P3 目标。

### §5. 读写路径改动点

#### 5.1 编码（`codec::encode_doc_value`）

入口：`DocValueParts { vector, vec_quantized=true, ... }`。

1. **Flags 字节**：`vec_quantized && vector` 同时为真时置
   `format::kFlagVecQuantized`（`0x08`），互斥地覆盖 `kFlagHasVector`——一
   条 kDoc record 的向量段要么是 f32、要么是码字，二者不并存。
2. **量化**：`vec::int8::quantize(v.data(), v.size())` 计算 codes / scale
   / sum_codes / sq_norm_codes（`QVector`）。
3. **写入码字**：`vbyte_encode(dim)` → push `kQuantizedVersion` → 4 字节
   scale f32 LE → int8 codes 二进制拷贝。
4. **后续段**按 vector → text → meta → fields → expiry 定序追加，与量化分支
   完全相同——量化与否不影响其他段。

#### 5.2 解码（`codec::decode_doc_value`）

入口：buffer 起点为 `kDocValueHeaderSize = 2`（Ver + Flags）。

1. **Flags 校验**：`(flags & kFlagVecQuantized) != 0` 时进入量化分支。
2. **码字解析**：`vbyte_read` 读 dim → 1 字节 SchemeVer → memcpy 出
   `vec_scale` → `vector_raw = buf.subspan(pos, dim)`（**zero-copy int8
   codes span**，生命周期跟随 buf）。
3. **SchemeVer 校验**：不等于 `format::kQuantizedVersion` 返回
   `DecodeError::kUnsupportedVersion`，旧读端无法解码新方案。
4. **可访问字段**：`DocValueView::vec_quantized` / `vec_scale` /
   `vector_raw`（int8 字节）/ `dim`——HNSW int8 路径直接使用
   `vector_raw` + `vec_scale`，**不经 dequant→requant 往返**。
5. **上层契约**：`doc_vector_f32` 把 `vector_raw` 反量化为 f32
   （`v̂[i] = codes[i] * scale / 127`），给 `get` / 非 int8 路径使用。

#### 5.3 HNSW 接入

量化集合下 HNSW 直接消费 `DocValueView::vector_raw`（int8 codes）和
`vec_scale` 建图，**省一次量化**——这就是与内存侧共用同一量化器的核心收益。
详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)。

#### 5.4 open 选项 / meta 持久化

`CaskOptions::vector_quantized = true` → 写入 `bitcask.meta` → 重开校验一
致（不一致 → `mode_mismatch`，与 `vector_dim` / `vector_metric` 同类检查）。
配 embedder 时与 MRL 正交：先 MRL 截断到 `vector_dim`，再 int8 量化落盘。

### §6. 召回测量 gate

测量隔离量化误差对召回的影响：brute-force f32 余弦 top-k 为真值，对比
「dequant-int8 库（= 落盘 int8 读回）vs f32 query」的 top-k 重叠。复用
`vec::int8` 的同一量化方案。harness：
`tests/hnsw_test.cpp::measure_quant_recall`（CI 回归红线 ≥ 0.95，可任意
改 n/dim/k 复跑）。

**结果（2026-06，合成归一化高斯，dim=2560，n=2000，nq=30）**：

| 指标 | f32 真值 vs 落盘 int8 |
|------|----------------------|
| recall@10 | **0.9867**（跌 ~1.33%） |
| recall@100 | **0.9953**（跌 ~0.47%） |

**决策：int8 保持 opt-in，f32 仍为默认。** 理由：recall@10 跌幅 ~1.3% 略超
「< 1% 才设默认」的保守线；top-10 是最常见 UI 路径，召回敏感。4× 磁盘换
~1.3% recall@10 对磁盘受限部署是合理取舍，由部署方 `{vector_quantized,
true}` 自选。

**真实语料再验证（设默认前必须）**：上述为合成语料。真实 qwen3 嵌入有聚簇
结构，int8 召回可能更高或更低。要把 int8 设为某类部署默认前，应在真实语料
上用同一 harness 复测（端点见 embedding_endpoint 文档）。当前结论：不设默认。

### §7. int8-only 内存模式实测（与落盘 int8 区分）

P3 落盘 int8 只省**磁盘**；HNSW 内存里仍是 **f32 vecs + int8 qcodes 两份**
（dot 模式），int8 在内存里是为 VNNI 提速、反而 +25% 内存。真正省**内存**要
int8-only 模式：丢掉常驻 f32 `vecs`、建图/精排都用 int8。代价是召回（无
f32 精排）。

实测（`hnsw_test::Int8OnlyMemoryAndRecall`，聚簇合成 dim=2560，n=3000，
nc=50，ef=64；建图+搜索分别在 f32 与 dequant-int8 上跑，真值用 f32
brute-force）：

| | f32 精排（现行 dot 模式） | int8-only |
|---|---|---|
| recall@10 | 1.0000（合成簇可分，饱和） | **0.9675**（Δ −3.25%） |
| 向量存储/向量（dim=2560） | 5·dim+8 = **12808 B** | dim+8 = **2568 B** |
| 1M 向量（仅向量存储） | **12.81 GB** | **2.57 GB** |

**结论：int8-only 省 ~80% 向量内存（~5×），代价 recall@10 约 −3% 量级。**
（邻接表两模式相同、未计入差值——计入后总占用比略小于 5×。）合成簇高度可分
使 f32=1.0 饱和、只暴露 int8 的隔离代价；真实语料 f32 < 1，int8 delta 量级
相近，设默认/上线前需真实语料复测。这是向量库内存墙的主要杠杆——比 P3 的磁
盘优化更接近瓶颈。**已落地（P5b）**：`HnswConfig::inmem_int8` /
`CaskOptions::vector_inmem_int8`（去常驻 f32 副本 + 查询侧量化，仅 kDot，
kL2 拒绝）——详见 [`hnsw-int8-only-design-zh.md`](hnsw-int8-only-design-zh.md)。

### §8. fixtures / 兼容 / 分期

#### fixtures

- 项目不考虑向后兼容；但 opt-in 默认关 → 既有 f32 数据零影响。
- 量化码字走与 f32 相同的黄金 fixture（encode/decode round-trip + dequant
  误差界），按 `(flags & kFlagVecQuantized)` 分支提供两条 fixture：
  - 未量化：标准 f32 字节流（dim × 4 字节 LE）。
  - 量化：`vbyte(dim) + SchemeVer + scale + codes`，round-trip 应保留 codes
    字节完全一致（量化是确定函数）+ dequant 误差界 < 5% @ cosine-归一化。

#### 兼容

- `decode_doc_value` 只接受 `kDocValueVersion == 3`；量化与否由 Flags 区分。
- 旧读端（无 int8 支持）忽略 `kFlagVecQuantized` 时，会把码字当作 f32 解码
  ——**必须**通过后续 bump `kDocValueVersion` 才能优雅拒绝，本设计选择
  「读端必须支持量化」作为隐式契约（与项目整体 v3-only 一致）。

#### 分期

- **P3a**：codec int8 编解码 + round-trip 测试 + fixture（纯格式，不接 open）。
- **P3b**：open `{vector_quantized}` 接线 + meta 持久化 + HNSW 直接接入 codes。
- **P3c**：§6 召回测量 + 决定默认与文档。

### §9. 行业对比：PQ vs per-vector int8

> 2026-06 归档。背景：评估 PQ 是否需要在 2.1.1 落地，结论 V7+。

#### 方案对比

| | per-vector int8（当前） | Product Quantization (PQ) «Jégou 等, TPAMI 2011» |
|---|---|---|
| 原理 | 每向量独立：`scale = max\|v[i]\|`, `codes[i] = round(v[i]/scale×127)` | 切 M 子段，每段在离线 k-means codebook 里找最近质心 |
| 每向量内存 | dim bytes + 4B (2.56 KB @2560d) | M bytes (32B @M=32) + 共享 codebook |
| 1M 向量 | 2.56 GB | ~35 MB（**73×**） |
| 压缩比 | 4× (f32→int8) | ~300× (f32→PQ) |
| recall 损失 | ~3% | ~5-15%（取决于 M/k） |
| 训练 | 不需要 | **需要离线 k-means** |
| 距离计算 | int8×int8 dot (VNNI) | 查表 (ADC) |

#### 各向量数据库采用情况

| 数据库 | 方案 | 说明 |
|---|---|---|
| FAISS (Meta) | 两者都有 | `IndexScalarQuantizer` (int8) + `IndexIVFPQ` (PQ) |
| Milvus | 两者都有 | SQ8 (int8) 和 PQ 可选；DiskANN 用 PQ 做盘上压缩 |
| Qdrant | per-vector int8 | 与 bitcask 最像——per-vector scalar，无训练 |
| Weaviate | PQ | 大规模场景默认 PQ |
| Pinecone | PQ 系 | 闭源，基于 PQ 原理 |
| pgvector | 全精度/f16 | 默认 f32，近年加 f16 和 binary，无 PQ |
| Chroma | 全精度 | 几乎不量化 |
| LanceDB | 两者都有 | IVF+PQ 或 scalar |

#### 规模分层

```
< 10M 向量  → per-vector int8 够用（内存可控，recall ≈97%，零训练）
10M ~ 1B   → PQ 主流（73× 压缩，但 recall 掉 5-15%，需训练管线）
> 1B       → PQ + DiskANN（盘上 PQ，内存只放粗粒度索引）
```

bitcask 处于**百万级**档位——per-vector int8 + int8-only 模式（内存 −80%）是
正确选择。PQ 的离线训练管线 + recall 损失在此规模不划算，留 V7+。

---

## 第二部分：理论（DiskANN / Vamana / RobustPrune）

> 以下理论段描述 100M+ 向量场景，作为 V7+ 大规模方向的决策依据；当前实现
> 仍以 HNSW 全内存为主。

### §10. 大规模方向：DiskANN vs mmap HNSW

#### 问题

100M+ 向量时，HNSW 图结构本身（邻接表 + node 元数据）≈ 10 GB，加上 int8
向量 ≈ 250 GB——内存放不下。需要一种盘上方案。

#### DiskANN（Microsoft Research）

**论文**：Subramanya 等，*DiskANN: Fast Accurate Billion-point Nearest
Neighbor Search on a Single Node*，NeurIPS 2019。
**目标**：单机 64 GB RAM 扛 10 亿向量。

##### 架构

```
┌─── RAM（小）───┐   ┌─────────── SSD ───────────┐
│ 入口点 + 缓存  │   │ Vamana 图邻接表            │
│ top-k 精排缓冲 │←→│ PQ 压缩向量（内联在节点旁） │
│ ~64 B/vec     │   │ 全量数据，TB 级随意         │
└────────────────┘   └────────────────────────────┘
```

##### 三个关键设计

**① Vamana 图（替代 HNSW 多层跳表）**

单层图，引入 α 参数控制建边多样性：

- α 小 → 只连近邻（局部密集，远距离跳数多）
- α 大 → 也连远邻（长程边，减少跳数）
- 兼顾短边（精局部搜索）+ 长边（快长程跳跃）
- 单层比 HNSW 多层**磁盘友好**（不用跨层跳 = 减少随机读）

**② PQ 内联存储**

每个节点在 SSD 上存 `[邻居 ID × R][PQ 码 M bytes]`：

- PQ 码做粗排距离计算——不需读完整向量
- 最终 top-K 才做精排（从 RAM cache 或 SSD 读 f32）

**③ Beam Search（驯服 SSD 随机读）**

HNSW 纯贪心遍历在 SSD 上每次随机读 ~100 μs 灾难。DiskANN：

- 维护候选队列，每次批量读 L 个候选的邻居（beam width = L）
- L 次 SSD 读 pipeline 成顺序预读
- 把 ~100 μs 随机读摊薄成 ~10 μs/节点有效吞吐
- 典型 beam width = 4-8，实测延迟 ~1-5 ms/query

##### 规模对比

| 规模 | HNSW（当前） | DiskANN |
|---|---|---|
| 1M | ✅ ~2.5 GB RAM，μs 级延迟 | 不值得 |
| 10M | ✅ ~25 GB，需大内存 | 可选但 HNSW 更优 |
| 100M | ⚠️ ~250 GB，内存不够 | ✅ ~6.4 GB RAM + SSD |
| 1B | ❌ 不可行 | ✅ ~64 GB RAM + TB SSD |

#### 为什么 HNSW 外存 mmap 不是好方案

1. **图遍历是随机访问**：跳邻居→跳邻居，mmap page fault 极频繁，性能不可控
2. **不改变加载模型**：search.ckpt 序列化是 flat 小端字节，理论可 mmap，但
   HnswIndex 访问层（NodeChunk/vector）需要全部改 mmap-aware 指针——工程量
   等同重写
3. **业界 100M+ 主流是 DiskANN**，不是 mmap HNSW

#### bitcask 的结论

当前百万级：HNSW 全内存（μs 级）+ search.ckpt 序列化持久化，完全够用。

若未来要扛 1 亿+ 向量，需要 DiskANN 类架构（Vamana 图 + PQ + SSD beam
search）——本质上是另一个引擎，属 V7+ 大版本架构变更。

### §11. Vamana 图详解

> DiskANN 使用的图索引结构，是 §10 大规模方向的核心组件。

#### 11.1 近邻图搜索的通用问题

所有图索引（HNSW / Vamana / NSW）核心思路一致：

```
图：节点 = 数据点，边 = 近邻关系
搜索：从入口点出发，贪心跳邻居 → 逐步逼近查询点
```

矛盾点：**纯短边图跳远距离慢**（像走小路），**纯长边图跳近距离不准**（像
只坐高铁不走路）。HNSW 和 Vamana 用不同策略解决这个矛盾。

#### 11.2 HNSW 的策略：多层跳表

```
Layer 2:  ●─────────────────●────────●          （稀疏，长程跳跃）
Layer 1:  ●──────●──────●──────●──────●          （中等密度）
Layer 0:  ●──●──●──●──●──●──●──●──●──●──●        （全量，精细搜索）
```

搜索从顶层大步跳、逐层下降做精细搜索。**内存极快**（指针跳转纳秒级），
**磁盘灾难**——每次降层是文件不同区域的随机读。

#### 11.3 Vamana 的策略：单层 + α 剪枝

Vamana 不用多层，靠建边时的 **RobustPrune** 算法在单层中兼顾长短边。
RobustPrune 算法出自 Subramanya 等, *DiskANN*, NeurIPS 2019（Algorithm 1）。

##### Robust Prune 算法（Subramanya 等, NeurIPS 2019, Algorithm 1）

```
RobustPrune(p, V, R, α):
  按 dist(p, ·) 升序排 V
  result = {}
  while V 非空 and |result| < R:
    v = V 中离 p 最近的              // 一定入选
    result.add(v)
    V.remove(v)
    对 V 中剩余的每个 u:
      if α × dist(p, v) ≤ dist(v, u):
        V.remove(u)                  // u 被 v "覆盖"，丢弃
  return result
```

##### α 参数的直觉

α 控制「覆盖」判定阈值：

```
α = 1.0（严格剪枝）：

  p ──── v（最近，入选）
       ╱ ╲
     u1   u2   ← dist(v,u1) < dist(p,v) → u1 被 v 覆盖 → 丢弃

  结果：邻居散布各方向但都很近 → 短边图，远距离跳数多

α = 1.2（放松剪枝）：

  p ──── v（最近，入选）
      ╱   ╲
    u1     u2   ← 1.2 × dist(p,v) 放宽阈值
                u1 可能不被覆盖 → 保留
                u2 在另一方向 → 也保留

  结果：既有近边（局部精搜）又有远边（长程跳跃），一层搞定
```

- **α 大** → 保留更多长边 → 远距离跳数少但图更密集
- **α 小** → 只留短边 → 局部精确但远距离慢
- DiskANN 论文推荐 **α = 1.2**，最大度 **R = 60-64**

##### 对比 HNSW 的邻居选择

HNSW 也有类似的 robust prune（论文中的 heuristic select），但：

- HNSW 在**每层独立建图**，层间通过概率分配节点 → 越高层节点越少
- Vamana 在**单层建图**，用 α 替代多层结构的长程能力

#### 11.4 为什么对磁盘友好

| | HNSW 多层 | Vamana 单层 |
|---|---|---|
| 搜索路径 | 顶层跳→降层→底层跳→降层… | 单层贪心跳 |
| 磁盘读 | 每次降层 = 跳到文件不同区域 | 邻居在节点旁（内联），locality 好 |
| beam search | 难批量化（跨层依赖） | 容易（同层，候选独立） |
| 随机读 | O(log N × 层跳数) | O(搜索半径 × beam width) |

节点布局：

```
SSD 上每个节点：
[邻居 ID × R][PQ 码 M bytes]
     ↑            ↑
  图遍历用      距离计算用
  （在一起，读一次全拿到）
```

#### 11.5 两阶段建图（大规模加速）

全量建 Vamana 图对 1 亿+ 点太慢。DiskANN 用两阶段：

1. **阶段 1**：随机采样 ~5%，建小图（快）
2. **阶段 2**：剩余点逐一插入——用小图当初始邻居候选，增量加边 + α 剪枝

#### 11.6 总结

Vamana = **单层图 + α 剪枝**，用一层替代 HNSW 多层跳表，让图遍历模式对
SSD 随机读更友好，同时保持搜索精度。是 DiskANN 实现单机十亿级向量搜索的
基础。

---

## 附录：参考论文

- Subramanya 等，*DiskANN: Fast Accurate Billion-point Nearest Neighbor
  Search on a Single Node*，NeurIPS 2019（Vamana / RobustPrune）。
- Guo 等，*Accelerating Large-Scale Inference with Anisotropic Vector
  Quantization*，ICML 2020（ScaNN / AVQ）。
- Jégou、Douze、Schmid，*Product Quantization for Nearest Neighbor Search*，
  IEEE TPAMI 2011（PQ / ADC）。
- Ootomo 等，*CAGRA: Highly Parallel Graph Construction and Approximate
  Nearest Neighbor Search for GPUs*，ICDE 2024（GPU ANN）。