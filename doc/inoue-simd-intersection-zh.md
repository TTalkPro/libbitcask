# Inoue 块过滤 + SIMD 精确匹配（交集内核设计方案）

本文档为 **MIXED 理论 + 实现** 文档，每节显式标注「§理论」或「§实现」以区分
算法理论与具体代码落地。理论段以 «Inoue 2015» / «Schlegel 2011» / «Lemire 2016»
论文为依据；实现段对照 `src/bm25/intersect.cpp` 与 `include/bitcask/intersect.hpp`
的当前代码。

**当前实现状态（2026-07 更新）**：ord 类型恒为 `uint64_t`，`intersect_u64` 是
唯一生产路径。自 `08fbc92` 起，`intersect_u32` 内核与 `narrow_ok` 门整体从
代码中移除；本文保留历史叙述作为决策记录，正文涉及 u32 路径处均加注
「已移除」。AVX-512 VP2INTERSECT 路线在 Zen 5 上方为优选，但当前代码未启用
（采用 8 轮 `permutexvar_epi64` 旋转 + `vpcompress` 的传统方案）。

## §理论 1. 设计动机

### 1.1 旋转法的局限（«Schlegel 2011» / «Lemire 2016» 现状）

当前 `intersect_u64` 的 AVX2 路径使用旋转法实现 4-lane 全对全比较（详见
`doc/intersect-kernel-internals-zh.md` §1）：

- 每个块固定执行 3 次 `_mm256_permute4x64_epi64` 旋转 + 4 次 `_mm256_cmpeq_epi64`
  + 3 次 OR + 4 次条件 `*cur++` 提取；
- **不重叠的块也照跑完全套 SIMD**——浪费在非匹配区域上的指令不可忽略；
- u64 扩展历史上依赖「成对 u32 索引模拟 64 位 lane 移动」（PairLut 512B +
  `permutevar8x32` 配对索引），代码复杂且难推理；当前实现改用原生
  `permute4x64` 立即数 + 4 个条件提取（详见 `intersect-kernel-internals-zh.md`
  §1.4 与 §3）。

### 1.2 Inoue 方法的核心思想

Inoue 等人在 «Inoue 2015» 中观察到：排序数组交集的瓶颈不是比较本身，
而是 `if (a[i] < b[j])` 三路分支的**预测失败**。论文给出实验证据——
Xeon 与 POWER7+ 上，`std::set_intersection` 大部分 CPU 时间花在
mispredict 罚款上而非实际比较。

解法：将交集分为两个阶段——

1. **块过滤**（O(1)/块）：用两个标量比较判断两个块是否可能重叠；
   不重叠的块直接跳过，零元素比较。
2. **精确匹配**（仅重叠块）：在确认重叠的块内做交集，可用旋转法、
   广播法、标量归并等任意策略。

对排序数组，块 max/min 就是首尾元素，**不需要 SIMD 归约**：

```cpp
if (a[i + BLOCK - 1] < b[j]) { i += BLOCK; continue; }  // a 整块 < b
if (b[j + BLOCK - 1] < a[i]) { j += BLOCK; continue; }  // b 整块 < a
// 否则：块重叠 → 精确匹配
```

**阶段 1 与元素大小（u32/u64）和 ISA（AVX2/AVX-512/NEON）完全无关**
——这是 Inoue 的核心收益（论文 §3.2）：块过滤用 O(1) 标量比较把
数据相关的整块跳过判断**前移**，把 SIMD 内核的输入严格限制到「值域
区间重叠」的块上，避开对「必不命中」块的浪费。

### 1.3 组合优势（理论对比表）

将 Inoue 块过滤与 SIMD 精确匹配组合后：

| 问题 | 旋转法 | Inoue + SIMD |
|---|---|---|
| 非重叠块 | 照跑 SIMD（浪费） | **跳过，0 条 SIMD 指令** |
| u64 旋转 | `permutevar8x32` + paired u32（模拟） | **`permute4x64_epi64` + 立即数（原生 u64）** |
| u64 压缩 | PairLut 16 项 + `permutevar8x32`（模拟） | **`_mm256_extract_epi64` 条件提取（零 LUT）** |
| u64 AVX-512 | 未实现 | **`compressstoreu` 一条指令（零 LUT）** |
| 不对称查询 | galloping 处理 | 块过滤 + 精确匹配，更少分支预测失败 |

## §实现 2. 架构设计

### 2.1 总体流程

```
输入：两个升序无重复的数组 a[], b[]
    │
    ├─ 悬殊形态（>32x）→ galloping（与 ISA 无关）
    │
    └─ 相近大小 → Inoue + SIMD 交集
         │
         ├─ 阶段 1：块过滤（标量 O(1)/块，与元素大小无关）
         │     a_max < b_min → 跳过 a 块
         │     b_max < a_min → 跳过 b 块
         │     否则 → 进入阶段 2
         │
         ├─ 阶段 2：精确匹配（按 ISA 分派，u64 单路径）
         │     u64 + AVX-512→ 8-lane 原生 permutexvar + compressstoreu
         │     u64 + AVX2   → 4-lane 原生 permute4x64 + 条件提取
         │     无 SIMD      → 标量双指针归并
         │
         └─ 尾部（不足一块）→ 标量归并
```

### 2.2 u64 AVX2 精确匹配内核

代码位置：`bitcask::bm25::exact_match_u64_avx2`（匿名命名空间，
定义于 `src/bm25/intersect.cpp`）：

```cpp
__attribute__((target("avx2")))
std::uint64_t* exact_match_u64_avx2(const std::uint64_t* a,
                                    const std::uint64_t* b,
                                    std::uint64_t* cur) {
    const __m256i va =
        _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(a));
    const __m256i vb =
        _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(b));

    // 三种循环移位的立即数形式（_MM_SHUFFLE 编码）：
    //   0x39 = (0,3,2,1)  → rot 1: lanes 1,2,3,0
    //   0x4E = (1,0,3,2)  → rot 2: lanes 2,3,0,1
    //   0x93 = (2,1,0,3)  → rot 3: lanes 3,0,1,2
    const __m256i cmp01 = _mm256_or_si256(
        _mm256_cmpeq_epi64(va, vb),
        _mm256_cmpeq_epi64(va,
                           _mm256_permute4x64_epi64(vb, 0x39)));
    const __m256i cmp23 = _mm256_or_si256(
        _mm256_cmpeq_epi64(
            va, _mm256_permute4x64_epi64(vb, 0x4E)),
        _mm256_cmpeq_epi64(
            va, _mm256_permute4x64_epi64(vb, 0x93)));
    const __m256i cmp = _mm256_or_si256(cmp01, cmp23);

    const unsigned mask = static_cast<unsigned>(
        _mm256_movemask_pd(_mm256_castsi256_pd(cmp)));

    if (mask & 1u) *cur++ = a[0];
    if (mask & 2u) *cur++ = a[1];
    if (mask & 4u) *cur++ = a[2];
    if (mask & 8u) *cur++ = a[3];
    return cur;
}
```

### 2.3 u64 AVX-512 精确匹配内核

代码位置：`bitcask::bm25::exact_match_u64_avx512`（匿名命名空间）。
AVX-512 路径额外需要一份 `kRot512` 旋转索引表（`alignas(64)`，8×8 个
`uint64_t`），每行是一组 `permutexvar_epi64` 的 lane 重排索引，
覆盖 `rot=0..7` 八种循环移位。

```cpp
__attribute__((target("avx512f")))
std::uint64_t* exact_match_u64_avx512(const std::uint64_t* a,
                                      const std::uint64_t* b,
                                      std::uint64_t* cur) {
    const __m512i va = _mm512_loadu_si512(a);
    const __m512i vb = _mm512_loadu_si512(b);
    __mmask8 cmp = _mm512_cmpeq_epi64_mask(va, vb);
    for (int r = 1; r < 8; ++r) {
        const __m512i ridx = _mm512_load_si512(kRot512[r]);
        const __m512i vbr = _mm512_permutexvar_epi64(ridx, vb);
        cmp |= _mm512_cmpeq_epi64_mask(va, vbr);
    }
    _mm512_mask_compressstoreu_epi64(cur, cmp, va);
    return cur + std::popcount(static_cast<unsigned>(cmp));
}
```

**AVX-512 优势**：

- 8 lane（AVX2 的 2 倍）；
- `permutexvar_epi64` 原生变量索引 u64 置换——免去 u32 配对索引表；
- `compressstoreu` 直接按 mask 压缩存储——零 LUT、零早退分支。

**AVX512-VP2INTERSECT 对照**：`vp2intersectq` 一条指令即可完成
8-lane u64 全对全相等比较（输出双侧 mask）。Intel 端 Tiger Lake 引入后
被砍且为微码慢速实现，但 **AMD Zen 5（EPYC Turin）提供硬件快速实现**。
若部署目标含 Zen 5，VP2INTERSECT 内核应优先于 8-旋转方案；写
AVX-512 内核前需先确认目标微架构。当前代码维持 8 旋转方案（兼容性优先）。

### 2.4 Inoue 包装器与块过滤阶段

代码位置：`bitcask::bm25::intersect_inoue_avx2` / `intersect_inoue_avx512`
（匿名命名空间）。包装器内嵌块过滤阶段，签名与精确匹配内核一致，
均返回推进后的 `cur` 游标指针：

```cpp
__attribute__((target("avx2")))
std::uint64_t* intersect_inoue_avx2(const std::uint64_t* a, std::size_t na,
                                    const std::uint64_t* b, std::size_t nb,
                                    std::uint64_t* cur) {
    constexpr std::size_t B = 4;
    std::size_t i = 0;
    std::size_t j = 0;

    while (i + B <= na && j + B <= nb) {
        // ── 阶段 1：块过滤（O(1)/块，零 SIMD）─────────────────────
        if (a[i + B - 1] < b[j]) { i += B; continue; }
        if (b[j + B - 1] < a[i]) { j += B; continue; }

        // ── 阶段 2：块重叠 → 精确匹配（委托给 ISA 最优内核）──────
        cur = exact_match_u64_avx2(a + i, b + j, cur);

        // 块推进（数据相关分支，每 B 元素摊薄）
        const std::uint64_t amax = a[i + B - 1];
        const std::uint64_t bmax = b[j + B - 1];
        if (amax <= bmax) i += B;
        if (bmax <= amax) j += B;
    }

    return intersect_scalar(a + i, na - i, b + j, nb - j, cur);
}
```

AVX-512 版本结构同形，仅 `B = 8` 且精确匹配阶段委托给
`exact_match_u64_avx512`。**前置条件**：两输入严格升序、无重复——
由 `PostingList` 不变量保证。

### 2.5 调度分发

代码位置：`bitcask::bm25::intersect_u64`（`src/bm25/intersect.cpp` 的
公共入口）。入口处先 `out.resize(min(na, nb))` 一次给足容量，
再分发到具体内核，末尾按 `cur - base` 截断：

```cpp
void intersect_u64(std::span<const std::uint64_t> a,
                   std::span<const std::uint64_t> b,
                   std::vector<std::uint64_t>& out) {
    out.clear();
    if (a.empty() || b.empty()) return;

    const std::size_t bound = std::min(a.size(), b.size());
    out.resize(bound);
    std::uint64_t* const base = out.data();
    std::uint64_t* cur = base;

    // 大小悬殊 → galloping（与 ISA 无关；对称检测避免顺序偏置）
    if (a.size() * 32 < b.size()) {
        cur = intersect_galloping(a.data(), a.size(), b.data(), b.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
    if (b.size() * 32 < a.size()) {
        cur = intersect_galloping(b.data(), b.size(), a.data(), a.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }

#ifdef BITCASK_INTERSECT_SIMD
    static const bool kHasAvx512f = __builtin_cpu_supports("avx512f");
    if (kHasAvx512f) {
        cur = intersect_inoue_avx512(a.data(), a.size(),
                                     b.data(), b.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
    static const bool kHasAvx2 = __builtin_cpu_supports("avx2");
    if (kHasAvx2) {
        cur = intersect_inoue_avx2(a.data(), a.size(),
                                   b.data(), b.size(), cur);
        out.resize(static_cast<std::size_t>(cur - base));
        return;
    }
#endif
    cur = intersect_scalar(a.data(), a.size(), b.data(), b.size(), cur);
    out.resize(static_cast<std::size_t>(cur - base));
}
```

四条 dispatch 路径与触发条件：

| 触发条件 | 调用内核 | 复杂度 | 备注 |
|---|---|---|---|
| `|a| × 32 < |b|` 或对称 | `intersect_galloping` | `O(|小|·log|大|)` | 指数探查 + `lower_bound`，与 ISA 无关 |
| `__builtin_cpu_supports("avx512f")` | `intersect_inoue_avx512` | `B=8`，块过滤 + 8-lane 全对全 | 消费级 12 代后无 AVX-512，部署目标需 Xeon/EPYC |
| `__builtin_cpu_supports("avx2")` | `intersect_inoue_avx2` | `B=4`，块过滤 + 4-lane 全对全 | 当前主流服务器与桌面基线 |
| 无 SIMD | `intersect_scalar` | `O(|a|+|b|)` | 标量双指针归并保底 |

**AVX-512 触发细节**：`kHasAvx512f` 与 `kHasAvx2` 均为 `static const bool`
一次性探测，避免每次调用重复触发 `__builtin_cpu_supports` 的开销。
运行时通过 `__attribute__((target(...)))` 多版本（multi-versioning）支持，
`-march` 基线维持 `x86-64/SSE2`，新增后端只需新增一个内核 + 一个分发分支。

### 2.6 u32 路径（已移除）

自 `08fbc92` 起，`intersect_u32` 内核与 `narrow_ok` 门整体从代码中移除——
ord 类型语义上就是 64 位单调序号，查询路径不做 u32 收窄。
本节保留作为决策背景，详见 §9。

## §理论 3. 性能分析

### 3.1 各场景预测

下表以当前实测数据为基准（i9-13900H），`BLOCK` = SIMD lane 数。

| 场景 | 当前旋转法 | Inoue + SIMD | 说明 |
|---|---|---|---|
| 两热词 100K×100K（100% 重叠） | 4370μs | ~4400μs | 几乎无块可跳，退化为纯 SIMD 精确匹配，持平 |
| 两热词 100K×100K（10% 重叠） | ~4370μs | ~3500μs | 90% 块被阶段 1 跳过，SIMD 只处理 10% 重叠块 |
| 10K 冷词 × 100K 热词 | galloping | 相当或略优 | 两者都利用大小不对称 |
| u64 交集（任何形态） | 标量 `set_intersection` | **~3-4x** | 原生 u64 SIMD + 块过滤 |
| u64 交集（不对称） | 标量 | **>5x** | 块过滤跳过大部分块 + SIMD 精确匹配 |

**关键前提**：「10% 重叠 → 90% 块被跳过」假设交集元素**在值域上成簇**。
块过滤跳过的条件是两个块的 `[min, max]` **区间不相交**，与元素重叠率
无关。若 doc ID 均匀散布全值域（真实 posting list 的常态），相近大小的
两列表即使 0% 元素重叠，块区间也几乎必然互相覆盖——阶段 1 一个块都
跳不掉，每块反而多付 2 次标量比较。该行预测必须用均匀分布 + 成簇分布
两种数据形态实测后才可作为决策依据（详见 §8.1 与 §8.2）。

### 3.2 旋转法 vs Inoue + SIMD 的指令开销对比

**以 u64 AVX2 为例，单块处理**：

| 操作 | 旋转法（旧 paired u32 原型） | Inoue + SIMD |
|---|---|---|
| 非重叠块 | 3×load ridx + 3×permutevar8x32 + 4×cmpeq + 3×or + 1×movemask + LUT lookup + permutevar8x32 + storeu | **2 条标量比较 → continue** |
| 重叠块 | 同上 | 3×permute4x64 + 4×cmpeq + 3×or + 1×movemask + 4×条件 store = 同阶，但指令更少且全是原生 u64 |

### 3.3 为什么块过滤对 u64 特别有效

u64 只有 4 lane（u32 的一半），SIMD 加速比本就有限。Inoue 的阶段 1 把
大量非重叠块用 O(1) 标量比较排除，只有少量重叠块进入 SIMD 精确匹配。
**即使精确匹配退化为标量归并，总工作量仍远小于全量 SIMD 旋转法**
——这是 u64 上「Inoue + SIMD」相对「纯 SIMD 旋转」的纯收益来源。

## §实现 4. 与现有代码的集成

### 4.1 改动范围

| 文件 | 改动 |
|---|---|
| `include/bitcask/intersect.hpp` | 声明 `intersect_u64()`（`std::span` + `std::vector` 接口） |
| `src/bm25/intersect.cpp` | `intersect_scalar` / `intersect_galloping` / `exact_match_u64_avx2` / `intersect_inoue_avx2` / `exact_match_u64_avx512` / `intersect_inoue_avx512` + `intersect_u64` 四路分发 |
| `src/bm25/inverted.cpp` | `run_must_intersect` lambda 的 `mk==2` 分支改调 `intersect_u64`；`mk≥3` 走 k-way leapfrog（见 §4.4） |

**不需要改的文件**：

- `inverted.hpp`（`PostingList` / `FlatPostings` 不变）
- `index.hpp` / `index.cpp`（ord 分配不变）
- merger（ord 保留不变）
- search_layer（查询入口不变）

### 4.2 测试策略

1. **对拍测试**：复刻 `IntersectU64` 系列——随机 × 12×12 尺寸组合 × 3 档
   重叠密度，全部对照 `std::set_intersection`。
2. **对抗性测试**：
   - 跨 2³² 边界的 u64 值域（验证原生 64 位比较非低 32 位巧合相等）；
   - 「低 32 位相同、高 32 位不同」模式（验证 `_mm256_cmpeq_epi64` 是
     真 64 位比较，不退化为 u32 比较）；
   - 自交全命中（覆盖「双进」块推进路径）。
3. **BoolSearchMustU64Fallback**：覆盖 `ord > 2³²` 强制走 u64 回退路径，
   改调 `intersect_u64` 后仍应通过。
4. **Sanitizer**：ASan + UBSan（所有 SIMD 内核必须配合 sanitizer 实测
   兜底，防范越界写）。
5. **基准对比**：与旧旋转法在 `BoolMustHot` / 4096 与 /100k 上对齐测量。
6. **分布形态矩阵**：基准必须覆盖 {均匀散布, 成簇, 区段错开} ×
   {0%, 10%, 100% 重叠} × {对称, 4x, 32x}。其中「均匀散布 + 相近大小」
   是真实 posting list 的常态，也是块过滤预期收益为零甚至为负的形态——
   该格子的结果决定 **u64 内核是否保留块过滤阶段**。只测成簇形态会
   系统性高估收益。
7. **块跳过率计数器**：基准内核加编译期开关统计「阶段 1 跳过块数 /
   总块数」，把"块过滤是否生效"从推测变成可观测指标。

### 4.3 触发条件

**u64 路径**（唯一生产路径，自 `08fbc92`）：`intersect_u64` 对所有
查询生效。原触发条件「累计写入超 2³² 使 u64 从影子路径变热路径」随
u32 收窄移除而失效；u32 路径替换为「不存在替换」问题，分布矩阵实验
降级为 u64 内核调优（块过滤阶段保留与否）。

**AVX-512 内核**：

- 触发条件：部署目标明确为带 AVX-512 的服务器（Xeon/EPYC）。
- 前置：① AVX-512 验证环境；② 确认目标微架构是否含
  AVX512-VP2INTERSECT——若含 Zen 5，VP2INTERSECT 路线应优先于
  8-旋转方案（详见 §2.3）。

### 4.4 与 k-way leapfrog 的分工

`run_must_intersect`（`src/bm25/inverted.cpp` 的 lambda）按
must 词数 `mk` 分派：

- `mk == 1`：live 过滤直拷（与旧实现首词分支等价）；
- `mk == 2`：**SIMD pairwise**——走 `intersect_u64`（本设计文档主路径）；
- `mk ≥ 3`：k-way leapfrog（详见 `doc/kway-blockmax-bmw-zh.md`）——
  实测两热词形态 leapfrog 比 SIMD 慢 ~10-13%，故 `mk == 2` 仍走 SIMD。

SIMD pairwise 与 leapfrog **结果语义等价**：ord ∈ 结果 ⟺ 出现在
全部 MUST 列表且各列表 live 标志全真。两者只在中间物化与游标接口
上不同，leapfrog 的 `advance(target)` 形态可作为后续块级元数据 /
BMW 的游标接口。

## §理论 5. Roaring 混合方案分析（历史参考，u32 路径已移除）

本节保留作历史背景；`08fbc92` 后 u32 收窄路径已整体移除，本节
关于「u32 Roaring + Inoue u64 回退」的 if/else 分发不再存在。

### 5.1 设计概述

Roaring bitmap 按密度自适应选择存储方式（Lucene 5+ / Elasticsearch
标准）：

```
每 65536 元素一个 chunk：
  ≤ 4096 个元素 → ArrayContainer（排序 u16[] 数组，2B/元素）
  > 4096 个元素 → BitmapContainer（65536-bit bitset，8KB 固定）
  连续值        → RunContainer（(start, length) 对，RLE 编码）

交集按类型分派：
  Array ∩ Array   → galloping / merge
  Array ∩ Bitmap  → 遍历 array 查 bitmap（O(|array|)）
  Bitmap ∩ Bitmap → 位运算 AND（一条 SIMD 指令 / 256 bit）
```

### 5.2 u64 支持分析

CRoaring 提供两个 API：

| API | key 宽度 | 内部结构 | 交集性能 |
|---|---|---|---|
| `roaring_bitmap_t` | **u32** | 65536 × chunk（每 chunk 65536 值） | 基准 |
| `roaring64_bitmap_t` | **u64** | ART（高 48 bit 自适应基数树）+ 65536 × chunk（低 16 bit） | 约为 u32 版的 **50%** |

Roaring64 的 ART 遍历 + 间接寻址比 u32 Roaring 的直接数组索引慢，
故引入 Roaring 时**优先 u32 容器**。

### 5.3 结论（已并入正文）

如引入 Roaring，组合为：u32 Roaring 做 AND + Inoue 原生 u64 SIMD
回退（不用 Roaring64，ART 开销使其不如原生 u64 SIMD）。**当前不引入
Roaring**——理由见 §8.4 的「u32 收窄移除」决策与§8.1 第 3 行的带宽
论证。

### 5.4 Roaring 引入的额外问题（理论代价）

即使只引入 u32 Roaring，也有以下工程成本：

| 问题 | 说明 |
|---|---|
| **tf/positions 丢失** | Roaring bitmap 只存 doc ID，不存 tf。需要混合存储：Roaring 存 ords + 并行数组存 tfs/positions。`PostingList` 内部表示需要完全重写 |
| **snapshot_flat 重写** | 当前拷贝 `vector<u64> ords + vector<u32> tfs`。Roaring 版需要拷贝 Roaring 容器或共享引用 |
| **save/load 格式变更** | 当前 v6：ord 用 FOR 块压缩 + tfs/dls VByte varint。需改成 Roaring 序列化格式（或维护两套） |
| **外部依赖** | CRoaring 是 header-only 或静态库，需引入构建系统 |
| **6 个模块受影响** | `PostingList`、`FlatPostings`、`intersect_u32`、`snapshot_flat`、`save/load`、`compact` 全要改 |

### 5.5 触发条件（理论）

**引入 u32 Roaring 替代排序数组**：

- 触发条件：存在高频 filter 查询（如 `status:active AND category:tech`），
  posting list 密度稳定超过 6.25%（4096/65536），且 Roaring AND 的加速
  可测量。
- 前置：`PostingList` 内部表示从 `vector<Posting>` 改为 Roaring + tfs[]
  并行数组。

## §理论 6. 与其他方案的对比

| 维度 | Schlegel/Lemire 旋转法（旧） | Inoue + SIMD（§2） | Galloping Only | Roaring u32 + Inoue u64（§5） |
|---|---|---|---|---|
| 代码复杂度 | 中（8KB LUT + 旋转 + 压缩） | **中低**（块过滤 + 精确匹配按需分派） | **低**（删 SIMD） | 高（PostList 重写 + CRoaring） |
| u64 支持 | paired u32 模拟 | **原生 u64 操作** | 标量 | Inoue 原生 u64（不用 Roaring64） |
| 非重叠块 | 全量 SIMD（浪费） | **跳过（O(1)）** | galloping 跳过 | Roaring AND / Inoue 跳过 |
| 对称热词 | ★★★★★（全量 SIMD） | ★★★★（退化为 SIMD） | ★★★（纯标量 -22%） | ★★★★ |
| 不对称查询 | ★★★（galloping） | ★★★★★（块过滤 + SIMD） | ★★★★（galloping） | ★★★★★ |
| 高频 filter 查询 | ★★★ | ★★★★ | ★★★ | ★★★★★（Bitmap AND 极快） |
| 跨平台 | x86-only | **通用骨架 + ISA 按需分派** | ★★★★★（纯标量） | 依赖 CRoaring |
| 依赖 | 无 | 无 | 无 | CRoaring |
| 改动范围 | 已实现 | 1-2 文件 | 删代码 | 6 文件（Roaring）+ 1-2 文件（Inoue u64） |

## §理论 7. 术语与参考

- **块过滤 / Block filtering**：Inoue 方法的阶段 1，用 O(1) 比较判断
  两个块是否可能重叠。对排序数组只需 `a_max < b_min` /
  `b_max < a_min` 两个判断。
- **精确匹配 / Exact match**：Inoue 方法的阶段 2，在确认重叠的块内
  找出实际交集元素。可用旋转法、广播法、标量归并等任意策略。
- **`permute4x64_epi64`（`vpermq`）**：AVX2 原生 u64 lane 置换指令，
  使用**立即数**控制 shuffle pattern。区别于 `permutevar8x32_epi32`
  （`vpermd`）的**变量索引** u32 lane 置换。
- **`permutexvar_epi64`（`vpermq` 变量索引）**：AVX-512 变量索引 u64
  lane 置换，配合预计算的 `kRot512` 旋转索引表实现 8-lane 全对全。
- **`vpcompressd` / `vpcompressq`（`compressstoreu`）**：AVX-512 按
  mask 压缩存储——零 LUT、零早退分支、只写 popcount 个元素。
- **Roaring bitmap**：密度自适应混合位图。按 65536 元素分 chunk，
  ≤4096 元素用排序数组，>4096 用 bitmap，连续值用 RLE。
- **Roaring64（`roaring64_bitmap_t`）**：CRoaring 的 u64 扩展。高 48 bit
  用 ART（自适应基数树），低 16 bit 用标准 Roaring chunk。性能约为
  u32 版的 50%。
- **AVX512-VP2INTERSECT**：Intel ISA 扩展（`vp2intersectd` / `vp2intersectq`），
  Tiger Lake 引入后被砍且为微码慢速实现；**AMD Zen 5（EPYC Turin）
  提供硬件快速实现**。

## §理论 8. 设计评审

2026-06 评审补充。本节记录该设计相对工业主流方案的定位偏差与
分析性缺陷。结论先行：**本设计在「flat 数组交集内核」局部题目内
是干净的，但有局部最优嫌疑**——核心收益假设（块过滤跳过率）对
真实 doc ID 分布大概率不成立，且若 V2 做 BM25 top-k，更高优先级的
是 Block-Max 类结构而非交集内核。

### 8.1 与工业界主流的差异

| 维度 | 本设计 | 工业主流（Lucene/ES 系） | 差距影响 |
|---|---|---|---|
| 数据形态 | flat `vector<u64>` 全量驻留，8B/id | 128-doc 块压缩（FOR/PFor）+ skip 结构，~1-2 bit/id 有效 | 100K posting = 800KB，出 L2 后内核是**带宽瓶颈**，§3 的指令数对比可能不兑现 |
| 跳跃粒度 | 块过滤一次跳 4-16 元素；>32x 走 galloping | skip list / 块元数据一次跳 128~数千 | 4x~32x 中等不对称区间两边线性推进，正是 skip 结构最赚的区间 |
| 评分集成 | 完整 must 交集 → 再评分 | WAND / Block-Max WAND / MaxScore：分数上界跳文档，top-k 出来时大部分 posting 未被触碰 | 交集内核优化的环节会被 BMW 整体绕开；纯 filter 场景工业答案是 Roaring AND（§5 已分析） |
| doc ID 宽度 | ord 恒 u64，单一 u64 路径（决策见 §9） | segment 内 u32 局部 ID（Lucene 段上限 2³¹），分段消解 u64 需求 | 有意识的取舍：接受 2x 内存带宽 / 半数 SIMD lane，换单路径简单性；带宽代价的正解是将来块压缩，不是 u32 收窄 |

另一处工程差异（评审时状态）：`run_must_intersect` 当时是 pairwise 物化
交集——k 个 must 词产生 k-1 次中间 vector 分配 + move。工业实现用
k-way leapfrog 或迭代器 advance 链零物化。短交集上此开销可能盖过内核
优化。（**此后 K1 已落地 k-way leapfrog**，见
`doc/kway-blockmax-bmw-zh.md` §2 与 §4.4。）

### 8.2 已知缺陷

#### 8.2.1 块过滤的收益假设与真实分布不符（最严重）

块过滤跳过的条件是块 `[min, max]` **区间不相交**，§3.1 的预测表把
「元素重叠率」当成了「块区间不相交率」。两者只在交集元素值域成簇时
近似：

```
成簇形态（块过滤有效）：
  a: [1..1000]            b: [900..1900]
  → a 的前 ~90% 块整块 < b[0]，阶段 1 直接跳过

均匀散布形态（块过滤失效，真实 posting 常态）：
  a: 1, 3, 5, 7, ...      b: 2, 4, 6, 8, ...
  → 0% 元素重叠，但每对块区间都互相覆盖
  → 阶段 1 一个块都跳不掉，每块多付 2 次标量比较 + 2 个分支
```

对相近大小、均匀分布的列表，块过滤阶段退化为纯开销。
**影响已降级（u32 收窄移除后）**：u64 内核的对照基线是标量
`set_intersection`，即使块跳过率为 0，SIMD 精确匹配部分仍稳赚——
本缺陷不再威胁路线成立性，只影响调优。处置：§4.2 第 6/7 条
（分布矩阵 + 跳过率计数器）决定 u64 内核**保留还是删除块过滤阶段**。

#### 8.2.2 引用的是 Inoue，实现的不是 Inoue

Inoue 2015 的核心贡献是**无分支 SIMD 低字节指纹过滤**消除
`if (a[i] < b[j])` 的预测失败；本文档 §2.4 的 max/min 标量块过滤
实质是经典 block-skipping merge，且重新引入了两个数据相关分支
（块很少被跳过时恒为 false、预测良好；跳过/不跳交替时会 mispredict）。
引用论文与实现物不一致——保留现名可以，但不应预期获得论文中
报告的 branch-miss 消除收益。

#### 8.2.3 u64 内核「零 LUT」的代价是 4 个内层分支

见 §2.2 的 4 个 `if (mask & k) *cur++`。`if` + capacity 检查 vs
512B PairLut（常驻 L1）无分支 lookup + permute + store——后者在 mask
难预测时可能更快。另外所有内核直接 `*cur++` 写入由 §4.1 入口处
`out.resize(bound)` 预分配的缓冲，不存在 `push_back` / `resize`
容量检查成本。两项的收益分析、实测方案与依赖顺序详见
`doc/intersect-kernel-internals-zh.md` §2 / §3。

#### 8.2.4 AVX-512 方案未对照 VP2INTERSECT

见 §2.3。Zen 5 的 `vp2intersectq` 一条指令即完成 8-lane u64 全对全
比较，8-旋转循环在该微架构上是错误选型。AVX-512 内核动工前必须
先确认部署微架构并对照评估。

#### 8.2.5 带宽瓶颈未纳入性能模型

§3 全部以指令条数论证，未考虑 100K×100K（u64 下 1.6MB 工作集）
已出 L2。带宽受限时减少 SIMD 指令几乎不改变吞吐——这同时削弱
旋转法和 Inoue 的差异，也意味着压缩（缩小工作集）比内核优化
对大 posting 更有效。基准须报告工作集大小与 L2/L3 边界的关系。

### 8.3 评审结论对路线的修正

1. **u64 路径为唯一热路径**（自 `08fbc92`）：`intersect_u64` 即生产
   路径，不再是影子路径，内核投入的 ROI 直接成立；其收益不依赖
   块过滤假设（SIMD 精确匹配相对标量本身就赚）。
2. **若 V2 确做 BM25 top-k**：交集内核之后的优化预算应转向
   Block-Max 元数据（每块 max tf/score）+ MaxScore / BMW，以及
   `run_must_intersect` 的 k-way 化（消除中间物化）。
   详细路线说明见 `doc/kway-blockmax-bmw-zh.md`
   （k-way → 块元数据 → BMW 的依赖链与各自收益）。
3. **AVX-512**：维持 §4.3 触发条件，新增前置——确认目标微架构后
   先评估 VP2INTERSECT 路线。

## §实现 9. 决策记录：u32 路径移除（`08fbc92`）

**决策**：ord 在类型语义上就是 64 位单调序号，查询路径不做 u32 收窄。
`narrow_ok` 门与 `intersect_u32` 内核自 `08fbc92` 起从代码中移除，
`bool_search` 统一走 `intersect_u64`。

**放弃 u32 收窄的理由**：

1. **查询时收窄有自身成本**：u64 → u32 需逐 posting 拷贝
   （读 8B/id + 写 4B/id），大 posting 上这趟带宽吃掉相当部分内核收益；
   若改为存储层原生 u32 则违背 ord=u64 的类型设计，且引入双格式
   （save/load、snapshot、merger 全要感知）。
2. **双路径维护成本**：两套内核族 × 两套对拍/对抗/sanitizer 测试矩阵
   + 收窄门自身的边界条件（恰好跨 2³² 的索引），换来的只是
   热路径上的常数加速。
3. **per-element 指令数两者同阶**：全对全旋转每块 B 次 permute+cmp
   覆盖 B² 对、推进 ≥B 个元素——per-element ALU 成本与 lane 宽度
   基本无关。u32 的真实优势主要是**内存占用/带宽减半**，不是指令吞吐。

**接受的代价**（诚实记录）：

- flat posting 8B/id，是 u32 的 2 倍内存与带宽；与工业压缩格式
  （~1-2 bit/id 有效）差距进一步放大。
- **该代价的正解是将来的块压缩（FOR/PFor + 块级元数据），不是
  u32 收窄**——压缩同时为 BMW/MaxScore 提供 skip 地基（§8.3 第 2 条），
  一份投入解两个问题。当前规模（100K×2 列表 = 1.6MB，在 L3 内）
  带宽尚不构成瓶颈，posting 规模显著增长时再触发压缩路线。

## §理论 10. 参考文献

- «Inoue 2015»：Hiroshi Inoue, Moriyoshi Ohara, Kenjiro Taura.
  "Faster Set Intersection with SIMD Instructions by Reducing
  Branch Mispredictions". PVLDB 8(3): 293–304, 2015. —— 注意其
  过滤阶段为**无分支 SIMD 字节指纹**，非本设计的标量 max/min
  块过滤。
- «Schlegel 2011»：Benjamin Schlegel, Thomas Willhalm, Wolfgang
  Lehner. "Fast Sorted-Set Intersection using SIMD Instructions".
  ADMS 2011. —— 当前 SIMD 精确匹配内核的旋转法谱系。
- «Lemire 2016»：Daniel Lemire, Leonid Boytsov, Nathan Kurz.
  "SIMD Compression and the Intersection of Sorted Integers".
  Software: Practice & Experience 46(6), 2016. —— 批量交集 +
  SIMD 压缩的混合范式。
- «Ding & Suel 2011»：Bin Ding, Torsten Suel. "Faster Top-k
  Document Retrieval Using Block-Max Indexes". SIGIR 2011. ——
  Block-Max WAND。
- «Broder 2003»：Andrei Z. Broder, David Carmel, Michael Herscovici,
  Aya Soffer, Jason Zien. "Efficient Query Evaluation using a
  Two-Level Retrieval Process". CIKM 2003. —— WAND。
- AVX512-VP2INTERSECT：Intel ISA Extensions Reference 319433-037；
  AMD Zen 5（EPYC Turin）提供 `vp2intersectd/q` 硬件快速实现。