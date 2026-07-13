// IvfSegment 单测（S32-M3 v1）：build/open/search 轮回、召回下限、live
// 过滤、gen 守卫、损坏拒载、空段、非连续 ord。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <vector>

#include "bitcask/detail/int8_kernels.hpp"
#include "bitcask/ivf_rq.hpp"

using bitcask::vec::IvfBuildSource;
using bitcask::vec::IvfSegment;
namespace fs = std::filesystem;

namespace {

// 聚簇合成（簇心独立种子；query 共享簇心——召回评估标准形态，同
// bench/ann_recall_harness.hpp 的教训注释）。
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

std::vector<std::uint64_t> brute_topk(const std::vector<float>& base,
                                      std::size_t n, std::size_t dim,
                                      const float* q, std::size_t k,
                                      std::uint64_t ord_stride) {
    std::vector<std::pair<float, std::uint64_t>> all;
    all.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = base.data() + i * dim;
        float dot = 0.0f;
        for (std::size_t d = 0; d < dim; ++d) dot += v[d] * q[d];
        all.push_back({dot, static_cast<std::uint64_t>(i) * ord_stride});
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

// int8 暴扫参照：与段内核同一算式（scalar 与 VNNI 的 dot 整数部分精确一致，
// int8_kernels.hpp 注释契约）→ 全扫结果必须与本参照**穷举对拍**一致。
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

TEST(IvfSegment, BuildOpenSearchRecall) {
    const std::size_t n = 4000, dim = 96, k = 10, nq = 50;
    auto base = make_clustered(n, dim, 32, 0.15f, 0xA1, 0xCC);
    auto queries = make_clustered(nq, dim, 32, 0.15f, 0xB2, 0xCC);
    const auto fp = tmp_path("bitcask_ivf_basic.biv");

    ASSERT_TRUE(IvfSegment::build(fp, dim, src_of(base, dim), 32, 777));
    IvfSegment seg;
    ASSERT_TRUE(seg.open(fp, dim, 777));
    EXPECT_EQ(seg.size(), n);
    EXPECT_EQ(seg.nlist(), 32u);
    EXPECT_EQ(seg.max_ord(), n - 1);

    // 自查询 top1。
    for (std::size_t i : {0u, 1234u, 3999u}) {
        auto hits = seg.search(
            std::span<const float>(base.data() + i * dim, dim), 1, 8);
        ASSERT_FALSE(hits.empty());
        EXPECT_EQ(hits[0].ord, i);
    }

    // 正确性主断言：全扫（nprobe=nlist,coarse_c=n → 两段扫精确退化）≡
    // int8 暴扫穷举对拍——分簇/布局/内核任何一环丢数据或算错都在此暴露
    //（量化误差被同侧抵消，非门槛）。
    for (std::size_t qi = 0; qi < nq; ++qi) {
        const float* q = queries.data() + qi * dim;
        auto truth = brute_topk_int8(base, n, dim, q, k);
        auto got = seg.search(std::span<const float>(q, dim), k, 32, nullptr,
                              static_cast<std::uint32_t>(n));
        ASSERT_EQ(got.size(), k);
        std::vector<std::uint64_t> gids(k);
        for (std::size_t i = 0; i < k; ++i) gids[i] = got[i].ord;
        std::sort(truth.begin(), truth.end());
        std::sort(gids.begin(), gids.end());
        EXPECT_EQ(gids, truth) << "全扫 ≠ int8 暴扫 @ query " << qi;
    }

    // 次级门：nprobe=8 对 **int8 真值** 的召回——隔离出 IVF 自身的损失
    // （簇漏）。f32 truth 会混入 int8 量化换位（紧簇合成语料下 top-10
    // 边界本就在量化噪声量级内），那一项由量化红线测试
    // （hnsw_test::measure_quant_recall 族）单独把守，不在此重复计账。
    auto recall_vs_int8 = [&](std::uint32_t nprobe) {
        std::size_t hit = 0;
        for (std::size_t qi = 0; qi < nq; ++qi) {
            const float* q = queries.data() + qi * dim;
            auto truth = brute_topk_int8(base, n, dim, q, k);
            auto got = seg.search(std::span<const float>(q, dim), k, nprobe);
            for (const auto& h : got) {
                if (std::find(truth.begin(), truth.end(), h.ord) !=
                    truth.end()) {
                    ++hit;
                }
            }
        }
        return static_cast<double>(hit) / static_cast<double>(nq * k);
    };
    EXPECT_GE(recall_vs_int8(8), 0.90) << "nprobe=8/32 簇路由召回不达标";
    // S32-M3.5-②:两段扫默认 C（max(8k,128)）的 1-bit 粗筛召回门——est
    // 有损排序由 C 冗余兜底,跌破此线说明粗筛层丢真近邻。
    EXPECT_GE(recall_vs_int8(32), 0.97) << "两段扫默认 C 召回门不达标";

    // v1 兼容:with_bits=false → 单段扫,默认参数即精确(对拍同门)。
    {
        const auto fp1 = tmp_path("bitcask_ivf_v1.biv");
        ASSERT_TRUE(IvfSegment::build(fp1, dim, src_of(base, dim), 32, 778,
                                      0x5EEDF00D, /*with_bits=*/false));
        IvfSegment s1;
        ASSERT_TRUE(s1.open(fp1, dim, 778));
        for (std::size_t qi = 0; qi < 10; ++qi) {
            const float* q = queries.data() + qi * dim;
            auto truth = brute_topk_int8(base, n, dim, q, k);
            auto got = s1.search(std::span<const float>(q, dim), k, 32);
            ASSERT_EQ(got.size(), k);
            std::vector<std::uint64_t> gids(k);
            for (std::size_t i = 0; i < k; ++i) gids[i] = got[i].ord;
            std::sort(truth.begin(), truth.end());
            std::sort(gids.begin(), gids.end());
            EXPECT_EQ(gids, truth) << "v1 单段扫 ≠ int8 暴扫 @ " << qi;
        }
        fs::remove(fp1);
    }

    // 分数降序。
    auto hits = seg.search(std::span<const float>(queries.data(), dim), k, 8);
    for (std::size_t i = 1; i < hits.size(); ++i) {
        EXPECT_LE(hits[i].score, hits[i - 1].score);
    }
    fs::remove(fp);
}

TEST(IvfSegment, LiveFilterAndNoncontiguousOrds) {
    const std::size_t n = 1000, dim = 16;
    auto base = make_clustered(n, dim, 16, 0.2f, 0xD3, 0xEE);
    const auto fp = tmp_path("bitcask_ivf_live.biv");
    // ord = i*3+…：非连续 ord 轮回（ord_stride=3）。
    ASSERT_TRUE(IvfSegment::build(fp, dim, src_of(base, dim, 3), 16, 5));
    IvfSegment seg;
    ASSERT_TRUE(seg.open(fp, dim, 5));
    EXPECT_EQ(seg.max_ord(), (n - 1) * 3);

    std::function<bool(std::uint64_t)> live = [](std::uint64_t ord) {
        return (ord / 3) % 2 == 0;  // 只留偶下标
    };
    auto hits = seg.search(std::span<const float>(base.data(), dim), 20,
                           16, &live);
    ASSERT_FALSE(hits.empty());
    for (const auto& h : hits) {
        EXPECT_EQ(h.ord % 3, 0u);          // ord 映射保真
        EXPECT_EQ((h.ord / 3) % 2, 0u);    // live 过滤生效
    }
    fs::remove(fp);
}

TEST(IvfSegment, GenGuardDimMismatchAndCorruption) {
    const std::size_t n = 500, dim = 16;
    auto base = make_clustered(n, dim, 8, 0.2f, 0x11, 0x22);
    const auto fp = tmp_path("bitcask_ivf_guard.biv");
    ASSERT_TRUE(IvfSegment::build(fp, dim, src_of(base, dim), 8, 42));

    IvfSegment seg;
    EXPECT_FALSE(seg.open(fp, dim, 43)) << "gen 不配必须拒载";
    EXPECT_FALSE(seg.open(fp, dim + 1, 42)) << "dim 不符必须拒载";
    EXPECT_TRUE(seg.open(fp, dim, 0)) << "expected_gen=0 = 不校验";
    EXPECT_TRUE(seg.open(fp, dim, 42));
    seg.close();

    // bits 区损坏 → bits_crc 拒载（v2;偏移 = header + cent + cidx 起首字节）。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        const long bits_at = 96 + 8L * dim * 4 + 8 * 16;  // nlist=8
        std::fseek(f, bits_at, SEEK_SET);
        int ch = std::fgetc(f);
        std::fseek(f, bits_at, SEEK_SET);
        std::fputc(ch ^ 0x3C, f);
        std::fclose(f);
        EXPECT_FALSE(seg.open(fp, dim, 42)) << "bits 区损坏必须被 CRC 检出";
        // 还原（后续 header 损坏用例要在干净体上做）。
        f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, bits_at, SEEK_SET);
        std::fputc(ch, f);
        std::fclose(f);
        EXPECT_TRUE(seg.open(fp, dim, 42));
        seg.close();
    }

    // 篡改 posting 一字节 → 逐簇 CRC 拒载；verify_crc=false 放行（可信盘）。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, -1, SEEK_END);
        int ch = std::fgetc(f);
        std::fseek(f, -1, SEEK_END);
        std::fputc(ch ^ 0x5A, f);
        std::fclose(f);
    }
    EXPECT_FALSE(seg.open(fp, dim, 42)) << "记录区损坏必须被逐簇 CRC 检出";
    EXPECT_TRUE(seg.open(fp, dim, 42, /*verify_crc=*/false));
    seg.close();

    // header 损坏 → 拒载。
    {
        std::FILE* f = std::fopen(fp.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 24, SEEK_SET);  // count 字段
        std::fputc(0x7F, f);
        std::fclose(f);
    }
    EXPECT_FALSE(seg.open(fp, dim, 42));
    fs::remove(fp);
}

TEST(IvfSegment, EmptyAndTinyBuild) {
    const auto fp = tmp_path("bitcask_ivf_empty.biv");
    IvfBuildSource empty;
    empty.count = 0;
    ASSERT_TRUE(IvfSegment::build(fp, 16, empty, 0, 9));
    IvfSegment seg;
    ASSERT_TRUE(seg.open(fp, 16, 9));
    EXPECT_EQ(seg.size(), 0u);
    EXPECT_TRUE(seg.search(std::vector<float>(16, 0.1f), 5, 0).empty());
    seg.close();

    // 极小库（n < nlist 下限）：nlist 自动收缩，功能完好。
    const std::size_t dim = 8;
    auto base = make_clustered(5, dim, 2, 0.2f, 0x77, 0x88);
    ASSERT_TRUE(IvfSegment::build(fp, dim, src_of(base, dim), 0, 9));
    ASSERT_TRUE(seg.open(fp, dim, 9));
    EXPECT_EQ(seg.size(), 5u);
    auto hits = seg.search(std::span<const float>(base.data(), dim), 5,
                           seg.nlist());
    EXPECT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].ord, 0u);
    fs::remove(fp);
}
