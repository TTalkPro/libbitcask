// DiskannSegment 实现（S32-M5 v1）。格式/契约见 include/bitcask/diskann.hpp。
//
// Vamana 建图 = 增量插入（Subramanya 等, NeurIPS 2019, Algorithm 1/2）：
// 逐点 beam search 取候选 → RobustPrune(α=1.2) 选边 → 反向边回插+超容
// 回剪。全程 int8 域（dist = -重建内积，越小越近——与 HNSW kDot 同约定）。

#include "bitcask/diskann.hpp"


#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "bitcask/codec.hpp"                // codec::crc32
#include "bitcask/detail/int8_kernels.hpp"  // 量化/int8 dot（与 HNSW 同源）
#include "vec_disk_internal.hpp"

namespace bitcask::vec {

using diskint::dot_f32;
using diskint::parallel_for;
using diskint::pwrite_all;

namespace {

constexpr char          kBdaMagic[4] = {'B', 'D', 'A', '1'};
constexpr std::uint32_t kBdaVersion = 1;
constexpr std::size_t   kBdaHeaderSize = 96;
constexpr std::size_t   kBdaHeaderCrcOff = 92;

constexpr float kAlpha = 1.2f;  // RobustPrune 放松系数（论文推荐值,末趟）

// 分层多起点入口集：medoid + 15 个均匀 stride 点（count 确定性推导,无需
// 入格式）。动机：极端聚簇语料的距离二态分布（簇内≈0.02/跨簇≈1.0）使
// α 覆盖判据剪光跨簇长边 → 图成簇孤岛,单 medoid 起点 beam 只达本簇
// （100k/781 簇实测 recall 归零;HNSW 层级/IVF 质心路由天然免疫此形态）。
// build 与 search 共用同一入口集：建图 beam 的 visited 因此跨岛,长边
// 在 prune 的 cand 里有机会存活;查询侧多起点再兜一层。
inline void seed_entries(std::uint64_t count, std::uint32_t medoid,
                         std::vector<std::uint32_t>& out) {
    out.clear();
    out.push_back(medoid);
    if (count <= 1) return;
    constexpr std::uint32_t kEntries = 16;
    const std::uint64_t step = std::max<std::uint64_t>(1, count / kEntries);
    for (std::uint64_t i = step / 2; i < count; i += step) {
        const auto id = static_cast<std::uint32_t>(i);
        if (id != medoid) out.push_back(id);
    }
}

// 建图期的内存态图 + int8 码字（build 私有）。
struct BuildCtx {
    std::uint32_t n = 0;
    std::uint16_t dim = 0;
    std::uint32_t r = 0;
    int8::Int8DotFn kern = nullptr;
    std::vector<std::int8_t>  codes;   // n × dim
    std::vector<float>        scales;  // n
    std::vector<std::int32_t> sums;    // n
    std::vector<std::uint32_t> nbrs;   // n × r（前 ncnt 有效）
    std::vector<std::uint32_t> ncnt;   // n

    // dist = 1 − 重建内积（余弦距离,归一化向量下 ∈ [0,2]）。**必须非负**：
    // RobustPrune 的 α 缩放假设度量距离 ≥ 0（论文 L2 域）——曾用 -dot
    // （负值域）,α 乘负数使覆盖条件反向放大 → 全图过度剪枝,l=64 召回
    // 崩至 0.58 且 est 换精确分无效（图坏,非排序坏）。
    [[nodiscard]] float dist(std::uint32_t a, std::uint32_t b) const {
        return 1.0f -
               kern(codes.data() + static_cast<std::size_t>(a) * dim,
                    codes.data() + static_cast<std::size_t>(b) * dim,
                    sums[b], scales[a], scales[b], dim);
    }
};

// 建图 beam search：从 medoid 出发，池宽 l，返回访问过的 (dist, id) 升序。
// visited 用版本戳（每次调用 ++gen，免 O(n) 清零——n 次插入合计 O(n²) 陷阱）。
void build_beam(const BuildCtx& c, std::uint32_t q,
                const std::vector<std::uint32_t>& entries, std::uint32_t l,
                std::vector<std::pair<float, std::uint32_t>>& out_visited,
                std::vector<std::uint32_t>& visited_mark,
                std::uint32_t gen) {
    out_visited.clear();
    // 池：(dist, id, expanded) 按 dist 升序维护（l 小，插入排序足够）。
    struct Ent {
        float dist;
        std::uint32_t id;
        bool expanded;
    };
    std::vector<Ent> pool;
    pool.reserve(l + 1);
    auto insert_pool = [&](std::uint32_t id) {
        if (visited_mark[id] == gen) return;
        visited_mark[id] = gen;
        const float dv = c.dist(q, id);
        if (pool.size() >= l && dv >= pool.back().dist) return;
        const Ent e{dv, id, false};
        const auto it = std::lower_bound(
            pool.begin(), pool.end(), e,
            [](const Ent& a, const Ent& b) { return a.dist < b.dist; });
        pool.insert(it, e);
        if (pool.size() > l) pool.pop_back();
    };
    for (const std::uint32_t e0 : entries) insert_pool(e0);
    for (;;) {
        Ent* best = nullptr;
        for (auto& e : pool) {
            if (!e.expanded) { best = &e; break; }
        }
        if (best == nullptr) break;
        best->expanded = true;
        const std::uint32_t u = best->id;
        out_visited.push_back({best->dist, u});
        const std::uint32_t cnt = c.ncnt[u];
        const std::uint32_t* nb =
            c.nbrs.data() + static_cast<std::size_t>(u) * c.r;
        for (std::uint32_t i = 0; i < cnt; ++i) insert_pool(nb[i]);
    }
    std::sort(out_visited.begin(), out_visited.end());
}

// RobustPrune（论文 Algorithm 2）+ **桥边配额**：cand 为 (dist_to_p, id)
// 集（可含重复，不含 p），输出 ≤ r 条边写回 nbrs[p]。
//
// 桥边配额（q = max(2, r/8)）：贪心近序填边被紧簇形态打穿——簇内三角
// "扁平"（成员两两等距）使 α 覆盖互剪失效,~128 个簇内候选在 sorted 序里
// 先耗尽全部 r 槽,跨簇候选永远轮不到 → 图成孤岛（100k/781 簇 recall
// 归零的第二半根因,与多起点互补）。近序填至 r−q 后,保留 q 个**最远**
// 未选候选做跨岛长边（NSG 系工程惯例;真实连续分布下 α 覆盖自然让位,
// 配额近似无损）。
void robust_prune(BuildCtx& c, std::uint32_t p,
                  std::vector<std::pair<float, std::uint32_t>>& cand,
                  float alpha) {
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end(),
                           [](const auto& a, const auto& b) {
                               return a.second == b.second;
                           }),
               cand.end());
    std::vector<std::uint8_t> pruned(cand.size(), 0);
    std::uint32_t* out = c.nbrs.data() + static_cast<std::size_t>(p) * c.r;
    std::uint32_t cnt = 0;
    const std::uint32_t bridge_q = std::max<std::uint32_t>(2, c.r / 8);
    const std::uint32_t near_cap = c.r > bridge_q ? c.r - bridge_q : c.r;
    std::vector<std::uint8_t> chosen(cand.size(), 0);
    for (std::size_t i = 0; i < cand.size() && cnt < near_cap; ++i) {
        if (pruned[i] != 0 || cand[i].second == p) continue;
        const std::uint32_t v = cand[i].second;
        out[cnt++] = v;
        chosen[i] = 1;
        for (std::size_t j = i + 1; j < cand.size(); ++j) {
            if (pruned[j] != 0) continue;
            const std::uint32_t u = cand[j].second;
            if (alpha * c.dist(v, u) <= cand[j].first) pruned[j] = 1;
        }
    }
    // 桥边：从尾部（最远）补未选候选（无视 pruned——长边本就是被覆盖
    // 判据误伤的对象）。
    for (std::size_t ri = cand.size(); ri > 0 && cnt < c.r; --ri) {
        const std::size_t i = ri - 1;
        if (chosen[i] != 0 || cand[i].second == p) continue;
        out[cnt++] = cand[i].second;
        chosen[i] = 1;
    }
    c.ncnt[p] = cnt;
}

}  // namespace

DiskannSegment::~DiskannSegment() { close(); }

void DiskannSegment::close() {
    map_.reset();  // B3：RAII munmap
    base_ = nullptr;
    if (fd_ >= 0) {
        io::close_handle(fd_);
        fd_ = -1;
    }
    nav_.clear();
    nav_.shrink_to_fit();
    count_ = 0;
}

bool DiskannSegment::build(std::string_view path, std::uint16_t dim,
                           const IvfBuildSource& src, std::uint32_t r,
                           std::uint32_t l_build, std::uint64_t gen,
                           std::uint64_t seed) try {
    if (dim == 0) return false;
    const std::uint32_t n = src.count;
    if (n > 0 && !src.get) return false;
    if (r == 0) r = 32;
    if (n > 1) r = std::min(r, n - 1);
    if (l_build == 0) l_build = std::max<std::uint32_t>(64, 2 * r);
    const std::size_t d = dim;

    BuildCtx c;
    c.n = n;
    c.dim = dim;
    c.r = r;
    c.kern = int8::pick_int8_dot_kernel();
    if (c.kern == nullptr) c.kern = &int8::dot_scalar_raw;
    c.codes.resize(static_cast<std::size_t>(n) * d);
    c.scales.resize(n);
    c.sums.resize(n);
    c.nbrs.assign(static_cast<std::size_t>(n) * r, 0);
    c.ncnt.assign(n, 0);

    const std::size_t snav = d + 8;  // int8 codes | scale f32 | sum i32
    std::vector<std::uint8_t> nav(static_cast<std::size_t>(n) * snav);
    std::vector<std::uint64_t> ords(n, 0);

    // ---- pass 1：量化 + 导航码 + 均值累计（并行；均值分线程局部再合）----
    const std::size_t nthreads =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
    std::vector<std::vector<double>> mean_parts(
        nthreads, std::vector<double>(d, 0.0));
    // 工位号 wid 替代旧 thread_local tls_tid 方案（审计脆弱性修复
    // 2026-07-13:tls 跨调用残留仅在 nthreads 恒定时安全）。
    diskint::parallel_for_worker(n, [&](std::size_t i, std::size_t wid) {
        thread_local int8::QVector qv;
        std::uint64_t ord;
        const float* v = nullptr;
        src.get(static_cast<std::uint32_t>(i), ord, v);
        ords[i] = ord;
        int8::quantize_into(v, d, qv);
        std::memcpy(c.codes.data() + i * d, qv.codes.data(), d);
        c.scales[i] = qv.scale;
        c.sums[i]   = qv.sum_codes;
        // nav 记录 = int8 三件套（v1;M5.5 换 PQ 时 bump ver）。
        std::memcpy(nav.data() + i * snav, qv.codes.data(), d);
        std::memcpy(nav.data() + i * snav + d, &qv.scale, 4);
        std::memcpy(nav.data() + i * snav + d + 4, &qv.sum_codes, 4);
        auto& mp = mean_parts[wid];
        for (std::size_t j = 0; j < d; ++j) {
            mp[j] += static_cast<double>(v[j]);
        }
    });
    std::uint64_t max_ord = 0;
    for (std::uint32_t i = 0; i < n; ++i) max_ord = std::max(max_ord, ords[i]);

    // ---- medoid：均值向量的 int8 最近点（全扫一次）----
    std::uint32_t medoid = 0;
    if (n > 0) {
        std::vector<float> mean(d, 0.0f);
        double sq = 0.0;
        for (std::size_t j = 0; j < d; ++j) {
            double acc = 0.0;
            for (std::size_t t = 0; t < nthreads; ++t) {
                acc += mean_parts[t][j];
            }
            mean[j] = static_cast<float>(acc);
            sq += acc * acc;
        }
        if (sq > 0.0) {
            const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
            for (auto& x : mean) x *= inv;
        }
        int8::QVector mq;
        int8::quantize_into(mean.data(), d, mq);
        float best = -2.0e38f;
        for (std::uint32_t i = 0; i < n; ++i) {
            const float sc = c.kern(mq.codes.data(), c.codes.data() + i * d,
                                    c.sums[i], mq.scale, c.scales[i], d);
            if (sc > best) { best = sc; medoid = i; }
        }
    }

    // ---- Vamana 增量建图（单写者顺序；随机插入序）----
    if (n > 1) {
        std::mt19937_64 rng(seed);
        // 随机初始图：每点 min(r, n-1) 条随机出边（去重、不指自身）。
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t* nb = c.nbrs.data() + static_cast<std::size_t>(i) * r;
            std::uint32_t cnt = 0;
            const std::uint32_t want = std::min(r, n - 1);
            while (cnt < want) {
                const auto x = static_cast<std::uint32_t>(rng() % n);
                if (x == i) continue;
                bool dup = false;
                for (std::uint32_t j2 = 0; j2 < cnt; ++j2) {
                    if (nb[j2] == x) { dup = true; break; }
                }
                if (!dup) nb[cnt++] = x;
            }
            c.ncnt[i] = cnt;
        }
        std::vector<std::uint32_t> order(n);
        for (std::uint32_t i = 0; i < n; ++i) order[i] = i;
        for (std::uint32_t i = n - 1; i > 0; --i) {
            std::swap(order[i],
                      order[static_cast<std::uint32_t>(rng() % (i + 1))]);
        }
        std::vector<std::pair<float, std::uint32_t>> visited;
        std::vector<std::uint32_t> mark(n, 0);
        std::uint32_t mark_gen = 0;
        std::vector<std::pair<float, std::uint32_t>> cand;
        std::vector<std::uint32_t> entries;
        seed_entries(n, medoid, entries);
        // 两趟 refresh（单趟在 100k 实测崩:L=64 召回 0.59——早插入点的边
        // 基于残缺图,无第二趟修不回来）。两趟**同 α=1.2**:论文的 1.0→1.2
        // 调度在余弦紧簇域实测更糟（α=1.0 覆盖边界被簇内微距噪声主导 →
        // 过度剪枝断图,recall 归零）——α 调度对度量域敏感,非普适。
        for (const float alpha : {kAlpha, kAlpha}) {
            for (const std::uint32_t p : order) {
                build_beam(c, p, entries, l_build, visited, mark,
                           ++mark_gen);
                cand = visited;
                const std::uint32_t cnt0 = c.ncnt[p];
                const std::uint32_t* nb0 =
                    c.nbrs.data() + static_cast<std::size_t>(p) * r;
                for (std::uint32_t j = 0; j < cnt0; ++j) {
                    cand.push_back({c.dist(p, nb0[j]), nb0[j]});
                }
                robust_prune(c, p, cand, alpha);
                // 反向边回插 + 超容回剪。
                const std::uint32_t cnt1 = c.ncnt[p];
                for (std::uint32_t j = 0; j < cnt1; ++j) {
                    const std::uint32_t v =
                        c.nbrs[static_cast<std::size_t>(p) * r + j];
                    std::uint32_t* vnb =
                        c.nbrs.data() + static_cast<std::size_t>(v) * r;
                    bool has = false;
                    for (std::uint32_t j2 = 0; j2 < c.ncnt[v]; ++j2) {
                        if (vnb[j2] == p) { has = true; break; }
                    }
                    if (has) continue;
                    if (c.ncnt[v] < r) {
                        vnb[c.ncnt[v]++] = p;
                    } else {
                        cand.clear();
                        cand.push_back({c.dist(v, p), p});
                        for (std::uint32_t j2 = 0; j2 < c.ncnt[v]; ++j2) {
                            cand.push_back({c.dist(v, vnb[j2]), vnb[j2]});
                        }
                        robust_prune(c, v, cand, alpha);
                    }
                }
            }
        }

        // 孤点救援：RobustPrune 回剪可能剪光个别点的**入度** → medoid 出发
        // 不可达（实测 ~1.5%）。入度 0 的点强插其最近邻的邻接（满则逐出
        // 该表最远边）——可达性恢复，全宽 beam 的精确退化路径成立。
        {
            std::vector<std::uint8_t> indeg(n, 0);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint32_t* nb =
                    c.nbrs.data() + static_cast<std::size_t>(i) * r;
                for (std::uint32_t j = 0; j < c.ncnt[i]; ++j) {
                    indeg[nb[j]] = 1;
                }
            }
            for (std::uint32_t o = 0; o < n; ++o) {
                if (indeg[o] != 0 || o == medoid) continue;
                // 最近邻（全扫;孤点占比极小,总代价可忽略）。
                float bestd = 3.4e38f;
                std::uint32_t t = medoid;
                for (std::uint32_t i2 = 0; i2 < n; ++i2) {
                    if (i2 == o) continue;
                    const float dd = c.dist(o, i2);
                    if (dd < bestd) { bestd = dd; t = i2; }
                }
                std::uint32_t* tnb =
                    c.nbrs.data() + static_cast<std::size_t>(t) * r;
                if (c.ncnt[t] < r) {
                    tnb[c.ncnt[t]++] = o;
                } else {
                    // 逐出最远边（保 o 入边——可达性优先于边质量）。
                    std::uint32_t worst = 0;
                    float worstd = -3.4e38f;
                    for (std::uint32_t j = 0; j < c.ncnt[t]; ++j) {
                        const float dd = c.dist(t, tnb[j]);
                        if (dd > worstd) { worstd = dd; worst = j; }
                    }
                    tnb[worst] = o;
                }
            }
        }
    }

    // ---- 落盘：header + nav + blocks（tmp+rename；CRC 增量累计）----
    const std::size_t bstride = 8 + d + 4 + 4 + 4 + 4 * static_cast<std::size_t>(r);
    const std::uint64_t nav_off = kBdaHeaderSize;
    const std::uint64_t blocks_off =
        nav_off + static_cast<std::uint64_t>(n) * snav;
    const std::uint64_t file_len =
        blocks_off + static_cast<std::uint64_t>(n) * bstride;

    const std::string fp(path);
    diskint::TmpFile tf;
    tf.path = fp + ".tmp";
    {
        auto h = io::open_handle(tf.path,
                                 io::OpenFlag::kTruncate |
                                     io::OpenFlag::kCloseOnExec,
                                 io::FileMode::kWorldReadable);
        if (!h) return false;
        tf.fd = *h;
    }
    const io::FileHandle fd = tf.fd;
    bool ok = true;
    const std::uint32_t nav_crc =
        n > 0 ? bitcask::codec::crc32(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(nav.data()),
                    nav.size()))
              : 0;
    ok = n == 0 || pwrite_all(fd, nav.data(), nav.size(), nav_off);
    std::uint32_t blocks_crc = 0;
    if (ok && n > 0) {
        std::vector<std::uint8_t> blk(bstride);
        std::uint64_t off = blocks_off;
        std::uint32_t crc = 0;
        bool first = true;
        for (std::uint32_t i = 0; ok && i < n; ++i, off += bstride) {
            std::memset(blk.data(), 0, bstride);
            std::memcpy(blk.data(), &ords[i], 8);
            std::memcpy(blk.data() + 8, c.codes.data() + i * d, d);
            std::memcpy(blk.data() + 8 + d, &c.scales[i], 4);
            std::memcpy(blk.data() + 8 + d + 4, &c.sums[i], 4);
            std::memcpy(blk.data() + 8 + d + 8, &c.ncnt[i], 4);
            std::memcpy(blk.data() + 8 + d + 12,
                        c.nbrs.data() + static_cast<std::size_t>(i) * r,
                        4 * static_cast<std::size_t>(c.ncnt[i]));
            ok = pwrite_all(fd, blk.data(), bstride, off);
            const std::span<const std::byte> sp(
                reinterpret_cast<const std::byte*>(blk.data()), bstride);
            crc = first ? bitcask::codec::crc32(sp)
                        : bitcask::codec::crc32_update(crc, sp);
            first = false;
        }
        blocks_crc = crc;
    }
    if (ok) {
        std::uint8_t hdr[kBdaHeaderSize] = {0};
        std::memcpy(hdr + 0, kBdaMagic, 4);
        std::memcpy(hdr + 4, &kBdaVersion, 4);
        std::memcpy(hdr + 12, &dim, 2);
        std::memcpy(hdr + 16, &r, 4);
        std::memcpy(hdr + 20, &medoid, 4);
        const auto count64 = static_cast<std::uint64_t>(n);
        std::memcpy(hdr + 24, &count64, 8);
        std::memcpy(hdr + 32, &max_ord, 8);
        std::memcpy(hdr + 40, &gen, 8);
        std::memcpy(hdr + 48, &nav_off, 8);
        std::memcpy(hdr + 56, &blocks_off, 8);
        std::memcpy(hdr + 64, &file_len, 8);
        std::memcpy(hdr + 72, &nav_crc, 4);
        std::memcpy(hdr + 76, &blocks_crc, 4);
        const std::uint32_t hcrc =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(hdr), kBdaHeaderCrcOff));
        std::memcpy(hdr + kBdaHeaderCrcOff, &hcrc, 4);
        ok = pwrite_all(fd, hdr, kBdaHeaderSize, 0);
    }
    if (ok) ok = tf.sync_close();
    if (!ok || !io::atomic_rename(tf.path, fp)) {  // S37-1：见 io.hpp C1 注释
        return false;  // TmpFile 析构清 tmp
    }
    tf.committed = true;
    return true;
} catch (...) {
    return false;  // bad_alloc 等（parallel_for_worker 重抛/本线程分配）;
                   // TmpFile 析构保证 fd/tmp 清理（审计修复 2026-07-13）
}

bool DiskannSegment::open(std::string_view path, std::uint16_t dim,
                          std::uint64_t expected_gen, bool verify_crc) {
    close();
    const std::string fp(path);
    const auto fh = io::open_handle(
        fp, io::OpenFlag::kReadOnly | io::OpenFlag::kCloseOnExec);
    if (!fh) return false;
    const io::FileHandle fd = *fh;
    std::uint8_t hdr[kBdaHeaderSize];
    // S37-1：定位读，不依赖 fd 内部偏移（见 ivf_rq 同处注释）。
    if (!io::pread_all(fd, hdr, kBdaHeaderSize, 0)) {
        io::close_handle(fd);
        return false;
    }
    if (std::memcmp(hdr, kBdaMagic, 4) != 0) {
        io::close_handle(fd);
        return false;
    }
    std::uint32_t ver = 0, r = 0, medoid = 0, nav_crc = 0, blocks_crc = 0;
    std::uint16_t fdim = 0;
    std::uint64_t count = 0, max_ord = 0, gen = 0;
    std::uint64_t nav_off = 0, blocks_off = 0, file_len = 0;
    std::memcpy(&ver, hdr + 4, 4);
    std::memcpy(&fdim, hdr + 12, 2);
    std::memcpy(&r, hdr + 16, 4);
    std::memcpy(&medoid, hdr + 20, 4);
    std::memcpy(&count, hdr + 24, 8);
    std::memcpy(&max_ord, hdr + 32, 8);
    std::memcpy(&gen, hdr + 40, 8);
    std::memcpy(&nav_off, hdr + 48, 8);
    std::memcpy(&blocks_off, hdr + 56, 8);
    std::memcpy(&file_len, hdr + 64, 8);
    std::memcpy(&nav_crc, hdr + 72, 4);
    std::memcpy(&blocks_crc, hdr + 76, 4);
    std::uint32_t stored = 0;
    std::memcpy(&stored, hdr + kBdaHeaderCrcOff, 4);
    const std::uint32_t calc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hdr), kBdaHeaderCrcOff));
    if (ver != kBdaVersion || fdim != dim || stored != calc || r == 0) {
        io::close_handle(fd);
        return false;
    }
    if (expected_gen != 0 && gen != 0 && gen != expected_gen) {
        io::close_handle(fd);
        return false;
    }
    if (count > 0 && medoid >= count) {
        io::close_handle(fd);
        return false;
    }
    const std::size_t snav = static_cast<std::size_t>(dim) + 8;
    const std::size_t bstride = 8 + static_cast<std::size_t>(dim) + 12 +
                                4 * static_cast<std::size_t>(r);
    if (nav_off != kBdaHeaderSize ||
        blocks_off != nav_off + count * snav ||
        file_len != blocks_off + count * bstride) {
        io::close_handle(fd);
        return false;
    }
    const auto fsize = io::handle_size(fd);
    if (!fsize || *fsize < file_len) {
        io::close_handle(fd);
        return false;
    }
    map_ = io::MappedFile::map_readonly(
        fd, static_cast<std::size_t>(*fsize), /*advise_random=*/false);
    if (!map_.valid()) {
        io::close_handle(fd);
        return false;
    }
    base_ = reinterpret_cast<const std::uint8_t*>(map_.data());
    fd_ = fd;
    dim_ = dim;
    r_ = r;
    medoid_ = medoid;
    count_ = count;
    max_ord_ = max_ord;
    gen_ = gen;
    blocks_off_ = blocks_off;
    // nav 区拷入 RAM（驻留导航码）。
    nav_.assign(base_ + nav_off, base_ + nav_off + count * snav);

    if (verify_crc && count > 0) {
        const std::uint32_t got_nav =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(nav_.data()),
                nav_.size()));
        const std::uint32_t got_blocks =
            bitcask::codec::crc32(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(base_ + blocks_off),
                static_cast<std::size_t>(count * bstride)));
        if (got_nav != nav_crc || got_blocks != blocks_crc) {
            close();
            return false;
        }
        // 邻接 id 越界防御（内存安全前置——search 直接解引块内 id）。
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint8_t* blk = base_ + blocks_off + i * bstride;
            std::uint32_t cnt = 0;
            std::memcpy(&cnt, blk + 8 + dim_ + 8, 4);
            if (cnt > r_) {
                close();
                return false;
            }
            const std::uint8_t* nb = blk + 8 + dim_ + 12;
            for (std::uint32_t j = 0; j < cnt; ++j) {
                std::uint32_t id = 0;
                std::memcpy(&id, nb + 4 * j, 4);
                if (id >= count) {
                    close();
                    return false;
                }
            }
        }
    }
    return true;
}

DiskannSegment::RecordView DiskannSegment::record_at(std::uint64_t i) const {
    const std::uint8_t* blk = base_ + blocks_off_ + i * block_stride();
    RecordView v;
    std::memcpy(&v.ord, blk, 8);
    v.codes = reinterpret_cast<const std::int8_t*>(blk + 8);
    std::memcpy(&v.scale, blk + 8 + dim_, 4);
    std::memcpy(&v.sum, blk + 8 + dim_ + 4, 4);
    return v;
}

std::vector<DiskannSegment::Hit> DiskannSegment::search(
    std::span<const float> query, std::size_t k, std::uint32_t l,
    const std::function<bool(std::uint64_t)>* live) const {
    std::vector<Hit> out;
    if (!opened() || count_ == 0 || k == 0 || query.size() != dim_) return out;
    if (l == 0) l = std::max<std::uint32_t>(2 * static_cast<std::uint32_t>(k),
                                            64);
    l = std::max<std::uint32_t>(l, static_cast<std::uint32_t>(k));  // 池 ≥ k
    l = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(l, count_));
    const std::size_t d = dim_;
    const std::size_t snav = nav_stride();
    const std::size_t bstride = block_stride();

    // 查询侧编码：int8。v1 导航码 = RAM int8（est ≡ 精评精度——beam 排序
    // 质量理论最优;1-bit est 在紧簇语料簇内零区分度,实测 l=64 召回崩至
    // 0.53/0.585,已废弃）。RAM 账 dim+8 B/向量;压到 PQ32 是 M5.5 的
    // 明确目标（那才兑现 DiskANN「RAM ≪ N·dim」）。
    thread_local int8::QVector qv;
    int8::quantize_into(query.data(), d, qv);
    int8::Int8DotFn kern = int8::pick_int8_dot_kernel();
    if (kern == nullptr) kern = &int8::dot_scalar_raw;
    auto nav_est = [&](std::uint32_t id) {
        const std::uint8_t* nv = nav_.data() + id * snav;
        float scale;
        std::int32_t sum;
        std::memcpy(&scale, nv + d, 4);
        std::memcpy(&sum, nv + d + 4, 4);
        return kern(qv.codes.data(),
                    reinterpret_cast<const std::int8_t*>(nv), sum, qv.scale,
                    scale, d);
    };

    // visited 版本戳（thread_local 复用，免每查询清零）。
    thread_local std::vector<std::uint32_t> stamp;
    thread_local std::uint32_t stamp_gen = 0;
    if (stamp.size() < count_) stamp.assign(count_, 0);
    ++stamp_gen;
    if (stamp_gen == 0) {  // 回绕：全清
        std::fill(stamp.begin(), stamp.end(), 0);
        stamp_gen = 1;
    }

    // 候选池：(est 降序, id, expanded)。l 有限，插入排序维护。
    struct Ent {
        float est;
        std::uint32_t id;
        bool expanded;
    };
    std::vector<Ent> pool;
    pool.reserve(l + 1);
    auto push_pool = [&](std::uint32_t id) {
        if (stamp[id] == stamp_gen) return;
        stamp[id] = stamp_gen;
        const float est = nav_est(id);
        if (pool.size() >= l && est <= pool.back().est) return;
        const Ent e{est, id, false};
        const auto it = std::lower_bound(
            pool.begin(), pool.end(), e,
            [](const Ent& a, const Ent& b) { return a.est > b.est; });
        pool.insert(it, e);
        if (pool.size() > l) pool.pop_back();
    };
    {
        thread_local std::vector<std::uint32_t> entries;
        seed_entries(count_, medoid_, entries);
        for (const std::uint32_t e0 : entries) push_pool(e0);
    }

    // 结果堆：(score, ord) 小顶 top-k（精评分数；live 结果侧过滤）。
    std::vector<std::pair<float, std::uint64_t>> heap;
    heap.reserve(k + 1);
    auto hcmp = [](const auto& a, const auto& b) { return a.first > b.first; };

    for (;;) {
        Ent* best = nullptr;
        for (auto& e : pool) {
            if (!e.expanded) { best = &e; break; }
        }
        if (best == nullptr) break;
        best->expanded = true;
        const std::uint8_t* blk =
            base_ + blocks_off_ +
            static_cast<std::uint64_t>(best->id) * bstride;
        // v1:est 即精评（同 int8 域,入池时已算）——块读只为邻接/ord。
        // M5.5(PQ nav)后此处恢复块内 int8 精评。
        const float score = best->est;
        if (heap.size() < k || score > heap.front().first) {
            std::uint64_t ord;
            std::memcpy(&ord, blk, 8);
            if (live == nullptr || !*live || (*live)(ord)) {
                heap.push_back({score, ord});
                std::push_heap(heap.begin(), heap.end(), hcmp);
                if (heap.size() > k) {
                    std::pop_heap(heap.begin(), heap.end(), hcmp);
                    heap.pop_back();
                }
            }
        }
        // 扩展邻接。use-site 越界防御（审计修复 2026-07-13）：邻接 id/ncnt
        // 的全量校验在 open 时挂 verify_crc 门（无条件校验需 touch 全部
        // 块页,违背 mmap 懒加载）——可信盘模式下损坏块的 id ≥ count 会
        // 把 nav_est/块读指出界外（OOB UB）。此处每邻居一次比较,代价
        // 相对 est 计算可忽略。
        std::uint32_t cnt = 0;
        std::memcpy(&cnt, blk + 8 + d + 8, 4);
        if (cnt > r_) cnt = r_;
        const std::uint8_t* nb = blk + 8 + d + 12;
        for (std::uint32_t j = 0; j < cnt; ++j) {
            std::uint32_t id = 0;
            std::memcpy(&id, nb + 4 * j, 4);
            if (id >= count_) continue;
            push_pool(id);
        }
    }
    std::sort_heap(heap.begin(), heap.end(), hcmp);  // 分数降序
    out.reserve(heap.size());
    for (const auto& [s, o] : heap) out.push_back({o, s});
    return out;
}

}  // namespace bitcask::vec
