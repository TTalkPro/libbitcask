// S45-7：checkpoint / close 时先把各分片的 building 小段合并再落盘。
//
// 覆盖：
//   1. 多分片写入（含覆盖与删除 ⇒ building 段里有死行、命名字段）后
//      checkpoint：落成的段数 = 按 kBuildingFlushDocThreshold 装箱的组数
//      （远少于分片数）；每个存活 key 的当前正文唯一词恰好 1 命中、旧版本
//      与已删 key 0 命中；命名字段可检。
//   2. 合并后继续写 / 删（落在合并段里的旧版本被正确标死）。
//   3. 重开后同样一致（段清单 + key 定位来自合并段）。
//   4. 分组上限：总文档数超过单组上限时拆成多段。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/segment_set.hpp>
#include <bitcask/text_plugin.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "support/test_paths.hpp"

namespace {

namespace fs = std::filesystem;

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

bitcask::CaskOptions text_opts() {
    bitcask::CaskOptions o;
    o.read_write = true;
    o.enable_search = true;
    bitcask::search::SearchLayerConfig s;
    s.analyzer_config.type = bitcask::text::AnalyzerType::Whitespace;
    o.search_config = s;
    o.sync_every_n = 0;
    return o;
}

std::size_t seg_files(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.path().filename().string().rfind("seg-", 0) == 0) ++n;
    }
    return n;
}

class ShardSegmentMergeTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = bitcask::test::unique_tmpdir(
            std::string("shard_seg_merge_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // 4 写线程写 keys 个 key，每个 key 写 versions 版（正文唯一词
    // tok_<key>_v<n>，title 字段 t_<key>），随后删掉 i % 7 == 0 的 key。
    void populate(bitcask::Cask& c, int keys, int versions) {
        std::vector<std::thread> ws;
        for (int t = 0; t < 4; ++t) {
            ws.emplace_back([&, t] {
                for (int i = t; i < keys; i += 4) {
                    const std::string key = "k" + std::to_string(i);
                    for (int v = 0; v < versions; ++v) {
                        const std::string text = "tok_" + key + "_v" +
                                                 std::to_string(v) + " common";
                        const std::string title = "t_" + key;
                        bitcask::DocInput d;
                        d.text = bytes(text);
                        d.fields.emplace_back("title", bytes(title));
                        ASSERT_TRUE(c.put_doc(bytes(key), d));
                    }
                }
            });
        }
        for (auto& w : ws) w.join();
        for (int i = 0; i < keys; i += 7) {
            ASSERT_TRUE(c.remove(bytes("k" + std::to_string(i))));
            current_.erase("k" + std::to_string(i));
        }
        for (int i = 0; i < keys; ++i) {
            if (i % 7 == 0) continue;
            current_["k" + std::to_string(i)] = versions - 1;
        }
        versions_ = versions;
        keys_ = keys;
    }

    // 逐 key 对拍；返回违例数。
    std::size_t check(bitcask::Cask& c) {
        std::size_t bad = 0;
        for (int i = 0; i < keys_; ++i) {
            const std::string key = "k" + std::to_string(i);
            const auto it = current_.find(key);
            for (int v = 0; v < versions_; ++v) {
                const std::string tok = "tok_" + key + "_v" + std::to_string(v);
                auto r = c.search_text(tok, 5);
                const std::size_t want = (it != current_.end() && it->second == v) ? 1 : 0;
                if (!r || r->hits.size() != want ||
                    (want == 1 && r->hits[0].key != key)) {
                    if (++bad <= 5) {
                        ADD_FAILURE() << tok << ": hits=" << (r ? r->hits.size() : 999)
                                      << " want=" << want;
                    }
                }
            }
            auto f = c.search_fields("title:t_" + key, 5);
            const std::size_t want_f = it != current_.end() ? 1 : 0;
            if (!f || f->hits.size() != want_f) {
                if (++bad <= 5) ADD_FAILURE() << key << ": title hits wrong";
            }
        }
        return bad;
    }

    fs::path dir_;
    std::map<std::string, int> current_;
    int versions_ = 0;
    int keys_ = 0;
};

TEST_F(ShardSegmentMergeTest, CheckpointMergesShardSegmentsIntoOne) {
    bitcask::oki::IndexRegistry reg;
    auto oc = bitcask::Cask::open(dir_.string(), text_opts(), &reg);
    ASSERT_TRUE(oc) << oc.error().detail;
    auto& c = *oc;
    ASSERT_GT(reg.index_pool()->shard_count(), 1u) << "本用例需要多分片";
    populate(*c, 2000, 3);  // 6000 篇 + 删除：远低于单组上限
    ASSERT_TRUE(c->checkpoint());
    EXPECT_EQ(c->text_plugin()->segment_set()->segment_count(), 1u)
        << "各分片 building 段应合成一个段";
    EXPECT_EQ(seg_files(dir_), 1u);
    EXPECT_EQ(check(*c), 0u);

    // 合并后继续：覆盖 / 删除合并段里的文档，再 checkpoint。
    for (int i = 1; i < 2000; i += 5) {
        const std::string key = "k" + std::to_string(i);
        if (!current_.contains(key)) continue;
        const std::string text = "tok_" + key + "_v" + std::to_string(versions_) + " common";
        const std::string title = "t_" + key;
        bitcask::DocInput d;
        d.text = bytes(text);
        d.fields.emplace_back("title", bytes(title));
        ASSERT_TRUE(c->put_doc(bytes(key), d));
        current_[key] = versions_;
    }
    for (int i = 3; i < 2000; i += 11) {
        const std::string key = "k" + std::to_string(i);
        ASSERT_TRUE(c->remove(bytes(key)));
        current_.erase(key);
    }
    ++versions_;
    c->flush_index();
    EXPECT_EQ(check(*c), 0u);
    ASSERT_TRUE(c->checkpoint());
    EXPECT_EQ(check(*c), 0u);
    c->close();

    bitcask::oki::IndexRegistry reg2;
    auto r = bitcask::Cask::open(dir_.string(), text_opts(), &reg2);
    ASSERT_TRUE(r) << r.error().detail;
    (*r)->flush_index();
    EXPECT_EQ(check(**r), 0u);
    EXPECT_EQ((*r)->docmap()->live_docs(), current_.size());
    (*r)->close();
}

TEST_F(ShardSegmentMergeTest, GroupsAreCappedAtSealThreshold) {
    bitcask::oki::IndexRegistry reg;
    auto oc = bitcask::Cask::open(dir_.string(), text_opts(), &reg);
    ASSERT_TRUE(oc) << oc.error().detail;
    auto& c = *oc;
    const unsigned shards = reg.index_pool()->shard_count();
    ASSERT_GT(shards, 1u);
    constexpr std::size_t cap = 65536;  // = TextPlugin::kBuildingFlushDocThreshold（私有）
    // 总量略超一组上限、每分片都远低于上限（不触发阈值封口）。
    const int keys = static_cast<int>(cap + cap / 4);
    ASSERT_LT(static_cast<std::size_t>(keys) / shards, cap / 2);
    populate(*c, keys, 1);
    ASSERT_TRUE(c->checkpoint());
    const auto segs = c->text_plugin()->segment_set()->segment_count();
    EXPECT_GE(segs, 2u) << "超过单组上限必须拆组";
    EXPECT_LT(segs, shards) << "仍应少于分片数";
    for (const auto& e : c->text_plugin()->segment_set()->entries_view()) {
        EXPECT_LE(e.doc_count, cap);
    }
    EXPECT_EQ(check(*c), 0u);
    c->close();
}

}  // namespace
