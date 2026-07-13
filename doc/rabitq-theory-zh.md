# RaBitQ 理论详解（1-bit 量化的无偏估计量与误差界）

> 状态：理论文档（2026-07-13，S32-M5 收官后整理）。
> 关联：[`vector-dual-engine-selection-zh.md`](vector-dual-engine-selection-zh.md)
> §3.3（速查版）、[`vector-ondisk-quant-design-zh.md`](vector-ondisk-quant-design-zh.md)
> §9（PQ 对比）。本库现役实现：IVF v2 bits 区（lite 版，`ivf_rq.cpp`）；
> M5 教训与升级路径见 §8。
> 论文：Gao & Long, *RaBitQ: Quantizing High-Dimensional Vectors with a
> Theoretical Error Bound for Approximate Nearest Neighbor Search*,
> SIGMOD 2024；Extended RaBitQ, 2025。

RaBitQ 不是"又一个量化技巧"：它把"每维 1 bit"这个看似暴力的压缩做出了
**可证明的无偏估计量和 O(1/√D) 误差界**——PQ 三十年谱系（Jégou 2011 起）
一直没有的东西。零训练（随机旋转替代学习码本）、位运算距离、带界误差，
三者同时成立。

---

## §1. 问题设定：为什么"每维 1 bit"够用

目标：用极少的 RAM 驻留码估计 ⟨o,q⟩（内积/距离），供粗筛**排序**。

朴素直觉（f32 压 32× 必然面目全非）错在混淆了"重建向量"与"估计内积"
——不需要重建 o，只需要估计一个标量。高维几何在这里是助力：

- 归一化后 o 在单位球面上，方向信息分散在 D 维；
- 每维 1 bit 共 D bit 的**总信息预算**随维度增长；
- RaBitQ 证明该预算下内积估计误差 **O(1/√D)**，且为渐近最优率。
  D=1024 时 ~2%，粗筛排序绰绰有余。

前提：误差必须**与数据无关**（不能存在对抗性坏向量）→ 随机旋转（§2）。

## §2. 构造：超立方体码本 + 随机旋转

**码本** = 单位超立方体角点集：

```
C = { (±1/√D, ±1/√D, ..., ±1/√D) }    2^D 个码字,每个都是单位向量
```

量化 = 取最近角点 = **每维取符号位**，物理存储 D bit。

**随机旋转 P**（Haar 随机正交阵）堵对抗洞：直接取符号可被数据对抗（能量
集中于一维的向量，符号码几乎零信息）。量化 `x̄ = sign(P⁻¹o)/√D`——旋转
把任意数据结构变成各向同性（每维能量 ~1/D，每 bit 信息均匀），**最坏情况
变平均情况**。这一步是全部理论保证的来源，也是"零训练"的来源：P 随机、
不从数据学 → 无码本、无漂移（对照：PQ 码本 k-means 学出，漂移即失配）。

## §3. 核心洞察：无偏估计量（三行推导）

把量化码 x̄（单位向量）分解到 o 方向与正交方向：

```
x̄ = α·o + β·e⊥        α = ⟨x̄,o⟩（量化保真度）, β = √(1−α²), e⊥ ⊥ o
⟨x̄,q⟩ = α·⟨o,q⟩ + β·⟨e⊥,q⟩
```

随机旋转的礼物：e⊥ 在 o 的正交球面上**均匀分布** → E[⟨e⊥,q⟩] = 0：

```
E[ ⟨x̄,q⟩ / α ] = ⟨o,q⟩          ← 无偏估计量
Var = (1−α²)(1−⟨o,q⟩²)/(D−1)
```

三个量的来历：
- `⟨x̄,q⟩`：查询时位运算（§5-①）；
- `α = ⟨x̄,o⟩`：**建索引时已知，每向量存 1 个 f32**——RaBitQ 记录里那个
  "校正标量"的真身；
- α 自身高度集中：旋转后坐标 ~N(0,1/D)，α → √(2/π) ≈ 0.80（D→∞）。

## §4. 误差数字感

代入 α≈0.8：误差标准差 ≈ 0.6/√D（带集中不等式的高概率界，非经验值）：

| D | 内积估计误差（1σ） |
|---|---|
| 128 | ~5.3% |
| 384 | ~3.1% |
| 1024 | ~1.9% |
| 2560 | ~1.2% |

定位：**1/8 的 int8 字节、2-3× 的 int8 误差、外加可证界**；PQ32 是 5-10%
经验误差且无界（对抗数据可任意坏）。

## §5. 工程实现三件套

**① 非对称位平面 popcount**（速度与精度兼得的机关）：估计 ⟨x̄,q⟩ 时
**查询不做 1-bit 量化**（对称化误差翻倍以上——本库 M5 教训的理论解释，
§8）。查询做 4-8 bit 标量量化后按位平面分解：

```
⟨x̄,q⟩ ∝ Σ_j 2^j · [ 2·popcount(x̄_bits AND qbit_j) − popcount(qbit_j) ]
```

4-8 次 AND+popcount 扫描——位运算速度 + 非对称精度。

**② 快速旋转**：朴素 P·v 是 O(D²)。生产用**结构化随机旋转**
`H·D₃·H·D₂·H·D₁`（H = Walsh-Hadamard，Dᵢ = 随机 ±1 对角阵），O(D·logD)，
理论性质近似保持（ES BBQ 等同款）。

**③ IVF 残差化**（论文原生形态）：量化的是**簇内残差方向**而非原始向量：

```
ô = (o_raw − c) / ‖o_raw − c‖          ← 量化这个
记录: D bits + ‖o_raw−c‖ f32 + α f32
‖q−o‖² = ‖q−c‖² + ‖o−c‖² − 2‖q−c‖‖o−c‖·est(⟨q̂,ô⟩)
```

`‖q−c‖` 每簇一次。残差化把 1 bit 花在"簇内区分"上——直接回应本库 v2
语料发现：簇内成员的**原始** sign 码几乎恒定（零区分度），**残差** sign
码高度可分。

## §6. Extended RaBitQ（2025）

码本从超立方体角点推广到嵌套格点（每维 B bit，B=2..8），同一套旋转 +
无偏估计理论覆盖到 int8 精度档——"1-bit 粗筛 + int8 精排"两层未来可统一
为"B-bit RaBitQ 单层，B 按 RAM 预算调"（Milvus 2.6 实现含此）。

## §7. 三方案对照（本库语境，1024d）

| | int8（现役） | PQ32 | RaBitQ 1-bit |
|---|---|---|---|
| 字节/向量 | 1032 | 32 | 128+8 |
| 误差 | ~1%（经验） | 5-10%（经验、**无界**） | ~1.9%（**可证界**） |
| 训练 | 无 | k-means 码本（漂移风险） | 无（随机旋转） |
| 距离算子 | VNNI dot | ADC 查表 | AND+popcount ×位平面 |
| 甜区 | 精排/中压缩 | 极限压缩（十亿级） | 中压缩粗筛/导航 |

## §8. 与本库实现的对照（理论解释实践）

本库有 RaBitQ 的两个"影子"，行为差异恰被理论精确解释：

**IVF v2 bits 区（lite 版，work）**：砍了三刀——无旋转、**对称** popcount
（query 也 1-bit）、校正用 μ=mean|v| 而非 α。每刀都放大误差，但 IVF 场景
est 只做 top-C 粗筛且 C=128 冗余充足 → 噪声被吸收，recall@10_i8 = 1.000
（S32-M3.5-② 实测）。

**M5 beam 导航（崩，已改 int8 nav）**：同一 lite est 驱动**逐步决策**——
误差沿路径复合、无冗余吸收；且无旋转+无残差化使簇内 sign 码恒定（零区分
度），l=64 召回崩至 ~0.55。踩的正是 RaBitQ 用旋转和非对称双双堵住的洞。

**lite → 正版升级路径**（M5.5 导航码评估项：RaBitQ 正版 vs PQ32）：
1. Hadamard 结构化旋转（O(D·logD)，建索引/查询侧各一次）；
2. est 改非对称位平面 popcount（query 4-8 bit）；
3. 校正标量 μ → α = ⟨x̄,o⟩；
4. DiskANN 场景配残差化（对 medoid/簇心）。

四步后误差从"无界经验"变"~1.9% 可证"，可能达到 beam 导航所需排序质量；
相比 PQ32 免训练管线（贴合本库零训练哲学），代价是 4×+ 的 RAM
（128+8 vs 32 B/向量 @1024d）——两者由召回 harness 对比出数后定。

## 附：参考文献

- Gao, Long, *RaBitQ: Quantizing High-Dimensional Vectors with a
  Theoretical Error Bound for Approximate Nearest Neighbor Search*,
  SIGMOD 2024。
- Gao, Long, *Extended RaBitQ: Practical High-Accuracy Quantization for
  Approximate Nearest Neighbor Search*, 2025（B-bit 推广）。
- Jégou, Douze, Schmid, *Product Quantization for Nearest Neighbor
  Search*, IEEE TPAMI 2011（对照系）。
- Elasticsearch BBQ（Better Binary Quantization）——RaBitQ 衍生的工业
  实现（结构化旋转 + 非对称位平面）。
