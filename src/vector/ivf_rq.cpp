// IvfSegment 实现（S32-M3 v1）。格式/契约见 include/bitcask/ivf_rq.hpp。

#include "bitcask/ivf_rq.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "bitcask/codec.hpp"                 // codec::crc32
#include "bitcask/detail/int8_kernels.hpp"   // 量化/int8 dot（与 HNSW 同源）

namespace bitcask::vec {

namespace {

constexpr char          kIvfMagic[4] = {'B', 'I', 'V', '1'};
constexpr std::uint32_t kIvfVersion  = 1;
constexpr std::size_t   kIvfHeaderSize = 96;
constexpr std::size_t   kIvfHeaderCrcOff = 92;
constexpr std::size_t   kCidxEntrySize = 16;  // off u64 | count u32 | crc u32

bool pwrite_all(int fd, const void* buf, std::size_t len, std::uint64_t off) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    while (len > 0) {
        const ssize_t w = ::pwrite(fd, p, len, static_cast<off_t>(off));
        if (w <= 0) return false;
        p   += w;
        off += static_cast<std::uint64_t>(w);
        len -= static_cast<std::size_t>(w);
    }
    return true;
}

// f32 内积（编译器自动向量化；质心暴扫用——nlist 有限，非热点瓶颈）。
inline float dot_f32(const float* a, const float* b, std::size_t dim) {
    float acc = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) acc += a[d] * b[d];
    return acc;
}

// 并行 for：[0, n) 均匀分片到 hardware_concurrency 线程。
template <typename Fn>
void parallel_for(std::size_t n, Fn&& fn) {
    const std::size_t nt = std::min<std::size_t>(
        std::max<std::size_t>(1, std::thread::hardware_concurrency()),
        n == 0 ? 1 : n);
    if (nt <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    constexpr std::size_t kBatch = 256;
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (std::size_t t = 0; t < nt; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t b = next.fetch_add(kBatch);
                if (b >= n) return;
                const std::size_t e = std::min(n, b + kBatch);
                for (std::size_t i = b; i < e; ++i) fn(i);
            }
        });
    }
    for (auto& th : pool) th.join();
}

// 自动 nlist：4·√N，clamp [16, 65536]，再保簇均 ≥ 8。
std::uint32_t auto_nlist(std::uint32_t n) {
    if (n == 0) return 16;
    auto nl = static_cast<std::uint32_t>(
        4.0 * std::sqrt(static_cast<double>(n)));
    nl = std::clamp<std::uint32_t>(nl, 16, 65536);
    nl = std::min(nl, std::max<std::uint32_t>(1, n / 8));
    return std::max<std::uint32_t>(nl, 1);
}

}  // namespace

IvfSegment::~IvfSegment() { close(); }

void IvfSegment::close() {
    if (raw_ != nullptr) {
        ::munmap(raw_, len_);
        raw_  = nullptr;
        base_ = nullptr;
        len_  = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    centroids_ = nullptr;
    cidx_      = nullptr;
    post_off_  = 0;
    count_ = 0;
    nlist_ = 0;
}

bool IvfSegment::build(std::string_view path, std::uint16_t dim,
                       const IvfBuildSource& src, std::uint32_t nlist,
                       std::uint64_t gen, std::uint64_t seed) {
    if (dim == 0) return false;
    const std::uint32_t n = src.count;
    if (n > 0 && !src.get) return false;
    if (nlist == 0) nlist = auto_nlist(n);
    if (n > 0) nlist = std::min(nlist, n);  // 簇数不超过样本数
    const std::size_t d = dim;

    // ---- 1) 采样球面 k-means（f32；随机初始化 + Lloyd 迭代）----
    // 样本上限：nlist×32 与 128K 取小（内存 ≤ 128K×dim×4；训练质量对
    // 路由召回足够——nprobe 是运行期补偿旋钮）。
    std::vector<float> centers(static_cast<std::size_t>(nlist) * d, 0.0f);
    if (n > 0) {
        const std::uint32_t s_cap = std::min<std::uint32_t>(
            std::max<std::uint32_t>(nlist * 32, 4096), 131072);
        const std::uint32_t s_n = std::min(n, s_cap);
        // 均匀步长抽样（确定性；源已是插入序，无顺序偏置担忧）。
        std::vector<float> sample(static_cast<std::size_t>(s_n) * d);
        {
            const double step = static_cast<double>(n) / s_n;
            parallel_for(s_n, [&](std::size_t si) {
                const auto i = static_cast<std::uint32_t>(
                    static_cast<double>(si) * step);
                std::uint64_t ord;
                const float* v = nullptr;
                src.get(std::min(i, n - 1), ord, v);
                std::memcpy(sample.data() + si * d, v, d * sizeof(float));
            });
        }
        // 初始化：打乱样本取前 nlist 个（Fisher-Yates 只跑前 nlist 步）。
        std::mt19937_64 rng(seed);
        std::vector<std::uint32_t> perm(s_n);
        for (std::uint32_t i = 0; i < s_n; ++i) perm[i] = i;
        for (std::uint32_t i = 0; i < nlist && i + 1 < s_n; ++i) {
            const auto j = i + static_cast<std::uint32_t>(
                rng() % (s_n - i));
            std::swap(perm[i], perm[j]);
        }
        for (std::uint32_t c = 0; c < nlist; ++c) {
            std::memcpy(centers.data() + static_cast<std::size_t>(c) * d,
                        sample.data() + static_cast<std::size_t>(perm[c]) * d,
                        d * sizeof(float));
        }
        // Lloyd × 8：assign（并行）→ 均值 → 归一化（球面）→ 空簇重播种。
        std::vector<std::uint32_t> sa(s_n, 0);
        for (int iter = 0; iter < 8; ++iter) {
            parallel_for(s_n, [&](std::size_t si) {
                const float* v = sample.data() + si * d;
                float best = -2.0f;
                std::uint32_t bc = 0;
                for (std::uint32_t c = 0; c < nlist; ++c) {
                    const float sc =
                        dot_f32(centers.data() +
                                static_cast<std::size_t>(c) * d, v, d);
                    if (sc > best) { best = sc; bc = c; }
                }
                sa[si] = bc;
            });
            std::vector<double> acc(static_cast<std::size_t>(nlist) * d, 0.0);
            std::vector<std::uint32_t> cnt(nlist, 0);
            for (std::uint32_t si = 0; si < s_n; ++si) {
                const float* v = sample.data() + static_cast<std::size_t>(si) * d;
                double* a = acc.data() + static_cast<std::size_t>(sa[si]) * d;
                for (std::size_t k2 = 0; k2 < d; ++k2) a[k2] += v[k2];
                ++cnt[sa[si]];
            }
            for (std::uint32_t c = 0; c < nlist; ++c) {
                float* cc = centers.data() + static_cast<std::size_t>(c) * d;
                if (cnt[c] == 0) {
                    // 空簇：重播种到随机样本点。
                    const auto ri = static_cast<std::uint32_t>(rng() % s_n);
                    std::memcpy(cc,
                                sample.data() +
                                    static_cast<std::size_t>(ri) * d,
                                d * sizeof(float));
                    continue;
                }
                const double* a = acc.data() + static_cast<std::size_t>(c) * d;
                double sq = 0.0;
                for (std::size_t k2 = 0; k2 < d; ++k2) sq += a[k2] * a[k2];
                const double inv =
                    sq > 0.0 ? 1.0 / std::sqrt(sq) : 0.0;
                for (std::size_t k2 = 0; k2 < d; ++k2) {
                    cc[k2] = static_cast<float>(a[k2] * inv);
                }
            }
        }
    }

    // ---- 2) 全量分簇（并行；cid 表 N×4B）----
    std::vector<std::uint32_t> cid(n, 0);
    parallel_for(n, [&](std::size_t i) {
        std::uint64_t ord;
        const float* v = nullptr;
        src.get(static_cast<std::uint32_t>(i), ord, v);
        float best = -2.0f;
        std::uint32_t bc = 0;
        for (std::uint32_t c = 0; c < nlist; ++c) {
            const float sc = dot_f32(
                centers.data() + static_cast<std::size_t>(c) * d, v, d);
            if (sc > best) { best = sc; bc = c; }
        }
        cid[i] = bc;
    });

    // ---- 3) 布局：簇计数 → 记录偏移 ----
    const std::size_t stride = static_cast<std::size_t>(dim) + 16;
    std::vector<std::uint32_t> ccount(nlist, 0);
    for (std::uint32_t i = 0; i < n; ++i) ++ccount[cid[i]];
    const std::uint64_t cent_off = kIvfHeaderSize;
    const std::uint64_t cidx_off =
        cent_off + static_cast<std::uint64_t>(nlist) * d * sizeof(float);
    const std::uint64_t post_off =
        cidx_off + static_cast<std::uint64_t>(nlist) * kCidxEntrySize;
    std::vector<std::uint64_t> cbase(nlist, 0);
    {
        std::uint64_t off = post_off;
        for (std::uint32_t c = 0; c < nlist; ++c) {
            cbase[c] = off;
            off += static_cast<std::uint64_t>(ccount[c]) * stride;
        }
    }
    const std::uint64_t file_len =
        post_off + static_cast<std::uint64_t>(n) * stride;

    // ---- 4) 写文件（tmp；记录按簇槽位 pwrite，页缓存吸收乱序）----
    const std::string fp(path);
    const std::string tmp = fp + ".tmp";
    const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd < 0) return false;
    bool ok = true;
    std::uint64_t max_ord = 0;
    // 质心区。
    ok = pwrite_all(fd, centers.data(),
                    static_cast<std::size_t>(nlist) * d * sizeof(float),
                    cent_off);
    // 记录区（单线程顺序过源——量化 + 定位写；游标按簇推进）。
    if (ok) {
        std::vector<std::uint64_t> cursor(cbase);
        std::vector<std::uint8_t> rec(stride);
        int8::QVector qv;
        for (std::uint32_t i = 0; ok && i < n; ++i) {
            std::uint64_t ord;
            const float* v = nullptr;
            src.get(i, ord, v);
            if (ord > max_ord) max_ord = ord;
            int8::quantize_into(v, d, qv);
            std::memcpy(rec.data(), &ord, 8);
            std::memcpy(rec.data() + 8, qv.codes.data(), d);
            std::memcpy(rec.data() + 8 + d, &qv.scale, 4);
            std::memcpy(rec.data() + 8 + d + 4, &qv.sum_codes, 4);
            ok = pwrite_all(fd, rec.data(), stride, cursor[cid[i]]);
            cursor[cid[i]] += stride;
        }
    }
    // cidx 区（回读记录区算逐簇 CRC——顺序读，页缓存已热）。
    if (ok) {
        std::vector<std::uint8_t> centry(kCidxEntrySize);
        std::vector<std::uint8_t> buf;
        for (std::uint32_t c = 0; ok && c < nlist; ++c) {
            const std::size_t bytes =
                static_cast<std::size_t>(ccount[c]) * stride;
            std::uint32_t crc = 0;
            if (bytes > 0) {
                buf.resize(bytes);
                ssize_t rd = ::pread(fd, buf.data(), bytes,
                                     static_cast<off_t>(cbase[c]));
                ok = rd == static_cast<ssize_t>(bytes);
                if (ok) {
                    crc = bitcask::codec::crc32(std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(buf.data()),
                        bytes));
                }
            }
            if (ok) {
                std::memcpy(centry.data(), &cbase[c], 8);
                std::memcpy(centry.data() + 8, &ccount[c], 4);
                std::memcpy(centry.data() + 12, &crc, 4);
                ok = pwrite_all(fd, centry.data(), kCidxEntrySize,
                                cidx_off + static_cast<std::uint64_t>(c) *
                                               kCidxEntrySize);
            }
        }
    }
    // header（字段全就位后 CRC）。
    if (ok) {
        std::uint8_t hdr[kIvfHeaderSize] = {0};
        std::memcpy(hdr + 0, kIvfMagic, 4);
        std::memcpy(hdr + 4, &kIvfVersion, 4);
        const std::uint32_t flags = 0;  // bit0 = 1-bit 码区（M3.5 预留）
        std::memcpy(hdr + 8, &flags, 4);
        std::memcpy(hdr + 12, &dim, 2);
        std::memcpy(hdr + 16, &nlist, 4);
        const auto count64 = static_cast<std::uint64_t>(n);
        std::memcpy(hdr + 24, &count64, 8);
        std::memcpy(hdr + 32, &max_ord, 8);
        std::memcpy(hdr + 40, &gen, 8);
        std::memcpy(hdr + 48, &cent_off, 8);
        std::memcpy(hdr + 56, &cidx_off, 8);
        std::memcpy(hdr + 64, &post_off, 8);
        std::memcpy(hdr + 72, &file_len, 8);
        const std::uint32_t hcrc =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(hdr), kIvfHeaderCrcOff));
        std::memcpy(hdr + kIvfHeaderCrcOff, &hcrc, 4);
        ok = pwrite_all(fd, hdr, kIvfHeaderSize, 0);
    }
    if (ok) ok = ::fdatasync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), fp.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool IvfSegment::open(std::string_view path, std::uint16_t dim,
                      std::uint64_t expected_gen, bool verify_crc) {
    close();
    const std::string fp(path);
    const int fd = ::open(fp.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::uint8_t hdr[kIvfHeaderSize];
    if (::read(fd, hdr, kIvfHeaderSize) !=
        static_cast<ssize_t>(kIvfHeaderSize)) {
        ::close(fd);
        return false;
    }
    if (std::memcmp(hdr, kIvfMagic, 4) != 0) {
        ::close(fd);
        return false;
    }
    std::uint32_t ver = 0, flags = 0, nlist = 0;
    std::uint16_t fdim = 0;
    std::uint64_t count = 0, max_ord = 0, gen = 0;
    std::uint64_t cent_off = 0, cidx_off = 0, post_off = 0, file_len = 0;
    std::memcpy(&ver, hdr + 4, 4);
    std::memcpy(&flags, hdr + 8, 4);
    std::memcpy(&fdim, hdr + 12, 2);
    std::memcpy(&nlist, hdr + 16, 4);
    std::memcpy(&count, hdr + 24, 8);
    std::memcpy(&max_ord, hdr + 32, 8);
    std::memcpy(&gen, hdr + 40, 8);
    std::memcpy(&cent_off, hdr + 48, 8);
    std::memcpy(&cidx_off, hdr + 56, 8);
    std::memcpy(&post_off, hdr + 64, 8);
    std::memcpy(&file_len, hdr + 72, 8);
    std::uint32_t stored = 0;
    std::memcpy(&stored, hdr + kIvfHeaderCrcOff, 4);
    const std::uint32_t calc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hdr), kIvfHeaderCrcOff));
    if (ver != kIvfVersion || flags != 0 || fdim != dim || stored != calc) {
        ::close(fd);
        return false;
    }
    if (expected_gen != 0 && gen != 0 && gen != expected_gen) {
        ::close(fd);
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 ||
        static_cast<std::uint64_t>(st.st_size) < file_len) {
        ::close(fd);
        return false;
    }
    // 布局自洽性（防越界寻址——mmap 后所有区指针由这些偏移导出）。
    const std::size_t stride = static_cast<std::size_t>(dim) + 16;
    const std::uint64_t cent_bytes =
        static_cast<std::uint64_t>(nlist) * dim * sizeof(float);
    const std::uint64_t cidx_bytes =
        static_cast<std::uint64_t>(nlist) * kCidxEntrySize;
    if (cent_off != kIvfHeaderSize || cidx_off != cent_off + cent_bytes ||
        post_off != cidx_off + cidx_bytes ||
        file_len != post_off + count * stride) {
        ::close(fd);
        return false;
    }
    void* raw = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                       PROT_READ, MAP_SHARED, fd, 0);
    if (raw == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    base_  = static_cast<const std::uint8_t*>(raw);
    raw_   = raw;
    len_   = static_cast<std::size_t>(st.st_size);
    fd_    = fd;
    dim_   = dim;
    nlist_ = nlist;
    count_ = count;
    max_ord_ = max_ord;
    gen_   = gen;
    centroids_ = reinterpret_cast<const float*>(base_ + cent_off);
    cidx_ = base_ + cidx_off;
    post_off_ = post_off;

    if (verify_crc) {
        // 逐簇 CRC + cidx 边界校验（S30 封口段 open 验 CRC 同款）。
        for (std::uint32_t c = 0; c < nlist_; ++c) {
            const std::uint8_t* e = cidx_ + static_cast<std::size_t>(c) *
                                                kCidxEntrySize;
            std::uint64_t off = 0;
            std::uint32_t cnt = 0, crc = 0;
            std::memcpy(&off, e, 8);
            std::memcpy(&cnt, e + 8, 4);
            std::memcpy(&crc, e + 12, 4);
            const std::uint64_t bytes =
                static_cast<std::uint64_t>(cnt) * stride;
            if (off < post_off || off + bytes > file_len) {
                close();
                return false;
            }
            if (bytes > 0) {
                const std::uint32_t got =
                    bitcask::codec::crc32(std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(base_ + off),
                        static_cast<std::size_t>(bytes)));
                if (got != crc) {
                    close();
                    return false;
                }
            }
        }
    }
    return true;
}

std::vector<IvfSegment::Hit> IvfSegment::search(
    std::span<const float> query, std::size_t k, std::uint32_t nprobe,
    const std::function<bool(std::uint64_t)>* live) const {
    std::vector<Hit> out;
    if (!opened() || count_ == 0 || k == 0 || query.size() != dim_) return out;
    if (nprobe == 0) nprobe = std::max<std::uint32_t>(nlist_ / 32, 8);
    nprobe = std::min(nprobe, nlist_);
    const std::size_t d = dim_;
    const std::size_t stride = rec_stride();

    // 1) 质心暴扫 top-nprobe。
    std::vector<std::pair<float, std::uint32_t>> cs(nlist_);
    for (std::uint32_t c = 0; c < nlist_; ++c) {
        cs[c] = {dot_f32(centroids_ + static_cast<std::size_t>(c) * d,
                         query.data(), d),
                 c};
    }
    std::partial_sort(cs.begin(), cs.begin() + nprobe, cs.end(),
                      [](const auto& a, const auto& b) {
                          return a.first > b.first;
                      });

    // 2) 查询量化一次（thread_local 复用）。
    thread_local int8::QVector qv;
    int8::quantize_into(query.data(), d, qv);
    int8::Int8DotFn kern = int8::pick_int8_dot_kernel();
    if (kern == nullptr) kern = &int8::dot_scalar_raw;

    // 3) posting 顺序扫 + 小顶堆 top-k（live 过滤在入堆前惰性调用——
    //    仅当分数进得了堆才付回调成本）。
    std::vector<std::pair<float, std::uint64_t>> heap;  // (score, ord) 小顶
    heap.reserve(k + 1);
    auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };
    for (std::uint32_t pi = 0; pi < nprobe; ++pi) {
        const std::uint32_t c = cs[pi].second;
        const std::uint8_t* e =
            cidx_ + static_cast<std::size_t>(c) * kCidxEntrySize;
        std::uint64_t off = 0;
        std::uint32_t cnt = 0;
        std::memcpy(&off, e, 8);
        std::memcpy(&cnt, e + 8, 4);
        const std::uint8_t* rec = base_ + off;
        for (std::uint32_t i = 0; i < cnt; ++i, rec += stride) {
            const auto* codes =
                reinterpret_cast<const std::int8_t*>(rec + 8);
            float scale;
            std::int32_t sum;
            std::memcpy(&scale, rec + 8 + d, 4);
            std::memcpy(&sum, rec + 8 + d + 4, 4);
            const float score =
                kern(qv.codes.data(), codes, sum, qv.scale, scale, d);
            if (heap.size() >= k && score <= heap.front().first) continue;
            std::uint64_t ord;
            std::memcpy(&ord, rec, 8);
            if (live != nullptr && *live && !(*live)(ord)) continue;
            heap.push_back({score, ord});
            std::push_heap(heap.begin(), heap.end(), cmp);
            if (heap.size() > k) {
                std::pop_heap(heap.begin(), heap.end(), cmp);
                heap.pop_back();
            }
        }
    }
    // sort_heap 按 cmp(greater)排定 → 分数降序（is_sorted(comp) 语义）。
    std::sort_heap(heap.begin(), heap.end(), cmp);
    out.reserve(heap.size());
    for (const auto& [s, o] : heap) out.push_back({o, s});
    return out;
}

}  // namespace bitcask::vec
