# SIMD 指令内幕:VNNI dpbusd 与偏置补偿

记录 bitcask 量化距离内核用到的 SIMD 指令的底层运算逻辑。
背景见 [int8-vnni-v4-zh.md](int8-vnni-v4-zh.md);内核实现集中于
`include/bitcask/detail/int8_kernels.hpp`,HNSW 调用点见
`src/vector/hnsw.cpp` 与 `include/bitcask/hnsw.hpp`。术语约定沿用
[hnsw-int8-only-design-zh.md](hnsw-int8-only-design-zh.md)。

## 1. 内核总览

`bitcask::vec::int8` 命名空间暴露 5 个标量入口与 3 个 x86-64 VNNI 入口,
调度入口 `pick_int8_dot_kernel()` 在运行时按 CPU 特性挑一个,绑定给
`HnswIndex::int8_dot_`(见 `include/bitcask/hnsw.hpp`)。

| 入口 | 类型 | 输入 | 输出 | 备注 |
|------|------|------|------|------|
| `quantize_into` / `quantize` | 标量 | f32[dim] | `QVector` | 预算 `sum_codes` 与 `sq_norm_codes` |
| `dot_scalar` | 标量 | `QVector` × `QVector` | f32 | 整数精确内积 |
| `dot_scalar_raw` | 标量 | int8[a] × int8[b] + 两 scale | f32 | 无 VNNI 的回退,也是 self_test 对照 |
| `l2_scalar` | 标量 | `QVector` × `QVector` | f32 | 直接走码字差平方 |
| `dequantize` | 标量 | `QVector` | f32[dim] | 测试辅助 |
| `dot_vnni512` | AVX-512 VNNI | int8[a] × int8[b] + 两 scale + sum_db | f32 | EVEX 512-bit |
| `dot_vnni` | AVX-VNNI | int8[a] × int8[b] + 两 scale + sum_db | f32 | VEX 256-bit(非 EVEX 版) |
| `l2_vnni512` | AVX-512 VNNI | int8[a] × int8[b] + sum_db | i32 codes | 暴露码空间 dot,外层拼 L2;当前生产未挂入分发 |

`dot_scalar_raw` 与 `dot_vnni512` / `dot_vnni` 共享同一个函数指针签名
`Int8DotFn`,所以 `int8_dot_` 可以在三者之间无缝切换。

## 2. 编译期 CPU 特性启用

内核没有用 `__AVX2__` / `__AVX512VNNI__` 这类预处理器宏,而是走 GCC/Clang
的多版本机制:函数用 `__attribute__((target(...)))` 注解,只在运行时被分
发器选中时才进入 ABI,未选中的实现不会污染编译产物的特性集合。

`int8_kernels.hpp` 里三个 VNNI 函数的注解:

- `dot_vnni512` 与 `l2_vnni512`:`__attribute__((target("avx512vnni")))`,
  EVEX 512-bit 版本,CPU 实际还需 `avx512f` 才合法;
- `dot_vnni`:`__attribute__((target("avxvnni")))`, VEX 256-bit 版本,
  在 12/13 代 Raptor Lake 这类砍掉 AVX-512 的机器上仍可用。

外壳 `#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))`
只在 GCC/Clang x86-64 上包含 `<immintrin.h>` 与三个 VNNI 内核。其他平台
直接拿到 nullptr,`int8_dot_` 走 `dot_scalar_raw` 标量回退。

构建系统刻意不传 `-march=native`(见任务书相关说明),通用二进制由运行
时分发兜底。

## 3. 运行时分发

`pick_int8_dot_kernel()` 用 `__builtin_cpu_supports` 探测,并以
`static const Int8DotFn kFn = []() -> Int8DotFn { ... }()` 缓存到静态
存储,首次调用之后零开销:

```
avx512vnni 可用 → dot_vnni512    (64 int8 / iter)
avxvnni     可用 → dot_vnni      (32 int8 / iter)
否则            → nullptr
```

`HnswIndex` 构造里把返回值赋给 `int8_dot_`。`inmem_int8` 模式下若
`int8_dot_ == nullptr` 会立刻换成 `&int8::dot_scalar_raw`,保证 int8-only
路径在无 VNNI 机器上仍能建图与查询(见 `src/vector/hnsw.cpp` 中
`HnswIndex` 构造函数里的 `if (cfg_.inmem_int8 && int8_dot_ == nullptr)`
分支)。

读者路径里 `dist_id_int8` / `dist_id_int8_node`(定义在
`include/bitcask/hnsw.hpp`)只是一层薄壳,把 `qcodes_of` / `qscale_of` /
`qsum_of` 取出来喂给 `int8_dot_`,取负号后作为「越小越近」距离返回。

## 4. `vpdpbusd` 指令语义

`vpdpbusd`(VEX/EVEX 编码的 *Packed Dot Product of Bytes, Unsigned×Signed
to Doubleword*)对每个 32-bit 槽位做四组乘加:

```
对每个 32-bit lane:
  acc[lane] += a_u8[0]*b_s8[0] + a_u8[1]*b_s8[1]
             + a_u8[2]*b_s8[2] + a_u8[3]*b_s8[3]
```

第一操作数 u8,第二操作数 s8,累加到 i32,中间乘积按有符号自动扩宽,
int8×int8 不会溢出。两个版本对应两条 intrinsic:

- 256-bit VEX:`_mm256_dpbusd_avx_epi32`(由 `dot_vnni` 调用)
- 512-bit EVEX:`_mm512_dpbusd_epi32`(由 `dot_vnni512` 调用)

注意 `_mm256_dpbusd_epi32`(无 `_avx` 后缀)是 EVEX 编码,在本工具链上
即便做 256-bit 操作也隐含要求 AVX-512-VNNI,与带 `_avx` 后缀版本语义
不同。本内核刻意避开它,确保只声明 `target("avxvnni")` 即可合法。

## 5. 偏置补偿技巧

量化码字是有符号 int8,范围 `[-127, 127]`(`quantize()` 内 clamp 上限避
开 `-128`,避免 XOR `0x80` 后回卷成 `0`)。但 `vpdpbusd` 要求第一个操作数
是 u8,处理方法:

```
query_codes XOR 0x80  → u8, [-127,127] 映到 [1,255] 安全
db_codes 保持 s8
```

数学等价性(`Σ(a+128)·b = Σa·b + 128·Σb`):

```
raw = Σ query_u8[i] * db_s8[i]
    = Σ (query[i] + 128) * db[i]
    = Σ query[i]*db[i] + 128 * Σ db[i]
```

所以 `Σ query[i]*db[i] = raw - 128 * Σ db[i]`,后者 `Σ db[i]` 在 `quantize()`
时算好存入 `QVector::sum_codes`(或量化副本的 `qsums`),运行时一次减法
即可消除偏置。

SIMD 主循环累加的是 biased 形式 `Σ(q[i]+128)·b[i]`;尾部标量循环必须
匹配这个不变量,否则 `-128*sum_db` 修正对不上。`dot_vnni512` 与
`dot_vnni` 的尾部都写成 `tail_dot + 128 * tail_sum_b`,显式加上
`128 * tail_sum_b` 是为了保持与 SIMD 头段同一偏置,避免尾段悄悄逃过
修正(详见 `int8_kernels.hpp` 两个内核的尾部代码)。

## 6. AVX-512 VNNI 的水平归约

`dot_vnni512` 用一条 `_mm512_reduce_add_epi32` 把 16 个 i32 通道归约到
标量;`dot_vnni` 不能用这条(AVX/AVX2 上不存在 `_mm256_reduce_add_epi32`),
只能手工做 8-lane 横向加:把 `__m256i` 拆成两个 `__m128i` 段,各自
`_mm_add_epi32` 后用 `_mm_srli_si128` 做 8 字节与 4 字节移位,得到 1 个
i32 后 `_mm_cvtsi128_si32` 取出来。手工归约代码见 `int8_kernels.hpp`
中 `dot_vnni` 的水平归约段。

## 7. 标量回退

无 x86-64 或无 VNNI 时 `pick_int8_dot_kernel()` 返回 nullptr,`HnswIndex`
走两条标量路径之一:

- 普通 `f32+int8` 模式:搜索仍以 f32 为准,int8 路径不被读,`int8_dot_`
  保持 nullptr(构造函数分支);
- `inmem_int8` 模式:`int8_dot_` 被强制设成 `&int8::dot_scalar_raw`。
  该函数签名与 `Int8DotFn` 一致(`sum_db` 参数忽略),在标量循环里直接
  做 `int8 × int8 → i64` 累加,再乘 `(scale_a * scale_b) / 127²` 得到
  重建内积。

`dot_scalar` 与 `dot_scalar_raw` 都用 `int64_t` 累加,避免 384+ 维下
`int32` 中间溢出。

## 8. self_test 的一致性护栏

`int8::self_test()` 在 384 维随机单位向量上做三项比对:

- 标量 int8 vs f32:容差 5%(int8 对称量化的合理范围);
- VNNI vs 标量 int8:容差 1e-5(float ULP);
- VNNI vs f32:同样 5%。

种子 `0xC0FFEE` 保证失败可复现。生产构建一般调用一次,失败时让 HNSW
改走纯 f32。

## 9. 调用方回顾

- `HnswIndex::dist_id_int8`(粗筛到候选):单 query 对多候选,每次 1 次
  `int8_dot_`;
- `HnswIndex::dist_id_int8_node`(选边/收缩):两节点间,无 query 参与;
- `HnswIndex::quantize_into`(`thread_local QVector`):粗筛前一次性预算,
  复用稳态容量零分配。

预取策略保留:`greedy_closest_int8` 与 `search_layer_int8` 在 `hnsw.cpp`
内对下一个候选的码字段按 64 字节递进 `_mm_prefetch(..., _MM_HINT_T0)`,
与 VNNI 主循环的 stride 对齐。

## 10. 实现细节速查

`dot_vnni512` 主体(`int8_kernels.hpp`):

```cpp
const __m512i sign_flip = _mm512_set1_epi8(static_cast<std::int8_t>(-128));
__m512i acc = _mm512_setzero_si512();
for (; i + 64 <= dim; i += 64) {
    const __m512i va = _mm512_loadu_si512(query_codes + i);
    const __m512i vb = _mm512_loadu_si512(db_codes     + i);
    const __m512i va_u8 = _mm512_xor_si512(va, sign_flip);
    acc = _mm512_dpbusd_epi32(acc, va_u8, vb);
}
const std::int32_t raw = _mm512_reduce_add_epi32(acc);
```

`dot_vnni` 主体用 `_mm256_set1_epi8` / `_mm256_loadu_si256` /
`_mm256_xor_si256` / `_mm256_dpbusd_avx_epi32`,水平归约走第 6 节描述的
手工 8-lane 路径。两条主循环都包在 `__attribute__((target(...)))` 里,
与运行时分发器一一对应。

性能数字(每条查询耗时、相对 f32 加速比)以 `bench/hnsw_bench` 实测为准,
本文不引用未经代码验证的吞吐数据。