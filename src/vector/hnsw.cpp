// HNSW 实现(V3.3 单写者 + 多读者)。算法对应 Malkov & Yashunin 2016;
// 工程选择见 doc/hnsw-design-zh.md §2,并发协议见 §3 与 hnsw.hpp 文件头。

#include "bitcask/hnsw.hpp"
#include "bitcask/detail/cpu_features.hpp"
#include "bitcask/codec.hpp"
#include "bitcask/detail/file_util.hpp"  // detail::FilePtr（RED-2 归并）
#include "hnsw_kernels.hpp"
#include "vec_disk_internal.hpp"  // diskint::pwrite_all（RED-7 归并）

#include <oneapi/tbb/parallel_for.h>       // S7-6：int8 路径 f32 精排距离批算并行

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <queue>
#include <string>

// V7:BCVS v2 payload 文件 mmap(只读 MAP_SHARED + madvise RANDOM)。

// S37-3.b：ISA 守卫改用 BITCASK_X86_64（原 __x86_64__ && (GNUC||clang)——
// MSVC 用 _M_X64，且「是不是 GCC/Clang」这半个条件只是因为内核用了 GCC
// 扩展，扩展消除后不该再有）。_mm_prefetch 是 SSE 基线 intrinsic，
// MSVC 下无需 /arch 开关。
#if BITCASK_X86_64
#include <immintrin.h>
#endif

namespace bitcask::vec {

// S7-6：int8 路径精排阶段对 found（≈ef 个候选）逐个算 f32 距离。候选数 ≥ 此
// 阈值才并行批算（甜区：大 ef / 大 k 的向量查询，如 k=256 → ef≥256，~503µs）。
// 小 ef 并行 task 唤醒开销 > 收益，走串行（同 S7-1/S7-5 的门控教训）。
// 并行**确定性**：各 task 只写自己的 found[i].first（互异下标），随后 partial_sort
// 串行 → 与串行逐字节同果，不改召回/排序。
namespace {
constexpr std::size_t kRerankParallelThreshold = 512;
}  // namespace

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


}  // namespace detail

namespace {

// V3.8:候选向量软件预取。大图下每个候选是 ~1.5KB(384d)的冷 DRAM
// 取数,先扫一遍邻居把向量首 256B 拉向 L1,再进距离循环——取数与计算
// 重叠,后续行交给硬件流预取。非 x86 为空操作。
inline void prefetch_vec(const float* p, std::size_t dim) {
#if BITCASK_X86_64
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
#if BITCASK_X86_64
    // V3.9:AVX-512F 优先(超集)。要求仅基础 AVX-512 Foundation,无 BW/VL,
    // 覆盖 Skylake-SP / Ice Lake / Zen4。运行时一次探测,零查询开销。
    static const bool kAvx512f = simd::have_avx512();  // S37-3：整集门
    if (kAvx512f) {
        return metric == HnswMetric::kDot ? detail::dot_avx512
                                          : detail::l2_avx512;
    }
    static const bool kAvx2 = simd::have_avx2_fma();  // S37-3
    if (kAvx2) {
        return metric == HnswMetric::kDot ? detail::dot_avx2
                                          : detail::l2_avx2;
    }
#endif
    return metric == HnswMetric::kDot ? detail::dot_scalar
                                      : detail::l2_scalar;
}

inline void cpu_pause() {
#if BITCASK_X86_64
    _mm_pause();  // S37-3.b：原 __builtin_ia32_pause（GCC 专有）；_mm_pause
                  // 是 SSE2 基线 intrinsic，MSVC/GCC/Clang 通用，无需 -m 开关
#endif
}

// per-node 自旋锁(1 字节,test-and-set + pause;临界区 ~百 ns)。
// S13-P7：per-node seqlock（写者单线程 → 无写-写互斥需求）。
// 写者：seq→奇（进入）… atomic_ref relaxed 写 adj … seq→偶（release 发布）。
// 读者：acquire 读 seq（奇则退避）→ relaxed 读数据 → acquire fence → 复读
// seq 一致才采信。数据字读写均经 atomic_ref → 无非原子冲突访问（TSan 干净）。
// 注：节点发布（count release）前的 adj 初始化是单线程预发布阶段的普通写，
// 与发布后的 atomic_ref 访问经 happens-before 分隔，不构成并发混用。
inline void seq_write_begin(std::atomic<std::uint32_t>& seq) {
    seq.fetch_add(1, std::memory_order_relaxed);  // → 奇
    std::atomic_thread_fence(std::memory_order_release);
}
inline void seq_write_end(std::atomic<std::uint32_t>& seq) {
    seq.fetch_add(1, std::memory_order_release);  // → 偶，发布本轮更新
}
inline std::uint32_t adj_load(const std::uint32_t* p) {
    return std::atomic_ref<const std::uint32_t>(*p).load(
        std::memory_order_relaxed);
}
inline void adj_store(std::uint32_t* p, std::uint32_t v) {
    std::atomic_ref<std::uint32_t>(*p).store(v, std::memory_order_relaxed);
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
      locks(new std::atomic<std::uint32_t>[kChunkSize]),
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
    vecs_map_.reset();  // B3：RAII munmap
    vecs_mmap_base_ = nullptr;
    if (vecs_payload_fd_ >= 0) {
        io::close_handle(vecs_payload_fd_);
        vecs_payload_fd_ = -1;
    }
    // S32-M2:qc8 mmap 同序释放。
    qc_map_.reset();
    qc_mmap_recs_ = nullptr;
    if (qc_payload_fd_ >= 0) {
        io::close_handle(qc_payload_fd_);
        qc_payload_fd_ = -1;
    }
    for (auto& slot : chunks_) {
        delete slot.load(std::memory_order_relaxed);
    }
}

namespace {

// V7:BCVS v2 段头 magic/version(search.ckpt kHnsw 段内嵌)。
constexpr std::uint32_t kBcvhMagic   = 0x32485642;  // "BVH2" (LE)
constexpr std::uint32_t kBcvhVersion = 2;
// S14-8:v3——码字外置 search.qc8，段内仅 ord/level/邻接 + payload_gen。
constexpr std::uint32_t kBcvhVersion3 = 3;

// S14-8:BCQ8 码字 payload 文件（append-only，前缀契约同 .vec）。
// 64B 头：magic|ver u32|dim u16|count u32|gen u64|rec_off u64|total u64
// |reserved|hcrc u32@60。记录区 rec_off=64 起，定长 stride = dim+8
// （qcodes int8[dim] | qscale f32 | qsum i32），按 node id 索引。
constexpr char          kBcq8Magic[4] = {'B', 'C', 'Q', '8'};
constexpr std::uint32_t kBcq8Version = 1;
constexpr std::size_t   kBcq8HeaderSize = 64;
constexpr std::uint32_t kBcq8HeaderCrcOff = kBcq8HeaderSize - 4;
// BCVP 头 reserved 区里 gen 的落位（[46..54)，v1 旧文件读出为 0 = 不校验）。
constexpr std::size_t   kBcvpGenOff = 46;

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

// S32-M2b:BCVP/BCQ8 全量写盘助手——save_*_payload 的全量兜底与 clone_live
// 流式外溢共用一份格式实现(位级同源,防双实现漂移)。取数回调按**输出文件
// 记录序**(0..n-1)调用,id 重映射由调用方在回调内完成。tmp+rename 原子;
// 失败清 tmp。峰值内存 = 头区(头+CRC 表)+ 一页。
template <typename GetVec>
bool write_bcvp_file(const std::string& fp, std::uint16_t dim, std::uint32_t n,
                     std::uint64_t watermark, std::uint64_t gen,
                     GetVec&& get_vec, std::uint64_t* out_vecs_off) {
    const std::size_t vec_bytes = static_cast<std::size_t>(dim) * sizeof(float);
    const std::size_t total_vecs = static_cast<std::size_t>(n) * vec_bytes;
    const std::uint32_t crc_count =
        total_vecs == 0
            ? 0u
            : static_cast<std::uint32_t>((total_vecs + kBcvpPageSize - 1) /
                                         kBcvpPageSize);
    std::size_t vecs_off =
        kBcvpHeaderSize + static_cast<std::size_t>(crc_count) * 4;
    if (total_vecs > 0) {
        vecs_off = (vecs_off + kBcvpPageSize - 1) &
                   ~(static_cast<std::size_t>(kBcvpPageSize) - 1);
    }

    std::vector<std::uint8_t> head(vecs_off, 0);
    head[0] = kBcvpMagic[0]; head[1] = kBcvpMagic[1];
    head[2] = kBcvpMagic[2]; head[3] = kBcvpMagic[3];
    std::memcpy(head.data() + 4,  &kBcvpVersion,  4);
    std::memcpy(head.data() + 8,  &dim,           2);
    std::memcpy(head.data() + 10, &n,             4);
    std::memcpy(head.data() + 14, &watermark,     8);
    std::memcpy(head.data() + 22, &kBcvpPageSize, 4);
    std::memcpy(head.data() + 26, &vecs_off,      8);
    std::memcpy(head.data() + 34, &total_vecs,    8);
    std::memcpy(head.data() + 42, &crc_count,     4);
    std::memcpy(head.data() + kBcvpGenOff, &gen,  8);
    const std::uint32_t header_crc = bitcask::codec::crc32(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(head.data()),
            kBcvpHeaderCrcOff));
    std::memcpy(head.data() + kBcvpHeaderCrcOff, &header_crc, 4);

    // T21：流式（分页 CRC + 回头补头）→ AtomicFileWriter；析构自动清 tmp。
    bitcask::detail::AtomicFileWriter w(fp);
    if (!w) return false;
    auto& f = w;

    bool ok = true;
    if (total_vecs > 0) {
        // 数据区：逐页组装（页与向量边界不对齐——页跨向量/向量跨页均有）。
        ok = std::fseek(f.get(), static_cast<long>(vecs_off), SEEK_SET) == 0;
        std::vector<std::uint8_t> page(kBcvpPageSize);
        std::size_t fill = 0;
        std::uint32_t pidx = 0;
        auto flush_page = [&](std::size_t len) {
            const std::uint32_t crc = bitcask::codec::crc32(
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(page.data()), len));
            std::memcpy(head.data() + kBcvpHeaderSize +
                            static_cast<std::size_t>(pidx) * 4,
                        &crc, 4);
            ++pidx;
            return std::fwrite(page.data(), 1, len, f.get()) == len;
        };
        for (std::uint32_t i = 0; ok && i < n; ++i) {
            const auto* src = reinterpret_cast<const std::uint8_t*>(get_vec(i));
            std::size_t rem = vec_bytes;
            while (ok && rem > 0) {
                const std::size_t take =
                    std::min(static_cast<std::size_t>(kBcvpPageSize) - fill,
                             rem);
                std::memcpy(page.data() + fill, src, take);
                src += take;
                rem -= take;
                fill += take;
                if (fill == kBcvpPageSize) {
                    ok = flush_page(kBcvpPageSize);
                    fill = 0;
                }
            }
        }
        if (ok && fill > 0) ok = flush_page(fill);  // 尾页（< 4KB）
    }
    // 回头补写 header + CRC 表。
    if (ok) ok = std::fseek(f.get(), 0, SEEK_SET) == 0;
    if (ok) ok = std::fwrite(head.data(), 1, head.size(), f.get()) ==
                 head.size();
    if (!ok || !w.commit()) return false;  // commit 内含 P6-DUR-1 的 fdatasync
    if (out_vecs_off != nullptr) *out_vecs_off = vecs_off;
    return true;
}

template <typename GetCodes, typename GetScale, typename GetSum>
bool write_bcq8_file(const std::string& fp, std::uint16_t dim, std::uint32_t n,
                     std::uint64_t gen, GetCodes&& get_codes,
                     GetScale&& get_scale, GetSum&& get_sum) {
    const std::size_t stride = static_cast<std::size_t>(dim) + sizeof(float) +
                               sizeof(std::int32_t);
    std::uint8_t hdr[kBcq8HeaderSize] = {0};
    std::memcpy(hdr + 0, kBcq8Magic, 4);
    std::memcpy(hdr + 4, &kBcq8Version, 4);
    std::memcpy(hdr + 8, &dim, 2);
    std::memcpy(hdr + 10, &n, 4);
    std::memcpy(hdr + 14, &gen, 8);
    const std::uint64_t rec_off = kBcq8HeaderSize;
    std::memcpy(hdr + 22, &rec_off, 8);
    const std::uint64_t total = static_cast<std::uint64_t>(n) * stride;
    std::memcpy(hdr + 30, &total, 8);
    const std::uint32_t hcrc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hdr), kBcq8HeaderCrcOff));
    std::memcpy(hdr + kBcq8HeaderCrcOff, &hcrc, 4);

    // T21：流式（批量 append）→ AtomicFileWriter；析构自动清 tmp。
    bitcask::detail::AtomicFileWriter w(fp);
    if (!w) return false;
    auto& f = w;
    bool ok = std::fwrite(hdr, 1, kBcq8HeaderSize, f.get()) == kBcq8HeaderSize;
    std::vector<std::uint8_t> batch;
    batch.reserve(std::min<std::size_t>(4096, n ? n : 1) * stride);
    for (std::uint32_t i = 0; ok && i < n; ++i) {
        const auto* q = reinterpret_cast<const std::uint8_t*>(get_codes(i));
        batch.insert(batch.end(), q, q + dim);
        const float s = get_scale(i);
        const auto* sp = reinterpret_cast<const std::uint8_t*>(&s);
        batch.insert(batch.end(), sp, sp + sizeof(float));
        const std::int32_t z = get_sum(i);
        const auto* zp = reinterpret_cast<const std::uint8_t*>(&z);
        batch.insert(batch.end(), zp, zp + sizeof(std::int32_t));
        if (batch.size() >= 4096 * stride || i + 1 == n) {
            ok = std::fwrite(batch.data(), 1, batch.size(), f.get()) ==
                 batch.size();
            batch.clear();
        }
    }
    return ok && w.commit();  // commit 内含 P6-DUR-1 的 fdatasync
}

}  // namespace

// S13-P8：结构化拷贝活子图（契约见头文件）。
std::shared_ptr<HnswIndex>
HnswIndex::clone_live(const std::function<bool(std::uint64_t)>& is_live,
                      std::string_view spill_vec_path,
                      std::string_view spill_qc_path) const {
    auto fresh = std::make_shared<HnswIndex>(cfg_);
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    fresh->max_inserted_ord_.store(
        max_inserted_ord_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    if (n == 0) return fresh;

    // pass 0：old_id → new_id 重映射（活节点按 id 序紧凑编号 = ord 序保持）。
    constexpr std::uint32_t kDead = 0xFFFFFFFFu;
    std::vector<std::uint32_t> remap(n, kDead);
    std::uint32_t nn = 0;
    for (std::uint32_t id = 0; id < n; ++id) {
        if (is_live(node_ord(id))) remap[id] = nn++;
    }
    if (nn == 0) return fresh;

    // S32-M2b：payload 外溢（契约见头文件）——活集 f32/码字从旧图（含
    // mmap 段）按 remap 直接流式写新 payload 文件，fresh 以 mmap attach，
    // 堆上不物化第二份数据。任一环节失败按种类回退堆拷贝（已 rename 的
    // 文件被后续 save 以同 gen 覆盖/收养，无一致性残留）。
    bool spill_vec = false;
    bool spill_qc  = false;
    {
        const bool want_vec = !spill_vec_path.empty() && !cfg_.inmem_int8;
        const bool want_qc  = !spill_qc_path.empty() && needs_qcodes_;
        if (want_vec || want_qc) {
            // 输出记录序（新 id）→ 旧 id 查表（nn × 4B，临时）。
            std::vector<std::uint32_t> old_of_new(nn);
            for (std::uint32_t id = 0; id < n; ++id) {
                if (remap[id] != kDead) old_of_new[remap[id]] = id;
            }
            fresh->ensure_payload_gen();
            const std::uint64_t wm =
                max_inserted_ord_.load(std::memory_order_relaxed);
            bool ok = true;
            if (want_vec) {
                ok = write_bcvp_file(
                    std::string(spill_vec_path), cfg_.dim, nn, wm,
                    fresh->payload_gen_,
                    [&](std::uint32_t i) { return vec_of(old_of_new[i]); },
                    nullptr);
            }
            if (ok && want_qc) {
                ok = write_bcq8_file(
                    std::string(spill_qc_path), cfg_.dim, nn,
                    fresh->payload_gen_,
                    [&](std::uint32_t i) { return qcodes_of(old_of_new[i]); },
                    [&](std::uint32_t i) { return qscale_of(old_of_new[i]); },
                    [&](std::uint32_t i) { return qsum_of(old_of_new[i]); });
            }
            if (ok) {
                // attach：load_* 需 count_ 就位（fresh 未发布无并发读者，
                // relaxed 即可；尾部仍以 release 重存同值）。
                fresh->count_.store(nn, std::memory_order_relaxed);
                if (want_vec) {
                    spill_vec = fresh->load_vec_payload(spill_vec_path);
                }
                if (want_qc) {
                    fresh->qc_pending_ = true;
                    spill_qc = fresh->load_qc_payload(spill_qc_path);
                    if (!spill_qc) fresh->qc_pending_ = false;
                }
            }
        }
    }

    // pass 1：节点数据（vec/qcodes/ord/level）+ 邻接块分配（零初始化）。
    // S32-M2b：已外溢的种类不进堆（chunk 容量 0，数据在 fresh 的 mmap）。
    std::uint32_t best_level = 0;
    std::uint32_t best_new_id = 0;
    bool have_entry = false;
    for (std::uint32_t old_id = 0; old_id < n; ++old_id) {
        const std::uint32_t new_id = remap[old_id];
        if (new_id == kDead) continue;
        const std::uint32_t ci = new_id >> kChunkBits;
        NodeChunk* c = fresh->chunks_[ci].load(std::memory_order_relaxed);
        if (c == nullptr) {
            c = new NodeChunk(cfg_.dim, !cfg_.inmem_int8 && !spill_vec,
                              fresh->needs_qcodes_ && !spill_qc);
            fresh->chunks_[ci].store(c, std::memory_order_release);
        }
        const std::uint32_t slot = new_id & kChunkMask;
        const NodeChunk* oc = chunk_of(old_id);
        const std::uint32_t oslot = old_id & kChunkMask;
        const std::uint32_t level = oc->levels[oslot];

        if (!cfg_.inmem_int8 && !spill_vec) {
            // vec_of 统一路由 mmap 段与 hot chunk 段。
            std::memcpy(c->vecs.data() +
                            static_cast<std::size_t>(slot) * cfg_.dim,
                        vec_of(old_id),
                        static_cast<std::size_t>(cfg_.dim) * sizeof(float));
        }
        if ((fresh->needs_qcodes_ || cfg_.inmem_int8) && !spill_qc) {
            // 量化副本直拷——不做反量化→再量化往返（无损、免两遍标量运算）。
            std::memcpy(c->qcodes.data() +
                            static_cast<std::size_t>(slot) * cfg_.dim,
                        qcodes_of(old_id),
                        static_cast<std::size_t>(cfg_.dim));
            c->qscales[slot] = qscale_of(old_id);
            c->qsums[slot]   = qsum_of(old_id);
        }
        c->ords[slot]   = oc->ords[oslot];
        c->levels[slot] = static_cast<std::uint8_t>(level);
        const std::size_t slots =
            (1 + cfg_.M * 2) + static_cast<std::size_t>(level) * (1 + cfg_.M);
        c->adj[slot] = c->alloc_adj(slots);

        if (!have_entry || level > best_level) {
            have_entry = true;
            best_level = level;
            best_new_id = new_id;
        }
    }

    // pass 2：邻接重映射 + 死邻过滤（一跳路径收缩补边）。
    // 本线程是唯一写者：旧图 adj 无并发写，普通读安全（并发读者只读不冲突）。
    std::vector<std::uint32_t> merged;
    for (std::uint32_t old_id = 0; old_id < n; ++old_id) {
        const std::uint32_t new_id = remap[old_id];
        if (new_id == kDead) continue;
        const NodeChunk* oc = chunk_of(old_id);
        const std::uint32_t oslot = old_id & kChunkMask;
        NodeChunk* c = fresh->chunk_of(new_id);
        const std::uint32_t slot = new_id & kChunkMask;
        const std::uint32_t level = oc->levels[oslot];

        for (std::uint32_t l = 0; l <= level; ++l) {
            const std::uint32_t* src = oc->adj[oslot] + layer_off(l);
            std::uint32_t* dst = c->adj[slot] + fresh->layer_off(l);
            const std::uint32_t cap = fresh->layer_cap(l);
            merged.clear();
            const std::uint32_t cnt = src[0];
            for (std::uint32_t i = 1; i <= cnt && merged.size() < cap; ++i) {
                const std::uint32_t nb = src[i];
                if (nb >= n) continue;  // 超出快照边界（并发插入的新节点）
                const std::uint32_t r = remap[nb];
                if (r != kDead && r != new_id) merged.push_back(r);
            }
            if (merged.empty() && cnt > 0) {
                // 一跳路径收缩：全部直接邻居已死——借道死邻的活邻居补边，
                // 保持该层连通性（否则本节点该层出边为空，可达性劣化）。
                for (std::uint32_t i = 1;
                     i <= cnt && merged.size() < cap; ++i) {
                    const std::uint32_t dead_nb = src[i];
                    if (dead_nb >= n || remap[dead_nb] != kDead) continue;
                    const NodeChunk* dc = chunk_of(dead_nb);
                    const std::uint32_t dslot = dead_nb & kChunkMask;
                    if (dc->levels[dslot] < l) continue;  // 防御（不应发生）
                    const std::uint32_t* dsrc = dc->adj[dslot] + layer_off(l);
                    const std::uint32_t dcnt = dsrc[0];
                    for (std::uint32_t j = 1;
                         j <= dcnt && merged.size() < cap; ++j) {
                        const std::uint32_t nb2 = dsrc[j];
                        if (nb2 >= n) continue;
                        const std::uint32_t r2 = remap[nb2];
                        if (r2 == kDead || r2 == new_id) continue;
                        if (std::find(merged.begin(), merged.end(), r2) ==
                            merged.end()) {
                            merged.push_back(r2);
                        }
                    }
                }
            }
            dst[0] = static_cast<std::uint32_t>(merged.size());
            for (std::size_t i = 0; i < merged.size(); ++i) {
                dst[1 + i] = merged[i];
            }
        }
    }

    // 发布（fresh 尚未对读者可见，本函数返回后由调用方 atomic store 换图）。
    fresh->entry_meta_.store(
        (static_cast<std::uint64_t>(best_level + 1) << 32) | best_new_id,
        std::memory_order_release);
    fresh->count_.store(nn, std::memory_order_release);
    return fresh;
}

std::uint32_t HnswIndex::copy_neighbors(std::uint32_t id, std::uint32_t layer,
                                        std::uint32_t* out) const {
    NodeChunk* c = chunk_of(id);
    const std::uint32_t slot = id & kChunkMask;
    auto& seq = c->locks[slot];
    // adj 指针在节点发布(count release)前写入且永不搬迁;经 count
    // acquire 或本锁的 happens-before 链均可见。
    const std::uint32_t* a = c->adj[slot] + layer_off(layer);
    const std::uint32_t cap = layer_cap(layer);
    // S13-P7 seqlock 读侧：纯读、零共享行写——hub 节点并发查询不再乒乓。
    // 拷贝方式分构建：TSan 构建逐字 atomic_ref（工具可见、零误报）；常规
    // 构建 memcpy（逐字循环阻断向量化，实测单线程 HNSW 查询 +17%——经典
    // seqlock 取舍：torn 读被 seq 复读检测丢弃，正确性同）。
    for (;;) {
        const std::uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) { cpu_pause(); continue; }  // 写者更新中
        const std::uint32_t n = adj_load(a);
        if (n > cap) { cpu_pause(); continue; }  // torn count，防 out 越界
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
        for (std::uint32_t i = 0; i < n; ++i) out[i] = adj_load(a + 1 + i);
#else
        std::memcpy(out, a + 1,
                    static_cast<std::size_t>(n) * sizeof(std::uint32_t));
#endif
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == s1) return n;
        // 期间有写者插入 → 丢弃重读。
    }
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
    out.reserve(ef);  // D1:保容量 ≥ ef，后续 clear+resize 不 realloc。
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

    // B5:从 thread_local buffer move 构造（保容量），函数尾 extract 回收。
    // Cand 用文件级全局别名（std::pair<float, std::uint32_t>）。
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
    out.reserve(ef);
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

    // Cand 用文件级全局别名（std::pair<float, std::uint32_t>）。
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
    // S29-3:thread_local 复用(对齐本文件 t_visited/pool/tl_cands_buf 模式)。
    // 每插入调用 ~10-18 次(自身选边 + 溢出邻居收缩),原每次 2 个 vector
    // 堆分配是插入路径最后的分配热点。尾部 swap 让两个缓冲轮换复用。
    // 单写者协议(insert 断言)保证无重入。
    static thread_local std::vector<std::pair<float, std::uint32_t>> picked;
    picked.clear();
    picked.reserve(m);
    static thread_local std::vector<const float*> picked_vecs;  // D6:缓存 vec_of，免内层循环冗余取指。
    picked_vecs.clear();
    picked_vecs.reserve(m);
    for (const auto& [d, id] : cands) {
        if (picked.size() >= m) break;
        bool ok = true;
        const float* v = vec_of(id);
        for (std::size_t pi = 0; pi < picked_vecs.size(); ++pi) {
            if (dist_(v, picked_vecs[pi], cfg_.dim) < d) {
                ok = false;
                break;
            }
        }
        if (ok) {
            picked.push_back({d, id});
            picked_vecs.push_back(v);
        }
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
    std::swap(cands, picked);  // S29-3:swap 而非 move,旧缓冲留池内轮换。
}

// P5:int8-only 版 select_neighbors。与 f32 版同启发式(Algorithm 4),
// 只把候选-已选距离换成 dist_id_int8_node(两节点皆有量化副本,无 f32)。
void HnswIndex::select_neighbors_int8(
    std::vector<std::pair<float, std::uint32_t>>& cands, std::uint32_t m) const {
    if (cands.size() <= m) return;
    // S29-3:同 select_neighbors,thread_local 复用。
    static thread_local std::vector<std::pair<float, std::uint32_t>> picked;
    picked.clear();
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
    std::swap(cands, picked);  // S29-3:swap 而非 move,旧缓冲留池内轮换。
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
    } else {
        if (!cfg_.inmem_int8 && c->vecs.empty()) {
            // checkpoint 加载的 chunk(needs_vecs=false)首次插入热数据:
            // 懒分配 vecs_。单写者协议下安全(count_.store 在后,读者看不到
            // 未就绪节点);首帧 assign 无旧指针可失效。
            c->vecs.assign(
                static_cast<std::size_t>(kChunkSize) * cfg_.dim, 0.0f);
        }
        // S32-M2:qc8 mmap 化后 boundary chunk(needs_qcodes=false 创建)
        // 首次热插入同款懒分配——该 chunk 内已发布节点全 <
        // qc_checkpoint_count_,读者经 qcodes_of 路由走 mmap,不触堆数组,
        // assign 无并发读者。
        if (needs_qcodes_ && c->qcodes.empty()) {
            c->qcodes.assign(
                static_cast<std::size_t>(kChunkSize) * cfg_.dim, 0);
            c->qscales.assign(kChunkSize, 0.0f);
            c->qsums.assign(kChunkSize, 0);
        }
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
        // S13-P5：thread_local 复用（单写者路径）。
        thread_local int8::QVector qv;
        int8::quantize_into(vec.data(), cfg_.dim, qv);
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
            // S13-P5：thread_local 复用（单写者路径）。
            thread_local int8::QVector qv;
            int8::quantize_into(vec.data(), cfg_.dim, qv);
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
    // S29-11-②:混合精度——默认模式下**导航**(贪心 + 逐层 ef 搜索)也走
    // int8(工作集 4× 塌缩,打掉建图超线性),入选邻居仍 f32 精选(见下)。
    const bool i8 = cfg_.inmem_int8;
    const bool nav_i8 =
        i8 || (cfg_.build_nav_int8 && needs_qcodes_ &&
               cfg_.metric == HnswMetric::kDot && int8_dot_ != nullptr);
    // V7:vecs_ 已走 hot_vecs_;vec_of(id) 路由(< checkpoint_count_ → mmap,
    // ≥ → hot_vecs_)。本节点是刚插入的 id,hot_vecs_ 末尾正是其 vec。
    const float* q = i8 ? nullptr : vec_of(id);
    const std::int8_t* qc = nav_i8 ? qcodes_of(id) : nullptr;
    const float        qs = nav_i8 ? qscale_of(id) : 0.0f;
    const std::int32_t qsum = nav_i8 ? qsum_of(id) : 0;

    // 上层贪心下降到 level+1。
    for (std::int32_t l = max_level;
         l > static_cast<std::int32_t>(level); --l) {
        cur = nav_i8 ? greedy_closest_int8(qc, qs, qsum, cur,
                                           static_cast<std::uint32_t>(l),
                                           n_bound, scratch.data())
                     : greedy_closest(q, cur, static_cast<std::uint32_t>(l),
                                      n_bound, scratch.data());
    }

    // 3) level..0:efConstruction 搜索 + 启发式选边 + 双向连边 + 邻居收缩。
    // ⑦ thread_local：search_layer 每层 out.clear() 后填充，跨 insert 复用。
    thread_local std::vector<std::pair<float, std::uint32_t>> found;
    for (std::int32_t l = std::min<std::int32_t>(
             static_cast<std::int32_t>(level), max_level);
         l >= 0; --l) {
        const auto lay = static_cast<std::uint32_t>(l);
        if (nav_i8) {
            search_layer_int8(qc, qs, qsum, cur, cfg_.ef_construction, lay,
                              n_bound, scratch.data(), found);
        } else {
            search_layer(q, cur, cfg_.ef_construction, lay, n_bound,
                         scratch.data(), found);
        }
        cur = found.front().second;  // 下层入口 = 本层最近

        auto picked = found;
        if (i8) {
            select_neighbors_int8(picked, cfg_.M);
        } else {
            // S29-11-②:混合精度的精选侧——ef 候选(≤ef_construction 条)
            // 重算 f32 距离后走 f32 启发式选边(候选重算成本 ≪ 导航
            // 全程,召回损失被压到入选边界的重排噪声)。
            if (nav_i8) {
                for (auto& pr : picked) pr.first = dist_id(q, pr.second);
                std::sort(picked.begin(), picked.end());
            }
            select_neighbors(q, picked, cfg_.M);  // L0 也选 M,容量 2M 留收缩余量
        }

        // 正向边:本节点已发布,读者可能在拷它的邻居 → 持自身锁写。
        {
            auto& my_seq = c->locks[slot];
            seq_write_begin(my_seq);
            std::uint32_t* my = c->adj[slot] + layer_off(lay);
            std::uint32_t cnt = adj_load(my);
            for (const auto& [d, nid] : picked) {
                adj_store(my + 1 + cnt, nid);
                ++cnt;
            }
            adj_store(my, cnt);
            seq_write_end(my_seq);
        }

        // 反向边 + 超容收缩:逐邻居持其锁改其邻接。
        for (const auto& [d, nid] : picked) {
            NodeChunk* nc = chunk_of(nid);
            const std::uint32_t nslot = nid & kChunkMask;
            auto& nseq = nc->locks[nslot];
            seq_write_begin(nseq);
            std::uint32_t* nb = nc->adj[nslot] + layer_off(lay);
            const std::uint32_t cap = layer_cap(lay);
            const std::uint32_t ncnt = adj_load(nb);
            if (ncnt < cap) {
                adj_store(nb + 1 + ncnt, id);
                adj_store(nb, ncnt + 1);
            } else {
                // 收缩:旧邻居 + 新候选并集,以 nid 为查询点重选 cap 条。
                // 持锁做距离计算(微秒级临界区):读者只在 copy_neighbors
                // 短暂争同一把锁,实测可接受;arena/锁外预选留 V3.x。
                // ⑦ thread_local：单写者 insert 收缩路径复用，clear 保留容量。
                thread_local std::vector<std::pair<float, std::uint32_t>> pool;
                pool.clear();
                pool.reserve(cap + 1);
                if (i8) {
                    for (std::uint32_t i = 1; i <= ncnt; ++i) {
                        const std::uint32_t nn = adj_load(nb + i);
                        pool.push_back({dist_id_int8_node(nid, nn), nn});
                    }
                    pool.push_back({dist_id_int8_node(nid, id), id});
                } else {
                    const float* nv = vec_of(nid);
                    for (std::uint32_t i = 1; i <= ncnt; ++i) {
                        const std::uint32_t nn = adj_load(nb + i);
                        pool.push_back({dist_id(nv, nn), nn});
                    }
                    pool.push_back({dist_id(nv, id), id});
                }
                std::sort(pool.begin(), pool.end());
                if (i8) select_neighbors_int8(pool, cap);
                else    select_neighbors(vec_of(nid), pool, cap);
                for (std::uint32_t i = 0; i < pool.size(); ++i) {
                    adj_store(nb + 1 + i, pool[i].second);
                }
                adj_store(nb, static_cast<std::uint32_t>(pool.size()));
            }
            seq_write_end(nseq);
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
        // S13-P5：thread_local 复用，消除每查询一次 codes 堆分配。
        thread_local int8::QVector qq;
        int8::quantize_into(q, cfg_.dim, qq);
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
                // S13-P5：候选按地址排序 + 相邻页区间合并后批量 madvise——
                // 原逐候选一次 syscall（k=256 时 ~768 次/查询），页常驻稳态
                // 下纯开销。合并后典型降到个位数 syscall。
                const std::size_t vec_bytes =
                    static_cast<std::size_t>(cfg_.dim) * sizeof(float);
                const auto page =
                    static_cast<std::uintptr_t>(io::page_size());
                const std::uintptr_t pmask = ~(page - 1);
                thread_local std::vector<std::uintptr_t> addrs;
                addrs.clear();
                for (std::size_t i = 0; i < rerank_n; ++i) {
                    const std::uint32_t id = found[i].second;
                    if (id < checkpoint_count_) {
                        addrs.push_back(
                            reinterpret_cast<std::uintptr_t>(vec_of(id)));
                    }
                }
                std::sort(addrs.begin(), addrs.end());
                std::size_t i = 0;
                while (i < addrs.size()) {
                    const std::uintptr_t start = addrs[i] & pmask;
                    std::uintptr_t end =
                        (addrs[i] + vec_bytes + page - 1) & pmask;
                    std::size_t j = i + 1;
                    // 下一候选起始页 ≤ 当前区间末页 ⟹ 合并（含相邻页）。
                    while (j < addrs.size() && (addrs[j] & pmask) <= end) {
                        const std::uintptr_t e2 =
                            (addrs[j] + vec_bytes + page - 1) & pmask;
                        if (e2 > end) end = e2;
                        ++j;
                    }
                    io::prefetch_range(reinterpret_cast<const void*>(start),
                                       end - start);
                    i = j;
                }
            }
            // 先把每个候选的 f32 距离算一次写回 .first(此前存的是 int8
            // 粗筛距离),之后 partial_sort 只比较缓存的浮点值。原写法在比较
            // 器里每次重算 2 次 dist_id、排完再算第 3 次,全维 SIMD 距离被重
            // 复计算 O(N log rerank_n) 次;此处降为每候选恰好 1 次(共 N 次)。
            // 线性遍历顺序与上面的 madvise 预取顺序一致,page-in 延迟仍被藏住。
            // S7-6：候选数过阈则并行批算 f32 距离（各写自己的 found[i].first，
            // 无共享可变态 → data-race-free；确定性，见 kRerankParallelThreshold）。
            if (found.size() >= kRerankParallelThreshold) {
                auto* fp = found.data();
                tbb::parallel_for(std::size_t{0}, found.size(),
                                  [&, fp](std::size_t i) {
                                      fp[i].first = dist_id(q, fp[i].second);
                                  });
            } else {
                for (auto& [d, id] : found) d = dist_id(q, id);
            }
            std::partial_sort(found.begin(),
                              found.begin() + static_cast<std::ptrdiff_t>(rerank_n),
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


// V7:BCVP payload 文件 = 头 + 每 4KB 页 CRC32 表 + 页对齐 vecs 数据。
// tmp + rename 原子写;inmem_int8 模式无 vecs_,save_vec_payload 是 no-op。
// S14-2:常规路径改为**增量追加**（见 hnsw.hpp 声明注释），本函数体的全量
// 重写降级为兜底（新建/rebuild 后的索引、目标文件身份不符、追加 IO 失败）。
bool HnswIndex::save_vec_payload(std::string_view path) const {
    if (cfg_.inmem_int8) return true;  // 无常驻 f32 → 无 .vec 文件
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    const std::string fp(path);

    // S14-2:优先追加——已知目标文件（load 或上次全量重写收养）且节点只增
    // （HNSW id 追加分配，正常运行恒真；rebuild 产出新对象、vec_file_ 为空）。
    // 追加失败清状态退全量重写兜底。
    if (vec_file_.valid && n >= vec_file_.count) {
        if (try_append_vec_payload(path, n)) return true;
        vec_file_.valid = false;
    }

    // S13-P8 流式写(峰值 = 头区 + 一页);S32-M2b:格式实现抽入
    // write_bcvp_file(与 clone_live 外溢共用,位级同源)。
    ensure_payload_gen();
    std::uint64_t vecs_off = 0;
    if (!write_bcvp_file(
            fp, cfg_.dim, n,
            max_inserted_ord_.load(std::memory_order_relaxed), payload_gen_,
            [this](std::uint32_t id) { return vec_of(id); }, &vecs_off)) {
        vec_file_.valid = false;  // 目标状态未知，下次全量重写
        return false;
    }
    // S14-2:收养新文件身份（rename 保 inode 不变）——后续 save 走追加。
    if (const auto ident = io::path_identity(fp)) {
        vec_file_ = VecFileState{true, *ident, vecs_off, n};
    } else {
        vec_file_.valid = false;
    }
    return true;
}

// RED-7：本地 pwrite_all 已删——统一用 vec_disk_internal.hpp 的
// diskint::pwrite_all（同签名，含 EINTR 重试审计修复）。

// S14-8:payload 代号惰性分配（幂等）。熵源：时刻 + 实例地址 + 节点数——
// 防御 rebuild 重映射后 .prev 回退误配，非加密用途。
void HnswIndex::ensure_payload_gen() const {
    if (payload_gen_ != 0) return;
    payload_gen_ =
        (static_cast<std::uint64_t>(::time(nullptr)) << 24) ^
        (reinterpret_cast<std::uintptr_t>(this) >> 4) ^
        count_.load(std::memory_order_relaxed);
    if (payload_gen_ == 0) payload_gen_ = 1;  // 0 保留为「不校验」
}

// S14-8:qc8 保存——同 .vec 的「优先追加、全量兜底」结构。
bool HnswIndex::save_qc_payload(std::string_view path) const {
    if (!needs_qcodes_) return true;  // 无码字配置：无 qc8 文件
    const std::uint32_t n = count_.load(std::memory_order_acquire);
    const std::string fp(path);
    if (qc_file_.valid && n >= qc_file_.count) {
        if (try_append_qc_payload(path, n)) return true;
        qc_file_.valid = false;
    }

    // 全量重写:S32-M2b 格式实现抽入 write_bcq8_file(与 clone_live 外溢
    // 共用,位级同源)。取数经访问器路由(mmap 段 + chunk 段合并,S32-M2)。
    ensure_payload_gen();
    if (!write_bcq8_file(
            fp, cfg_.dim, n, payload_gen_,
            [this](std::uint32_t id) { return qcodes_of(id); },
            [this](std::uint32_t id) { return qscale_of(id); },
            [this](std::uint32_t id) { return qsum_of(id); })) {
        qc_file_.valid = false;
        return false;
    }
    if (const auto ident = io::path_identity(fp)) {
        qc_file_ = VecFileState{true, *ident, kBcq8HeaderSize, n};
    } else {
        qc_file_.valid = false;
    }
    return true;
}

// S14-8:qc8 增量追加（前缀不变契约同 try_append_vec_payload）。
bool HnswIndex::try_append_qc_payload(std::string_view path,
                                      std::uint32_t n) const {
    const std::size_t stride =
        static_cast<std::size_t>(cfg_.dim) + sizeof(float) +
        sizeof(std::int32_t);
    const std::string fp(path);
    const auto fh = io::open_handle(
        fp, io::OpenFlag::kWriteOnly | io::OpenFlag::kCloseOnExec);
    if (!fh) return false;
    const io::FileHandle fd = *fh;
    const auto ident = io::handle_identity(fd);
    if (!ident || *ident != qc_file_.id) {
        io::close_handle(fd);
        return false;
    }
    const auto fsize = io::handle_size(fd);
    if (!fsize) {
        io::close_handle(fd);
        return false;
    }
    const std::uint64_t old_end =
        qc_file_.data_off +
        static_cast<std::uint64_t>(qc_file_.count) * stride;
    if (*fsize < old_end) {
        io::close_handle(fd);
        return false;
    }
    bool ok = true;
    if (n > qc_file_.count) {
        std::vector<std::uint8_t> batch;
        std::uint64_t off = old_end;
        for (std::uint32_t id = qc_file_.count; ok && id < n; ++id) {
            // S32-M2:经访问器路由(追加区间通常全在 chunk 堆——
            // qc_file_.count ≥ qc_checkpoint_count_;统一走访问器免边界
            // 假设)。
            const auto* q =
                reinterpret_cast<const std::uint8_t*>(qcodes_of(id));
            batch.insert(batch.end(), q, q + cfg_.dim);
            const float s = qscale_of(id);
            const auto* sp = reinterpret_cast<const std::uint8_t*>(&s);
            batch.insert(batch.end(), sp, sp + sizeof(float));
            const std::int32_t z = qsum_of(id);
            const auto* zp = reinterpret_cast<const std::uint8_t*>(&z);
            batch.insert(batch.end(), zp, zp + sizeof(std::int32_t));
            if (batch.size() >= 4096 * stride || id + 1 == n) {
                ok = diskint::pwrite_all(fd, batch.data(), batch.size(), off);
                off += batch.size();
                batch.clear();
            }
        }
        if (ok) ok = io::sync_data(fd);
        if (ok) {
            (void)io::truncate_handle(fd, (
                qc_file_.data_off + static_cast<std::uint64_t>(n) * stride));
        }
    } else if (n == qc_file_.count) {
        io::close_handle(fd);
        return true;  // 无新节点，无事可做
    }
    if (ok) {
        std::uint8_t hdr[kBcq8HeaderSize] = {0};
        std::memcpy(hdr + 0, kBcq8Magic, 4);
        std::memcpy(hdr + 4, &kBcq8Version, 4);
        std::memcpy(hdr + 8, &cfg_.dim, 2);
        std::memcpy(hdr + 10, &n, 4);
        std::memcpy(hdr + 14, &payload_gen_, 8);
        std::memcpy(hdr + 22, &qc_file_.data_off, 8);
        const std::uint64_t total = static_cast<std::uint64_t>(n) * stride;
        std::memcpy(hdr + 30, &total, 8);
        const std::uint32_t hcrc =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(hdr), kBcq8HeaderCrcOff));
        std::memcpy(hdr + kBcq8HeaderCrcOff, &hcrc, 4);
        ok = diskint::pwrite_all(fd, hdr, kBcq8HeaderSize, 0) && io::sync_data(fd);
    }
    io::close_handle(fd);
    if (ok) qc_file_.count = n;
    return ok;
}

// S14-8:qc8 载入（v3 且 needs_qcodes_ 时）。前缀契约 + gen 配对。
// S32-M2:mmap 化——此前 fread+memcpy 进堆(int8 码字硬驻留,高维下占堆
// ~95%);现 MAP_SHARED 只读 mmap,前 n 条记录经 qcodes_of 路由直读页缓存
// (可回收;内存不足退化成慢而非 OOM)。header 校验逻辑与 fread 版逐字保留。
bool HnswIndex::load_qc_payload(std::string_view path) {
    if (!qc_pending_) return true;
    const std::size_t stride = qc_stride();
    const std::string fp(path);

    // 已持有 mmap 时先拆(load 由 open 期单线程串入,契约同 load_vec_payload)。
    qc_map_.reset();  // B3：RAII munmap
    qc_mmap_recs_ = nullptr;
    qc_checkpoint_count_ = 0;
    if (qc_payload_fd_ >= 0) {
        io::close_handle(qc_payload_fd_);
        qc_payload_fd_ = -1;
    }

    const auto fh = io::open_handle(
        fp, io::OpenFlag::kReadOnly | io::OpenFlag::kCloseOnExec);
    if (!fh) return false;
    const io::FileHandle fd = *fh;
    std::uint8_t hdr[kBcq8HeaderSize];
    // S37-1：定位读，不依赖 fd 内部偏移（见 ivf_rq 同处注释）。
    if (!io::pread_all(fd, hdr, kBcq8HeaderSize, 0)) {
        io::close_handle(fd);
        return false;
    }
    if (std::memcmp(hdr, kBcq8Magic, 4) != 0) {
        io::close_handle(fd);
        return false;
    }
    std::uint32_t ver = 0, count = 0;
    std::uint16_t dim = 0;
    std::uint64_t gen = 0, rec_off = 0;
    std::memcpy(&ver, hdr + 4, 4);
    std::memcpy(&dim, hdr + 8, 2);
    std::memcpy(&count, hdr + 10, 4);
    std::memcpy(&gen, hdr + 14, 8);
    std::memcpy(&rec_off, hdr + 22, 8);
    std::uint32_t stored = 0;
    std::memcpy(&stored, hdr + kBcq8HeaderCrcOff, 4);
    const std::uint32_t calc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hdr), kBcq8HeaderCrcOff));
    if (ver != kBcq8Version || dim != cfg_.dim || stored != calc) {
        io::close_handle(fd);
        return false;
    }
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    if (count < n) {  // 前缀不足
        io::close_handle(fd);
        return false;
    }
    // gen 配对：双方非零才校验（legacy 0 跳过）。
    if (gen != 0 && payload_gen_ != 0 && gen != payload_gen_) {
        io::close_handle(fd);
        return false;
    }

    // 前缀契约:文件须物理持有 [0, n) 记录字节(header.count 声称更多但
    // 尾部缺失 = torn append,不影响前缀装载)。
    const auto fsize = io::handle_size(fd);
    if (!fsize) {
        io::close_handle(fd);
        return false;
    }
    const auto file_size = static_cast<std::size_t>(*fsize);
    if (file_size < rec_off + static_cast<std::uint64_t>(n) * stride) {
        io::close_handle(fd);
        return false;
    }
    // B3：MappedFile（advise_random = MADV_RANDOM——图导航随机 touch
    // 码字,预读收益小,与 .vec 同款）。
    qc_map_ = io::MappedFile::map_readonly(fd, file_size,
                                           /*advise_random=*/true);
    if (!qc_map_.valid()) {
        io::close_handle(fd);
        return false;
    }
    qc_mmap_recs_ =
        reinterpret_cast<const std::uint8_t*>(qc_map_.data()) + rec_off;
    qc_payload_fd_ = fd;  // fd 持有至 mmap 生命周期末(destructor close)
    qc_checkpoint_count_ = n;
    // S37-1：由已持有的句柄取身份（原按 fstat 的 st）——比按路径 stat 更准，
    // 排除「校验与收养之间路径被替换」的窗口。取不到则不收养（valid=false
    // ⇒ 下次 save 走全量重写，与原 fstat 失败路径同向）。
    if (const auto self = io::handle_identity(fd)) {
        qc_file_ = VecFileState{true, *self, rec_off, n};
    } else {
        qc_file_.valid = false;
    }
    qc_pending_ = false;
    return true;
}

// S14-2:.vec 增量追加。只写 [vec_file_.count, n) 的新向量（前缀不变契约：
// offset < 旧数据尾的字节一律不碰，torn append 不伤 ckpt 声称的有效前缀）。
// 顺序：数据 pwrite → fdatasync → header 原地重写（version=2）→ fdatasync。
// header 先于数据落盘的窗口不存在；header 更新丢失时文件仍是旧 count 的
// 合法 v1/v2 文件（配旧 ckpt 恰好成对）。页 CRC 表在 v2 中不再维护
// （load 从未校验过它，v1 遗留表成为数据区前的死字节，vecs_off 照常跳过）。
bool HnswIndex::try_append_vec_payload(std::string_view path,
                                       std::uint32_t n) const {
    const std::size_t vec_bytes =
        static_cast<std::size_t>(cfg_.dim) * sizeof(float);
    const std::string fp(path);
    const auto fh = io::open_handle(
        fp, io::OpenFlag::kWriteOnly | io::OpenFlag::kCloseOnExec);
    if (!fh) return false;
    const io::FileHandle fd = *fh;
    const auto ident = io::handle_identity(fd);
    if (!ident || *ident != vec_file_.id) {
        io::close_handle(fd);  // 路径已指向别的文件（外部替换）→ 全量重写
        return false;
    }
    const auto fsize = io::handle_size(fd);
    if (!fsize) {
        io::close_handle(fd);
        return false;
    }
    // 前缀完整性：文件必须仍持有 [0, vec_file_.count) 的数据字节。
    const std::uint64_t old_end =
        vec_file_.data_off +
        static_cast<std::uint64_t>(vec_file_.count) * vec_bytes;
    if (*fsize < old_end) {
        io::close_handle(fd);
        return false;
    }
    if (n == vec_file_.count) {
        io::close_handle(fd);  // 无新向量 ⇒ watermark 也未变，无事可做
        return true;
    }

    // 按 chunk 连续段聚合 pwrite（chunk 内 id 连续 ⇒ 内存连续；mmap 段同理，
    // 在 chunk 边界多切一刀无碍正确性）。
    bool ok = true;
    {
        std::uint32_t id = vec_file_.count;
        std::uint64_t off = vec_file_.data_off +
                            static_cast<std::uint64_t>(id) * vec_bytes;
        while (ok && id < n) {
            const std::uint32_t chunk_end =
                ((id >> kChunkBits) + 1u) << kChunkBits;
            const std::uint32_t run_end = std::min(n, chunk_end);
            const std::size_t len =
                static_cast<std::size_t>(run_end - id) * vec_bytes;
            ok = diskint::pwrite_all(fd, vec_of(id), len, off);
            off += len;
            id = run_end;
        }
        if (ok) ok = io::sync_data(fd);
        // 裁到精确末端：覆盖掉可能更长的旧代垃圾尾（.prev 回退场景）。
        // best-effort——mmap 前缀 [0, checkpoint_count_) 远在截断点之前。
        if (ok) {
            (void)io::truncate_handle(fd, (
                vec_file_.data_off +
                static_cast<std::uint64_t>(n) * vec_bytes));
        }
    }

    // header 原地重写（64B，字段偏移与 v1 相同；version=2、crc_count=0）。
    if (ok) {
        std::uint8_t hdr[kBcvpHeaderSize] = {0};
        hdr[0] = kBcvpMagic[0]; hdr[1] = kBcvpMagic[1];
        hdr[2] = kBcvpMagic[2]; hdr[3] = kBcvpMagic[3];
        const std::uint32_t v2 = 2;
        std::memcpy(hdr + 4,  &v2,       4);
        std::memcpy(hdr + 8,  &cfg_.dim, 2);
        std::memcpy(hdr + 10, &n,        4);
        const std::uint64_t watermark =
            max_inserted_ord_.load(std::memory_order_relaxed);
        std::memcpy(hdr + 14, &watermark, 8);
        std::memcpy(hdr + 22, &kBcvpPageSize, 4);
        std::memcpy(hdr + 26, &vec_file_.data_off, 8);
        const std::uint64_t total =
            static_cast<std::uint64_t>(n) * vec_bytes;
        std::memcpy(hdr + 34, &total, 8);
        // hdr+42 crc_count = 0（零初始化）。
        // S14-8:gen 印章（追加继承 payload_gen_——可能为 0 = legacy 链）。
        std::memcpy(hdr + kBcvpGenOff, &payload_gen_, 8);
        const std::uint32_t hcrc = bitcask::codec::crc32(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(hdr), kBcvpHeaderCrcOff));
        std::memcpy(hdr + kBcvpHeaderCrcOff, &hcrc, 4);
        ok = diskint::pwrite_all(fd, hdr, kBcvpHeaderSize, 0) && io::sync_data(fd);
    }
    io::close_handle(fd);
    if (ok) vec_file_.count = n;
    return ok;
}

// V7:mmap .vec payload。PRECONDITION:deserialize() 已设 count_/cfg_。校验
// magic/version/dim/count 与 cfg_ 一致;设 vecs_mmap_* / checkpoint_count_。
bool HnswIndex::load_vec_payload(std::string_view path) {
    if (cfg_.inmem_int8) return true;  // 无 payload,语义上 no-op

    const std::string fp(path);
    const auto fh = io::open_handle(
        fp, io::OpenFlag::kReadOnly | io::OpenFlag::kCloseOnExec);
    if (!fh) return false;
    io::FileHandle fd = *fh;

    // 已持有 mmap 时先拆——契约要求 load 前为空(load 由 open 期单线程串入)。
    vecs_map_.reset();  // B3：RAII munmap
    vecs_mmap_base_ = nullptr;
    if (vecs_payload_fd_ >= 0) {
        io::close_handle(vecs_payload_fd_);
        vecs_payload_fd_ = -1;
    }

    std::uint8_t hdr[kBcvpHeaderSize];
    // S37-1：定位读，不依赖 fd 内部偏移（见 ivf_rq 同处注释）。
    if (!io::pread_all(fd, hdr, kBcvpHeaderSize, 0)) {
        io::close_handle(fd);
        return false;
    }
    if (hdr[0] != kBcvpMagic[0] || hdr[1] != kBcvpMagic[1] ||
        hdr[2] != kBcvpMagic[2] || hdr[3] != kBcvpMagic[3]) {
        io::close_handle(fd);
        return false;
    }
    std::uint32_t version;
    std::memcpy(&version, hdr + 4, 4);
    // S14-2:v1 = 全量重写世代（有页 CRC 表，从未校验）；v2 = 被追加过的
    // 世代（不再维护页表）。两者数据区语义相同，均按前缀契约装载。
    if (version != 1 && version != 2) {
        io::close_handle(fd);
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
    // S14-8:payload 代号配对（双方非零才校验）——闭合「rebuild 全量重写后
    // .prev 回退，旧图配新 payload（id 已重映射）被前缀检查误收」的隐患。
    std::uint64_t file_gen = 0;
    std::memcpy(&file_gen, hdr + kBcvpGenOff, 8);
    if (file_gen != 0 && payload_gen_ != 0 && file_gen != payload_gen_) {
        io::close_handle(fd);
        return false;
    }

    // 与 deserialize() 状态交叉校验。S14-2 前缀契约：文件物理持有量
    // （header.count）允许 ≥ ckpt 声称的 n——.prev 回退时 .vec 已被新代
    // 追加过是常态，只要求 [0, n) 前缀存在；旧 v1 文件恒等值，天然通过。
    const std::uint32_t n = count_.load(std::memory_order_relaxed);
    const std::uint64_t need_bytes =
        static_cast<std::uint64_t>(n) *
        static_cast<std::uint64_t>(cfg_.dim) * 4u;
    if (dim != cfg_.dim || count < n || vecs_len_u64 < need_bytes) {
        io::close_handle(fd);
        return false;
    }

    // header_crc
    std::uint32_t stored_hdr_crc;
    std::memcpy(&stored_hdr_crc, hdr + kBcvpHeaderCrcOff, 4);
    const std::uint32_t calc_hdr_crc = bitcask::codec::crc32(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(hdr), kBcvpHeaderCrcOff));
    if (stored_hdr_crc != calc_hdr_crc) {
        io::close_handle(fd);
        return false;
    }

    // 文件大小校验(vecs_off + vecs_len 不能超出文件)。
    const auto fsize = io::handle_size(fd);
    if (!fsize) {
        io::close_handle(fd);
        return false;
    }
    const std::size_t file_size = static_cast<std::size_t>(*fsize);
    // S14-2:只要求有效前缀 [0, n) 的字节在文件内（header.count 声称的
    // 更长区域允许缺失——torn append 的尾巴不影响前缀装载）。
    if (file_size < vecs_off_u64 + need_bytes) {
        io::close_handle(fd);
        return false;
    }
    // count=0 时文件可能仅 header(无 vecs 段),mmap 整个文件即可。
    // B3：MappedFile（advise_random = MADV_RANDOM——随机访问模式,稀疏读,
    // 预取收益小）。
    vecs_map_ = io::MappedFile::map_readonly(fd, file_size,
                                             /*advise_random=*/true);
    if (!vecs_map_.valid()) {
        io::close_handle(fd);
        return false;
    }

    vecs_mmap_base_  = reinterpret_cast<const float*>(
        vecs_map_.data() + vecs_off_u64);
    vecs_payload_fd_ = fd;  // fd 持有至 mmap 生命周期末(destructor close)
    checkpoint_count_ = n;
    // S14-2:记录追加目标。count 取 n（ckpt 有效前缀）而非 header.count——
    // 文件尾部可能是被 .prev 回退否掉的新代数据，下次追加从 n 起覆盖。
    if (const auto self = io::handle_identity(fd)) {  // S37-1，见 qc 同处注释
        vec_file_ = VecFileState{true, *self, vecs_off_u64, n};
    } else {
        vec_file_.valid = false;
    }
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
    // S25-M5:全程 size_t 运算，避免 int 域乘法溢出（cfg_.M 大时
    // (1+M*2)*4*8 在 int/uint32 域溢出 → reserve 远小于实际写入量）。
    const std::size_t per_node =
        24 + static_cast<std::size_t>(cfg_.dim) +
        (1 + static_cast<std::size_t>(cfg_.M) * 2) * 4 * 8;
    buf.reserve(64 + static_cast<std::size_t>(n) * per_node);
    vs_put32(buf, kBcvhMagic);
    // S14-8:v3——码字外置 qc8，段内不再内嵌（bit1 = 有 qc8 文件）。
    vs_put32(buf, kBcvhVersion3);
    const std::uint32_t flags = (cfg_.inmem_int8 ? 0u : 1u) |   // bit0 has_payload
                                (needs_qcodes_ ? 2u : 0u);      // bit1 has_qc8
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
    // S14-8:payload 代号（与 .vec/.qc8 头配对；save 流程先落 payload 后
    // serialize，二者取同一 gen）。
    ensure_payload_gen();
    vs_put64(buf, payload_gen_);

    std::vector<std::uint32_t> scratch(1 + cfg_.M * 2);
    for (std::uint32_t id = 0; id < n; ++id) {
        const NodeChunk* c = chunk_of(id);
        const std::uint32_t slot = id & kChunkMask;
        vs_put64(buf, c->ords[slot]);
        const std::uint8_t level = c->levels[slot];
        buf.push_back(level);
        // S14-8:v3 不内嵌码字——qcodes/qscale/qsum 在 search.qc8。
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
    if (!save_qc_payload(bp + ".qc8")) return false;  // S14-8
    std::vector<std::uint8_t> buf;
    if (!serialize(buf)) return false;
    // P6-DUR-1：rename 前 fdatasync——图虽可从向量重建，但断电丢页 ≠ 无害：
    // rename 已覆盖旧 base，最终路径下留半截文件，load 端 CRC 拒收后退全量
    // 重建。T21 起该纪律由 detail::atomic_write_bytes 统一承载。
    return bitcask::detail::atomic_write_bytes(
        bp, std::as_bytes(std::span(buf)));
}

bool HnswIndex::load(std::string_view base_path) {
    const std::string bp(base_path);
    auto buf = bitcask::detail::read_file_bytes<std::uint8_t>(bp);
    if (!buf) return false;
    if (!deserialize(*buf)) return false;
    // V7:deserialize 之后装 payload。inmem_int8 或 count=0 不读 .vec。
    if (!cfg_.inmem_int8 && count_.load(std::memory_order_relaxed) > 0) {
        const std::string vec_path = bp + ".vec";
        if (!load_vec_payload(vec_path)) return false;
    }
    // S14-8:v3 码字外置——qc_pending_ 自门（v2/无码字为 no-op）。
    if (count_.load(std::memory_order_relaxed) > 0 &&
        !load_qc_payload(bp + ".qc8")) {
        return false;
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
    const std::uint32_t file_ver = rd32at(4);
    if (rd32at(0) != kBcvhMagic ||
        (file_ver != kBcvhVersion && file_ver != kBcvhVersion3)) {
        return false;
    }

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
    // S14-8:v3 头多带 payload 代号；码字待 load_qc_payload 补（flags bit1
    // 为假 = 写端无码字 → 与 v2 零占位同语义，本地保持零）。
    payload_gen_ = 0;
    qc_pending_ = false;
    if (file_ver == kBcvhVersion3) {
        if (!need(8)) return false;
        std::memcpy(&payload_gen_, p, 8); p += 8;
        qc_pending_ = needs_qcodes_ && (flags & 2u) != 0;
    }
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
    // S32-M2:v3 + qc_pending(码字将由 .qc8 mmap 覆盖)→ chunk 不分配堆
    // qcodes(容量 0,与 needs_vecs=false 同型);v2 内嵌码字仍需堆数组。
    // 后续热插入经 insert 的懒分配补齐。
    const bool chunk_qcodes = needs_qcodes_ && !qc_pending_;
    for (std::uint32_t id = 0; id < cnt; ++id) {
        const std::uint32_t ci = id >> kChunkBits;
        NodeChunk* c = chunks_[ci].load(std::memory_order_relaxed);
        if (c == nullptr) {
            c = new NodeChunk(cfg_.dim, false, chunk_qcodes);
            chunks_[ci].store(c, std::memory_order_relaxed);
        }
        const std::uint32_t slot = id & kChunkMask;

        if (!need(file_ver == kBcvhVersion3 ? 13 : 13 + qbytes)) return false;
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
        // S14-8:v3 段内无码字（qc8 外置），本块仅 v2 走。
        if (file_ver == kBcvhVersion3) {
            // no inline codes
        } else if (needs_qcodes_) {
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
