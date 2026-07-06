#include "bitcask/index.hpp"

#include "bitcask/codec.hpp"  // S18-2：sidecar CRC
#include "bitcask/vbyte.hpp"   // S21-2 A2：sidecar v2 行 gap+vbyte

#include <algorithm>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#endif

namespace bitcask::index {

namespace {

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
// Tier-2 SIMD:fill_is_live 快路径 4 ords 一组,AVX2 vpgatherdq
// (_mm256_i64gather_epi64) 一次取 __m256i 索引(8 × 64-bit)但仅消费
// 低 4 个索引、返 4 × 64-bit 值(__m256i 高 128 bit 是无定义垃圾,绝不读)。
// 每轮 4 ords = 1 gather;低 lane 提取低字节写入 out。
// 注:LTO 模式下 _mm256_extract_epi64 会被拆成对 _mm256_extractf128_si256
// 的非立即数调用失败,故走 store + 数组索引(编译为 vmovq)。
__attribute__((target("avx2")))
__attribute__((noinline))
inline void fill_is_live_inbounds_avx2(const std::uint8_t* live_arr,
                                       const std::uint64_t* ords,
                                       char* out, std::size_t n) noexcept {
    std::size_t i = 0;
    alignas(32) std::uint64_t lanes[4];
    for (; i + 4 <= n; i += 4) {
        __m256i idx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ords + i));
        __m256i b = _mm256_i64gather_epi64(
            reinterpret_cast<const long long*>(live_arr), idx, 1);
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), b);
        out[i + 0] = static_cast<char>(lanes[0] & 0xFF);
        out[i + 1] = static_cast<char>(lanes[1] & 0xFF);
        out[i + 2] = static_cast<char>(lanes[2] & 0xFF);
        out[i + 3] = static_cast<char>(lanes[3] & 0xFF);
    }
    for (; i < n; ++i) out[i] = static_cast<char>(live_arr[ords[i]]);
}
#endif

}

// S21-1：布局契约——DocLoc 消 padding 后 16B，DocSlot 去 ord 后 24B。
// serialize_docmap/deserialize_docmap 逐字段读写，不受内存布局影响。
static_assert(sizeof(DocLoc) == 16, "DocLoc 应为 16B（offset 前置消 padding）");
static_assert(sizeof(DocSlot) == 24, "DocSlot 应为 24B（ord 不常驻 slots_）");

void Index::ensure_capacity_locked(std::uint64_t ord) {
    const std::size_t want = static_cast<std::size_t>(ord) + 1;
    if (live_.size() < want) {
        live_.resize(want, false);
        doc_lens_.resize(want, 0);
        // S21-1：meta 惰性——未启用（首个非空 set_meta 前）不跟平。
        if (!meta_blobs_.empty()) meta_blobs_.resize(want);
    }
    const std::size_t ci = static_cast<std::size_t>(ord) / kChunkOrds;
    if (chunks_.size() <= ci) {
        chunks_.resize(ci + 1);
    }
    if (!chunks_[ci]) {
        chunks_[ci] = std::make_unique<Chunk>();
        ++chunks_alloc_;
    }
}

std::uint64_t Index::alloc_ord() {
    std::unique_lock lk(mutex_);
    return next_ord_++;
}

void Index::put_doc(std::string_view ext_id, std::uint64_t ord,
                    const DocSlot& slot) {
    std::unique_lock lk(mutex_);

    next_ord_ = std::max(next_ord_, ord + 1);
    ensure_capacity_locked(ord);

    const auto ci = ord / kChunkOrds;
    const auto si = ord % kChunkOrds;
    auto* chunk = chunks_[ci].get();

    if (auto it = ext2ord_.find(ext_id); it != ext2ord_.end()) {
        const std::uint64_t old_ord = it->second;
        if (old_ord < live_.size() && live_[old_ord]) {
            live_[old_ord] = false;
            const auto oc = old_ord / kChunkOrds;
            if (chunks_[oc]) --chunks_[oc]->live_count;
            ++retired_since_compact_;  // S12-2：覆盖写退休旧版本
        }
        it->second = ord;
    } else {
        ext2ord_.emplace(std::string(ext_id), ord);
        ++live_docs_;
    }

    chunk->slots[si]    = slot;
    chunk->ord2ext[si].assign(ext_id);
    ++chunk->live_count;
    live_[ord]      = true;
    doc_lens_[ord]  = slot.doc_len;
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
}

bool Index::remove(std::string_view ext_id, std::uint64_t tomb_ord) {
    std::unique_lock lk(mutex_);

    next_ord_ = std::max(next_ord_, tomb_ord + 1);
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
    // S18-2：delta 窗口删除日志自记账（S14-4 门限：链覆盖区内的旧墓碑不入，
    // 防跨文件 stale removal 重放误杀复活文档）。在 found-check **之前**入账
    // ——与旧 recover_tomb 语义一致（keydir 半边的 remove_if_older 重放可能
    // 仍需要该条目，即使 docmap 已无此 key）。ckpt 载入重放产生的污染由
    // 载入方收尾 clear_removals()。
    if (tomb_ord >= delta_window_wm_) {
        removals_.emplace_back(std::string(ext_id), tomb_ord);
    }

    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) {
        return false;
    }
    const std::uint64_t cur_ord = it->second;
    if (cur_ord < live_.size() && live_[cur_ord]) {
        live_[cur_ord] = false;
        const auto ci = cur_ord / kChunkOrds;
        if (chunks_[ci]) --chunks_[ci]->live_count;
        ++retired_since_compact_;  // S12-2：删除退休当前版本
    }
    ext2ord_.erase(it);
    --live_docs_;
    return true;
}

std::optional<DocHit> Index::get(std::string_view ext_id) const {
    std::shared_lock lk(mutex_);
    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) {
        return std::nullopt;
    }
    const std::uint64_t ord = it->second;
    const auto ci = ord / kChunkOrds;
    const auto si = ord % kChunkOrds;
    return DocHit{chunks_[ci]->slots[si], ord};
}

std::optional<std::uint64_t> Index::ord_of(std::string_view ext_id) const {
    std::shared_lock lk(mutex_);
    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> Index::ord_to_ext(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    if (ord >= live_.size()) {
        return std::nullopt;
    }
    const auto ci = ord / kChunkOrds;
    const auto si = ord % kChunkOrds;
    if (ci >= chunks_.size() || !chunks_[ci]) {
        return std::nullopt;
    }
    return chunks_[ci]->ord2ext[si];
}

bool Index::is_live(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    return ord < live_.size() && live_[ord];
}

std::uint32_t Index::doc_len(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    if (ord >= doc_lens_.size()) return 0;
    return doc_lens_[ord];
}

std::vector<std::byte> Index::meta_blob(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    if (ord >= meta_blobs_.size()) return {};
    // 锁内拷贝返回:并发 set_meta 会重分配 meta_blobs_[ord],返回内部指针/span
    // 会在锁外悬垂。空 vector → 空返回（无 meta 的文档,filter 直接判 false）。
    return meta_blobs_[ord];
}

// S13-P8：锁内求值（契约见头文件）。
bool Index::eval_meta(std::uint64_t ord, const meta::MetaFilter& filter) const {
    std::shared_lock lk(mutex_);
    if (ord >= meta_blobs_.size()) return false;
    const auto& blob = meta_blobs_[ord];
    if (blob.empty()) return false;  // 无 meta 恒不通过（与 materialize_hits 一致）
    return filter.evaluate(std::span<const std::byte>(blob));
}

void Index::set_doc_len(std::uint64_t ord, std::uint32_t len) {
    std::unique_lock lk(mutex_);
    if (ord >= doc_lens_.size()) return;  // 未登记（空 job 守卫路径）：no-op
    doc_lens_[ord] = len;
    const auto ci = ord / kChunkOrds;
    const auto si = ord % kChunkOrds;
    if (ci < chunks_.size() && chunks_[ci]) chunks_[ci]->slots[si].doc_len = len;
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
}

void Index::set_meta(std::uint64_t ord, std::span<const std::byte> blob) {
    std::unique_lock lk(mutex_);
    ensure_capacity_locked(ord);
    // 与 put_doc 共锁：调用顺序保证 ord 此前已在 slots_/live_/doc_lens_ 注册。
    if (blob.empty()) {
        // S21-1：meta 未启用时无槽可清——ord >= size() 本就是空 blob 语义。
        if (ord < meta_blobs_.size()) meta_blobs_[ord].clear();
    } else {
        // S21-1：首个非空 blob 才启用 meta 列（跟平到 live_ 现宽）。
        if (meta_blobs_.size() < live_.size()) meta_blobs_.resize(live_.size());
        meta_blobs_[ord].assign(blob.begin(), blob.end());
    }
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
#endif
void Index::fill_is_live(std::span<const std::uint64_t> ords,
                         std::span<char> out) const {
    std::shared_lock lk(mutex_);
    const std::size_t bound = live_.size();
    // 快路径:FlatPostings ords 升序去重 → ords.back() < bound ⟺ 全在界,
    // 单次比较省掉 per-element 分支。占绝大多数查询(recents)。
    if (!ords.empty() && ords.back() < bound) {
        const auto* live_arr = live_.data();
        const auto* ords_arr = ords.data();
        char* out_arr = out.data();
        const std::size_t n = ords.size();
        std::size_t i = 0;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
        if (__builtin_cpu_supports("avx2")) {
            fill_is_live_inbounds_avx2(live_arr, ords_arr, out_arr, n);
            return;
        }
#endif
        for (; i < n; ++i) out_arr[i] = static_cast<char>(live_arr[ords_arr[i]]);
        return;
    }
    // 慢路径:含越界或ds → per-element 越界检查。
    for (std::size_t i = 0; i < ords.size(); ++i) {
        out[i] = static_cast<char>(ords[i] < bound && live_[ords[i]]);
    }
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
#endif
void Index::fill_doc_lens(std::span<const std::uint64_t> ords,
                          std::span<std::uint32_t> out) const {
    std::shared_lock lk(mutex_);
    // P2.4：读 SoA 紧凑数组（gather 的 cache 流量 ↓8x，见 index.hpp 注释）。
    const std::size_t bound = doc_lens_.size();
    // 同 fill_is_live:ords 升序去重 → ords.back() < bound 全在界。
    if (!ords.empty() && ords.back() < bound) {
        const auto* dls_arr = doc_lens_.data();
        const auto* ords_arr = ords.data();
        auto* out_arr = out.data();
        const std::size_t n = ords.size();
        std::size_t i = 0;
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
        // AVX2 vpgatherqd(_mm256_i64gather_epi32) 一次取 8 个 64-bit 索引
        // 但仅消费低 4 个、返 4 个 32-bit 值(__m128i)。每轮 4 ords 一次
        // gather;高 4 索引通过 lane shift 喂下一轮。
        if (__builtin_cpu_supports("avx2")) {
            for (; i + 4 <= n; i += 4) {
                __m256i idx = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(ords_arr + i));
                __m128i v = _mm256_i64gather_epi32(
                    reinterpret_cast<const int*>(dls_arr), idx, 4);
                _mm_storeu_si128(
                    reinterpret_cast<__m128i*>(out_arr + i), v);
            }
        }
#endif
        for (; i < n; ++i) out_arr[i] = dls_arr[ords_arr[i]];
        return;
    }
    for (std::size_t i = 0; i < ords.size(); ++i) {
        out[i] = ords[i] < bound ? doc_lens_[ords[i]] : 0;
    }
}

IndexInfo Index::info() const {
    std::shared_lock lk(mutex_);
    return IndexInfo{
        .live_docs        = live_docs_,
        .total_ords       = next_ord_,
        .next_ord         = next_ord_,
        .chunks_allocated = chunks_alloc_,
        .chunks_freed     = chunks_freed_,
    };
}

std::uint64_t Index::compact_chunks() {
    std::unique_lock lk(mutex_);
    std::uint64_t freed = 0;
    for (auto& chunk_ptr : chunks_) {
        if (chunk_ptr && chunk_ptr->live_count == 0) {
            chunk_ptr.reset();
            ++freed;
        }
    }
    chunks_freed_ += freed;
    if (freed > 0) dirty_.store(true, std::memory_order_relaxed);  // S18-2
    return freed;
}

// ---- S18-2：docmap 持久化记账 ----

void Index::begin_delta_window(std::uint64_t wm) {
    std::unique_lock lk(mutex_);
    delta_window_wm_ = wm;
}

std::vector<std::pair<std::string, std::uint64_t>>
Index::removals_snapshot() const {
    std::shared_lock lk(mutex_);
    return removals_;
}

void Index::clear_removals() {
    std::unique_lock lk(mutex_);
    removals_.clear();
}

// ---- S18-2：docmap sidecar（"BCIS"）序列化（自 SearchLayer 平移）----

namespace {
constexpr std::uint32_t kSidecarMagic   = 0x42434953;  // "BCIS"
// S21-2 A2：v2 行编码 gap+vbyte（ord 差分 + 标量 vbyte，tstamp 保持定宽
// 4B），固定 34B/行 → 典型 12-15B/行。写端恒写 v2；读端 v1/v2 双收。
// 旧读端遇 v2 → version 不符拒收 → 组件退 fold（可重建，降级安全）。
constexpr std::uint32_t kSidecarVersion   = 2;
constexpr std::uint32_t kSidecarVersionV1 = 1;
void sc_put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void sc_put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
}
}  // namespace

bool Index::serialize_docmap(std::vector<std::uint8_t>& buf,
                             std::uint64_t covers_next_ord) const {
    buf.clear();
    // S4:预留容量——v2 变长典型 ≤15B/行 + 变长 ext_id（按 48B/行估值），
    // 常态零 realloc（偏小由几何增长兜底，reserve 仅设容量、绝不溢出）。
    const std::uint64_t live = info().live_docs;
    buf.reserve(28 + static_cast<std::size_t>(live) * (15 + 48));
    sc_put32(buf, kSidecarMagic);
    sc_put32(buf, kSidecarVersion);
    sc_put64(buf, covers_next_ord);
    // 行数占位,回填。
    const std::size_t cnt_pos = buf.size();
    sc_put64(buf, 0);
    std::uint64_t rows = 0;
    bool ok = true;
    std::uint64_t prev_ord = 0;
    for_each_live([&](std::uint64_t ord, const std::string& ext,
                      const DocSlot& slot) {
        if (ext.size() > 0xFFFF) { ok = false; return; }
        codec::vbyte_encode(ord - prev_ord, buf);  // gap：live 按 ord 升序遍历
        prev_ord = ord;
        codec::vbyte_encode(ext.size(), buf);
        const auto* kd = reinterpret_cast<const std::uint8_t*>(ext.data());
        buf.insert(buf.end(), kd, kd + ext.size());
        codec::vbyte_encode(slot.loc.file_id, buf);
        codec::vbyte_encode(slot.loc.offset, buf);
        codec::vbyte_encode(slot.loc.total_sz, buf);
        sc_put32(buf, slot.tstamp);   // 定宽：unix 时间戳 vbyte 需 5B
        codec::vbyte_encode(slot.doc_len, buf);
        ++rows;
    });
    if (!ok) return false;
    std::memcpy(buf.data() + cnt_pos, &rows, 8);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 8));
    sc_put32(buf, crc);
    return true;
}

std::optional<std::uint64_t>
Index::deserialize_docmap(std::span<const std::uint8_t> buf) {
    if (buf.size() < 28) return std::nullopt;
    auto rd32 = [&](std::size_t off) {
        std::uint32_t v; std::memcpy(&v, buf.data() + off, 4); return v;
    };
    if (rd32(0) != kSidecarMagic) return std::nullopt;
    const std::uint32_t ver = rd32(4);
    if (ver != kSidecarVersion && ver != kSidecarVersionV1) {
        return std::nullopt;
    }
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + buf.size() - 4, 4);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 12));
    if (crc != stored_crc) return std::nullopt;

    const std::uint8_t* p = buf.data() + 8;
    const std::uint8_t* end = buf.data() + buf.size() - 4;
    auto need = [&](std::size_t n) {
        return static_cast<std::size_t>(end - p) >= n;
    };
    std::uint64_t covers = 0, rows = 0;
    std::memcpy(&covers, p, 8); p += 8;
    std::memcpy(&rows, p, 8); p += 8;
    if (rows > (1ull << 40)) return std::nullopt;
    bool vfail = false;
    auto vb = [&]() -> std::uint64_t {  // 边界安全 vbyte
        std::uint64_t v = 0, shift = 0;
        while (true) {
            if (p >= end || shift > 63) { vfail = true; return 0; }
            const std::uint8_t byte = *p++;
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return v;
            shift += 7;
        }
    };
    const bool v2 = ver >= kSidecarVersion;
    std::uint64_t prev_ord = 0;
    for (std::uint64_t i = 0; i < rows; ++i) {
        std::uint64_t ord = 0;
        std::uint64_t klen = 0;
        if (v2) {
            ord = prev_ord + vb();  // gap（二补数回绕，正确性不依赖升序）
            prev_ord = ord;
            klen = vb();
            if (vfail || klen > 0xFFFF) return std::nullopt;
        } else {
            if (!need(10)) return std::nullopt;
            std::memcpy(&ord, p, 8); p += 8;
            std::uint16_t k16; std::memcpy(&k16, p, 2); p += 2;
            klen = k16;
        }
        if (!need(static_cast<std::size_t>(klen))) return std::nullopt;
        std::string ext(reinterpret_cast<const char*>(p), klen); p += klen;
        DocSlot slot;
        if (v2) {
            const std::uint64_t fid = vb(), off = vb(), tsz = vb();
            if (vfail || !need(4)) return std::nullopt;
            std::memcpy(&slot.tstamp, p, 4); p += 4;
            const std::uint64_t dl = vb();
            if (vfail || fid > 0xFFFFFFFFull || tsz > 0xFFFFFFFFull ||
                dl > 0xFFFFFFFFull) {
                return std::nullopt;
            }
            slot.loc.offset   = off;
            slot.loc.file_id  = static_cast<std::uint32_t>(fid);
            slot.loc.total_sz = static_cast<std::uint32_t>(tsz);
            slot.doc_len      = static_cast<std::uint32_t>(dl);
        } else {
            if (!need(20)) return std::nullopt;
            std::memcpy(&slot.loc.file_id, p, 4); p += 4;
            std::memcpy(&slot.loc.offset, p, 8); p += 8;
            std::memcpy(&slot.loc.total_sz, p, 4); p += 4;
            std::memcpy(&slot.tstamp, p, 4); p += 4;
            std::memcpy(&slot.doc_len, p, 4); p += 4;
        }
        put_doc(ext, ord, slot);  // 重建 ext2ord/live/doc_lens/水位
    }
    if (p != end) return std::nullopt;
    return covers;
}

}  // namespace bitcask::index
