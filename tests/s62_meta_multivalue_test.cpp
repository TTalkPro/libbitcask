// S62：meta 字符串数组（反馈 2026-11-13 #4，锦书）。设计 designs/meta-multivalue-design-zh.md。
//   ① 编解码（类型标记 5）与零分配查找；② 过滤 Contains / ContainsAny 与既有操作符的
//   类型不匹配规则；③ bitcask.meta 懒升 v7 纪元门禁（只在写入数组值时、先于记录落盘）；
//   ④ 检索 / 向量 / 迭代器 / range 的过滤里用上；⑤ 重开 / checkpoint / merge 后不变。
#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/meta_codec.hpp>
#include <bitcask/meta_file.hpp>
#include <bitcask/meta_filter.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace meta = bitcask::meta;
using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::DocInput;
using bitcask::DocPatch;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;
using meta::MetaCondition;
using meta::MetaFilter;
using meta::MetaOp;
using meta::MetaValue;
using meta::StringArray;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

std::vector<std::byte> blob(std::vector<meta::MetaEntry> es) {
    std::sort(es.begin(), es.end(), [](auto& a, auto& b) { return a.key < b.key; });
    std::vector<std::byte> out;
    meta::encode_meta(out, es);
    return out;
}

MetaFilter cond(std::string key, MetaOp op, MetaValue v = {}, std::vector<MetaValue> vs = {}) {
    MetaFilter f;
    f.conditions.push_back(MetaCondition{std::move(key), op, std::move(v), std::move(vs)});
    return f;
}

bool eval(const std::vector<std::byte>& b, std::string key, MetaOp op, MetaValue v = {},
          std::vector<MetaValue> vs = {}) {
    return cond(std::move(key), op, std::move(v), std::move(vs)).evaluate(b);
}

// ---------------- ① 编解码 ----------------

TEST(MetaArrayCodec, RoundTripAndLookupAroundArray) {
    const auto b = blob({{"a_int", std::int64_t{7}},
                         {"alias", StringArray{"青龙", "孟章", "青龙"}},  // 重复保留、顺序保留
                         {"empty", StringArray{}},
                         {"z_str", std::string("tail")}});
    auto d = meta::decode_meta(b);
    ASSERT_TRUE(d) << d.error();
    ASSERT_EQ(d->size(), 4U);
    EXPECT_EQ(std::get<StringArray>((*d)[1].value), (StringArray{"青龙", "孟章", "青龙"}));
    EXPECT_TRUE(std::get<StringArray>((*d)[2].value).empty());
    // 数组 key 前后的标量都能查到（跳读正确）——这正是旧读端会读错的位置。
    EXPECT_EQ(std::get<std::int64_t>(meta::meta_lookup(b, "a_int")), 7);
    EXPECT_EQ(std::get<std::string>(meta::meta_lookup(b, "z_str")), "tail");
    EXPECT_EQ(std::get<StringArray>(meta::meta_lookup(b, "alias")).size(), 3U);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(meta::meta_lookup(b, "nope")));
    EXPECT_TRUE(meta::meta_has_string_array(b));
    EXPECT_FALSE(meta::meta_has_string_array(blob({{"x", std::int64_t{1}}, {"y", std::string("s")}})));
    EXPECT_FALSE(meta::meta_has_string_array({}));
}

TEST(MetaArrayCodec, CorruptArrayIsRejectedNotOverread) {
    auto b = blob({{"alias", StringArray{"ab", "cd"}}});
    // 把数组元素个数改成很大（count 字节紧跟在 tag 5 之后）。
    auto it = std::find(b.begin(), b.end(), std::byte{5});
    ASSERT_NE(it, b.end());
    *(it + 1) = std::byte{0x7F};  // varint 末字节（高位置位表示结束）：count = 127
    EXPECT_FALSE(meta::decode_meta(b));
    EXPECT_FALSE(meta::meta_find_raw(b, "alias").found);
    b.resize(b.size() - 1);  // 截断
    EXPECT_FALSE(meta::decode_meta(b));
}

// ---------------- ② 过滤 ----------------

TEST(MetaArrayFilter, ContainsTruthTable) {
    const auto b = blob({{"alias", StringArray{"青龙", "孟章"}},
                         {"name", std::string("青龙")},
                         {"n", std::int64_t{3}},
                         {"tags", StringArray{}}});
    // 数组
    EXPECT_TRUE(eval(b, "alias", MetaOp::Contains, std::string("孟章")));
    EXPECT_FALSE(eval(b, "alias", MetaOp::Contains, std::string("龙")));  // 不是子串匹配
    EXPECT_TRUE(eval(b, "alias", MetaOp::ContainsAny, {}, {std::string("x"), std::string("青龙")}));
    EXPECT_FALSE(eval(b, "alias", MetaOp::ContainsAny, {}, {std::string("x")}));
    EXPECT_FALSE(eval(b, "tags", MetaOp::Contains, std::string("")));  // 空数组
    // 标量 string 当单元素集合
    EXPECT_TRUE(eval(b, "name", MetaOp::Contains, std::string("青龙")));
    EXPECT_TRUE(eval(b, "name", MetaOp::ContainsAny, {}, {std::string("青龙")}));
    // 其它类型 / 不存在 / 操作数不是 string
    EXPECT_FALSE(eval(b, "n", MetaOp::Contains, std::string("3")));
    EXPECT_FALSE(eval(b, "missing", MetaOp::Contains, std::string("青龙")));
    EXPECT_FALSE(eval(b, "alias", MetaOp::Contains, std::int64_t{1}));
    // 既有操作符碰数组：类型不匹配
    EXPECT_FALSE(eval(b, "alias", MetaOp::Eq, std::string("青龙")));
    EXPECT_TRUE(eval(b, "alias", MetaOp::Neq, std::string("青龙")));
    EXPECT_FALSE(eval(b, "alias", MetaOp::In, {}, {std::string("青龙")}));
    EXPECT_FALSE(eval(b, "alias", MetaOp::Gt, std::int64_t{0}));
    EXPECT_TRUE(eval(b, "alias", MetaOp::Exists));
    EXPECT_TRUE(eval(b, "tags", MetaOp::Exists));
    // 数组之后的 key 上的既有条件不受影响
    EXPECT_TRUE(eval(b, "name", MetaOp::Eq, std::string("青龙")));
    EXPECT_TRUE(eval(b, "n", MetaOp::Gte, std::int64_t{3}));
}

TEST(MetaArrayFilter, CombinesWithAndOr) {
    const auto b = blob({{"alias", StringArray{"青龙", "孟章"}}, {"kind", std::string("神兽")}});
    MetaFilter f;
    f.logic = MetaFilter::Logic::And;
    f.conditions.push_back({"kind", MetaOp::Eq, std::string("神兽"), {}});
    auto child = std::make_unique<MetaFilter>();
    child->logic = MetaFilter::Logic::Or;
    child->conditions.push_back({"alias", MetaOp::Contains, std::string("白虎"), {}});
    child->conditions.push_back({"alias", MetaOp::Contains, std::string("孟章"), {}});
    f.children.push_back(std::move(child));
    EXPECT_TRUE(f.evaluate(b));
    f.conditions[0].value = std::string("人");
    EXPECT_FALSE(f.evaluate(b));
}

// ---------------- ③～⑤ 端到端 ----------------

CaskOptions options(std::uint16_t vector_dim = 0) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    opts.vector_dim = vector_dim;
    return opts;
}

std::uint8_t epoch(const fs::path& dir) {
    auto mc = meta::read_meta(dir.string());
    EXPECT_TRUE(mc) << (mc ? "" : mc.error().message);
    return mc ? mc->version : 0;
}

class MetaArrayCask : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() / (std::string("bitcask_s62_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    std::unique_ptr<Cask> open(std::uint16_t vector_dim = 0) {
        auto c = Cask::open(dir_.string(), options(vector_dim), &test_registry());
        EXPECT_TRUE(c) << (c ? "" : c.error().detail);
        return c ? std::move(*c) : nullptr;
    }
    static void put(Cask& c, std::string_view key, std::string_view text,
                    const std::vector<std::byte>& m) {
        DocInput d;
        d.text = bytes(text);
        d.meta = m;
        ASSERT_TRUE(c.put_doc(bytes(key), d));
    }
    fs::path dir_;
};

std::set<std::string> keys(const bitcask::TextSearchResult& r) {
    std::set<std::string> s;
    for (const auto& h : r.hits) s.insert(h.key);
    return s;
}

TEST_F(MetaArrayCask, EpochOnlyBumpsWhenAnArrayIsWritten) {
    auto c = open();
    put(*c, "a", "plain doc", blob({{"x", std::int64_t{1}}}));
    EXPECT_EQ(epoch(dir_), 5) << "不写数组 ⇒ 不升纪元";
    put(*c, "b", "array doc", blob({{"alias", StringArray{"青龙"}}}));
    EXPECT_EQ(epoch(dir_), 7) << "第一条含数组值的记录 ⇒ v7";
    put(*c, "c", "plain again", blob({{"x", std::int64_t{2}}}));
    EXPECT_EQ(epoch(dir_), 7);
    // 原子批（无数组）不得把 v7 降回 v6。
    DocInput d;
    d.text = bytes("batch doc");
    Cask::BatchOp op;
    op.type = Cask::BatchOp::Type::kPutDoc;
    op.key = bytes("d");
    op.doc = &d;
    ASSERT_TRUE(c->put_batch_atomic(std::span<const Cask::BatchOp>(&op, 1)));
    EXPECT_EQ(epoch(dir_), 7);
    c->close();
    c = open();
    EXPECT_EQ(epoch(dir_), 7) << "重开保留纪元";
    c->close();
}

TEST_F(MetaArrayCask, AtomicBatchAndUpdateDocBumpToV7) {
    {
        auto c = open();
        // v5 → 原子批（无数组）⇒ v6
        DocInput d;
        d.text = bytes("doc");
        Cask::BatchOp op;
        op.type = Cask::BatchOp::Type::kPutDoc;
        op.key = bytes("k1");
        op.doc = &d;
        ASSERT_TRUE(c->put_batch_atomic(std::span<const Cask::BatchOp>(&op, 1)));
        EXPECT_EQ(epoch(dir_), 6);
        // v6 → update_doc 写入数组（走原子批路径）⇒ v7
        DocPatch p;
        p.set_meta.push_back({"alias", StringArray{"孟章"}});
        auto r = c->update_doc(bytes("k1"), p);
        ASSERT_TRUE(r) << r.error().detail;
        EXPECT_EQ(epoch(dir_), 7);
        c->close();
    }
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    {
        auto c = open();
        // v5 → 含数组的原子批 ⇒ 直接 v7
        const auto m = blob({{"alias", StringArray{"白虎"}}});
        DocInput d;
        d.text = bytes("doc");
        d.meta = m;
        Cask::BatchOp op;
        op.type = Cask::BatchOp::Type::kPutDoc;
        op.key = bytes("k1");
        op.doc = &d;
        ASSERT_TRUE(c->put_batch_atomic(std::span<const Cask::BatchOp>(&op, 1)));
        EXPECT_EQ(epoch(dir_), 7);
        c->close();
    }
}

TEST_F(MetaArrayCask, FiltersWorkInSearchIteratorsAndRange) {
    auto c = open();
    put(*c, "qinglong", "dragon of the east", blob({{"alias", StringArray{"青龙", "孟章"}},
                                                    {"kind", std::string("神兽")}}));
    put(*c, "baihu", "tiger of the west", blob({{"alias", StringArray{"白虎", "监兵"}},
                                               {"kind", std::string("神兽")}}));
    put(*c, "single", "the east wind", blob({{"alias", std::string("孟章")}}));  // 标量
    put(*c, "none", "the east sea", blob({{"kind", std::string("地名")}}));

    auto f = cond("alias", MetaOp::Contains, std::string("孟章"));
    auto r = c->search_text("east", 10, &f);
    ASSERT_TRUE(r);
    EXPECT_EQ(keys(*r), (std::set<std::string>{"qinglong", "single"}));

    auto fa = cond("alias", MetaOp::ContainsAny, {}, {std::string("监兵"), std::string("孟章")});
    auto ra = c->search_text("of", 10, &fa);
    ASSERT_TRUE(ra);
    EXPECT_EQ(keys(*ra), (std::set<std::string>{"qinglong", "baihu"}));

    // 全表迭代器
    {
        auto it = c->make_iter();
        it->set_filter(&f);
        ASSERT_TRUE(it->start());
        std::set<std::string> got;
        while (true) {
            auto e = it->next();
            ASSERT_TRUE(e);
            if (!*e) break;
            got.emplace(reinterpret_cast<const char*>((*e)->key.data()), (*e)->key.size());
        }
        it->release();
        EXPECT_EQ(got, (std::set<std::string>{"qinglong", "single"}));
    }
    // range
    {
        bitcask::RangeOptions ro;
        ro.filter = &f;
        auto it = c->make_range_iter(ro);
        ASSERT_TRUE(it) << it.error().detail;
        std::set<std::string> got;
        while (true) {
            auto e = (*it)->next();
            ASSERT_TRUE(e);
            if (!*e) break;
            got.emplace(reinterpret_cast<const char*>((*e)->key.data()), (*e)->key.size());
        }
        EXPECT_EQ(got, (std::set<std::string>{"qinglong", "single"}));
    }
    // checkpoint + 重开（docmap.ckpt 带着数组 meta）+ merge 后结果不变
    ASSERT_TRUE(c->checkpoint());
    c->close();
    c = open();
    auto r2 = c->search_text("east", 10, &f);
    ASSERT_TRUE(r2);
    EXPECT_EQ(keys(*r2), (std::set<std::string>{"qinglong", "single"}));
    ASSERT_TRUE(c->close_write_file());
    for (int i = 0; i < 3; ++i) {
        put(*c, "baihu", "tiger of the west", blob({{"alias", StringArray{"白虎", "监兵"}}}));
    }
    ASSERT_TRUE(c->close_write_file());
    std::vector<std::string> files;
    {
        const auto st = c->status();
        std::uint32_t active = 0;
        for (const auto& fsx : st.files) active = std::max(active, fsx.file_id);
        for (const auto& fsx : st.files) {
            if (fsx.file_id != active) files.push_back(fsx.filename);
        }
    }
    ASSERT_TRUE(c->merge(files));
    auto r3 = c->search_text("east", 10, &f);
    ASSERT_TRUE(r3);
    EXPECT_EQ(keys(*r3), (std::set<std::string>{"qinglong", "single"}));
    EXPECT_EQ(epoch(dir_), 7);
    // update_doc 换掉数组
    DocPatch p;
    p.set_meta.push_back({"alias", StringArray{"苍龙"}});
    ASSERT_TRUE(c->update_doc(bytes("qinglong"), p));
    auto r4 = c->search_text("east", 10, &f);
    ASSERT_TRUE(r4);
    EXPECT_EQ(keys(*r4), (std::set<std::string>{"single"}));
    c->close();
}

TEST_F(MetaArrayCask, VectorSearchTraversalFilter) {
    auto c = open(/*vector_dim=*/4);
    ASSERT_TRUE(c);
    const float v1[4] = {1, 0, 0, 0};
    const float v2[4] = {0.9F, 0.1F, 0, 0};
    const float v3[4] = {0.8F, 0.2F, 0, 0};
    const auto m1 = blob({{"alias", StringArray{"甲", "乙"}}});
    const auto m2 = blob({{"alias", StringArray{"丙"}}});
    const auto m3 = blob({{"alias", std::string("乙")}});
    for (auto [k, v, m] : {std::tuple{"v1", v1, &m1}, std::tuple{"v2", v2, &m2},
                           std::tuple{"v3", v3, &m3}}) {
        DocInput d;
        d.text = bytes("vec");
        d.vector = std::span<const float>(v, 4);
        d.meta = *m;
        ASSERT_TRUE(c->put_doc(bytes(k), d));
    }
    auto f = cond("alias", MetaOp::Contains, std::string("乙"));
    auto r = c->search_vector(std::span<const float>(v1, 4), 10, /*ef=*/0, &f);
    ASSERT_TRUE(r) << r.error().detail;
    EXPECT_EQ(keys(*r), (std::set<std::string>{"v1", "v3"}));
    c->close();
}

}  // namespace
