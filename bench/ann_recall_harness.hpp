// S32-M0c:ANN 召回评估基建（设计 doc/vector-dual-engine-selection-zh.md §7
// M0、来源 S29-11 §2「没有召回标尺一切免谈」）。
//
// 三件套：
//   1. 固定语料——种子固定的聚簇合成（归一化,cosine 场景标准形态;聚簇度
//      可调,nc=0 退化为纯随机高斯 = 导航最坏形态）;
//   2. 暴力精确 top-k 真值——离线算一次、缓存盘上（BCGT 文件,参数键入
//      文件名 + 头部校验,参数不符自动重建;多线程暴扫,100k×1k 查询秒级）;
//   3. recall@k 计量——引擎无关（吃 (query → 命中 ord 列表) 回调）。
//
// 用法（hnsw_bench.cpp 的 BM_Hnsw_RecallQps 为参照;IvfPlugin bench 复用
// 同一真值缓存 → 两引擎同标尺对账）。验收门（S32 全批通用）:任何改动
// recall@10 降幅 > 1pt 须显式声明并提供配置回退。
//
// 缓存目录:默认 std::filesystem::temp_directory_path();环境变量
// BITCASK_ANN_TRUTH_DIR 覆盖（CI 可指向持久卷免重算）。内容由种子完全
// 决定,丢失重建无害。

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <bitcask/hw_crc32.hpp>

namespace bitcask::bench_ann {

// ---- 语料 ----------------------------------------------------------------

// 聚簇合成:nc 个归一化高斯簇心（由 center_seed 独立生成——base 与
// queries 传同一 center_seed 即共享簇分布,查询才有可分的真近邻;query
// 用随机簇心是召回评估的经典错误:真值边际微小,任何 ANN 都测得崩）,
// 成员 = 心 + sigma·噪声后归一化。nc = 0 → 纯随机高斯（无簇,导航跳数
// 取上界一侧,吞吐负载用;此时 center_seed 无效）。
inline std::vector<float> make_corpus(std::size_t n, std::size_t dim,
                                      std::uint32_t nc, float sigma,
                                      std::uint64_t member_seed,
                                      std::uint64_t center_seed) {
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> centers;
    if (nc > 0) {
        std::mt19937_64 crng(center_seed);
        centers.resize(static_cast<std::size_t>(nc) * dim);
        for (auto& x : centers) x = g(crng);
    }
    std::mt19937_64 rng(member_seed);
    std::vector<float> out(n * dim);
    for (std::size_t i = 0; i < n; ++i) {
        float* v = out.data() + i * dim;
        const float* c =
            nc > 0 ? centers.data() + (i % nc) * dim : nullptr;
        double sq = 0.0;
        for (std::size_t d = 0; d < dim; ++d) {
            v[d] = (c != nullptr ? c[d] : 0.0f) + (nc > 0 ? sigma : 1.0f) * g(rng);
            sq += static_cast<double>(v[d]) * v[d];
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (std::size_t d = 0; d < dim; ++d) v[d] *= inv;
    }
    return out;
}

// ---- 真值（暴力精确 top-k,内积/cosine-归一化语义）------------------------

struct TruthParams {
    std::size_t   n = 0;
    std::size_t   dim = 0;
    std::size_t   nq = 0;
    std::size_t   k = 100;          // 真值取 top-100,recall@10 取前缀
    std::uint32_t nc = 64;          // 语料簇数（0 = 纯随机）
    float         sigma = 0.15f;    // 簇内噪声
    std::uint64_t base_seed   = 0xBA5E5EED;   // base 成员噪声种子
    std::uint64_t query_seed  = 0xC0DE5EED;   // query 成员噪声种子
    std::uint64_t center_seed = 0xCE27E25D;   // 簇心种子（base/query 共享）
};

// nq × k 的真值 ord 表（行主序）。
struct GroundTruth {
    TruthParams                p;
    std::vector<std::uint64_t> ids;  // nq*k
    [[nodiscard]] std::span<const std::uint64_t> row(std::size_t qi) const {
        return {ids.data() + qi * p.k, p.k};
    }
};

namespace detail {

inline constexpr char kGtMagic[4] = {'B', 'C', 'G', 'T'};
inline constexpr std::uint32_t kGtVersion = 1;

inline std::string cache_path(const TruthParams& p) {
    const char* env = std::getenv("BITCASK_ANN_TRUTH_DIR");
    const std::filesystem::path dir =
        (env != nullptr && *env != '\0')
            ? std::filesystem::path(env)
            : std::filesystem::temp_directory_path();
    char name[192];
    std::snprintf(name, sizeof(name),
                  "bcgt_n%zu_d%zu_nq%zu_k%zu_c%u_s%llx_%llx_%llx.bin",
                  p.n, p.dim, p.nq, p.k, p.nc,
                  static_cast<unsigned long long>(p.base_seed),
                  static_cast<unsigned long long>(p.query_seed),
                  static_cast<unsigned long long>(p.center_seed));
    return (dir / name).string();
}

// 头 = magic|ver|参数快照;体 = ids;尾 = CRC32(头+体)。参数不符/损坏
// 一律视为未命中（重建覆盖）。sigma 以位型存取（浮点相等语义）。
inline bool load_truth_cache(const std::string& fp, const TruthParams& p,
                             std::vector<std::uint64_t>& ids) {
    std::FILE* f = std::fopen(fp.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    const std::size_t want =
        4 + 4 + 8 * 6 + 4 + 4 + p.nq * p.k * 8 + 4;
    if (sz < 0 || static_cast<std::size_t>(sz) != want) {
        std::fclose(f);
        return false;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    const bool rd = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (!rd) return false;
    std::uint32_t crc_stored = 0;
    std::memcpy(&crc_stored, buf.data() + buf.size() - 4, 4);
    const std::uint32_t crc_calc = bitcask::hw::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size() - 4));
    if (crc_stored != crc_calc) return false;
    const std::uint8_t* q = buf.data();
    if (std::memcmp(q, kGtMagic, 4) != 0) return false;
    q += 4;
    std::uint32_t ver = 0;
    std::memcpy(&ver, q, 4); q += 4;
    if (ver != kGtVersion) return false;
    std::uint64_t vals[6];
    std::memcpy(vals, q, 48); q += 48;
    std::uint32_t nc = 0;
    std::memcpy(&nc, q, 4); q += 4;
    std::uint32_t sigma_bits = 0, want_sigma_bits = 0;
    std::memcpy(&sigma_bits, q, 4); q += 4;
    std::memcpy(&want_sigma_bits, &p.sigma, 4);
    if (vals[0] != p.n || vals[1] != p.dim || vals[2] != p.nq ||
        vals[3] != p.base_seed || vals[4] != p.query_seed ||
        vals[5] != p.center_seed || nc != p.nc ||
        sigma_bits != want_sigma_bits) {
        return false;
    }
    ids.resize(p.nq * p.k);
    std::memcpy(ids.data(), q, ids.size() * 8);
    return true;
}

inline void save_truth_cache(const std::string& fp, const TruthParams& p,
                             const std::vector<std::uint64_t>& ids) {
    std::vector<std::uint8_t> buf;
    buf.reserve(64 + ids.size() * 8);
    auto put = [&buf](const void* src, std::size_t len) {
        const auto* b = static_cast<const std::uint8_t*>(src);
        buf.insert(buf.end(), b, b + len);
    };
    put(kGtMagic, 4);
    put(&kGtVersion, 4);
    const std::uint64_t vals[6] = {p.n, p.dim, p.nq, p.base_seed,
                                   p.query_seed, p.center_seed};
    put(vals, 48);
    put(&p.nc, 4);
    put(&p.sigma, 4);
    put(ids.data(), ids.size() * 8);
    const std::uint32_t crc = bitcask::hw::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data()), buf.size()));
    put(&crc, 4);
    const std::string tmp = fp + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return;  // 缓存写失败非致命（下次重算）
    const bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    if (ok) {
        (void)std::rename(tmp.c_str(), fp.c_str());
    } else {
        (void)std::remove(tmp.c_str());
    }
}

}  // namespace detail

// 真值构建/加载。base/queries 必须由同一 TruthParams 的种子生成（本函数
// 不复算语料,调用方经 make_corpus 持有并传入——避免双份大缓冲）。
inline GroundTruth load_or_build_truth(const TruthParams& p,
                                       std::span<const float> base,
                                       std::span<const float> queries) {
    GroundTruth gt;
    gt.p = p;
    const std::string fp = detail::cache_path(p);
    if (detail::load_truth_cache(fp, p, gt.ids)) return gt;

    gt.ids.assign(p.nq * p.k, 0);
    const std::size_t nthreads =
        std::max<std::size_t>(1, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    std::atomic<std::size_t> next{0};
    for (std::size_t t = 0; t < nthreads; ++t) {
        pool.emplace_back([&]() {
            std::vector<std::pair<float, std::uint64_t>> all(p.n);
            for (;;) {
                const std::size_t qi = next.fetch_add(1);
                if (qi >= p.nq) return;
                const float* q = queries.data() + qi * p.dim;
                for (std::size_t i = 0; i < p.n; ++i) {
                    const float* v = base.data() + i * p.dim;
                    float dot = 0.0f;
                    for (std::size_t d = 0; d < p.dim; ++d) dot += v[d] * q[d];
                    all[i] = {dot, static_cast<std::uint64_t>(i)};
                }
                std::partial_sort(all.begin(),
                                  all.begin() + static_cast<long>(p.k),
                                  all.end(), std::greater<>());
                for (std::size_t j = 0; j < p.k; ++j) {
                    gt.ids[qi * p.k + j] = all[j].second;
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    detail::save_truth_cache(fp, p, gt.ids);
    return gt;
}

// ---- 召回计量（引擎无关）--------------------------------------------------

// search_fn(qi) → 该查询的命中 ord 列表（长度 ≥ k 由调用方保证语义;
// 不足按缺失计）。返回 recall@k = |命中 ∩ 真值前 k| / (nq·k)。
inline double recall_at(const GroundTruth& gt, std::size_t k,
                        const std::function<std::vector<std::uint64_t>(
                            std::size_t)>& search_fn) {
    std::size_t hit = 0;
    for (std::size_t qi = 0; qi < gt.p.nq; ++qi) {
        const auto truth = gt.row(qi);
        const auto got = search_fn(qi);
        for (std::size_t j = 0; j < got.size() && j < k; ++j) {
            const auto* end = truth.data() + k;
            if (std::find(truth.data(), end, got[j]) != end) ++hit;
        }
    }
    return static_cast<double>(hit) / static_cast<double>(gt.p.nq * k);
}

}  // namespace bitcask::bench_ann
