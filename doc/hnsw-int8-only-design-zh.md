# P5 — HNSW int8-only 内存模式（实现细节）

> 前置阅读：`hnsw-design-zh.md`（V3 HNSW 基础设计）、`int8-vnni-v4-zh.md`（V4.2 量化检索的原始 VNNI 设计）、`hnsw-memory-footprint-zh.md`（内存账推导）、`hnsw-lifecycle-zh.md`（持久化与生命周期）
> 内核实现：`include/bitcask/detail/int8_kernels.hpp`（量化 + VNNI 内核 + 运行时分发）；调用入口 `src/vector/hnsw.cpp` 内 `HnswIndex::insert`/`HnswIndex::search`/`HnswIndex::clone_live` 等
> 状态：已实现（opt-in）。

## 1. 背景与动机

默认 HNSW 路径下（VNNI 存在 + `kDot`），`NodeChunk` 同时常驻两份向量：

- `vecs`（`float[dim]`）——建图选边 / f32 精排 / serialize；
- `qcodes`/`qscales`/`qsums`（`int8[dim]` + 标量）——VNNI 粗筛提速。

int8 在内存里**为速度**，反而**+25% 内存**。2560d × 1M 向量 ≈ 12.1 GiB 常驻（见 `hnsw-memory-footprint-zh.md` §7.1）——多数部署场景的真正墙。

**P5 = 丢掉常驻 f32，全走 int8**。`vector_inmem_int8 = true` 时 `NodeChunk` 不分配 `vecs`（容量 0），仅存量化副本；建图 + 查询全程 int8，向量存储内存降 ~80%。

## 2. 量化策略：对称 int8 量化

定义于 `bitcask::vec::int8` 命名空间（`int8_kernels.hpp`）：

```cpp
struct QVector {
    std::vector<std::int8_t> codes;     // dim 元素，值域 [-127, 127]
    float                    scale;     // 原向量的 max |v[i]|
    std::int32_t             sum_codes; // Σ codes[i] —— VNNI 偏置补偿
    std::int32_t             sq_norm_codes; // Σ codes[i]² —— L2 fast path
};
```

量化算式（`int8::quantize_into`，`int8_kernels.hpp`）：

```
scale = max |v[i]|        (max_abs == 0 → 1.0；零向量也能定义)
inv_scale = 127 / scale
codes[i] = clamp(round(v[i] * inv_scale), -127, 127)
sum_codes = Σ codes[i]
sq_norm_codes = Σ codes[i]²
```

**反量化**（测试 / `HnswIndex::node_vec` 用于 rebuild 路径，`hnsw.cpp`）：

```
v_hat[i] = codes[i] * scale / 127
```

`quantize_into`（就地量化）与 `quantize`（返回新对象）算法等价；前者接受 `QVector& out`，复用 `out.codes` 容量（调用方常传 `thread_local`，稳态零分配）。量化内 round 走 `std::round`（半离零），**不**换 SIMD round（`_mm256_cvtps_epi32` 是 round-half-to-even，`.5` 边界结果不同，违反位级不变约定 —— codes 会进 checkpoint）。

## 3. 量化副本 → dot 乘的偏置修正

### 3.1 目标算式

VNNI 内核（`vpdpbusd`/`vpdpbusd_avx`）的 intrinsic 签名是 **unsigned × signed（u8 × s8 → i32 accumulate）**。我们的 codes 是 signed int8。直接的"符号相乘→i32 累加"无法直接用上 VNNI 指令，需要把一边翻成 unsigned。

**技巧**：

```
query 侧 codes XOR 0x80   →  unsigned byte (s8 in [-127,127] → u8 in [1,255])
db 侧 codes 保持 signed int8
vpdpbusd：acc[i32] += Σ va_u8[u8] * vb[s8]，每 4 字节 lane
```

`codes != -128`（quantize 已 clamp 到 `[-127, 127]`）→ XOR `0x80` 不溢出 → 安全。

### 3.2 偏置补偿算式

VNNI 输出的 `raw` 实际是**带偏置**的：

```
raw = Σ (query_u8[i] * db_s8[i])
    = Σ (query[i] + 128) * db[i]
    = Σ query[i] * db[i] + 128 · Σ db[i]
```

所以真实内积要从 raw 减去 `128 · sum_db`：

```
Σ query[i] * db[i]  =  raw - 128 · sum_db
```

`sum_db` 在 `quantize_into` 一次性预计算存进 `QVector.sum_codes`（`int8_kernels.hpp` 的 `QVector` 定义）。VNNI 内核入参直接拿 `sum_db`，**每节点不重复算**。

**完整重建内积**：

```
dot_codes = raw - 128 · sum_db
dot       = (scale_q · scale_db / (127·127)) · dot_codes
```

实现见 `dot_vnni512` / `dot_vnni` / `dot_scalar_raw`（`int8_kernels.hpp`）。`l2_vnni512` 同样基于该 dot 走 `||a-b||² = ||a||² + ||b||² - 2·dot(a,b)`。

### 3.3 标量尾处理

VNNI 主循环以 64 字节（512-bit）或 32 字节（256-bit）步进，尾部用标量补齐。**关键**：标量尾也必须满足同样的偏置约定，否则 SIMD 与 scalar 结果无法相加。

```
biased_tail = Σ q[i]*b[i] + 128 * Σ b[i]    (i in tail)
raw_total   = raw + tail_dot + 128 * tail_sum_b
dot_codes   = raw_total - 128 * sum_db
```

实现见 `dot_vnni512` / `dot_vnni` 函数体的标量 tail 循环（`int8_kernels.hpp`）。

## 4. 启用条件与 fallback

`HnswIndex` 构造函数（`hnsw.cpp`）汇总各路径需求：

```cpp
HnswIndex::HnswIndex(const HnswConfig& cfg)
    : cfg_(cfg),
      dist_(pick_kernel(cfg.metric)),
      int8_dot_(int8::pick_int8_dot_kernel()),
      needs_qcodes_(cfg.inmem_int8 ||
                    (int8_dot_ != nullptr && cfg.metric == HnswMetric::kDot)),
      ...
{
    assert(cfg_.dim > 0 && cfg_.M >= 2);
    assert(!(cfg_.inmem_int8 && cfg_.metric != HnswMetric::kDot) &&
           "inmem_int8 requires kDot metric");
    if (cfg_.inmem_int8 && int8_dot_ == nullptr) {
        int8_dot_ = &int8::dot_scalar_raw;        // 标量兜底
    }
}
```

### 4.1 三档路径

| 情形 | `int8_dot_` | `needs_qcodes_` | `inmem_int8` | 路径 |
|---|---|---|---|---|
| 默认（VNNI+kDot） | 非空（VNNI 函数指针） | `true` | `false` | f32 + int8：建图 f32，查询 int8 粗筛 + f32 精排 |
| 无 VNNI+kDot | `nullptr` | `false` | `false` | f32-only：`qcodes/qscales/qsums` 不分配 |
| `kL2` + 无 int8 | `nullptr` | `false` | `false` | f32-only（同上） |
| `kL2` + `inmem_int8=true` | （构造时 assert 拒绝） | n/a | n/a | 上游 `VectorPlugin` 构造拒绝 → `kInvalidOption` |
| **`inmem_int8=true`（无 f32）** | 非空（VNNI 或标量兜底） | `true` | `true` | **int8-only**：建图 + 查询 全程 int8，无 f32 精排 |
| `inmem_int8=true` 且无 VNNI | 标量 `int8::dot_scalar_raw`（构造兜底） | `true` | `true` | int8-only 标量版：无 f32，距离走标量 int8 |

### 4.2 `HnswIndex::search` 的路径选择

`src/vector/hnsw.cpp` 的 `HnswIndex::search`：

```cpp
const bool use_int8 =
    cfg_.inmem_int8 ||
    ((int8_dot_ != nullptr) && (cfg_.metric == HnswMetric::kDot) &&
     (cfg_.dim >= 64));

if (use_int8) {
    thread_local int8::QVector qq;
    int8::quantize_into(q, cfg_.dim, qq);
    for (l = max_level; l > 0; --l) cur = greedy_closest_int8(qq..., cur, l, ...);
    search_layer_int8(qq..., cur, ef, 0, n, scratch, found);

    if (!cfg_.inmem_int8) {
        // 默认 f32+int8 路径：对 top k*3 做 f32 精排，召回对齐纯 f32
        // ...
        found.resize(rerank_n);
    }
    // int8-only 路径：found 已按 int8 距离升序，直接取。
} else {
    // f32 路径
    for (l = max_level; l > 0; --l) cur = greedy_closest(q, cur, l, ...);
    search_layer(q, cur, ef, 0, n, scratch, found);
}
```

**`dim >= 64` 守卫**：小维度 int8 路径收益小（粗筛→精排总开销 > 直接 f32），仅 dim ≥ 64 启用（默认 f32+int8）。int8-only 强制 int8 路径不受此限（用户显式要省内存）。

### 4.3 fallback 到 fp32 的具体条件

- `vector_inmem_int8=false`（默认）+ `metric=kDot` + 有 VNNI → 默认 f32+int8（粗筛 + 精排）。
- `vector_inmem_int8=false` + `metric=kL2` → **强制纯 f32**（int8 路径仅 kDot）。L2 不支持 int8 因 VNNI dpbusd 只算点积，需走 `l2_vnni512`（依 `||a-b||² = ||a||² + ||b||² - 2·dot`，但当前未启用，因 L2 用例少）。
- `vector_inmem_int8=false` + 无 VNNI（`int8_dot_ == nullptr`） → 退 f32-only：`qcodes`/`qscales`/`qsums` 不分配（构造时按 `needs_qcodes_ = false`）。
- `vector_inmem_int8=true` + 有 VNNI → int8-only（VNNI 内核）。
- `vector_inmem_int8=true` + 无 VNNI → int8-only（标量 `dot_scalar_raw`，`int8_kernels.hpp`）。

`vector_inmem_int8=true` + `metric=kL2` 是**非法组合**，由 `HnswIndex` 构造函数 `assert` + `VectorPlugin` 构造时 `kInvalidOption` 拒绝。

## 5. SIMD 内核（`int8_kernels.hpp`）

### 5.1 三档 tier

`int8::pick_int8_dot_kernel`（`int8_kernels.hpp`）按 CPU 特性分发：

| Tier | ISA | 步长 | 内核函数 | 指令 |
|---|---|---|---|---|
| 1 | AVX-512 VNNI | 64 int8 / 迭代 | `dot_vnni512` | `_mm512_dpbusd_epi32`（unsigned × signed → i32 acc） |
| 2 | AVX-VNNI | 32 int8 / 迭代 | `dot_vnni` | `_mm256_dpbusd_avx_epi32`（VEX-encoded 变体；EVEX `_mm256_dpbusd_epi32` 在本工具链上要求 AVX-512 VNNI 才能发射 256-bit 操作，故不用） |
| 3 | 标量兜底 | 1 int8 | `dot_scalar_raw` | 普通 `int32_t` 累加 |

**分发实现**（`int8_kernels.hpp`）：

```cpp
inline Int8DotFn pick_int8_dot_kernel() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    static const Int8DotFn kFn = []() -> Int8DotFn {
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx512vnni")) return &dot_vnni512;
        if (__builtin_cpu_supports("avxvnni"))     return &dot_vnni;
        return nullptr;
    }();
    return kFn;
#else
    return nullptr;
#endif
}
```

### 5.2 内核结构（VNNI 路径共通）

主循环：

```cpp
const __m512i sign_flip = _mm512_set1_epi8(static_cast<std::int8_t>(-128));
__m512i acc = _mm512_setzero_si512();

for (; i + 64 <= dim; i += 64) {
    const __m512i va = _mm512_loadu_si512(query_codes + i);
    const __m512i vb = _mm512_loadu_si512(db_codes     + i);
    const __m512i va_u8 = _mm512_xor_si512(va, sign_flip);    // 0x80 XOR：s8 → u8
    acc = _mm512_dpbusd_epi32(acc, va_u8, vb);                // u8 × s8 → i32 acc
}
```

水平归约（`dot_vnni512`）：

```cpp
const std::int32_t raw = _mm512_reduce_add_epi32(acc);       // 16 × i32 → i32
```

水平归约（`dot_vnni`，AVX-VNNI 无 `_mm256_reduce_add_epi32`）：

```cpp
const __m128i lo = _mm256_castsi256_si128(acc);
const __m128i hi = _mm256_extracti128_si256(acc, 1);
const __m128i sum4 = _mm_add_epi32(lo, hi);                  // 4 × i32
const __m128i sum2 = _mm_add_epi32(sum4, _mm_srli_si128(sum4, 8)); // 2 × i32
const __m128i sum1 = _mm_add_epi32(sum2, _mm_srli_si128(sum2, 4)); // 1 × i32
const std::int32_t raw = _mm_cvtsi128_si32(sum1);
```

标量 tail + 偏置合并（已见 §3.3）。

最终乘以 `(scale_q · scale_db / 127²)` 得 `dot`。

### 5.3 标量兜底 `dot_scalar_raw`

签名匹配 `Int8DotFn`：

```cpp
inline float dot_scalar_raw(const std::int8_t* a, const std::int8_t* b,
                            std::int32_t /*sum_db*/, float scale_a,
                            float scale_b, std::size_t dim) noexcept {
    std::int64_t raw = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        raw += static_cast<std::int32_t>(a[i]) * static_cast<std::int32_t>(b[i]);
    }
    return static_cast<float>(raw) * (scale_a * scale_b) / (127.0f * 127.0f);
}
```

**注意**：`sum_db` 参数被忽略——标量是纯 signed × signed 累加，不带 +128 偏置，所以无需 `128·sum_db` 补偿。P5 int8-only 在无 VNNI 机器上走此路径，保证 int8-only 模式不依赖 VNNI 即可工作。

### 5.4 L2 VNNI 内核

`l2_vnni512`（`int8_kernels.hpp`）复用 dot 的 VNNI 循环，外部做 `||a-b||² = ||a||² + ||b||² - 2·dot(a,b)` 装配。本仓库当前未在生产路径启用（生产仅 kDot + cosine）。

### 5.5 正确性校验

`int8::self_test(dim=384, seed=0xC0FFEE)`（`int8_kernels.hpp`）：构造两个单位归一化随机向量，比较标量 int8 与 VNNI int8 的 dot/L2：

- 标量 vs f32：相对误差 < 5%（int8 symmetric 量化在 unit vector 上的典型误差）。
- VNNI vs 标量：相对误差 < 1e-5（位级一致，乘以相同 scale 常数）。
- L2 同 dot 容忍度。

确定性（固定 seed），进程启动期调用一次即可。

## 6. int8-only 在 HNSW 中的工程适配

### 6.1 `NodeChunk::vecs` 不分配

`HnswIndex::NodeChunk::NodeChunk` 构造函数（`hnsw.cpp`）：

```cpp
HnswIndex::NodeChunk::NodeChunk(std::size_t dim, bool needs_vecs, bool needs_qcodes)
    : vecs(needs_vecs ? static_cast<std::size_t>(kChunkSize) * dim : 0),
      ...
      qcodes(needs_qcodes ? static_cast<std::size_t>(kChunkSize) * dim : 0),
      qscales(needs_qcodes ? kChunkSize : 0, 0.0f),
      qsums(needs_qcodes ? kChunkSize : 0, 0) { ... }
```

`VectorPlugin` 构造时传 `!cfg_.inmem_int8` 给 `needs_vecs`（`vector_plugin.cpp` 内 `VectorPlugin::rebuild` 与 `clone_live` 同步），`inmem_int8=true` → `vecs` 容量 0，**不分配 4·D·kChunkSize 字节**。

### 6.2 `HnswIndex::node_vec` 的反量化兜底

`HnswIndex::node_vec`（`hnsw.cpp`）：

```cpp
std::span<const float> HnswIndex::node_vec(std::uint32_t id) const {
    if (!cfg_.inmem_int8) return {vec_of(id), cfg_.dim};
    thread_local std::vector<float> buf;
    buf.resize(cfg_.dim);
    const std::int8_t* codes = qcodes_of(id);
    const float factor = qscale_of(id) / 127.0f;
    for (std::uint32_t i = 0; i < cfg_.dim; ++i) {
        buf[i] = static_cast<float>(codes[i]) * factor;
    }
    return {buf.data(), cfg_.dim};
}
```

仅 `VectorPlugin::rebuild → HnswIndex::clone_live` 在拷贝活子图时调 `old->node_vec(id)`。**注意**：`buf` 是 thread_local，clone_live 单写者，下次插入会覆盖——本路径纯同步消费，无并发问题。

int8-only 的 `clone_live` **不**走 `node_vec`——它直拷 `qcodes/qscales/qsums`（`hnsw.cpp` 内 `clone_live` 的 pass 1），免去反量化→再量化往返（无损）。

### 6.3 距离路径全面切换

int8-only 下三处距离调用全切到 int8：

| 调用点 | int8-only 版本 |
|---|---|
| 上层贪心下降 | `HnswIndex::greedy_closest_int8`（`hnsw.cpp`） |
| 每层束搜索 | `HnswIndex::search_layer_int8`（`hnsw.cpp`） |
| 邻居选择启发式 | `HnswIndex::select_neighbors_int8`（`hnsw.cpp`），候选-已选比较走 `HnswIndex::dist_id_int8_node` |
| 反向边收缩 | insert 内收缩分支走 `dist_id_int8_node` |

`HnswIndex::dist_id_int8`（查询→节点）与 `HnswIndex::dist_id_int8_node`（节点→节点，int8-only 建图选边用）签名不同：后者无 `query_*` 参数，两节点都查 `qcodes_of`/`qscale_of`/`qsum_of`。

### 6.4 持久化

int8-only 模式下 `vecs` 不存在 → `HnswIndex::save_vec_payload` / `load_vec_payload` 是 no-op（`save_vec_payload` 入口 `if (cfg_.inmem_int8) return true;`，`load_vec_payload` 同）。`.qc8` 照常写入（`needs_qcodes_ = true`）。`.ckpt` 段头 `flags` bit0（has_payload）= 0、bit1（has_qc8）= 1（`HnswIndex::serialize` 内 `flags` 计算）。

### 6.5 并发协议

不变：单写者（reducer）+ 多读者（查询）。`atomic<shared_ptr<HnswIndex>>` 换指针语义保持。int8-only 仅改节点内存布局与距离实现，并发约束一字未改。

## 7. 内存账（精确，按当前实现）

详细推导见 `hnsw-memory-footprint-zh.md`。这里给关键对比（`M=16`，1M 节点）：

| 模式 | 每节点字节 | 1M 节点 | D=2560（qwen3） |
|---|---|---|---|
| 默认 f32+int8 | `5·D + 165.5` | 0.75 → 12.08 GiB | **12.08 GiB** |
| f32-only | `4·D + 157.5` | 0.62 → 9.68 GiB | 9.68 GiB |
| **int8-only** | `D + 165.5` | 0.27 → 2.54 GiB | **2.54 GiB** |

D=2560 时 int8-only 相对默认 f32+int8 降 **~79%**。对应到 `HnswIndex::search` 路径：int8 VNNI 粗筛 → 无 f32 精排（`if (!cfg_.inmem_int8)` 分支跳过），found 按 int8 距离序直取。

## 8. 关键符号索引

| 概念 | 代码位置 |
|---|---|
| 量化策略 / QVector 结构 | `bitcask::vec::int8::QVector`（`int8_kernels.hpp`） |
| 对称 int8 量化 | `int8::quantize_into` / `int8::quantize`（`int8_kernels.hpp`） |
| 偏置补偿算式 | `int8::dot_vnni512` / `int8::dot_vnni`（`int8_kernels.hpp`），含注释解释 `Σ(a+128)·b = Σa·b + 128·Σb` |
| 标量兜底（无 VNNI 时） | `int8::dot_scalar_raw`（`int8_kernels.hpp`） |
| 运行时分发 | `int8::pick_int8_dot_kernel`（`int8_kernels.hpp`） |
| L2 VNNI | `int8::l2_vnni512` / `int8::l2_scalar`（`int8_kernels.hpp`） |
| 启动期自检 | `int8::self_test`（`int8_kernels.hpp`） |
| `HnswConfig.inmem_int8` 字段 | `include/bitcask/hnsw.hpp` 的 `HnswConfig` |
| `HnswIndex::needs_qcodes_` 派生 | `HnswIndex` 构造函数（`src/vector/hnsw.cpp`） |
| NodeChunk 分配条件（按 `inmem_int8`） | `HnswIndex::NodeChunk::NodeChunk` 构造函数（`src/vector/hnsw.cpp`） |
| int8-only 路径的搜索 / 建图 / 启发式 | `HnswIndex::search` / `HnswIndex::greedy_closest_int8` / `HnswIndex::search_layer_int8` / `HnswIndex::select_neighbors_int8`（`src/vector/hnsw.cpp`） |
| 反量化兜底（rebuild 专用） | `HnswIndex::node_vec`（`src/vector/hnsw.cpp`） |
| 节点→节点距离（int8-only 建图选边） | `HnswIndex::dist_id_int8_node`（`hnsw.hpp`） |
| 持久化（int8-only 无 .vec） | `HnswIndex::save_vec_payload` / `HnswIndex::load_vec_payload` 入口 `inmem_int8` 早返（`src/vector/hnsw.cpp`） |
| VectorPlugin 接线（`inmem_int8` 透传） | `VectorPlugin::VectorPlugin` 构造函数（`src/search/vector_plugin.cpp`） |
| L2 + inmem_int8 拒绝 | `HnswIndex` 构造 `assert` + `VectorPlugin` 上游 `kInvalidOption` |