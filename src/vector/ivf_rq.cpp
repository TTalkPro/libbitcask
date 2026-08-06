// IvfSegment 实现（S32-M3 v1）。格式/契约见 include/bitcask/ivf_rq.hpp。

#include "bitcask/ivf_rq.hpp"

#include <fcntl.h>
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
#include "vec_disk_internal.hpp"             // S32-M5 抽取:共用内部件

namespace bitcask::vec {

namespace {

constexpr char          kIvfMagic[4] = {'B', 'I', 'V', '1'};
constexpr std::uint32_t kIvfVersion  = 1;
constexpr std::size_t   kIvfHeaderSize = 96;
constexpr std::size_t   kIvfHeaderCrcOff = 92;
constexpr std::size_t   kCidxEntrySize = 16;  // off u64 | count u32 | crc u32
constexpr std::uint32_t kIvfVersion2 = 2;     // S32-M3.5-②:+1-bit 码区
constexpr std::uint32_t kFlagBits    = 1u;    // flags bit0 = bits 区存在

using diskint::pwrite_all;
using diskint::dot_f32;
using diskint::parallel_for;
using diskint::sign_encode;
using diskint::hamming_bytes;

// S32-M3.5-③:两级质心分组参数。nlist < 64 不分组（组区仅 nc2=0 标记）。
constexpr std::uint32_t kGroupMinNlist = 64;
inline std::uint32_t group_count(std::uint32_t nlist) {
    if (nlist < kGroupMinNlist) return 0;
    return std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(
            2.0 * std::sqrt(static_cast<double>(nlist))),
        8, 1024);
}
// 查询/assign 的探组数:覆盖 top-want 质心所需组数 ×2 冗余。
inline std::uint32_t probe_groups(std::uint32_t want, std::uint32_t nlist,
                                  std::uint32_t nc2) {
    const std::uint32_t avg = std::max<std::uint32_t>(1, nlist / nc2);
    return std::clamp<std::uint32_t>((want + avg - 1) / avg * 2 + 2, 4, nc2);
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
    map_.reset();  // B3：RAII munmap
    base_ = nullptr;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    centroids_ = nullptr;
    cidx_      = nullptr;
    post_off_  = 0;
    bits_      = nullptr;
    count_ = 0;
    nlist_ = 0;
}

bool IvfSegment::build(std::string_view path, std::uint16_t dim,
                       const IvfBuildSource& src, std::uint32_t nlist,
                       std::uint64_t gen, std::uint64_t seed,
                       bool with_bits) try {
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

    // ---- 1b) 质心两级分组（S32-M3.5-③）:质心再聚 nc2 组——assign 与
    // 查询的质心选择都从 O(nlist) 降到 O(nc2 + G·组均)。
    const std::uint32_t nc2 = with_bits ? group_count(nlist) : 0;
    std::vector<float> gcent;
    std::vector<std::uint32_t> group_off;   // nc2+1
    std::vector<std::uint32_t> group_members;  // nlist
    if (nc2 > 0) {
        gcent.assign(static_cast<std::size_t>(nc2) * d, 0.0f);
        std::vector<std::uint32_t> ga(nlist, 0);
        std::mt19937_64 grng(seed ^ 0x9E3779B97F4A7C15ull);
        // 初始化:均匀取质心为组心。
        for (std::uint32_t g = 0; g < nc2; ++g) {
            const std::uint32_t c =
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(g) * nlist) / nc2);
            std::memcpy(gcent.data() + static_cast<std::size_t>(g) * d,
                        centers.data() + static_cast<std::size_t>(c) * d,
                        d * sizeof(float));
        }
        for (int iter = 0; iter < 4; ++iter) {
            parallel_for(nlist, [&](std::size_t c) {
                const float* v =
                    centers.data() + static_cast<std::size_t>(c) * d;
                float best = -2.0f;
                std::uint32_t bg = 0;
                for (std::uint32_t g = 0; g < nc2; ++g) {
                    const float sc = dot_f32(
                        gcent.data() + static_cast<std::size_t>(g) * d, v, d);
                    if (sc > best) { best = sc; bg = g; }
                }
                ga[c] = bg;
            });
            std::vector<double> acc(static_cast<std::size_t>(nc2) * d, 0.0);
            std::vector<std::uint32_t> cnt2(nc2, 0);
            for (std::uint32_t c = 0; c < nlist; ++c) {
                const float* v =
                    centers.data() + static_cast<std::size_t>(c) * d;
                double* a = acc.data() + static_cast<std::size_t>(ga[c]) * d;
                for (std::size_t j = 0; j < d; ++j) a[j] += v[j];
                ++cnt2[ga[c]];
            }
            for (std::uint32_t g = 0; g < nc2; ++g) {
                float* gc = gcent.data() + static_cast<std::size_t>(g) * d;
                if (cnt2[g] == 0) {
                    const auto rc = static_cast<std::uint32_t>(grng() % nlist);
                    std::memcpy(gc,
                                centers.data() +
                                    static_cast<std::size_t>(rc) * d,
                                d * sizeof(float));
                    continue;
                }
                const double* a = acc.data() + static_cast<std::size_t>(g) * d;
                double sq = 0.0;
                for (std::size_t j = 0; j < d; ++j) sq += a[j] * a[j];
                const double inv = sq > 0.0 ? 1.0 / std::sqrt(sq) : 0.0;
                for (std::size_t j = 0; j < d; ++j) {
                    gc[j] = static_cast<float>(a[j] * inv);
                }
            }
        }
        // CSR。
        group_off.assign(nc2 + 1, 0);
        group_members.assign(nlist, 0);
        for (std::uint32_t c = 0; c < nlist; ++c) ++group_off[ga[c] + 1];
        for (std::uint32_t g = 0; g < nc2; ++g) {
            group_off[g + 1] += group_off[g];
        }
        std::vector<std::uint32_t> cur(group_off.begin(),
                                       group_off.end() - 1);
        for (std::uint32_t c = 0; c < nlist; ++c) {
            group_members[cur[ga[c]]++] = c;
        }
    }

    // ---- 2) 全量分簇（并行；cid 表 N×4B）。nc2>0 走两级路由:先组后
    // 组内质心——assign 亚优落簇无正确性影响(查询 probe 多簇冗余)。----
    std::vector<std::uint32_t> cid(n, 0);
    const std::uint32_t g_probe =
        nc2 > 0 ? probe_groups(1, nlist, nc2) : 0;
    parallel_for(n, [&](std::size_t i) {
        std::uint64_t ord;
        const float* v = nullptr;
        src.get(static_cast<std::uint32_t>(i), ord, v);
        float best = -2.0f;
        std::uint32_t bc = 0;
        if (nc2 > 0) {
            // 两级:top-g_probe 组 → 组内质心。
            thread_local std::vector<std::pair<float, std::uint32_t>> gs;
            gs.resize(nc2);
            for (std::uint32_t g = 0; g < nc2; ++g) {
                gs[g] = {dot_f32(gcent.data() +
                                     static_cast<std::size_t>(g) * d,
                                 v, d),
                         g};
            }
            std::partial_sort(gs.begin(), gs.begin() + g_probe, gs.end(),
                              [](const auto& a, const auto& b) {
                                  return a.first > b.first;
                              });
            for (std::uint32_t gi = 0; gi < g_probe; ++gi) {
                const std::uint32_t g = gs[gi].second;
                for (std::uint32_t m = group_off[g]; m < group_off[g + 1];
                     ++m) {
                    const std::uint32_t c = group_members[m];
                    const float sc = dot_f32(
                        centers.data() + static_cast<std::size_t>(c) * d, v,
                        d);
                    if (sc > best) { best = sc; bc = c; }
                }
            }
        } else {
            for (std::uint32_t c = 0; c < nlist; ++c) {
                const float sc = dot_f32(
                    centers.data() + static_cast<std::size_t>(c) * d, v, d);
                if (sc > best) { best = sc; bc = c; }
            }
        }
        cid[i] = bc;
    });

    // ---- 3) 布局：簇计数 → 记录偏移 ----
    const std::size_t stride = static_cast<std::size_t>(dim) + 16;
    std::vector<std::uint32_t> ccount(nlist, 0);
    for (std::uint32_t i = 0; i < n; ++i) ++ccount[cid[i]];
    const std::size_t sbits =
        (static_cast<std::size_t>(dim) + 7) / 8 + sizeof(float);
    const std::uint64_t cent_off = kIvfHeaderSize;
    const std::uint64_t cidx_off =
        cent_off + static_cast<std::uint64_t>(nlist) * d * sizeof(float);
    const std::uint64_t gidx_off =
        cidx_off + static_cast<std::uint64_t>(nlist) * kCidxEntrySize;
    const std::uint64_t gidx_len =
        !with_bits ? 0
                   : 4 + (nc2 > 0
                              ? (static_cast<std::uint64_t>(nc2) + 1) * 4 +
                                    static_cast<std::uint64_t>(nlist) * 4 +
                                    static_cast<std::uint64_t>(nc2) * d *
                                        sizeof(float)
                              : 0);
    const std::uint64_t bits_off = gidx_off + gidx_len;
    const std::uint64_t bits_len =
        with_bits ? static_cast<std::uint64_t>(n) * sbits : 0;
    const std::uint64_t post_off = bits_off + bits_len;
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
    diskint::TmpFile tf;
    tf.path = fp + ".tmp";
    tf.fd = ::open(tf.path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                   0644);
    if (tf.fd < 0) return false;
    const int fd = tf.fd;
    bool ok = true;
    std::uint64_t max_ord = 0;
    // 质心区。
    ok = pwrite_all(fd, centers.data(),
                    static_cast<std::size_t>(nlist) * d * sizeof(float),
                    cent_off);
    // 组区（S32-M3.5-③;仅 ver=2）。
    if (ok && with_bits) {
        std::vector<std::uint8_t> gbuf;
        gbuf.reserve(static_cast<std::size_t>(gidx_len));
        auto putb = [&gbuf](const void* src2, std::size_t len) {
            const auto* b = static_cast<const std::uint8_t*>(src2);
            gbuf.insert(gbuf.end(), b, b + len);
        };
        putb(&nc2, 4);
        if (nc2 > 0) {
            putb(group_off.data(), (static_cast<std::size_t>(nc2) + 1) * 4);
            putb(group_members.data(), static_cast<std::size_t>(nlist) * 4);
            putb(gcent.data(),
                 static_cast<std::size_t>(nc2) * d * sizeof(float));
        }
        ok = gbuf.size() == gidx_len &&
             pwrite_all(fd, gbuf.data(), gbuf.size(), gidx_off);
    }
    // 记录区（单线程顺序过源——量化 + 定位写；游标按簇推进）。
    if (ok) {
        std::vector<std::uint64_t> cursor(cbase);
        std::vector<std::uint8_t> rec(stride);
        std::vector<std::uint8_t> brec(sbits);
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
            if (ok && with_bits) {
                // bits 记录与 posting 记录同槽位（同序偏移换 stride）。
                float mu = 0.0f;
                sign_encode(v, d, brec.data(), &mu);
                std::memcpy(brec.data() + sbits - 4, &mu, 4);
                const std::uint64_t rec_idx =
                    (cursor[cid[i]] - post_off) / stride;
                ok = pwrite_all(fd, brec.data(), sbits,
                                bits_off + rec_idx * sbits);
            }
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
                ok = diskint::pread_all(fd, buf.data(), bytes, cbase[c]);
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
    // bits 区 CRC（顺序回读；页缓存已热）。
    std::uint32_t bits_crc = 0;
    if (ok && with_bits && bits_len > 0) {
        std::vector<std::uint8_t> bbuf(static_cast<std::size_t>(bits_len));
        ok = diskint::pread_all(fd, bbuf.data(), bbuf.size(), bits_off);
        if (ok) {
            bits_crc = bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(bbuf.data()),
                bbuf.size()));
        }
    }
    // header（字段全就位后 CRC）。
    if (ok) {
        std::uint8_t hdr[kIvfHeaderSize] = {0};
        std::memcpy(hdr + 0, kIvfMagic, 4);
        const std::uint32_t ver = with_bits ? kIvfVersion2 : kIvfVersion;
        std::memcpy(hdr + 4, &ver, 4);
        const std::uint32_t flags = with_bits ? kFlagBits : 0;
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
        if (with_bits) {
            std::memcpy(hdr + 80, &bits_off, 8);
            std::memcpy(hdr + 88, &bits_crc, 4);
        }
        const std::uint32_t hcrc =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(hdr), kIvfHeaderCrcOff));
        std::memcpy(hdr + kIvfHeaderCrcOff, &hcrc, 4);
        ok = pwrite_all(fd, hdr, kIvfHeaderSize, 0);
    }
    if (ok) ok = tf.sync_close();
    if (!ok || std::rename(tf.path.c_str(), fp.c_str()) != 0) {
        return false;  // TmpFile 析构清 tmp
    }
    tf.committed = true;
    return true;
} catch (...) {
    return false;  // bad_alloc 等;TmpFile 析构保证 fd/tmp 清理（审计修复）
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
    std::uint64_t bits_off = 0;
    std::uint32_t bits_crc = 0;
    std::memcpy(&bits_off, hdr + 80, 8);
    std::memcpy(&bits_crc, hdr + 88, 4);
    std::uint32_t stored = 0;
    std::memcpy(&stored, hdr + kIvfHeaderCrcOff, 4);
    const std::uint32_t calc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hdr), kIvfHeaderCrcOff));
    const bool ver_ok =
        (ver == kIvfVersion && flags == 0) ||
        (ver == kIvfVersion2 && (flags & ~kFlagBits) == 0);
    if (!ver_ok || fdim != dim || stored != calc) {
        ::close(fd);
        return false;
    }
    const bool has_bits = (flags & kFlagBits) != 0;
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
    const std::size_t sbits =
        (static_cast<std::size_t>(dim) + 7) / 8 + sizeof(float);
    const std::uint64_t cent_bytes =
        static_cast<std::uint64_t>(nlist) * dim * sizeof(float);
    const std::uint64_t cidx_bytes =
        static_cast<std::uint64_t>(nlist) * kCidxEntrySize;
    const std::uint64_t bits_len = has_bits ? count * sbits : 0;
    const std::uint64_t gidx_off = cidx_off + cidx_bytes;
    // ver=1:无组区无 bits;ver=2:组区 [gidx_off, bits_off)（自描述,mmap
    // 后二次校验 nc2/CSR）,bits [bits_off, post_off)。
    const bool v2 = ver == kIvfVersion2;
    const std::uint64_t gidx_end = v2 ? bits_off : gidx_off;
    if (cent_off != kIvfHeaderSize || cidx_off != cent_off + cent_bytes ||
        (v2 && (bits_off < gidx_off + 4 || !has_bits)) ||
        (!v2 && (bits_off != 0 || has_bits)) ||
        post_off != gidx_end + bits_len ||
        file_len != post_off + count * stride) {
        ::close(fd);
        return false;
    }
    map_ = io::MappedFile::map_readonly(
        fd, static_cast<std::size_t>(st.st_size), /*advise_random=*/false);
    if (!map_.valid()) {
        ::close(fd);
        return false;
    }
    base_  = reinterpret_cast<const std::uint8_t*>(map_.data());
    fd_    = fd;
    dim_   = dim;
    nlist_ = nlist;
    count_ = count;
    max_ord_ = max_ord;
    gen_   = gen;
    centroids_ = reinterpret_cast<const float*>(base_ + cent_off);
    cidx_ = base_ + cidx_off;
    post_off_ = post_off;
    bits_ = has_bits ? base_ + bits_off : nullptr;
    // 组区解析（S32-M3.5-③;自描述,长度/CSR 严格校验）。
    nc2_ = 0;
    group_off_ = nullptr;
    group_members_ = nullptr;
    gcent_ = nullptr;
    if (v2) {
        const std::uint8_t* g = base_ + gidx_off;
        std::uint32_t nc2 = 0;
        std::memcpy(&nc2, g, 4);
        const std::uint64_t want_len =
            4 + (nc2 > 0
                     ? (static_cast<std::uint64_t>(nc2) + 1) * 4 +
                           static_cast<std::uint64_t>(nlist) * 4 +
                           static_cast<std::uint64_t>(nc2) * dim *
                               sizeof(float)
                     : 0);
        if (bits_off - gidx_off != want_len) {
            close();
            return false;
        }
        if (nc2 > 0) {
            group_off_ = reinterpret_cast<const std::uint32_t*>(g + 4);
            group_members_ = group_off_ + nc2 + 1;
            gcent_ = reinterpret_cast<const float*>(
                g + 4 + (static_cast<std::size_t>(nc2) + 1) * 4 +
                static_cast<std::size_t>(nlist) * 4);
            if (group_off_[0] != 0 || group_off_[nc2] != nlist) {
                close();
                return false;
            }
            for (std::uint32_t gi = 0; gi < nc2; ++gi) {
                if (group_off_[gi] > group_off_[gi + 1]) {
                    close();
                    return false;
                }
            }
            nc2_ = nc2;
        }
    }

    if (verify_crc && has_bits && bits_len > 0) {
        const std::uint32_t got =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(bits_),
                static_cast<std::size_t>(bits_len)));
        if (got != bits_crc) {
            close();
            return false;
        }
    }
    // cidx 边界校验**无条件**（审计修复 2026-07-13）：此前误挂 verify_crc
    // 门——可信盘模式（verify_crc=false）下损坏 cidx 的 off/count 会把
    // 查询扫描指出 mmap 界外（OOB 读 UB）。内存安全前置不是完整性选项，
    // O(nlist) 纯头查无 IO 代价；CRC（逐簇全量读）保持可选。
    for (std::uint32_t c = 0; c < nlist_; ++c) {
        const std::uint8_t* e =
            cidx_ + static_cast<std::size_t>(c) * kCidxEntrySize;
        std::uint64_t off = 0;
        std::uint32_t cnt = 0, crc = 0;
        std::memcpy(&off, e, 8);
        std::memcpy(&cnt, e + 8, 4);
        std::memcpy(&crc, e + 12, 4);
        const std::uint64_t bytes = static_cast<std::uint64_t>(cnt) * stride;
        if (off < post_off || off + bytes > file_len) {
            close();
            return false;
        }
        if (verify_crc && bytes > 0) {
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
    return true;
}

std::vector<IvfSegment::Hit> IvfSegment::search(
    std::span<const float> query, std::size_t k, std::uint32_t nprobe,
    const std::function<bool(std::uint64_t)>* live,
    std::uint32_t coarse_c) const {
    std::vector<Hit> out;
    if (!opened() || count_ == 0 || k == 0 || query.size() != dim_) return out;
    if (nprobe == 0) nprobe = std::max<std::uint32_t>(nlist_ / 32, 8);
    nprobe = std::min(nprobe, nlist_);
    const std::size_t d = dim_;
    const std::size_t stride = rec_stride();

    // 1) 质心选择:top-nprobe。nprobe ≥ nlist → 全簇捷径（免排序,对拍
    // 精确）;组区在 → 两级（组心扫 + top-G 组成员扫,S32-M3.5-③）;
    // 否则全量暴扫。
    auto by_score = [](const auto& a, const auto& b) {
        return a.first > b.first;
    };
    std::vector<std::pair<float, std::uint32_t>> cs;
    if (nprobe >= nlist_) {
        cs.resize(nlist_);
        for (std::uint32_t c = 0; c < nlist_; ++c) cs[c] = {0.0f, c};
    } else if (nc2_ > 0) {
        const std::uint32_t g_probe = probe_groups(nprobe, nlist_, nc2_);
        std::vector<std::pair<float, std::uint32_t>> gs(nc2_);
        for (std::uint32_t g = 0; g < nc2_; ++g) {
            gs[g] = {dot_f32(gcent_ + static_cast<std::size_t>(g) * d,
                             query.data(), d),
                     g};
        }
        std::partial_sort(gs.begin(), gs.begin() + g_probe, gs.end(),
                          by_score);
        for (std::uint32_t gi = 0; gi < g_probe; ++gi) {
            const std::uint32_t g = gs[gi].second;
            for (std::uint32_t m = group_off_[g]; m < group_off_[g + 1];
                 ++m) {
                const std::uint32_t c = group_members_[m];
                cs.push_back(
                    {dot_f32(centroids_ + static_cast<std::size_t>(c) * d,
                             query.data(), d),
                     c});
            }
        }
        nprobe = std::min<std::uint32_t>(
            nprobe, static_cast<std::uint32_t>(cs.size()));
        std::partial_sort(cs.begin(), cs.begin() + nprobe, cs.end(),
                          by_score);
    } else {
        cs.resize(nlist_);
        for (std::uint32_t c = 0; c < nlist_; ++c) {
            cs[c] = {dot_f32(centroids_ + static_cast<std::size_t>(c) * d,
                             query.data(), d),
                     c};
        }
        std::partial_sort(cs.begin(), cs.begin() + nprobe, cs.end(),
                          by_score);
    }

    // 2) 查询量化一次（thread_local 复用）。
    thread_local int8::QVector qv;
    int8::quantize_into(query.data(), d, qv);
    int8::Int8DotFn kern = int8::pick_int8_dot_kernel();
    if (kern == nullptr) kern = &int8::dot_scalar_raw;

    // 3) posting 扫描 + 小顶堆 top-k（live 过滤在入堆前惰性调用——
    //    仅当分数进得了堆才付回调成本）。
    std::vector<std::pair<float, std::uint64_t>> heap;  // (score, ord) 小顶
    heap.reserve(k + 1);
    auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };

    // 阶段 B 共用的 int8 精排入堆。
    auto score_record = [&](const std::uint8_t* rec) {
        const auto* codes = reinterpret_cast<const std::int8_t*>(rec + 8);
        float scale;
        std::int32_t sum;
        std::memcpy(&scale, rec + 8 + d, 4);
        std::memcpy(&sum, rec + 8 + d + 4, 4);
        const float score =
            kern(qv.codes.data(), codes, sum, qv.scale, scale, d);
        if (heap.size() >= k && score <= heap.front().first) return;
        std::uint64_t ord;
        std::memcpy(&ord, rec, 8);
        if (live != nullptr && *live && !(*live)(ord)) return;
        heap.push_back({score, ord});
        std::push_heap(heap.begin(), heap.end(), cmp);
        if (heap.size() > k) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.pop_back();
        }
    };

    if (bits_ != nullptr) {
        // S32-M3.5-②:两段扫——阶段 A 对称 1-bit popcount 粗筛（字节量
        // 8× 缩,est = μ_v·(d − 2·hamming),μ_q/常数不影响排序）取 top-C,
        // 阶段 B 仅对 C 个候选 int8 精排。est 有损,召回由 C 冗余兜底。
        const std::size_t sbits = bits_stride();
        const std::size_t nbytes = sbits - sizeof(float);
        thread_local std::vector<std::uint8_t> qbits;
        qbits.resize(nbytes);
        {
            float mu_q_unused = 0.0f;
            sign_encode(query.data(), d, qbits.data(), &mu_q_unused);
        }
        const std::size_t cc =
            coarse_c != 0 ? coarse_c
                          : std::max<std::size_t>(8 * k, 128);
        // (est, 全局记录序) 小顶堆。
        std::vector<std::pair<float, std::uint64_t>> coarse;
        coarse.reserve(cc + 1);
        for (std::uint32_t pi = 0; pi < nprobe; ++pi) {
            const std::uint32_t c = cs[pi].second;
            const std::uint8_t* e =
                cidx_ + static_cast<std::size_t>(c) * kCidxEntrySize;
            std::uint64_t off = 0;
            std::uint32_t cnt = 0;
            std::memcpy(&off, e, 8);
            std::memcpy(&cnt, e + 8, 4);
            const std::uint64_t prefix = (off - post_off_) / stride;
            const std::uint8_t* b = bits_ + prefix * sbits;
            for (std::uint32_t i = 0; i < cnt; ++i, b += sbits) {
                const std::uint32_t h =
                    hamming_bytes(qbits.data(), b, nbytes);
                float mu;
                std::memcpy(&mu, b + nbytes, 4);
                const float est =
                    mu * static_cast<float>(static_cast<std::int32_t>(d) -
                                            2 * static_cast<std::int32_t>(h));
                if (coarse.size() >= cc && est <= coarse.front().first) {
                    continue;
                }
                coarse.push_back({est, prefix + i});
                std::push_heap(coarse.begin(), coarse.end(), cmp);
                if (coarse.size() > cc) {
                    std::pop_heap(coarse.begin(), coarse.end(), cmp);
                    coarse.pop_back();
                }
            }
        }
        for (const auto& [est, idx] : coarse) {
            score_record(base_ + post_off_ + idx * stride);
        }
    } else {
        // v1 文件:单段 int8 全量扫。
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
                score_record(rec);
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
