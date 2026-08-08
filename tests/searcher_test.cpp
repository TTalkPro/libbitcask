// Searcher 门面等价测试（S19-1）：门面结果 == Cask 门面结果（同库同数据）。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "bitcask/cask.hpp"
#include "bitcask/keydir_registry.hpp"
#include "bitcask/searcher.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {
keydir::KeyDirRegistry& test_registry() {
    static keydir::KeyDirRegistry reg;
    return reg;
}
std::span<const std::byte> sv_bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
}  // namespace

TEST(SearcherFacade, MatchesCaskFacade) {
    const fs::path dir = fs::temp_directory_path() / "bitcask_searcher_eq";
    fs::remove_all(dir);
    fs::create_directories(dir);

    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    search::SearchLayerConfig scfg;
    opts.search_config = scfg;
    opts.vector_dim = 4;
    opts.vector_metric = meta::VectorMetric::kDot;

    auto c = Cask::open(dir.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 5; ++i) {
        DocInput doc;
        std::string text = "hello facade doc " + std::to_string(i);
        doc.text = sv_bytes(text);
        const float v[4] = {static_cast<float>(i), 1.0F, 0.0F, 0.0F};
        doc.vector = std::span<const float>(v, 4);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("k" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(1000 + i)));
    }

    auto* tp = (*c)->text_plugin();
    auto* vp = (*c)->vector_plugin();
    auto* hy = (*c)->hybrid_searcher();
    ASSERT_NE(tp, nullptr);
    ASSERT_NE(vp, nullptr);
    ASSERT_NE(hy, nullptr);

    text::Searcher ts(**c, *tp);
    vec::Searcher vs(**c, *vp);
    search::CaskHybridSearcher hs(**c, *hy);

    // 文本：门面 == Cask 门面（逐 hit 对比）。
    auto a = ts.search_text("hello", 10);
    auto b = (*c)->search_text("hello", 10);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->hits.size(), b->hits.size());
    for (std::size_t i = 0; i < a->hits.size(); ++i) {
        EXPECT_EQ(a->hits[i].key, b->hits[i].key);
        EXPECT_EQ(a->hits[i].ord, b->hits[i].ord);
        EXPECT_DOUBLE_EQ(a->hits[i].score, b->hits[i].score);
    }

    // 向量。
    const float q[4] = {2.0F, 1.0F, 0.0F, 0.0F};
    auto av = vs.search({q, 4}, 3);
    auto bv = (*c)->search_vector({q, 4}, 3);
    ASSERT_TRUE(av && bv);
    ASSERT_EQ(av->hits.size(), bv->hits.size());
    for (std::size_t i = 0; i < av->hits.size(); ++i) {
        EXPECT_EQ(av->hits[i].key, bv->hits[i].key);
    }

    // 混合。
    auto ah = hs.search("hello", {q, 4}, 3);
    auto bh = (*c)->search_hybrid("hello", {q, 4}, 3);
    ASSERT_TRUE(ah && bh);
    ASSERT_EQ(ah->hits.size(), bh->hits.size());
    for (std::size_t i = 0; i < ah->hits.size(); ++i) {
        EXPECT_EQ(ah->hits[i].key, bh->hits[i].key);
    }

    // 批量文本。
    const std::string_view qs[] = {"hello", "facade", "doc"};
    auto ab = ts.search_text_batch(qs, 5);
    auto bb = (*c)->search_text_batch(qs, 5);
    ASSERT_EQ(ab.size(), bb.size());
    for (std::size_t i = 0; i < ab.size(); ++i) {
        ASSERT_EQ(ab[i].has_value(), bb[i].has_value());
        if (ab[i]) { EXPECT_EQ(ab[i]->hits.size(), bb[i]->hits.size()); }
    }

    (*c)->close();
    fs::remove_all(dir);
}
