// HNSW 实现(V3.3 单写者 + 多读者)。算法对应 Malkov & Yashunin 2016;
// 工程选择见 doc/hnsw-design-zh.md §2,并发协议见 §3 与 hnsw.hpp 文件头。

#include "bitcask/hnsw.hpp"
#include "bitcask/codec.hpp"
#include "hnsw_kernels.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>

// V7:BCVS v2 payload 文件 mmap(只读 MAP_SHARED + madvise RANDOM)。
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace bitcask::vec {

// V3.9:距离内核从匿名命名空间外移一份到 bitcask::vec::detail,只给
// cpp/bench/distance_bench.cpp 等 micro-bench 走 hnsw_kernels.hpp 直接调
// 用做对拍/计时。生产路径 pick_kernel() 仍用本 TU 内同函数(在匿名命名
// 空间里被强引用,链接器裁掉它们的具名符号时不会丢弃 —— 实际上 pick_kernel
// 返回函数指针强保)。算法、签名、target 属性不变,只换命名空间。

namespace detail {

// ---- 距离内核:返回值统一为"越小越近"的 distance ----
// kDot:dist = -dot(归一化向量下 = 余弦距离的单调变换);
// kL2 :dist = 平方欧氏。

float dot_scalar(const float* a, const float* b, std::size_t n) {
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) s += a[i] * b[i];
    return -s;
}

float l2_scalar(const float* a, const float* b, std::size_t n) {
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define BITCASK_HNSW_SIMD 1

__attribute__((target("avx2,fma")))
float hsum256(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo);
}

// V3.8:4 路独立累加器打破 FMA 依赖链。单累加器下每次 fmadd 依赖上一次
// 结果(FMA 延迟 ~4cyc → 1 FMA/4cyc);4 路交错把循环顶到加载口上限
// (2 加载/cyc = 1 FMA/cyc),内核理论余量 ~4×。384d=12 轮、2560d=80 轮
// 整除主循环;8 宽次级循环 + 标量尾兜任意 n。注:求和顺序改变,结果与
// 旧内核可有最后一两 ulp 漂移(测试容差均覆盖)。
__attribute__((target("avx2,fma")))
float dot_avx2(const float* a, const float* b, std::size_t n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps(), acc3 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),
                               _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8),
                               _mm256_loadu_ps(b + i + 8), acc1);
        acc2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16),
                               _mm256_loadu_ps(b + i + 16), acc2);
        acc3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24),
                               _mm256_loadu_ps(b + i + 24), acc3);
    }
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),
                               _mm256_loadu_ps(b + i), acc0);
    }
    float s = hsum256(_mm256_add_ps(_mm256_add_ps(acc0, acc1),
                                    _mm256_add_ps(acc2, acc3)));
    for (; i < n; ++i) s += a[i] * b[i];
    return -s;
}

__attribute__((target("avx2,fma")))
float l2_avx2(const float* a, const float* b, std::size_t n) {
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps(), acc3 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i),
                                        _mm256_loadu_ps(b + i));
        const __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8),
                                        _mm256_loadu_ps(b + i + 8));
        const __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16),
                                        _mm256_loadu_ps(b + i + 16));
        const __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24),
                                        _mm256_loadu_ps(b + i + 24));
        acc0 = _mm256_fmadd_ps(d0, d0, acc0);
        acc1 = _mm256_fmadd_ps(d1, d1, acc1);
        acc2 = _mm256_fmadd_ps(d2, d2, acc2);
        acc3 = _mm256_fmadd_ps(d3, d3, acc3);
    }
    for (; i + 8 <= n; i += 8) {
        const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i),
                                       _mm256_loadu_ps(b + i));
        acc0 = _mm256_fmadd_ps(d, d, acc0);
    }
    float s = hsum256(_mm256_add_ps(_mm256_add_ps(acc0, acc1),
                                    _mm256_add_ps(acc2, acc3)));
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

// V3.9:AVX-512 距离内核。__m512 = 16 floats,主循环 stride = 64(4 路累加 ×
// 16),把 384d 缩到 6 轮、1536d 缩到 24 轮;在 Skylake-SP/Ice Lake 这类双
// FMA 单元上,主循环理论上限 ~2 FMA/cyc(2 加载 + 2 FMA / cyc),相对 AVX2
// 内核理论再翻倍。仅用 AVX512F 子集(无 BW/VL),最大化可移植。注意:
// 求和顺序随累加器宽度变宽,与 AVX2 末位 ulp 可能有数 ulp 漂移,正确性
// 检验见 cpp/bench/distance_bench.cpp。
__attribute__((target("avx512f")))
float hsum512(__m512 v) {
#if defined(__GNUC__) && (__GNUC__ >= 10)
    // GCC 10+/Clang 的 _mm512_reduce_add_ps:内部即树形归并,单指令。
    return _mm512_reduce_add_ps(v);
#else
    // 兼容旧编译器:手工两两归并(8 步加法 vs 横向 ~16 步)。
    __m256 lo = _mm512_castps512_ps256(v);
    __m256 hi = _mm512_extractf64x4_ps(v, 1);
    __m256 s256 = _mm256_add_ps(lo, hi);
    __m128 lo2 = _mm256_castps256_ps128(s256);
    __m128 hi2 = _mm256_extractf128_ps(s256, 1);
    __m128 s128 = _mm_add_ps(lo2, hi2);
    s128 = _mm_hadd_ps(s128, s128);
    s128 = _mm_hadd_ps(s128, s128);
    return _mm_cvtss_f32(s128);
#endif
}

__attribute__((target("avx512f")))
float dot_avx512(const float* a, const float* b, std::size_t n) {
    __m512 acc0 = _mm512_setzero_ps(), acc1 = _mm512_setzero_ps();
    __m512 acc2 = _mm512_setzero_ps(), acc3 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i),
                               _mm512_loadu_ps(b + i), acc0);
        acc1 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 16),
                               _mm512_loadu_ps(b + i + 16), acc1);
        acc2 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 32),
                               _mm512_loadu_ps(b + i + 32), acc2);
        acc3 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i + 48),
                               _mm512_loadu_ps(b + i + 48), acc3);
    }
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm512_fmadd_ps(_mm512_loadu_ps(a + i),
                               _mm512_loadu_ps(b + i), acc0);
    }
    float s = hsum512(_mm512_add_ps(_mm512_add_ps(acc0, acc1),
                                    _mm512_add_ps(acc2, acc3)));
    for (; i < n; ++i) s += a[i] * b[i];
    return -s;
}

__attribute__((target("avx512f")))
float l2_avx512(const float* a, const float* b, std::size_t n) {
    __m512 acc0 = _mm512_setzero_ps(), acc1 = _mm512_setzero_ps();
    __m512 acc2 = _mm512_setzero_ps(), acc3 = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i),
                                        _mm512_loadu_ps(b + i));
        const __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16),
                                        _mm512_loadu_ps(b + i + 16));
        const __m512 d2 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 32),
                                        _mm512_loadu_ps(b + i + 32));
        const __m512 d3 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 48),
                                        _mm512_loadu_ps(b + i + 48));
        acc0 = _mm512_fmadd_ps(d0, d0, acc0);
        acc1 = _mm512_fmadd_ps(d1, d1, acc1);
        acc2 = _mm512_fmadd_ps(d2, d2, acc2);
        acc3 = _mm512_fmadd_ps(d3, d3, acc3);
    }
    for (; i + 16 <= n; i += 16) {
        const __m512 d = _mm512_sub_ps(_mm512_loadu_ps(a + i),
                                       _mm512_loadu_ps(b + i));
        acc0 = _mm512_fmadd_ps(d, d, acc0);
    }
    float s = hsum512(_mm512_add_ps(_mm512_add_ps(acc0, acc1),
                                    _mm512_add_ps(acc2, acc3)));
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}
#endif

}  // namespace detail

namespace {

// V3.8:候选向量软件预取。大图下每个候选是 ~1.5KB(384d)的冷 DRAM
// 取数,先扫一遍邻居把向量首 256B 拉向 L1,再进距离循环——取数与计算
// 重叠,后续行交给硬件流预取。非 x86 为空操作。
inline void prefetch_vec(const float* p, std::size_t dim) {
#ifdef BITCASK_HNSW_SIMD
    // V3.9:拉宽到 384B(96 floats)以覆盖 384d(1.5KB)前两 AVX-512 行 =
    // 128B;另加 256B 段为 384d 第 2-3 个 cache line 提前热身。AVX2 也
    // 受益(384d 头 64B×4 cache line 已覆盖)。
    // ⑯:按 dim 守卫每条 prefetch(仿 int8 路径)——小 dim 不越过向量尾预取
    // 到下个节点;大 dim 仍取头 384B(其余交硬件流预取)。
    const char* c = reinterpret_cast<const char*>(p);
    const std::size_t bytes = dim * sizeof(float);
    _mm_prefetch(c, _MM_HINT_T0);
    if (bytes > 64)  _mm_prefetch(c + 64, _MM_HINT_T0);
    if (bytes > 128) _mm_prefetch(c + 128, _MM_HINT_T0);
    if (bytes > 192) _mm_prefetch(c + 192, _MM_HINT_T0);
    if (bytes > 256) _mm_prefetch(c + 256, _MM_HINT_T0);
    if (bytes > 320) _mm_prefetch(c + 320, _MM_HINT_T0);
#else
    (void)p;
    (void)dim;
#endif
}

using DistFn = float (*)(const float*, const float*, std::size_t);

DistFn pick_kernel(HnswMetric metric) {
#ifdef BITCASK_HNSW_SIMD
    // V3.9:AVX-512F 优先(超集)。要求仅基础 AVX-512 Foundation,无 BW/VL,
    // 覆盖 Skylake-SP / Ice Lake / Zen4。运行时一次探测,零查询开销。
    static const bool kAvx512f = __builtin_cpu_supports("avx512f");
    if (kAvx512f) {
        return metric == HnswMetric::kDot ? detail::dot_avx512
                                          : detail::l2_avx512;
    }
    static const bool kAvx2 = __builtin_cpu_supports("avx2") &&
                              __builtin_cpu_supports("fma");
    if (kAvx2) {
        return metric == HnswMetric::kDot ? detail::dot_avx2
                                          : detail::l2_avx2;
    }
#endif
    return metric == HnswMetric::kDot ? detail::dot_scalar
                                      : detail::l2_scalar;
}

inline void cpu_pause() {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#endif
}

// per-node 自旋锁(1 字节,test-and-set + pause;临界区 ~百 ns)。
inline void lock_node(std::atomic<std::uint8_t>& l) {
    while (l.exchange(1, std::memory_order_acquire) != 0) {
        while (l.load(std::memory_order_relaxed) != 0) cpu_pause();
    }
}
inline void unlock_node(std::atomic<std::uint8_t>& l) {
    l.store(0, std::memory_order_release);
}

// ---- visited 标记:thread_local 版本化数组 ----
// 方案说明(V3.3,与任务书"取最简单且正确者"一致):每读者线程一份
// {marks, epoch, owner};owner 是 HnswIndex 的**全局自增实例 id**
// (非 this 指针——指针在 delete/new 后可复用,会让陈旧 marks 与新实例
// 的 epoch 假性匹配)。owner 切换时整组清零 + epoch 归零;同实例内
// epoch 自增免清零,回绕时整组清一次。多实例被同线程交替查询会触发
// 反复清零,正确性不受影响(本引擎单集合单图,常态零开销)。
struct VisitedTable {
    std::vector<std::uint32_t> marks;
    std::uint32_t epoch = 0;
    std::uint64_t owner = 0;
};
thread_local VisitedTable t_visited;

std::atomic<std::uint64_t> g_instance_seq{1};

}  // namespace

HnswIndex::NodeChunk::NodeChunk(std::size_t dim, bool needs_vecs,
                                bool needs_qcodes)
    : vecs(needs_vecs ? static_cast<std::size_t>(kChunkSize) * dim : 0),
      ords(kChunkSize, 0),
      levels(kChunkSize, 0),
      adj(kChunkSize, nullptr),
      locks(new std::atomic<std::uint8_t>[kChunkSize]),
      qcodes(needs_qcodes ? static_cast<std::size_t>(kChunkSize) * dim : 0),
      qscales(needs_qcodes ? kChunkSize : 0, 0.0f),
      qsums(needs_qcodes ? kChunkSize : 0, 0) {
    for (std::uint32_t i = 0; i < kChunkSize; ++i) {
        locks[i].store(0, std::memory_order_relaxed);
    }
}

HnswIndex::HnswIndex(const HnswConfig& cfg)
    : cfg_(cfg),
      dist_(pick_kernel(cfg.metric)),
      int8_dot_(int8::pick_int8_dot_kernel()),
      needs_qcodes_(cfg.inmem_int8 ||
                    (int8_dot_ != nullptr && cfg.metric == HnswMetric::kDot)),
      inv_log_m_(1.0 / std::log(static_cast<double>(cfg.M))),
      instance_id_(g_instance_seq.fetch_add(1, std::memory_order_relaxed)),
      rng_(cfg.seed) {
    assert(cfg_.dim > 0 && cfg_.M >= 2);
    // P5:int8-only 仅 kDot;距离=int8 重建内积。kL2 由上游 open 拒绝。
    assert(!(cfg_.inmem_int8 && cfg_.metric != HnswMetric::kDot) &&
           "inmem_int8 requires kDot metric");
    // int8-only 必须有可用 int8 dot——无 VNNI 时回退标量(否则建图/查询
    // 无 f32 可算)。默认 f32+int8 路径不变:int8_dot_ 为 null 时退 f32。
    if (cfg_.inmem_int8 && int8_dot_ == nullptr) {
        int8_dot_ = &int8::dot_scalar_raw;
    }
}

// P5:int8-only 无常驻 f32,从量化副本反量化到 thread_local 缓冲。
std::span<const float> HnswIndex::node_vec(std::uint32_t id) const {
    if (!cfg_.inmem_int8) {
        return {vec_of(id), cfg_.dim};
    }
    thread_local std::vector<float> buf;
    buf.resize(cfg_.dim);
    const std::int8_t* codes = qcodes_of(id);
    const float factor = qscale_of(id) / 127.0f;
    for (std::uint32_t i = 0; i < cfg_.dim; ++i) {
        buf[i] = static_cast<float>(codes[i]) * factor;
    }
    return {buf.data(), cfg_.dim};
}

HnswIndex::~HnswIndex() {
    // V7:mmap payload 先于 chunk 释放——mmap 区域只读、生命周期与 fd 绑定,
    // close fd 前 munmap 防止其他进程拿同一文件 mmap 时 kernel 行为未定义。
    if (vecs_mmap_raw_ != nullptr) {
        ::munmap(vecs_mmap_raw_, vecs_mmap_len_);
        vecs_mmap_raw_  = nullptr;
        vecs_mmap_base_ = nullptr;
        vecs_mmap_len_  = 0;
    }
    if (vecs_payload_fd_ >= 0) {
        ::close(vecs_payload_fd_);
        vecs_payload_fd_ = -1;
    }
    for (auto& slot : chunks_) {
        delete slot.load(std::memory_order_relaxed);
    }
}

std::uint32_t HnswIndex::copy_neighbors(std::uint32_t id, std::uint32_t layer,
                                        std::uint32_t* out) const {
    NodeChunk* c = chunk_of(id);
    const std::uint32_t slot = id & kChunkMask;
    auto& lk = c->locks[slot];
    lock_node(lk);
    // adj 指针在节点发布(count release)前写入且永不搬迁;经 count
    // acquire 或本锁的 happens-before 链均可见。
    const std::uint32_t* a = c->adj[slot] + layer_off(layer);
    const std::uint32_t n = a[0];
    std::memcpy(out, a + 1, static_cast<std::size_t>(n) * sizeof(std::uint32_t));
    unlock_node(lk);
    return n;
}

std::uint32_t HnswIndex::greedy_closest(const float* q, std::uint32_t start,
                                        std::uint32_t layer, std::uint32_t n,
                                        std::uint32_t* scratch) const {
    std::uint32_t cur = start;
    float cur_d = dist_id(q, cur);
    bool improved = true;
    while (improved) {
        improved = false;
        const std::uint32_t cnt = copy_neighbors(cur, layer, scratch);
        for (std::uint32_t i = 0; i < cnt; ++i) {
            if (scratch[i] < n) prefetch_vec(vec_of(scratch[i]), cfg_.dim);
        }
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid >= n) continue;  // 本地 count 快照之外:尚未对我发布
            const float d = dist_id(q, nid);
            if (d < cur_d) {
                cur_d = d;
                cur = nid;
                improved = true;
            }
        }
    }
    return cur;
}

using Cand = std::pair<float, std::uint32_t>;
// B5:priority_queue 底层 vector 跨调用复用（ef=256 → ~4KB × 2 省首次 alloc + 几何增长）。
// ReusablePQ 继承暴露 protected Container c 供函数尾 extract 保容量。
template <class Compare>
struct ReusablePQ : std::priority_queue<Cand, std::vector<Cand>, Compare> {
    using Base = std::priority_queue<Cand, std::vector<Cand>, Compare>;
    using Base::Base;
    std::vector<Cand> extract() && noexcept { return std::move(this->c); }
};
thread_local std::vector<Cand> tl_cands_buf;
thread_local std::vector<Cand> tl_top_buf;

void HnswIndex::search_layer(
    const float* q, std::uint32_t entry, std::size_t ef, std::uint32_t layer,
    std::uint32_t n, std::uint32_t* scratch,
    std::vector<std::pair<float, std::uint32_t>>& out) const {
    // visited:thread_local 版本化数组(方案见文件顶部注释)。
    auto& vt = t_visited;
    if (vt.owner != instance_id_) {
        vt.owner = instance_id_;
        vt.epoch = 0;
        std::fill(vt.marks.begin(), vt.marks.end(), 0);
    }
    if (vt.marks.size() < n) vt.marks.resize(n, 0);
    if (++vt.epoch == 0) {
        std::fill(vt.marks.begin(), vt.marks.end(), 0);
        vt.epoch = 1;
    }
    const std::uint32_t ep = vt.epoch;
    std::uint32_t* visited = vt.marks.data();

    using Cand [[maybe_unused]] = std::pair<float, std::uint32_t>;
    // B5:从 thread_local buffer move 构造（保容量），函数尾 extract 回收。
    tl_cands_buf.clear();
    tl_top_buf.clear();
    ReusablePQ<std::greater<>> cands(std::greater<>{}, std::move(tl_cands_buf));
    ReusablePQ<std::less<Cand>> top(std::less<Cand>{}, std::move(tl_top_buf));

    const float d0 = dist_id(q, entry);
    cands.push({d0, entry});
    top.push({d0, entry});
    visited[entry] = ep;

    while (!cands.empty()) {
        const auto [d, id] = cands.top();
        if (d > top.top().first && top.size() >= ef) break;  // 收敛
        cands.pop();
        const std::uint32_t cnt = copy_neighbors(id, layer, scratch);
        // 预取与计算分两遍:未访问的在界邻居先把向量段拉过来。
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid < n && visited[nid] != ep) prefetch_vec(vec_of(nid), cfg_.dim);
        }
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid >= n) continue;  // 本地 count 快照之外(见 hpp 协议)
            if (visited[nid] == ep) continue;
            visited[nid] = ep;
            const float nd = dist_id(q, nid);
            if (top.size() < ef || nd < top.top().first) {
                cands.push({nd, nid});
                top.push({nd, nid});
                if (top.size() > ef) top.pop();
            }
        }
    }

    out.clear();
    out.resize(top.size());
    for (std::size_t i = top.size(); i-- > 0;) {
        out[i] = top.top();
        top.pop();
    }
    tl_cands_buf = std::move(cands).extract();
    tl_top_buf = std::move(top).extract();
}

// V4.2:int8 粗筛版 greedy_closest,与 f32 版同结构,只换 dist_id →
// dist_id_int8。粗筛阶段不要求数值精度,目的是把图遍历导到正确区域。
std::uint32_t HnswIndex::greedy_closest_int8(
    const std::int8_t* query_codes, float query_scale,
    std::int32_t query_sum, std::uint32_t start, std::uint32_t layer,
    std::uint32_t n, std::uint32_t* scratch) const {
    std::uint32_t cur = start;
    float cur_d = dist_id_int8(query_codes, query_scale, query_sum, cur);
    bool improved = true;
    while (improved) {
        improved = false;
        const std::uint32_t cnt = copy_neighbors(cur, layer, scratch);
        for (std::uint32_t i = 0; i < cnt; ++i) {
            if (scratch[i] < n) {
                const char* pc = reinterpret_cast<const char*>(qcodes_of(scratch[i]));
                _mm_prefetch(pc, _MM_HINT_T0);
                if (cfg_.dim > 64)  _mm_prefetch(pc + 64, _MM_HINT_T0);
                if (cfg_.dim > 128) _mm_prefetch(pc + 128, _MM_HINT_T0);
                if (cfg_.dim > 192) _mm_prefetch(pc + 192, _MM_HINT_T0);
                if (cfg_.dim > 256) _mm_prefetch(pc + 256, _MM_HINT_T0);
                if (cfg_.dim > 320) _mm_prefetch(pc + 320, _MM_HINT_T0);
            }
        }
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid >= n) continue;
            const float d = dist_id_int8(query_codes, query_scale, query_sum,
                                         nid);
            if (d < cur_d) {
                cur_d = d;
                cur = nid;
                improved = true;
            }
        }
    }
    return cur;
}

// V4.2:int8 粗筛版 search_layer,与 f32 版同结构。预取仍对 f32 向量
// 段发(冷拉后段距离不需要重读——int8 阶段之后才是 f32 重排)。
void HnswIndex::search_layer_int8(
    const std::int8_t* query_codes, float query_scale, std::int32_t query_sum,
    std::uint32_t entry, std::size_t ef, std::uint32_t layer, std::uint32_t n,
    std::uint32_t* scratch,
    std::vector<std::pair<float, std::uint32_t>>& out) const {
    auto& vt = t_visited;
    if (vt.owner != instance_id_) {
        vt.owner = instance_id_;
        vt.epoch = 0;
        std::fill(vt.marks.begin(), vt.marks.end(), 0);
    }
    if (vt.marks.size() < n) vt.marks.resize(n, 0);
    if (++vt.epoch == 0) {
        std::fill(vt.marks.begin(), vt.marks.end(), 0);
        vt.epoch = 1;
    }
    const std::uint32_t ep = vt.epoch;
    std::uint32_t* visited = vt.marks.data();

    using Cand [[maybe_unused]] = std::pair<float, std::uint32_t>;
    tl_cands_buf.clear();
    tl_top_buf.clear();
    ReusablePQ<std::greater<>> cands(std::greater<>{}, std::move(tl_cands_buf));
    ReusablePQ<std::less<Cand>> top(std::less<Cand>{}, std::move(tl_top_buf));

    const float d0 = dist_id_int8(query_codes, query_scale, query_sum, entry);
    cands.push({d0, entry});
    top.push({d0, entry});
    visited[entry] = ep;

    while (!cands.empty()) {
        const auto [d, id] = cands.top();
        if (d > top.top().first && top.size() >= ef) break;
        cands.pop();
        const std::uint32_t cnt = copy_neighbors(id, layer, scratch);
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid < n && visited[nid] != ep) {
                const char* pc = reinterpret_cast<const char*>(qcodes_of(nid));
                _mm_prefetch(pc, _MM_HINT_T0);
                if (cfg_.dim > 64)  _mm_prefetch(pc + 64, _MM_HINT_T0);
                if (cfg_.dim > 128) _mm_prefetch(pc + 128, _MM_HINT_T0);
                if (cfg_.dim > 192) _mm_prefetch(pc + 192, _MM_HINT_T0);
                if (cfg_.dim > 256) _mm_prefetch(pc + 256, _MM_HINT_T0);
                if (cfg_.dim > 320) _mm_prefetch(pc + 320, _MM_HINT_T0);
            }
        }
        for (std::uint32_t i = 0; i < cnt; ++i) {
            const std::uint32_t nid = scratch[i];
            if (nid >= n) continue;
            if (visited[nid] == ep) continue;
            visited[nid] = ep;
            const float nd = dist_id_int8(query_codes, query_scale, query_sum,
                                          nid);
            if (top.size() < ef || nd < top.top().first) {
                cands.push({nd, nid});
                top.push({nd, nid});
                if (top.size() > ef) top.pop();
            }
        }
    }

    out.clear();
    out.resize(top.size());
    for (std::size_t i = top.size(); i-- > 0;) {
        out[i] = top.top();
        top.pop();
    }
    tl_cands_buf = std::move(cands).extract();
    tl_top_buf = std::move(top).extract();
}

void HnswIndex::select_neighbors(
    const float* q, std::vector<std::pair<float, std::uint32_t>>& cands,
    std::uint32_t m) const {
    (void)q;  // cands 的 dist 已按 q 预计算;参数留作语义自注释
    // Algorithm 4 简化版:cands 按 dist 升序;候选与已选集逐一比较,
    // 离 query 更近于离任何已选者才保留——分散方向,聚簇数据下保召回。
    if (cands.size() <= m) return;
    std::vector<std::pair<float, std::uint32_t>> picked;
    picked.reserve(m);
    for (const auto& [d, id] : cands) {
        if (picked.size() >= m) break;
        bool ok = true;
        const float* v = vec_of(id);
        for (const auto& [pd, pid] : picked) {
            if (dist_(v, vec_of(pid), cfg_.dim) < d) {  // 离已选者比离 query 还近
                ok = false;
                break;
            }
        }
        if (ok) picked.push_back({d, id});
    }
    // 不足 m 时用剩余最近者补齐(论文 keepPruned 变体)。
    if (picked.size() < m) {
        for (const auto& c : cands) {
            if (picked.size() >= m) break;
            if (std::find_if(picked.begin(), picked.end(), [&](auto& p) {
                    return p.second == c.second;
                }) == picked.end()) {
                picked.push_back(c);
            }
        }
    }
    cands = std::move(picked);
}

// P5:int8-only 版 select_neighbors。与 f32 版同启发式(Algorithm 4),
// 只把候选-已选距离换成 dist_id_int8_node(两节点皆有量化副本,无 f32)。
void HnswIndex::select_neighbors_int8(
    std::vector<std::pair<float, std::uint32_t>>& cands, std::uint32_t m) const {
    if (cands.size() <= m) return;
    std::vector<std::pair<float, std::uint32_t>> picked;
    picked.reserve(m);
    for (const auto& [d, id] : cands) {
        if (picked.size() >= m) break;
        bool ok = true;
        for (const auto& [pd, pid] : picked) {
            if (dist_id_int8_node(id, pid) < d) {  // 离已选者比离 query 还近
                ok = false;
                break;
            }
        }
        if (ok) picked.push_back({d, id});
    }
    if (picked.size() < m) {
        for (const auto& c : cands) {
            if (picked.size() >= m) break;
            if (std::find_if(picked.begin(), picked.end(), [&](auto& p) {
                    return p.second == c.second;
                }) == picked.end()) {
                picked.push_back(c);
            }
        }
    }
    cands = std::move(picked);
}

void HnswIndex::insert(std::uint64_t ord, std::span<const float> vec) {
    assert(vec.size() == cfg_.dim);
    // 单写者声明:多写者不支持(全引擎统一约束,设计 §3/§7)。
    const bool was_active = writer_active_.exchange(true);
    assert(!was_active && "HnswIndex::insert: single writer only");
    (void)was_active;
    struct Guard {
        std::atomic<bool>& f;
        ~Guard() { f.store(false); }
    } guard{writer_active_};
    // 水位幂等(回放重叠区;ord 引擎全局单调)。
    const std::uint64_t prev = max_inserted_ord_.load(std::memory_order_relaxed);
    if (prev != static_cast<std::uint64_t>(-1) && ord <= prev) return;
    max_inserted_ord_.store(ord, std::memory_order_relaxed);

    const std::uint32_t id = count_.load(std::memory_order_relaxed);
    const std::uint32_t ci = id >> kChunkBits;
    assert(ci < kMaxChunks && "HnswIndex capacity exceeded (kMaxChunks)");
    NodeChunk* c = chunks_[ci].load(std::memory_order_relaxed);
    if (c == nullptr) {
        c = new NodeChunk(cfg_.dim, !cfg_.inmem_int8, needs_qcodes_);
        chunks_[ci].store(c, std::memory_order_release);
    } else if (!cfg_.inmem_int8 && c->vecs.empty()) {
        // checkpoint 加载的 chunk(needs_vecs=false)首次插入热数据:
        // 懒分配 vecs_。单写者协议下安全(count_.store 在后,读者看不到
        // 未就绪节点);首帧 assign 无旧指针可失效。
        c->vecs.assign(
            static_cast<std::size_t>(kChunkSize) * cfg_.dim, 0.0f);
    }
    const std::uint32_t slot = id & kChunkMask;

    // 层数:floor(-ln(U) * mL),截断防极端。
    double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
    if (u < 1e-12) u = 1e-12;
    auto level = static_cast<std::uint32_t>(-std::log(u) * inv_log_m_);
    if (level > 31) level = 31;

    // 1) 写满本节点数据:vec/ord/level + 零初始化邻接块。
    // P5:int8-only 不存常驻 f32,只落量化副本——建图/查询全程 int8。默认
    // f32+int8 路径不变:vecs_ 走 hot_vecs_,VNNI 在时附带量化副本粗筛。
    // V7:vecs_ 已从 NodeChunk 移出——checkpoint 加载的 vecs 由 mmap 覆盖,
    // 新插入追加 hot_vecs_;vec_of(id) 统一路由两者。
    if (cfg_.inmem_int8) {
        auto qv = int8::quantize(vec.data(), cfg_.dim);
        std::memcpy(c->qcodes.data() +
                        static_cast<std::size_t>(slot) * cfg_.dim,
                    qv.codes.data(),
                    static_cast<std::size_t>(cfg_.dim) * sizeof(std::int8_t));
        c->qscales[slot] = qv.scale;
        c->qsums[slot]   = qv.sum_codes;
    } else {
        // V7:vecs_ 走 NodeChunk::vecs(定容;仅 hot chunk 分配)。
        // checkpoint flush 时 save_vec_payload 合并 mmap + chunk vecs 写新 payload。
        std::memcpy(c->vecs.data() +
                        static_cast<std::size_t>(slot) * cfg_.dim,
                    vec.data(),
                    static_cast<std::size_t>(cfg_.dim) * sizeof(float));
        // V4.2:同步落 int8 量化副本。int8 路径存在时(VNNI)下游 search 用
        // 4× 缩的带宽 + VNNI 加速;int8_dot_ == nullptr 时这段不被读,浪费
        // 一些内存但功能不变(仅 kDot 有意义,kL2 见 search 路径判断)。
        if (int8_dot_ != nullptr && cfg_.metric == HnswMetric::kDot) {
            auto qv = int8::quantize(vec.data(), cfg_.dim);
            std::memcpy(c->qcodes.data() +
                            static_cast<std::size_t>(slot) * cfg_.dim,
                        qv.codes.data(),
                        static_cast<std::size_t>(cfg_.dim) * sizeof(std::int8_t));
            c->qscales[slot] = qv.scale;
            c->qsums[slot]   = qv.sum_codes;
        }
    }
    c->ords[slot] = ord;
    c->levels[slot] = static_cast<std::uint8_t>(level);
    const std::size_t slots =
        (1 + cfg_.M * 2) + static_cast<std::size_t>(level) * (1 + cfg_.M);
    c->adj[slot] = c->alloc_adj(slots);  // arena 分配，地址此后永不搬迁

    // 2) 发布:此后读者可见本节点(邻接为空 → 图内不可达,无害)。
    count_.store(id + 1, std::memory_order_release);

    // entry_meta_ 仅写者改,relaxed 自读即可。
    const std::uint64_t em = entry_meta_.load(std::memory_order_relaxed);
    if (em == 0) {  // 首节点
        entry_meta_.store((static_cast<std::uint64_t>(level + 1) << 32) | id,
                          std::memory_order_release);
        return;
    }
    const auto max_level = static_cast<std::int32_t>(em >> 32) - 1;
    auto cur = static_cast<std::uint32_t>(em & 0xFFFFFFFFu);

    // 写者侧搜索的可见边界 = id(自身排除:防低层把自己选成自己邻居)。
    const std::uint32_t n_bound = id;
    // ⑦ thread_local 复用(insert 单写者，本线程独占）。
    thread_local std::vector<std::uint32_t> scratch;
    scratch.resize(1 + cfg_.M * 2);

    // P5:int8-only 用本节点量化副本作建图 query(无常驻 f32);默认用 f32。
    const bool i8 = cfg_.inmem_int8;
    // V7:vecs_ 已走 hot_vecs_;vec_of(id) 路由(< checkpoint_count_ → mmap,
    // ≥ → hot_vecs_)。本节点是刚插入的 id,hot_vecs_ 末尾正是其 vec。
    const float* q = i8 ? nullptr : vec_of(id);
    const std::int8_t* qc = i8 ? qcodes_of(id) : nullptr;
    const float        qs = i8 ? qscale_of(id) : 0.0f;
    const std::int32_t qsum = i8 ? qsum_of(id) : 0;

    // 上层贪心下降到 level+1。
    for (std::int32_t l = max_level;
         l > static_cast<std::int32_t>(level); --l) {
        cur = i8 ? greedy_closest_int8(qc, qs, qsum, cur,
                                       static_cast<std::uint32_t>(l), n_bound,
                                       scratch.data())
                 : greedy_closest(q, cur, static_cast<std::uint32_t>(l), n_bound,
                                  scratch.data());
    }

    // 3) level..0:efConstruction 搜索 + 启发式选边 + 双向连边 + 邻居收缩。
    // ⑦ thread_local：search_layer 每层 out.clear() 后填充，跨 insert 复用。
    thread_local std::vector<std::pair<float, std::uint32_t>> found;
    for (std::int32_t l = std::min<std::int32_t>(
             static_cast<std::int32_t>(level), max_level);
         l >= 0; --l) {
        const auto lay = static_cast<std::uint32_t>(l);
        if (i8) {
            search_layer_int8(qc, qs, qsum, cur, cfg_.ef_construction, lay,
                              n_bound, scratch.data(), found);
        } else {
            search_layer(q, cur, cfg_.ef_construction, lay, n_bound,
                         scratch.data(), found);
        }
        cur = found.front().second;  // 下层入口 = 本层最近

        auto picked = found;
        if (i8) select_neighbors_int8(picked, cfg_.M);
        else    select_neighbors(q, picked, cfg_.M);  // L0 也选 M,容量 2M 留收缩余量

        // 正向边:本节点已发布,读者可能在拷它的邻居 → 持自身锁写。
        {
            auto& my_lk = c->locks[slot];
            lock_node(my_lk);
            std::uint32_t* my = c->adj[slot] + layer_off(lay);
            for (const auto& [d, nid] : picked) {
                my[++my[0]] = nid;
            }
            unlock_node(my_lk);
        }

        // 反向边 + 超容收缩:逐邻居持其锁改其邻接。
        for (const auto& [d, nid] : picked) {
            NodeChunk* nc = chunk_of(nid);
            const std::uint32_t nslot = nid & kChunkMask;
            auto& nlk = nc->locks[nslot];
            lock_node(nlk);
            std::uint32_t* nb = nc->adj[nslot] + layer_off(lay);
            const std::uint32_t cap = layer_cap(lay);
            if (nb[0] < cap) {
                nb[++nb[0]] = id;
            } else {
                // 收缩:旧邻居 + 新候选并集,以 nid 为查询点重选 cap 条。
                // 持锁做距离计算(微秒级临界区):读者只在 copy_neighbors
                // 短暂争同一把锁,实测可接受;arena/锁外预选留 V3.x。
                // ⑦ thread_local：单写者 insert 收缩路径复用，clear 保留容量。
                thread_local std::vector<std::pair<float, std::uint32_t>> pool;
                pool.clear();
                pool.reserve(cap + 1);
                if (i8) {
                    for (std::uint32_t i = 1; i <= nb[0]; ++i) {
                        pool.push_back({dist_id_int8_node(nid, nb[i]), nb[i]});
                    }
                    pool.push_back({dist_id_int8_node(nid, id), id});
                } else {
                    const float* nv = vec_of(nid);
                    for (std::uint32_t i = 1; i <= nb[0]; ++i) {
                        pool.push_back({dist_id(nv, nb[i]), nb[i]});
                    }
                    pool.push_back({dist_id(nv, id), id});
                }
                std::sort(pool.begin(), pool.end());
                if (i8) select_neighbors_int8(pool, cap);
                else    select_neighbors(vec_of(nid), pool, cap);
                nb[0] = static_cast<std::uint32_t>(pool.size());
                for (std::uint32_t i = 0; i < pool.size(); ++i) {
                    nb[i + 1] = pool[i].second;
                }
            }
            unlock_node(nlk);
        }
    }

    // 4) 层提升:完整连边后才更新 entry(读者拿到的恒为可达入口)。
    if (static_cast<std::int32_t>(level) > max_level) {
        entry_meta_.store((static_cast<std::uint64_t>(level + 1) << 32) | id,
                          std::memory_order_release);
    }
}

std::vector<HnswIndex::Hit> HnswIndex::search(
    std::span<const float> query, std::size_t k, std::size_t ef,
    const std::function<bool(std::uint64_t)>* live) const {
    std::vector<Hit> hits;
    if (k == 0) return hits;
    assert(query.size() == cfg_.dim);

    // 一致快照:先 entry_meta_(acquire)再 count_(acquire)。entry 的
    // 发布 happens-after 其 count 发布 → 看到新 entry 必看到 count > id。
    const std::uint64_t em = entry_meta_.load(std::memory_order_acquire);
    if (em == 0) return hits;  // 空图
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    const auto max_level = static_cast<std::int32_t>(em >> 32) - 1;
    auto cur = static_cast<std::uint32_t>(em & 0xFFFFFFFFu);
    if (ef < k) ef = k;

    // ⑦ thread_local 复用:并发读者各持一份,消除每次查询的 scratch malloc。
    thread_local std::vector<std::uint32_t> scratch;
    scratch.resize(1 + cfg_.M * 2);
    const float* q = query.data();

    // V4.2:int8 粗筛 + f32 精排。int8 路径只在 VNNI 存在 + kDot 度量下
    // 启用。粗筛用与 f32 相同的 ef(无扩展),以 int8 VNNI 距离遍历图;
    // 精排仅对 top k*3 候选做 f32 距离(固定开销 ~30 次距离计算),保证
    // 召回与纯 f32 一致。
    // P5:int8-only 强制 int8 路径(无 f32 可算,且 dim<64 也得走)。默认
    // 路径仍按 VNNI+kDot+dim≥64 才启 int8 粗筛,否则纯 f32。
    const bool use_int8 =
        cfg_.inmem_int8 ||
        ((int8_dot_ != nullptr) && (cfg_.metric == HnswMetric::kDot) &&
         (cfg_.dim >= 64));

    // ⑦ thread_local：search_layer 内 out.clear() 后填充，跨查询复用。
    thread_local std::vector<std::pair<float, std::uint32_t>> found;
    if (use_int8) {
        const int8::QVector qq = int8::quantize(q, cfg_.dim);
        for (std::int32_t l = max_level; l > 0; --l) {
            cur = greedy_closest_int8(qq.codes.data(), qq.scale, qq.sum_codes,
                                      cur, static_cast<std::uint32_t>(l), n,
                                      scratch.data());
        }
        search_layer_int8(qq.codes.data(), qq.scale, qq.sum_codes,
                          cur, ef, 0, n, scratch.data(), found);
        // int8-only:无 f32 可精排,found 已按 int8 距离升序,直接取。
        // 默认 f32+int8:对 top k*3 做 f32 精排,召回对齐纯 f32。
        if (!cfg_.inmem_int8) {
            const std::size_t rerank_n = std::min(found.size(), k * 3);
            // V7:mmap'd vecs_ 预取——sort 前 madvise(WILLNEED) top 候选页,
            // 内核异步 page-in,延迟藏在 sort 比较后面(O(N log k) 次 dist_id)。
            if (vecs_mmap_base_ != nullptr) {
                const std::size_t vec_bytes =
                    static_cast<std::size_t>(cfg_.dim) * sizeof(float);
                const std::size_t prefetch_n =
                    std::min(found.size(), rerank_n);
                for (std::size_t i = 0; i < prefetch_n; ++i) {
                    const std::uint32_t id = found[i].second;
                    if (id < checkpoint_count_) {
                        const void* v = vec_of(id);
                        ::madvise(const_cast<void*>(v), vec_bytes,
                                  MADV_WILLNEED);
                    }
                }
            }
            // 先把每个候选的 f32 距离算一次写回 .first(此前存的是 int8
            // 粗筛距离),之后 partial_sort 只比较缓存的浮点值。原写法在比较
            // 器里每次重算 2 次 dist_id、排完再算第 3 次,全维 SIMD 距离被重
            // 复计算 O(N log rerank_n) 次;此处降为每候选恰好 1 次(共 N 次)。
            // 线性遍历顺序与上面的 madvise 预取顺序一致,page-in 延迟仍被藏住。
            for (auto& [d, id] : found) d = dist_id(q, id);
            std::partial_sort(found.begin(), found.begin() + rerank_n,
                              found.end(),
                              [](const auto& a, const auto& b) {
                                  return a.first < b.first;
                              });
            found.resize(rerank_n);
        }
    } else {
        for (std::int32_t l = max_level; l > 0; --l) {
            cur = greedy_closest(q, cur, static_cast<std::uint32_t>(l), n,
                                 scratch.data());
        }
        search_layer(q, cur, ef, 0, n, scratch.data(), found);
    }

    hits.reserve(k);
    for (const auto& [d, id] : found) {
        const std::uint64_t ord = ord_of(id);
        if (live != nullptr && *live && !(*live)(ord)) continue;  // 结果侧滤死
        // score 语义:kDot 返回内积本身(d = -dot);kL2 返回 -距离。
        hits.push_back({ord, -d});  // kDot:-(-dot)=内积;kL2:-平方距离
        if (hits.size() >= k) break;
    }
    return hits;
}

// ---- V7:BCVS v2 快照(协议注释见 hnsw.hpp;格式见设计 §5)----
//
// 双文件模型:search.ckpt 的 kHnsw 段嵌入 BVH2 头(qcodes/adj/ords/levels +
// entry/count + flags),search.vec 单独 mmap(BCVP,vecs_ f32 字节流 + 每 4KB
// 页 CRC32)。写入顺序 save_vec_payload → serialize → SearchCheckpoint::write;
// 读出顺序 fread(.ckpt) → deserialize → load_vec_payload(.vec)。
//
// BVH2 segment 布局(LE,嵌 search.ckpt kHnsw 段内):
//   magic "BVH2" (4) | version u32=2 (4) | flags u32 (4) | dim u16 (2) |
//   metric u8 (1) | M u32 (4) | efc u32 (4) | seed u64 (8) | count u32 (4) |
//   entry_meta u64 (8) | max_ord u64 (8)
//   --- per-node (id=0..count-1) ---
//     ord u64 | level u8 | qcodes int8[dim] | qscale f32 | qsum i32
//     for l=0..level: cnt u32 | neighbors[cnt] u32
//   --- end ---
//   crc32 u32 (covers magic..last_neighbor)

namespace {

// V7:BCVS v2 段头 magic/version(search.ckpt kHnsw 段内嵌)。
constexpr std::uint32_t kBcvhMagic   = 0x32485642;  // "BVH2" (LE)
constexpr std::uint32_t kBcvhVersion = 2;

// V7:BCVS v2 payload 文件(独立 .vec)magic/version。
constexpr char          kBcvpMagic[4] = {'B', 'C', 'V', 'P'};
constexpr std::uint32_t kBcvpVersion = 1;
// BCVP 头 64 字节(数据 46B + 14B 填充 + 4B header_crc,凑 2 的幂好对齐)。
constexpr std::size_t   kBcvpHeaderSize  = 64;
constexpr std::uint32_t kBcvpPageSize    = 4096;
// header_crc 在 header 末尾 4 字节,覆盖 [0, header_crc_offset)。
constexpr std::uint32_t kBcvpHeaderCrcOff = kBcvpHeaderSize - 4;

void vs_put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 2);
}
void vs_put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void vs_put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
}

}  // namespace

// V7:BCVP payload 文件 = 头 + 每 4KB 页 CRC32 表 + 页对齐 vecs 数据。
// tmp + rename 原子写;inmem_int8 模式无 vecs_,save_vec_payload 是 no-op。
bool HnswIndex::save_vec_payload(std::string_view path) const {
    if (cfg_.inmem_int8) return true;  // 无常驻 f32 → 无 .vec 文件
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    const std::string fp(path);

    const std::size_t vec_bytes =
        static_cast<std::size_t>(cfg_.dim) * sizeof(float);
    const std::size_t total_vecs =
        static_cast<std::size_t>(n) * vec_bytes;
    const std::uint32_t crc_count =
        total_vecs == 0
            ? 0u
            : static_cast<std::uint32_t>((total_vecs + kBcvpPageSize - 1) /
                                         kBcvpPageSize);

    // vecs_off:头后接 CRC 表,非空数据需页对齐;空数据保留在 header 末尾。
    std::size_t vecs_off =
        kBcvpHeaderSize + static_cast<std::size_t>(crc_count) * 4;
    if (total_vecs > 0) {
        vecs_off = (vecs_off + kBcvpPageSize - 1) &
                   ~(static_cast<std::size_t>(kBcvpPageSize) - 1);
    }
    const std::size_t total_size = vecs_off + total_vecs;

    std::vector<std::uint8_t> buf(total_size, 0);
    // magic "BCVP"
    buf[0] = kBcvpMagic[0]; buf[1] = kBcvpMagic[1];
    buf[2] = kBcvpMagic[2]; buf[3] = kBcvpMagic[3];
    std::memcpy(buf.data() + 4,  &kBcvpVersion,    4);
    std::memcpy(buf.data() + 8,  &cfg_.dim,        2);
    std::memcpy(buf.data() + 10, &n,               4);
    const std::uint64_t watermark = max_inserted_ord_.load(
        std::memory_order_relaxed);
    std::memcpy(buf.data() + 14, &watermark,       8);
    std::memcpy(buf.data() + 22, &kBcvpPageSize,   4);
    std::memcpy(buf.data() + 26, &vecs_off,        8);
    std::memcpy(buf.data() + 34, &total_vecs,      8);
    std::memcpy(buf.data() + 42, &crc_count,       4);
    // bytes 46..59:保留为 0(初始化已零);header_crc 落在 offset 60-63。
    const std::uint32_t header_crc = bitcask::codec::crc32(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(buf.data()),
            kBcvpHeaderCrcOff));
    std::memcpy(buf.data() + kBcvpHeaderCrcOff, &header_crc, 4);

    // vecs 数据区:从 vec_of() 拷贝。mmap 段与 hot_vecs_ 段都由 vec_of 路由。
    for (std::uint32_t id = 0; id < n; ++id) {
        const float* v = vec_of(id);
        std::memcpy(buf.data() + vecs_off + static_cast<std::size_t>(id) * vec_bytes,
                    v, vec_bytes);
    }

    // 每页 CRC32(覆盖实际数据尾页可能 < 4KB)。
    const std::size_t crc_off = kBcvpHeaderSize;
    for (std::uint32_t p = 0; p < crc_count; ++p) {
        const std::size_t off = vecs_off + static_cast<std::size_t>(p) * kBcvpPageSize;
        const std::size_t len =
            std::min(static_cast<std::size_t>(kBcvpPageSize),
                     total_vecs - static_cast<std::size_t>(p) * kBcvpPageSize);
        const std::uint32_t crc = bitcask::codec::crc32(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buf.data() + off), len));
        std::memcpy(buf.data() + crc_off + static_cast<std::size_t>(p) * 4,
                    &crc, 4);
    }

    const std::string tmp = fp + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool wrote =
        std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!wrote || std::rename(tmp.c_str(), fp.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// V7:mmap .vec payload。PRECONDITION:deserialize() 已设 count_/cfg_。校验
// magic/version/dim/count 与 cfg_ 一致;设 vecs_mmap_* / checkpoint_count_。
bool HnswIndex::load_vec_payload(std::string_view path) {
    if (cfg_.inmem_int8) return true;  // 无 payload,语义上 no-op

    const std::string fp(path);
    int fd = ::open(fp.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    // 已持有 mmap 时先拆——契约要求 load 前为空(load 由 open 期单线程串入)。
    if (vecs_mmap_raw_ != nullptr) {
        ::munmap(vecs_mmap_raw_, vecs_mmap_len_);
        vecs_mmap_raw_  = nullptr;
        vecs_mmap_base_ = nullptr;
        vecs_mmap_len_  = 0;
    }
    if (vecs_payload_fd_ >= 0) {
        ::close(vecs_payload_fd_);
        vecs_payload_fd_ = -1;
    }

    std::uint8_t hdr[kBcvpHeaderSize];
    if (::read(fd, hdr, kBcvpHeaderSize) !=
        static_cast<ssize_t>(kBcvpHeaderSize)) {
        ::close(fd);
        return false;
    }
    if (hdr[0] != kBcvpMagic[0] || hdr[1] != kBcvpMagic[1] ||
        hdr[2] != kBcvpMagic[2] || hdr[3] != kBcvpMagic[3]) {
        ::close(fd);
        return false;
    }
    std::uint32_t version;
    std::memcpy(&version, hdr + 4, 4);
    if (version != kBcvpVersion) {
        ::close(fd);
        return false;
    }
    std::uint16_t dim;
    std::memcpy(&dim, hdr + 8, 2);
    std::uint32_t count;
    std::memcpy(&count, hdr + 10, 4);
    std::uint64_t vecs_off_u64;
    std::memcpy(&vecs_off_u64, hdr + 26, 8);
    std::uint64_t vecs_len_u64;
    std::memcpy(&vecs_len_u64, hdr + 34, 8);

    // 与 deserialize() 状态交叉校验。
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    if (dim != cfg_.dim || count != n || vecs_len_u64 !=
        static_cast<std::uint64_t>(n) *
            static_cast<std::uint64_t>(cfg_.dim) * 4u) {
        ::close(fd);
        return false;
    }

    // header_crc
    std::uint32_t stored_hdr_crc;
    std::memcpy(&stored_hdr_crc, hdr + kBcvpHeaderCrcOff, 4);
    const std::uint32_t calc_hdr_crc = bitcask::codec::crc32(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(hdr), kBcvpHeaderCrcOff));
    if (stored_hdr_crc != calc_hdr_crc) {
        ::close(fd);
        return false;
    }

    // 文件大小校验(vecs_off + vecs_len 不能超出文件)。
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);
    if (file_size < vecs_off_u64 + vecs_len_u64) {
        ::close(fd);
        return false;
    }
    // count=0 时文件可能仅 header(无 vecs 段),mmap 整个文件即可。
    void* raw = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (raw == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    vecs_mmap_raw_   = raw;
    vecs_mmap_base_  = reinterpret_cast<const float*>(
        static_cast<std::byte*>(raw) + vecs_off_u64);
    vecs_mmap_len_   = file_size;
    vecs_payload_fd_ = fd;  // fd 持有至 mmap 生命周期末(destructor close)
    checkpoint_count_ = n;
    // 随机访问模式:稀疏读,预取收益小,直接 MADV_RANDOM。
    ::madvise(raw, file_size, MADV_RANDOM);
    return true;
}

bool HnswIndex::serialize(std::vector<std::uint8_t>& buf) const {
    // 读者协议快照:entry 先于 count(同 search;entry 发布 happens-after
    // 其 count 发布)。n 之后追加的节点/反向边一律不进本快照。
    const std::uint64_t em = entry_meta_.load(std::memory_order_acquire);
    const std::uint32_t n  = count_.load(std::memory_order_acquire);
    // entry 必 < n(发布序保证);防御性兜底:不一致就放弃本次快照。
    if (em != 0 && static_cast<std::uint32_t>(em & 0xFFFFFFFFu) >= n) {
        return false;
    }

    buf.clear();
    // 粗估:头 64B + 每节点 17+dim + 邻接上限 ~ (1+M*2)*4*(level+1)
    buf.reserve(64 + static_cast<std::size_t>(n) *
                         (24 + static_cast<std::size_t>(cfg_.dim) +
                          (1 + cfg_.M * 2) * 4 * 8));
    vs_put32(buf, kBcvhMagic);
    vs_put32(buf, kBcvhVersion);
    const std::uint32_t flags = cfg_.inmem_int8 ? 0u : 1u;  // bit 0 = has_payload
    vs_put32(buf, flags);
    vs_put16(buf, cfg_.dim);
    buf.push_back(static_cast<std::uint8_t>(cfg_.metric));
    vs_put32(buf, cfg_.M);
    vs_put32(buf, cfg_.ef_construction);
    vs_put64(buf, cfg_.seed);
    vs_put32(buf, n);
    vs_put64(buf, em);
    // 落盘水位 = 已保存节点的最大 ord(见 hpp:不抄 max_inserted_ord_ 原子,
    // 防 mid-insert 领先 count 的窗口;ord 按插入序单调 → 尾节点即最大)。
    vs_put64(buf, n > 0 ? ord_of(n - 1) : static_cast<std::uint64_t>(-1));

    std::vector<std::uint32_t> scratch(1 + cfg_.M * 2);
    const std::size_t qbytes =
        static_cast<std::size_t>(cfg_.dim) * sizeof(std::int8_t);
    for (std::uint32_t id = 0; id < n; ++id) {
        const NodeChunk* c = chunk_of(id);
        const std::uint32_t slot = id & kChunkMask;
        vs_put64(buf, c->ords[slot]);
        const std::uint8_t level = c->levels[slot];
        buf.push_back(level);
        // V7:直存 qcodes/qscale/qsum——省启动量化 pass(V1 是读盘后量化)。
        // needs_qcodes_ 为假(kL2/无 VNNI)时三段未分配:写零占位,保持盘上
        // 格式定长不变(读端按 needs_qcodes_ 决定存或跳)。
        if (needs_qcodes_) {
            const std::int8_t* qcodes =
                c->qcodes.data() + static_cast<std::size_t>(slot) * cfg_.dim;
            buf.insert(buf.end(),
                       reinterpret_cast<const std::uint8_t*>(qcodes),
                       reinterpret_cast<const std::uint8_t*>(qcodes) + qbytes);
            const auto* sp =
                reinterpret_cast<const std::uint8_t*>(&c->qscales[slot]);
            buf.insert(buf.end(), sp, sp + sizeof(float));
            const auto* zp =
                reinterpret_cast<const std::uint8_t*>(&c->qsums[slot]);
            buf.insert(buf.end(), zp, zp + sizeof(std::int32_t));
        } else {
            buf.insert(buf.end(), qbytes + sizeof(float) + sizeof(std::int32_t),
                       std::uint8_t{0});
        }
        for (std::uint32_t l = 0; l <= level; ++l) {
            // 持节点锁拷邻接(与并发写者互斥);≥ n 的邻居(快照水位外的
            // 反向边)滤掉,保证文件内不变量 id < count。
            const std::uint32_t cnt = copy_neighbors(id, l, scratch.data());
            std::uint32_t kept = 0;
            for (std::uint32_t i = 0; i < cnt; ++i) {
                if (scratch[i] < n) ++kept;
            }
            vs_put32(buf, kept);
            for (std::uint32_t i = 0; i < cnt; ++i) {
                if (scratch[i] < n) vs_put32(buf, scratch[i]);
            }
        }
    }

    // CRC 覆盖 magic..last_neighbor,即整个 buf 减去尾部 4 字节 CRC。
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    vs_put32(buf, crc);
    return true;
}

bool HnswIndex::save(std::string_view base_path) const {
    // V7:双文件——.vec 是 payload,.ckpt 是头。先 payload 后头:若 .ckpt 写
    // 成功而 .vec 缺失,load 端按 has_payload 拒收;反之 .vec 孤儿文件由
    // 下次 save 覆盖。
    const std::string bp(base_path);
    const std::string vec_path = bp + ".vec";
    if (!save_vec_payload(vec_path)) return false;
    std::vector<std::uint8_t> buf;
    if (!serialize(buf)) return false;
    const std::string tmp = bp + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!wrote || std::rename(tmp.c_str(), bp.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool HnswIndex::load(std::string_view base_path) {
    const std::string bp(base_path);
    std::FILE* f = std::fopen(bp.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long fsz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsz < 0) { std::fclose(f); return false; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(fsz));
    const bool rd = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!rd) return false;
    if (!deserialize(buf)) return false;
    // V7:deserialize 之后装 payload。inmem_int8 或 count=0 不读 .vec。
    if (!cfg_.inmem_int8 && count_.load(std::memory_order_relaxed) > 0) {
        const std::string vec_path = bp + ".vec";
        if (!load_vec_payload(vec_path)) return false;
    }
    return true;
}

bool HnswIndex::deserialize(std::span<const std::uint8_t> buf) {
    // open 期单线程(本实例尚未发布给任何读者)——成员虽是 atomic,直填
    // relaxed 即可;对外可见性由调用方的发布点(shared_ptr atomic store /
    // count_ release)建立。
    assert(count_.load(std::memory_order_relaxed) == 0 &&
           "HnswIndex::deserialize: 仅限空图(open 期)调用");

    // 防御性释放残留 chunk:契约要求空图调用,但失败后在同一实例重试,
    // 下方分配循环会覆盖旧 chunk 指针而泄漏(assert 在 release 被编译掉)。
    for (auto& slot : chunks_) {
        delete slot.exchange(nullptr, std::memory_order_relaxed);
    }
    checkpoint_count_ = 0;

    // 最小:magic(4)+ver(4)+flags(4)+header(39)+至少一个空邻接(4)+crc(4)。
    if (buf.size() < 51 + 4 + 4) return false;

    auto rd32at = [&](std::size_t off) {
        std::uint32_t v;
        std::memcpy(&v, buf.data() + off, 4);
        return v;
    };
    if (rd32at(0) != kBcvhMagic || rd32at(4) != kBcvhVersion) return false;

    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + buf.size() - 4, 4);
    // CRC 覆盖 magic..last_neighbor(整个 buf 减去尾部 4 字节)。
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size() - 4));
    if (crc != stored_crc) return false;

    const std::uint8_t* p   = buf.data() + 8;
    const std::uint8_t* end = buf.data() + buf.size() - 4;
    auto need = [&](std::size_t nb) {
        return static_cast<std::size_t>(end - p) >= nb;
    };

    std::uint32_t flags;
    std::memcpy(&flags, p, 4); p += 4;
    const bool has_payload = (flags & 1u) != 0;  // 留档,校验交由 load 路径
    (void)has_payload;
    std::uint16_t dim;
    std::memcpy(&dim, p, 2); p += 2;
    const std::uint8_t metric = *p++;
    std::uint32_t m, efc;
    std::memcpy(&m, p, 4); p += 4;
    std::memcpy(&efc, p, 4); p += 4;
    p += 8;  // seed:信息留档,不参与校验(层数随机性不影响图有效性)
    (void)efc;
    // config 一致性:dim/metric/M 决定布局与距离语义,任一不符整体拒绝。
    if (dim != cfg_.dim || metric != static_cast<std::uint8_t>(cfg_.metric) ||
        m != cfg_.M) {
        return false;
    }

    std::uint32_t cnt = 0;
    std::uint64_t em = 0, max_ord = 0;
    std::memcpy(&cnt, p, 4); p += 4;
    std::memcpy(&em, p, 8); p += 8;
    std::memcpy(&max_ord, p, 8); p += 8;
    if (cnt > kMaxChunks * static_cast<std::uint64_t>(kChunkSize)) return false;
    if (cnt == 0) {
        // 空图:entry 必须也为空,水位必须为 -1。
        if (em != 0 || max_ord != static_cast<std::uint64_t>(-1) || p != end) {
            return false;
        }
        max_inserted_ord_.store(max_ord, std::memory_order_relaxed);
        entry_meta_.store(em, std::memory_order_relaxed);
        count_.store(cnt, std::memory_order_release);
        return true;
    }
    const auto entry_id    = static_cast<std::uint32_t>(em & 0xFFFFFFFFu);
    const auto entry_level = static_cast<std::int64_t>(em >> 32) - 1;
    if (em == 0 || entry_id >= cnt || entry_level < 0 || entry_level > 31) {
        return false;
    }

    const std::size_t qbytes =
        static_cast<std::size_t>(cfg_.dim) * sizeof(std::int8_t);
    std::uint64_t prev_ord = 0;
    bool have_prev = false;
    for (std::uint32_t id = 0; id < cnt; ++id) {
        const std::uint32_t ci = id >> kChunkBits;
        NodeChunk* c = chunks_[ci].load(std::memory_order_relaxed);
        if (c == nullptr) {
        c = new NodeChunk(cfg_.dim, false, needs_qcodes_);
            chunks_[ci].store(c, std::memory_order_relaxed);
        }
        const std::uint32_t slot = id & kChunkMask;

        if (!need(13 + qbytes)) return false;
        std::uint64_t ord;
        std::memcpy(&ord, p, 8); p += 8;
        // ord 严格递增是写者不变量(插入序分配),也是水位幂等的前提。
        if (have_prev && ord <= prev_ord) return false;
        prev_ord = ord;
        have_prev = true;
        const std::uint8_t level = *p++;
        if (level > 31) return false;
        // V7:直读 qcodes(免去 V1 的启动量化 pass)。needs_qcodes_ 为假时
        // 盘上是零占位且本地未分配——跳过存储,仅推进游标。
        if (needs_qcodes_) {
            std::memcpy(c->qcodes.data() +
                            static_cast<std::size_t>(slot) * cfg_.dim,
                        p, qbytes);
            p += qbytes;
            std::memcpy(&c->qscales[slot], p, sizeof(float));
            p += sizeof(float);
            std::memcpy(&c->qsums[slot],   p, sizeof(std::int32_t));
            p += sizeof(std::int32_t);
        } else {
            p += qbytes + sizeof(float) + sizeof(std::int32_t);
        }
        c->ords[slot]   = ord;
        c->levels[slot] = level;
        const std::size_t slots =
            (1 + cfg_.M * 2) + static_cast<std::size_t>(level) * (1 + cfg_.M);
        c->adj[slot] = c->alloc_adj(slots);
        auto* adj = c->adj[slot];
        for (std::uint32_t l = 0; l <= level; ++l) {
            if (!need(4)) return false;
            std::uint32_t nb_cnt;
            std::memcpy(&nb_cnt, p, 4); p += 4;
            if (nb_cnt > layer_cap(l)) return false;
            if (!need(static_cast<std::size_t>(nb_cnt) * 4)) return false;
            std::uint32_t* row = adj + layer_off(l);
            row[0] = nb_cnt;
            for (std::uint32_t i = 0; i < nb_cnt; ++i) {
                std::uint32_t nid;
                std::memcpy(&nid, p, 4); p += 4;
                if (nid >= cnt || nid == id) return false;
                row[i + 1] = nid;
            }
        }
    }
    if (p != end) return false;
    // 水位与尾节点 ord 必一致(save 即按此落盘;不符 = 文件不自洽)。
    if (max_ord != prev_ord) return false;

    // 第二遍:邻居层数覆盖校验——layer-l 表只允许 level ≥ l 的节点,
    // 否则 copy_neighbors(nid, l) 会越过其邻接块(内存安全,非仅逻辑)。
    // 顺带校验 entry 的 level 与 entry_meta 一致。
    auto level_of = [&](std::uint32_t id) -> std::uint8_t {
        return chunks_[id >> kChunkBits]
            .load(std::memory_order_relaxed)
            ->levels[id & kChunkMask];
    };
    if (level_of(entry_id) != static_cast<std::uint8_t>(entry_level)) {
        return false;
    }
    for (std::uint32_t id = 0; id < cnt; ++id) {
        const NodeChunk* c = chunk_of(id);
        const std::uint32_t slot = id & kChunkMask;
        const std::uint32_t* adj = c->adj[slot];
        for (std::uint32_t l = 0; l <= c->levels[slot]; ++l) {
            const std::uint32_t* row = adj + layer_off(l);
            for (std::uint32_t i = 1; i <= row[0]; ++i) {
                if (level_of(row[i]) < l) return false;
            }
        }
    }

    max_inserted_ord_.store(max_ord, std::memory_order_relaxed);
    entry_meta_.store(em, std::memory_order_relaxed);
    count_.store(cnt, std::memory_order_release);
    return true;
}

}  // namespace bitcask::vec
