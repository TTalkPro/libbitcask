// DiskannSegment 单测（S32-M5 v1）：build/open/search 轮回、全宽召回门
//（图可达性下的精确退化路径）、live 过滤、gen/CRC 守卫、空段。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <vector>

#include "bitcask/detail/int8_kernels.hpp"
#include "bitcask/diskann.hpp"

using bitcask::vec::DiskannSegment;
using bitcask::vec::IvfBuildSource;
namespace fs = std::filesystem;

namespace {

std::vector<float> make_clustered(std::size_t n, std::size_t dim,
                                  std::uint32_t nc, float sigma,
                                  std::uint64_t member_seed,
                                  std::uint64_t center_seed) {
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> centers(static_cast<std::size_t>(nc) * dim);
    {
        std::mt19937_64 crng(center_seed);
        for (auto& x : centers) x = g(crng);
    }
    std::mt19937_64 rng(member_seed);
    std::vector<float> out(n * dim);
    for (std::size_t i = 0; i < n; ++i) {
        float* v = out.data() + i * dim;
        const float* c = centers.data() + (i % nc) * dim;
        double sq = 0.0;
        for (std::size_t d = 0; d < dim; ++d) {
            v[d] = c[d] + sigma * g(rng);
            sq += static_cast<double>(v[d]) * v[d];
        }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (std::size_t d = 0; d < dim; ++d) v[d] *= inv;
    }
    return out;
}

std::vector<std::uint64_t> brute_topk_int8(const std::vector<float>& base,
                                           std::size_t n, std::size_t dim,
                                           const float* q, std::size_t k) {
    namespace i8 = bitcask::vec::int8;
    i8::QVector qq;
    i8::quantize_into(q, dim, qq);
    i8::QVector bv;
    std::vector<std::pair<float, std::uint64_t>> all;
    all.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        i8::quantize_into(base.data() + i * dim, dim, bv);
        const float s = i8::dot_scalar_raw(qq.codes.data(), bv.codes.data(),
                                           bv.sum_codes, qq.scale, bv.scale,
                                           dim);
        all.push_back({s, static_cast<std::uint64_t>(i)});
    }
    std::partial_sort(all.begin(), all.begin() + static_cast<long>(k),
                      all.end(), std::greater<>());
    std::vector<std::uint64_t> ids(k);
    for (std::size_t i = 0; i < k; ++i) ids[i] = all[i].second;
    return ids;
}

IvfBuildSource src_of(const std::vector<float>& base, std::size_t dim,
                      std::uint64_t ord_stride = 1) {
    IvfBuildSource s;
    s.count = static_cast<std::uint32_t>(base.size() / dim);
    s.get = [&base, dim, ord_stride](std::uint32_t i, std::uint64_t& ord,
                                     const float*& vec) {
        ord = static_cast<std::uint64_t>(i) * ord_stride;
        vec = base.data() + static_cast<std::size_t>(i) * dim;
    };
    return s;
}

std::string tmp_path(const char* name) {
    return (fs::temp_directory_path() / name).string();
}

}  // namespace

TEST(DiskannSegment, BuildOpenSearchRecall) {
    const std::size_t n = 3000, dim = 96, k = 10, nq = 40;
    auto base = make_clustered(n, dim, 24, 0.15f, 0xA7, 0xC9);
    auto queries = make_clustered(nq, dim, 24, 0.15f, 0xB8, 0xC9);
    const auto fp = tmp_path("bitcask_diskann_basic.bda");

    ASSERT_TRUE(DiskannSegment::build(fp, dim, src_of(base, dim), 32, 64, 55));
    DiskannSegment seg;
    ASSERT_TRUE(seg.open(fp, dim, 55));
    EXPECT_EQ(seg.size(), n);
    EXPECT_EQ(seg.r(), 32u);
    EXPECT_EQ(seg.max_ord(), n - 1);

    // 自查询 top1（beam 从 medoid 可达 + 精评 int8 自身分数最高）。
    for (std::size_t i : {0u, 1499u, 2999u}) {
        auto hits = seg.search(
            std::span<const float>(base.data() + i * dim, dim), 1, 64);
        ASSERT_FALSE(hits.empty());
        EXPECT_EQ(hits[0].ord, i);
    }

    auto recall_vs_int8 = [&](std::uint32_t l) {
        std::size_t hit = 0;
        for (std::size_t qi = 0; qi < nq; ++qi) {
            const float* q = queries.data() + qi * dim;
            auto truth = brute_topk_int8(base, n, dim, q, k);
            auto got = seg.search(std::span<const float>(q, dim), k, l);
            for (const auto& h : got) {
                if (std::find(truth.begin(), truth.end(), h.ord) !=
                    truth.end()) {
                    ++hit;
                }
            }
        }
        return static_cast<double>(hit) / static_cast<double>(nq * k);
    };
    // 全宽（l = n）= 精确退化路径：图连通则全图访问，唯一残差 = 极端
    // 删边孤点（Vamana 反向边回插使其极罕见）——门槛 ≥ 0.99。
    EXPECT_GE(recall_vs_int8(static_cast<std::uint32_t>(n)), 0.99)
        << "全宽 beam 召回不达标（图可达性缺口）";
    // 常规宽度 l=64 的 beam 召回门。
    EXPECT_GE(recall_vs_int8(64), 0.90) << "beam l=64 召回不达标";

    // 分数降序。
    auto hits = seg.search(std::span<const float>(queries.data(), dim), k, 64);
    ASSERT_EQ(hits.size(), k);
    for (std::size_t i = 1; i < hits.size(); ++i) {
        EXPECT_LE(hits[i].score, hits[i - 1].score);
    }
    fs::remove(fp);
}

// 定性对照（v1 定级依据）：**连续距离分布**（弱聚簇 σ=0.5,簇际交叠）下
// Vamana 召回正常——证明 100k 极端二态语料（781 等距孤簇,簇内三角扁平）
// 的失败是语料病态形态而非引擎缺陷（HNSW 层级/IVF 质心路由结构性免疫,
// 单层图正中要害;文献评测语料 SIFT/DEEP 均为连续分布）。
TEST(DiskannSegment, ContinuousDistributionRecall) {
    const std::size_t n = 20000, dim = 96, k = 10, nq = 40;
    auto base = make_clustered(n, dim, 400, 0.5f, 0x91, 0xA2);
    auto queries = make_clustered(nq, dim, 400, 0.5f, 0x93, 0xA2);
    const auto fp = tmp_path("bitcask_diskann_cont.bda");
    ASSERT_TRUE(DiskannSegment::build(fp, dim, src_of(base, dim), 32, 64, 66));
    DiskannSegment seg;
    ASSERT_TRUE(seg.open(fp, dim, 66));

    std::size_t hit = 0;
    for (std::size_t qi = 0; qi < nq; ++qi) {
        const float* q = queries.data() + qi * dim;
        auto truth = brute_topk_int8(base, n, dim, q, k);
        auto got = seg.search(std::span<const float>(q, dim), k, 64);
        for (const auto& h : got) {
            if (std::find(truth.begin(), truth.end(), h.ord) != truth.end()) {
                ++hit;
            }
        }
    }
    const double recall =
        static_cast<double>(hit) / static_cast<double>(nq * k);
    EXPECT_GE(recall, 0.85) << "连续分布下 beam L=64 召回不达标";
    fs::remove(fp);
}

TEST(DiskannSegment, LiveFilterAndNoncontiguousOrds) {
    const std::size_t n = 800, dim = 16;
    auto base = make_clustered(n, dim, 16, 0.2f, 0xD5, 0xE6);
    const auto fp = tmp_path("bitcask_diskann_live.bda");
    ASSERT_TRUE(
        DiskannSegment::build(fp, dim, src_of(base, dim, 5), 16, 32, 7));
    DiskannSegment seg;
    ASSERT_TRUE(seg.open(fp, dim, 7));
    EXPECT_EQ(seg.max_ord(), (n - 1) * 5);

    std::function<bool(std::uint64_t)> live = [](std::uint64_t ord) {
        return (ord / 5) % 2 == 0;
    };
    auto hits = seg.search(std::span<const float>(base.data(), dim), 20,
                           static_cast<std::uint32_t>(n), &live);
    ASSERT_FALSE(hits.empty());
    for (const auto& h : hits) {
        EXPECT_EQ(h.ord % 5, 0u);
        EXPECT_EQ((h.ord / 5) % 2, 0u);
    }
    fs::remove(fp);
}

TEST(DiskannSegment, GenGuardAndCorruption) {
    const std::size_t n = 400, dim = 16;
    auto base = make_clustered(n, dim, 8, 0.2f, 0x21, 0x32);
    const auto fp = tmp_path("bitcask_diskann_guard.bda");
    ASSERT_TRUE(DiskannSegment::build(fp, dim, src_of(base, dim), 16, 32, 42));

    DiskannSegment seg;
    EXPECT_FALSE(seg.open(fp, dim, 43)) << "gen 不配必须拒载";
    EXPECT_FALSE(seg.open(fp, dim + 1, 42)) << "dim 不符必须拒载";
    EXPECT_TRUE(seg.open(fp, dim, 0));
    EXPECT_TRUE(seg.open(fp, dim, 42));
    seg.close();

    // nav 区损坏（header 后首字节）→ nav_crc 拒载；还原后复活。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 96, SEEK_SET);
        int ch = std::fgetc(f);
        std::fseek(f, 96, SEEK_SET);
        std::fputc(ch ^ 0x5A, f);
        std::fclose(f);
        EXPECT_FALSE(seg.open(fp, dim, 42)) << "nav 损坏必须被 CRC 检出";
        f = std::fopen(fp.c_str(), "r+b");
        std::fseek(f, 96, SEEK_SET);
        std::fputc(ch, f);
        std::fclose(f);
        EXPECT_TRUE(seg.open(fp, dim, 42));
        seg.close();
    }
    // blocks 区损坏（文件尾字节）→ blocks_crc 拒载；verify_crc=false 放行。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, -1, SEEK_END);
        int ch = std::fgetc(f);
        std::fseek(f, -1, SEEK_END);
        std::fputc(ch ^ 0x3C, f);
        std::fclose(f);
        EXPECT_FALSE(seg.open(fp, dim, 42));
        EXPECT_TRUE(seg.open(fp, dim, 42, /*verify_crc=*/false));
        seg.close();
    }
    // header 损坏 → 拒载。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 24, SEEK_SET);  // count 字段
        std::fputc(0x7F, f);
        std::fclose(f);
        EXPECT_FALSE(seg.open(fp, dim, 42));
    }
    fs::remove(fp);
}

TEST(DiskannSegment, EmptyAndTinyBuild) {
    const auto fp = tmp_path("bitcask_diskann_empty.bda");
    IvfBuildSource empty;
    empty.count = 0;
    ASSERT_TRUE(DiskannSegment::build(fp, 16, empty, 0, 0, 9));
    DiskannSegment seg;
    ASSERT_TRUE(seg.open(fp, 16, 9));
    EXPECT_EQ(seg.size(), 0u);
    EXPECT_TRUE(seg.search(std::vector<float>(16, 0.1f), 5, 0).empty());
    seg.close();

    const std::size_t dim = 8;
    auto base = make_clustered(3, dim, 2, 0.2f, 0x71, 0x82);
    ASSERT_TRUE(DiskannSegment::build(fp, dim, src_of(base, dim), 0, 0, 9));
    ASSERT_TRUE(seg.open(fp, dim, 9));
    EXPECT_EQ(seg.size(), 3u);
    auto hits = seg.search(std::span<const float>(base.data(), dim), 3, 8);
    EXPECT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].ord, 0u);
    fs::remove(fp);
}
