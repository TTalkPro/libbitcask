# V4 预研:int8 量化 + AVX-VNNI 距离内核

记录 V4 把距离内核从 f32/AVX2 切到 int8/VNNI 的动机、收益账与本机硬件
约束。背景:V3.9 距离内核见 [hnsw-design-zh.md](hnsw-design-zh.md) §2.3
(f32 内积/L2, AVX2 FMA, 每迭代 8 元素);术语约定沿用
[hnsw-int8-only-design-zh.md](hnsw-int8-only-design-zh.md);指令层细节见
[simd-vnni-internals-zh.md](simd-vnni-internals-zh.md)。

文件标题里的「V4」指 HNSW 引擎自身的版本号(参见 `src/vector/hnsw.cpp`
中 `V4.2:int8 粗筛` 类注释),不是 `int8_kernels.hpp` 里某个内核的版本
号。后者无版本标注,只是按 CPU 特性分了三档入口。

## 1. `vpdpbusd` 指令语义

`vpdpbusd`(VEX/EVEX 编码的 *Packed Dot Product of Bytes, Unsigned×Signed
to Doubleword*)的核心操作:在 32-bit 槽位内做四组乘加,第一操作数 u8,
第二操作数 s8,累加到 i32。

```
对每个 32-bit lane:
  acc[lane] += a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]   (a 为 u8, b 为 s8)
```

中间乘积按有符号自动扩宽, int8×int8 不会溢出。两条本引擎实际用到的
intrinsic:

- `_mm256_dpbusd_avx_epi32`(VEX 256-bit, 见 `dot_vnni`)
- `_mm512_dpbusd_epi32`(EVEX 512-bit, 见 `dot_vnni512` 与 `l2_vnni512`)

一个 256-bit ymm 装 32 个 int8, `vpdpbusd` 一次完成 32 次乘加; 512-bit
zmm 装 64 个 int8, 一次完成 64 次乘加。 VNNI 之前要 `vpmaddubsw` +
`vpmaddwd` + `vpaddd` 三条指令凑出同样效果, VNNI 把它压成一条, μops
大幅减少。

对比 V3.9 内核:f32 + AVX2 FMA 一个 ymm 装 8 个 float, 每迭代 8 维;
int8 + AVX-VNNI 每迭代 32 维, 单指令吞吐 4×。 2560 维内积从 320 次
迭代降到 80 次。

## 2. 偏置修正:Σ(a+128)·b = Σa·b + 128·Σb

量化码字是有符号 int8(`[-127, 127]`),但 `vpdpbusd` 第一操作数是 u8。
处理方法是给 query 一边 XOR `0x80`(等价于 `+128`),运行时再用预算好的
`Σ db[i]` 反向补偿:

```
query_u8 = query_s8 XOR 0x80    // [-127,127] → [1,255] 安全,避开 -128 回卷
db_s8    = db_s8                // 第二操作数保持有符号

raw   = Σ query_u8[i] * db_s8[i]
      = Σ (query[i] + 128) * db[i]
      = Σ query[i]*db[i] + 128 * Σ db[i]

Σ query[i]*db[i] = raw - 128 * sum_db
```

`sum_db` 在量化时一次算好, 存入 `QVector::sum_codes`(详见
`int8_kernels.hpp` 中 `quantize()` 与 `QVector` 结构)。 运行时每条距离
计算只需 1 次 int32 减法与 1 次 f32 标量乘。

为什么 `-128` 不在码字范围? `quantize()` 把 `v[i]/scale*127` clamp 到
`[-127, 127]`, `int8_t(-128)` 的二进制是 `0x80`, XOR 后回到 `0`, 与
正值 `0` 无法区分; clamp 一档后 XOR 永远是非零, 与「正零」无歧义。

SIMD 主循环累加的是 biased 形式 `Σ(q+128)·b`, 尾部标量循环必须匹配。
`dot_vnni512` 与 `dot_vnni` 都显式加上 `128 * tail_sum_b`, 否则
`-128*sum_db` 修正只在头段生效, 尾段会出现系统性偏置。

## 3. AVX-VNNI vs AVX-512 VNNI

两者都是同一指令 `vpdpbusd` 的不同编码宽度, bitcask 用三条入口覆盖两
档 CPU, 见 `int8_kernels.hpp`:

| 入口 | intrinsic | 每 iter 元素 | 编码 | 注解 |
|------|-----------|--------------|------|------|
| `dot_vnni` | `_mm256_dpbusd_avx_epi32` | 32 | VEX | `target("avxvnni")` |
| `dot_vnni512` | `_mm512_dpbusd_epi32` | 64 | EVEX | `target("avx512vnni")` |
| `l2_vnni512` | `_mm512_dpbusd_epi32` | 64 | EVEX | `target("avx512vnni")` |

`_mm256_dpbusd_epi32`(无 `_avx` 后缀)是 EVEX 编码, 在本工具链上即便做
256-bit 操作也隐含要求 AVX-512-VNNI; 本内核刻意避开, 只声明
`target("avxvnni")` 即可合法, 确保无 AVX-512 的 12/13 代机器也能跑。

Raptor Lake 等 12/13 代 CPU 的 P-core 支持 AVX-VNNI, 但因 E-core 没有
AVX-512, 整个 512-bit 指令家族被熔断, V4 在这些机器上稳定走
`_mm256_dpbusd_avx_epi32`, 单指令 32 个 int8。 Ice Lake / Sapphire
Rapids 之后 P-core 同时有 AVX-512-VNNI, 选 `_mm512_dpbusd_epi32`, 单
指令 64 个 int8。

AMD Zen 4 没有原生 512-bit ALU, 512-bit VNNI 由双 256-bit 流水线实现,
理论吞吐与两条 AVX-VNNI 相近; Zen 5 起有原生 512-bit 数据通路,
512-bit VNNI 单周期吞吐 64 个 int8。本引擎的运行时分发
(`pick_int8_dot_kernel`)把这层差异全部吞掉, 业务代码无需判断。

## 4. 量化方案要点

`int8_kernels.hpp` 中的 `quantize()` 实现 per-vector 对称量化:

- `scale = max |v[i]|`(标量扫一遍, O(dim));
- `codes[i] = round(v[i] / scale * 127)` 后 clamp 到 `[-127, 127]`;
- 同时预算 `sum_codes = Σ codes[i]` 与 `sq_norm_codes = Σ codes[i]²`,
  之后每次距离调用只需 1 次 SIMD pass + O(1) 标量拼装。

注意 `round` 用 `std::round`(半离零舍入), 而非 SIMD 的
`_mm256_cvtps_epi32`(半向偶舍入), 两者在 `.5` 边界结果不同; 代码注释
里特意点出「int8 量化码字要进 checkpoint, 必须保持位级不变」。

量化本身有损(per-vector 1 个 scale 共享, 召回率小幅下降)。 通用解法是
int8 粗筛 + 对 top 候选用原始 f32 重排(rerank), 精度基本无损。 本引
擎在 `HnswIndex::dist_id_int8`(`include/bitcask/hnsw.hpp`)里先走 int8
粗筛, f32 精排由 `dist_` 路径在 `node_vec` 取出原始向量后做。

`inmem_int8` 模式没有常驻 f32, 选边/收缩时改用
`HnswIndex::dist_id_int8_node`(同 int8 距离, 但无 query), 即「两端都是
量化副本」的同质距离; 这是 V4 与 V3 在选边阶段的唯一差异。

## 5. 吞吐理论(FMA / cycle)

`vpdpbusd` 在主流 CPU 上每个 cycle 完成的乘加数:

- AVX-VNNI(256-bit):8 个 32-bit lane, 每个 lane 4 个乘加, 共 32 乘
  加 / cycle;
- AVX-512 VNNI(512-bit):16 个 32-bit lane, 每个 lane 4 个乘加, 共 64
  乘加 / cycle;
- AVX2 FMA(256-bit f32):8 lane × 1 FMA = 8 乘加 / cycle。

按 384 维向量、查询 vs 单个候选估算(不计访存):

- 纯标量:384 次乘加;
- AVX2 FMA:384 / 8 = 48 cycle;
- AVX-VNNI:384 / 32 = 12 cycle;
- AVX-512 VNNI:384 / 64 = 6 cycle。

实际吞吐受三件事制约: 访存带宽、预取命中率、分支代价。 HNSW 距离循环
的访存多为随机, 2560 维时 int8 数据是 f32 的 1/4, 带宽压力同时降 4×,
算力与带宽两个瓶颈被同时收紧, 这就是「int8 + VNNI」收益呈乘性的根本
原因。

具体吞吐数字以本机 `bench/hnsw_bench` 实测为准, 本文不引用未经验证
的绝对值。