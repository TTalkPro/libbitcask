# 交集内核内幕：旋转法原理与输出段微优化

本文档为 **MIXED 理论 + 实现** 文档，聚焦 `src/bm25/intersect.cpp`
中 `intersect_u64` 各内核的内部机制。每节显式标注「§理论」或「§实现」
以区分算法谱系（«Schlegel 2011» / «Lemire 2016» / «Inoue 2015»）
与具体代码落地。前置阅读：`doc/inoue-simd-intersection-zh.md`（块过滤
设计与全 dispatch 表）。

本文三部分：①旋转法（«Schlegel 2011» / «Lemire 2016»）的深度原理介绍；
②预分配 + 裸指针游标输出段微优化——**已落地**，`intersect_u64` 入口一次
`resize` 上界 + 各内核裸指针 `cur` 写出 + 末尾截断；③压缩段「条件分支
vs PairLut」实测选型——AVX-512 已落地 `compressstoreu`，AVX2 仍维持
4 条件分支形式（待 §3.3 三档基准定夺）。

## §理论 1. 旋转法（«Schlegel 2011» / «Lemire 2016»）深度介绍

### 1.1 要解决的问题：标量归并的分支预测失败

两个升序块各 B 个元素求交。标量归并：

```cpp
if (a[i] < b[j]) ++i; else if (b[j] < a[i]) ++j; else { 输出; ++i; ++j; }
```

瓶颈不是比较（1 周期），而是**三路分支无法预测**——元素交错顺序由
数据决定，本质随机。«Schlegel 2011» 在 Xeon 上测得：随机数据下约
每 2 个元素错预测一次，每次 ~15-20 周期，真实成本 **~8-10 周期/元素**，
其中 ~90% 是 mispredict 罚款。所有 SIMD 交集方法的共同出发点：
**用固定指令序列换掉数据相关分支**。

### 1.2 核心想法：旋转实现全对全比较

不知道两块元素如何对位，就把 B×B 所有配对全比一遍——换来零分支、
固定指令数、完美流水线。做法：va 固定，vb **循环旋转** B-1 次，
每轮做 lane 对 lane 相等比较。B=4：

```
        lane:    0      1      2      3
rot 0:  a0:b0  a1:b1  a2:b2  a3:b3   ← 主对角线
rot 1:  a0:b1  a1:b2  a2:b3  a3:b0
rot 2:  a0:b2  a1:b3  a2:b0  a3:b1
rot 3:  a0:b3  a1:b0  a2:b1  a3:b2
```

4 轮旋转把 16 个配对按**对角线**切成 4 份，每对恰比较一次。
4 轮掩码 OR 起来，lane i = 「a[i] 是否出现在 b 块中」。

对应代码（`bitcask::bm25::exact_match_u64_avx2`，定义于
`src/bm25/intersect.cpp`）：

```cpp
const __m256i cmp01 = _mm256_or_si256(
    _mm256_cmpeq_epi64(va, vb),                             // rot 0
    _mm256_cmpeq_epi64(va, _mm256_permute4x64_epi64(vb, 0x39)));  // rot 1
const __m256i cmp23 = _mm256_or_si256(
    _mm256_cmpeq_epi64(va, _mm256_permute4x64_epi64(vb, 0x4E)),  // rot 2
    _mm256_cmpeq_epi64(va, _mm256_permute4x64_epi64(vb, 0x93))); // rot 3
const __m256i cmp = _mm256_or_si256(cmp01, cmp23);
const unsigned mask = static_cast<unsigned>(
    _mm256_movemask_pd(_mm256_castsi256_pd(cmp)));          // 4-bit 命中掩码
```

要点：

- **旋转模式是编译期常量** → 用 `permute4x64` 立即数形式
  (`0x39` / `0x4E` / `0x93` 即 `_MM_SHUFFLE` 编码的三种循环移位)，
  不需从内存加载索引向量——u64 内核比旧 paired-u32 模拟干净的原因。
- **`cmp01` 与 `cmp23` 两条链无数据依赖** → 乱序核并行发射，
  比较延迟互相隐藏。固定指令序列的第二重收益：无 mispredict 之外
  还有高 ILP。
- **正确性依赖「升序 + 无重复」**：每个 a[i] 在 b 块至多命中一次，
  OR 累积不混淆。前提由 `PostingList` 不变量保证。

### 1.3 块间推进（实现）

代码位置：`bitcask::bm25::intersect_inoue_avx2` 内嵌块过滤+块推进。
比较两块最大值：

```cpp
// 阶段 2 委托精确匹配内核后
const std::uint64_t amax = a[i + B - 1];
const std::uint64_t bmax = b[j + B - 1];
if (amax <= bmax) i += B;    // 相等时两边都推进
if (bmax <= amax) j += B;
```

**正确性证明（不丢匹配）**：若仅 a 推进（`amax < bmax`），a 块所有
元素 ≤ amax，小于 b 块剩余未匹配区间，丢弃的 a 元素不可能再匹配。
相等时双推进同理。这两个分支数据相关，但每 B 个元素付一次，被摊薄。

### 1.4 压缩输出

mask 标出命中 lane，还要把命中元素**紧凑**写出。这是旋转法第三段，
各实现差异最大处：

| 宽度 | mask 取值数 | 经典做法 |
|---|---|---|
| u32 × 8 lane（AVX2） | 256 | 256 项 LUT（8KB）+ `permutevar8x32` + store |
| u64 × 4 lane（AVX2） | 16 | 16 项 PairLut（512B）+ paired `permutevar8x32`；或当前代码的 4 × 条件 `*cur++` |
| AVX-512（u64 × 8 lane） | — | `vpcompressq` 一条指令，LUT 消失（`_mm512_mask_compressstoreu_epi64`） |

当前实现两段选型：

- **AVX2（u64 × 4 lane）**：4 个条件 `if (mask & k) *cur++`——
  简单、可预测 mask 时极快，随机 mask 时 ~70 周期/块成为主要开销。
- **AVX-512（u64 × 8 lane）**：`_mm512_mask_compressstoreu_epi64`——
  一条指令按 mask 只写 popcount 个元素，零 LUT、零早退分支。

压缩段选型见 §3。

### 1.5 成本账与谱系

**u64 AVX2 每块（4 元素）**：2 load + 3 permute + 4 cmpeq + 3 or +
1 movemask ≈ 13 条 SIMD 指令覆盖 16 配对、推进 ≥4 元素，
约 3 条指令/元素，全程无数据相关分支（压缩段除外）。对比标量
~8-10 周期/元素。**旋转法不是比较得更快，而是从不猜错。**

谱系：

- «Schlegel 2011»：用 SSE4.2 `pcmpestrm` 做 u16 全对全；
- «Lemire 2016»：推广到 u32 shuffle + galloping 混合；
- 当前实现：AVX2 `permute4x64_epi64` + AVX-512 `permutexvar_epi64` +
  `vpcompressq`（原始 13 条 + `vpcompressq` 一条）。

**局限**：全对全是「块内」无分支，但**每块照付全套 13 条指令，
哪怕两块毫无交集**。低重叠率下大部分 SIMD 工作花在空块上——
这正是 Inoue 块过滤（`intersect_inoue_avx2` 内两次标量区间比较）
要在它前面挡掉的开销。互补关系：**Inoue 管「这块要不要算」，
旋转法管「要算的块怎么算到最快」**。

## §实现 2. 微优化①：预分配 min(na, nb) 上界 + 裸指针游标（已落地）

本节描述的优化**已实现**：`intersect_u64` 入口 `out.resize(min(na, nb))`
一次给足，各内核经裸指针 `cur` 写出（标量 / galloping / AVX2 /
AVX-512 `compressstoreu`），末尾 `out.resize(cur - base)` 截断。
下文「push_back 成本」分析为落地前的动机记录，保留作历史背景。

### 2.1 push_back 的真实成本（落地前动机）

改造前四处逐元素 `push_back`：标量归并、galloping、AVX2 内核、AVX-512
每重叠块一次 `resize`。每次 `push_back` 编译后：

```
载入 size、capacity → 比较 → 分支（满？）
  ├─ 不满：写元素、size+1 写回内存
  └─ 满：扩容（分配+搬运+释放）
```

三个隐藏成本：

1. 每元素一次「容量检查 + 分支」，即使从不扩容也付；
2. size 字段内存读改写形成跨迭代 store-load 依赖链，限制乱序吞吐；
3. 编译器无法把写出位置保持在寄存器（`out` 成员可能被任何调用改）。

### 2.2 改法（已落地）

交集结果天然上界 `min(na, nb)`：

```cpp
out.resize(std::min(na, nb));          // 一次给足
std::uint64_t* const base = out.data();
std::uint64_t* cur = base;
... *cur++ = v;  或  store + cur += popcount(mask);
out.resize(cur - base);                // 一次截断
```

写出变成「一条 store + 寄存器自增」。一次性 resize 的零填充是
一趟顺序写，远比每元素一次分支便宜。

**受益最大是 AVX-512 路径**：改造前每重叠块 `out.resize(old + cnt)`
（函数调用 + 分支只为腾位置），改造后整段变成：

```cpp
_mm512_mask_compressstoreu_epi64(cur, cmp, va);
cur += std::popcount(static_cast<unsigned>(cmp));   // mask==0 早退分支也可删
```

每块固定两条指令，完全无分支。标量/galloping 省 per-element 检查，
SIMD 省 per-block resize——**对所有路径纯收益**。

### 2.3 实现细节

当前内核写出精确元素数——AVX2 用 4 个条件 `*cur++`、AVX-512 用
`compressstoreu`（按 mask 只写 popcount 个），均**不会越写**，故落地
代码 `out.resize(min(na, nb))` **无 kSlack 余量**。

如下游 §3 的「无条件 SIMD store」PairLut 变体落地，无条件 store 会
写满整个向量宽度（哪怕只 2 个命中），预分配须多留 `kSlack = B` 余量；
多写部分被后续写出覆盖或最终截断丢弃。

## §实现 3. 微优化②：压缩段「4×条件分支 vs PairLut 无分支」实测

### 3.1 当前实现的分支形态依赖

`exact_match_u64_avx2` 每重叠块 4 个数据相关分支：

- **mask 几乎恒 0**（稀疏交集）或**恒满**（自交）：预测近乎全对，
  成本 ≈ 0，此写法很快；
- **mask 随机**（中等密度，每 lane ~50% 命中）：每分支 ~50% 错预测
  × ~15-20 周期，最坏 ~70 周期/块——旋转比较部分才 ~10 周期。
  **压缩段成为内核主要开销**，且破坏了旋转法的块内无分支设计。

### 3.2 PairLut 无分支替代

AVX2 无 64-bit lane 变量置换 → 把 4 个 u64 lane 视作 8 个 u32 lane，
用 `permutevar8x32` 以成对索引搬移。16 项表（4-bit mask 全取值），
每项 8 个 u32 索引：

```cpp
// 例: mask = 0b0101（lane0、lane2 命中）
// 表项 = {0,1, 4,5, _,_, _,_}
const __m256i perm = _mm256_load_si256(&kPairLut[mask]);
const __m256i packed = _mm256_permutevar8x32_epi32(va, perm);
_mm256_storeu_si256(cur, packed);       // 无条件写 32B
cur += std::popcount(mask);
```

固定 3 条指令 + 1 次 L1 表加载（整表 512B 常驻 L1），零分支，
对 mask 形态不敏感。

### 3.3 为什么实测而非直接换

两实现各有必胜区：分支版赢「可预测 mask」（省表加载 + permute），
无分支版赢「随机 mask」。基准须覆盖三点：

1. 每 lane 50% 命中率（分支版最坏情况，对抗构造）；
2. 稀疏交集（mask ≈ 0，分支版最好情况）；
3. 全命中 / 自交（mask 恒满）。

综合赢者上位——把 `inoue-simd-intersection-zh.md` §2.2 的「零 LUT 更优」
从断言变成有数据的结论。

### 3.4 依赖关系

无分支版需「无条件写 32B + 游标前进」，**前提是 §2 的预分配缓冲
就位**（push_back 接口下做不了无条件 store）。实施顺序：①先 ②后。

## §理论 4. 必须 / 应该 / 必须不 三类子查询的 intersect 调度

`bool_search` 入口（`src/bm25/inverted.cpp`）将查询解析为三类 term：
`must_terms` / `should_terms` / `must_not_terms`。交集内核只参与
候选集构造阶段（评分前的 ords 集合），具体调用形态如下。

### 4.1 must 子查询的 intersect 分派

代码位置：`run_must_intersect` lambda（`src/bm25/inverted.cpp`）。
按 must 词数 `mk` 分三档：

| `mk` | 调用形态 | 入口函数 | 备注 |
|---|---|---|---|
| `1` | live 过滤直拷 | （lambda 内联循环） | 单列表；只需 live 标志过滤 |
| `2` | **SIMD pairwise** | `intersect_u64`（本文档主路径） | 实测两热词形态 leapfrog 比 SIMD 慢 ~10-13%（`BoolMustHot` 4096：44.3 → 50.3μs），两次 live 过滤拷贝的代价小于 SIMD 对标量的优势 |
| `≥ 3` | k-way leapfrog | （lambda 内联 leapfrog） | 收益来自消除 k-1 轮物化 + 多列表互相 gallop；详见 `doc/kway-blockmax-bmw-zh.md` |

**`mk == 2` 路径**：

```cpp
if (mk == 2) {
    std::vector<std::uint64_t> a;
    std::vector<std::uint64_t> b;
    auto fill = [&](const TermPostings& tp,
                    std::vector<std::uint64_t>& dst) {
        dst.reserve(tp.fp.size());
        for (std::size_t i = 0; i < tp.fp.size(); ++i) {
            if (tp.live[i]) dst.push_back(tp.fp.ords[i]);
        }
    };
    fill(must_tps[must_order[0]], a);
    fill(must_tps[must_order[1]], b);
    intersect_u64(a, b, acc);
    return acc;
}
```

**`mk ≥ 3` leapfrog 接口**：每个 must 词维护一个 `Cur{ords, live, n, i}`，
驱动游标（最短列表）依序推进，其他游标 `advance(target)`（指数探查
+ 二分收尾）。**此 `advance(target)` 形态就是后续块级元数据 / BMW 的
游标接口**（详见 `doc/kway-blockmax-bmw-zh.md`）。

**结果语义**：ord ∈ 结果 ⟺ 出现在全部 MUST 列表且各列表 live 标志
全真。pairwise 与 leapfrog 等价，区别只在中间物化与游标接口。

### 4.2 must_order：最短列表优先

`run_must_intersect` 上方先按 posting 数升序排 `must_order`——最短
list 先进交集，accumulator 尽早缩小；交集一旦为空提前退出。交集与
处理顺序无关，结果集语义不变（`must_tps` 本体不重排，评分仍按
原始 must 词顺序使用）。

### 4.3 should / must_not 的调度

should / must_not 不参与交集调度，而是用其他方式处理：

- **should 词**（`src/bm25/inverted.cpp` 的 `else if (!should_tps.empty())` 分支）：
  无 must 词时取所有 should 词 ords 的并集，再 `sort + unique`。
  有 must 词时 should 词**只参与打分**（`score_bm25` 循环内按 IDF +
  tf 累加），不扩大候选集——否则「只含 should、不含 must」的文档会
  错误进入结果（违反 MUST 语义）。
- **must_not 词**：所有 must_not 词的 live ords 并集 + `sort + unique`
  → `must_not_ords`；候选集构造后，对每个 ord 在 `must_not_ords` 上
  `binary_search` 排除（`must_not` 路径在 §4.4 末段）。

must_not 词路径**不调用 `intersect_u64`**——单次 `binary_search`
O(log |must_not_ords|)，常数小，远比准备一个 SIMD 内核的中间物化便宜。

### 4.4 must_not 排除（标量二分）

候选集构造完成后（`candidates`）做一次 must_not 排除：

```cpp
std::vector<std::uint64_t> filtered;
filtered.reserve(candidates.size());
for (auto ord : candidates) {
    if (!std::binary_search(must_not_ords.begin(), must_not_ords.end(), ord)) {
        filtered.push_back(ord);
    }
}
candidates = std::move(filtered);
```

注意：`must_not_ords` 已 `sort + unique`（前面 `fill_live` 后处理），
故 `binary_search` 前提成立。`must_not` 词特意不填 `dls`（`with_dls=false`）——
其池槽陈旧 dls 在 must_not 路径永不被读。

## §实现 5. small-list galloping vs large-list SIMD 切换阈值

代码位置：`bitcask::bm25::intersect_u64` 入口。两条 galloping 分支：

```cpp
if (a.size() * 32 < b.size()) {
    cur = intersect_galloping(a.data(), a.size(), b.data(), b.size(), cur);
    ...
    return;
}
if (b.size() * 32 < a.size()) {
    cur = intersect_galloping(b.data(), b.size(), a.data(), a.size(), cur);
    ...
    return;
}
```

**阈值 32 的来源**：与 «Schlegel 2011» 的 "if |A|/32 < |B| use SIMD on A"
经验法则一致——`SIMD 宽度 ≈ 32 元素步距`（AVX2 上 8 元素 × 每轮 4 轮
旋转 OR = 32 元素）；当小列表长度 ≤ 大列表长度 / 32 时，每轮 SIMD
处理 32 元素已经超过小列表总长，SIMD 一次性"扫过多"小列表，浪费；
galloping 以小列表为驱动每次精准定位大列表才是 O(|小|·log|大|) 亚线性。

`a.size() * 32 < b.size()` 与 `b.size() * 32 < a.size()` **对称检测**
确保无论调用者把大列表放在 `a` 还是 `b`，都触发 galloping 分支
（无对称保证时，调用者顺序偏置会导致一半的大列表在前位置的情况
不触发 galloping）。

galloping 实现细节（`bitcask::bm25::intersect_galloping`）：

```cpp
for (std::size_t i = 0; i < ns && lo < nl; ++i) {
    const std::uint64_t v = s[i];
    std::size_t step = 1;
    std::size_t hi = lo;
    while (hi < nl && l[hi] < v) {       // 指数探查
        lo = hi + 1;
        hi += step;
        step <<= 1;
    }
    if (hi >= nl) hi = nl - 1;
    if (l[hi] < v) break;
    const auto* it = std::lower_bound(l + lo, l + hi + 1, v);  // 二分收尾
    lo = static_cast<std::size_t>(it - l);
    if (lo < nl && l[lo] == v) {
        *cur++ = v;
        ++lo;
    }
}
```

要点：

- 小列表 `s` 顺序遍历，每个 `v` 用指数步长 `step` 探查大列表
  `l[hi] < v`——找到上界；
- 上界定位后 `std::lower_bound` 在 `[lo, hi]` 收尾；
- 命中即 `*cur++ = v` 并把 `lo` 推到命中后一位；下个 `v` 接着 `lo` 起步。
- 复杂度 O(|s| · log(|l|/|s|)) ≈ O(|小| · log|大|)，亚线性。

SIMD 路径上不区分大小（`intersect_inoue_avx2` / `intersect_inoue_avx512`
对任意大小都工作），仅由 32x 阈值让位给 galloping。

## §实现 6. 总结：三段无分支化的全景

| 内核段 | 无分支手段 | 状态 |
|---|---|---|
| 块内比较 | 旋转法全对全（§1） | 已落地 |
| 压缩 | PairLut 或 vpcompress（§3） | AVX-512 已落地（`compressstoreu`）；AVX2 待实测选型（仍 4 条件分支） |
| 写出 | 预分配 + 裸指针游标（§2） | **已落地** |

三件做完，整条内核从头到尾才真正没有数据相关分支——这是当前
u64 内核对标量 `set_intersection` 实现 ~3-4x、对 u32 paired 旋转法
实现 1.2-1.5x 的来源。

## §理论 7. 参考文献

- «Schlegel 2011»：Benjamin Schlegel, Thomas Willhalm, Wolfgang
  Lehner. "Fast Sorted-Set Intersection using SIMD Instructions".
  ADMS 2011.
- «Lemire 2016»：Daniel Lemire, Leonid Boytsov, Nathan Kurz.
  "SIMD Compression and the Intersection of Sorted Integers".
  Software: Practice & Experience 46(6), 2016.
- «Inoue 2015»：Hiroshi Inoue, Moriyoshi Ohara, Kenjiro Taura.
  "Faster Set Intersection with SIMD Instructions by Reducing
  Branch Mispredictions". VLDB 2015.

## §实现 8. 相关代码位置速查

| 符号 | 位置 | 作用 |
|---|---|---|
| `bitcask::bm25::intersect_u64` | `src/bm25/intersect.cpp`，公共入口 | 四路 dispatch：galloping / AVX-512 / AVX2 / 标量 |
| `bitcask::bm25::intersect_galloping` | 匿名命名空间 | 指数探查 + `lower_bound` 二分收尾，`O(|小|·log|大|)` |
| `bitcask::bm25::intersect_scalar` | 匿名命名空间 | 标量双指针归并保底 |
| `bitcask::bm25::exact_match_u64_avx2` | 匿名命名空间，`__attribute__((target("avx2")))` | 4-lane 全对全 + 4 条件 `*cur++` |
| `bitcask::bm25::intersect_inoue_avx2` | 匿名命名空间，`__attribute__((target("avx2")))` | Inoue 块过滤 (`B=4`) + `exact_match_u64_avx2` |
| `bitcask::bm25::exact_match_u64_avx512` | 匿名命名空间，`__attribute__((target("avx512f")))` | 8-lane 全对全 + `_mm512_mask_compressstoreu_epi64` |
| `bitcask::bm25::intersect_inoue_avx512` | 匿名命名空间，`__attribute__((target("avx512f")))` | Inoue 块过滤 (`B=8`) + `exact_match_u64_avx512` |
| `kRot512[8][8]` | 匿名命名空间，`alignas(64) static constexpr std::uint64_t` | AVX-512 旋转索引表，覆盖 `rot=0..7` 八种循环移位 |
| `run_must_intersect` lambda | `src/bm25/inverted.cpp` | must 子查询交集入口；`mk==1` / `mk==2` 走 SIMD pairwise / `mk≥3` 走 k-way leapfrog |
| `bool_search` must_not 排除 | `src/bm25/inverted.cpp` 末段 `std::binary_search` | 候选集构造后 `must_not_ords` 上 O(log) 排除 |