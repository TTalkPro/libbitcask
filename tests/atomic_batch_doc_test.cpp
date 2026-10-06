// 下游反馈 2026-10-06（feedbacks/2026-10-06-atomic-batch-doc-meta.md）回归：
// 原子批 / TxnCask 收结构化文档（BatchOp::kPutDoc / TxnOp::kPutDoc）——
// 文档（meta / vector / fields / expiry_at）与它的二级索引项同批落盘。
//   1. 批内文档与 put_doc 存储逐字段相同，并完整进索引（text / fields /
//      vector / meta filter），重启（data fold 全量重建）后仍在。
//   2. 下游场景：meta 由 c=red 改 c=blue，文档 + 新索引项 + 删旧索引项一批完成。
//   3. 撕裂批：文档成员与索引项一起不可见（keydir 与检索两侧）。
//   4. 校验：doc 缺失 / 向量维度错 / 未知 op → kInvalidOption，零副作用。
//   5. expiry_at 在批内生效；TxnCask::commit 同样收 kPutDoc。

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/meta_codec.hpp>
#include <bitcask/meta_file.hpp>
#include <bitcask/txn.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace bitcask;

keydir::KeyDirRegistry& test_registry() {
    static keydir::KeyDirRegistry reg;
    return reg;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::vector<std::byte> color_blob(const std::string& c) {
    std::vector<meta::MetaEntry> entries{{"c", meta::MetaValue(c)}};
    std::vector<std::byte> blob;
    meta::encode_meta(blob, entries);
    return blob;
}

meta::MetaFilter color_filter(const std::string& c) {
    meta::MetaFilter f;
    f.conditions.push_back({"c", meta::MetaOp::Eq, meta::MetaValue(c), {}});
    return f;
}

Cask::BatchOp doc_op(std::string_view k, const DocInput& d) {
    return {.type = Cask::BatchOp::Type::kPutDoc, .key = bytes(k), .doc = &d};
}
Cask::BatchOp put_op(std::string_view k, std::string_view v) {
    return {.type = Cask::BatchOp::Type::kPut, .key = bytes(k), .value = bytes(v)};
}
Cask::BatchOp remove_op(std::string_view k) {
    return {.type = Cask::BatchOp::Type::kRemove, .key = bytes(k)};
}

std::vector<std::string> keys_of(const TextSearchResult& r) {
    std::vector<std::string> out;
    for (const auto& h : r.hits) out.push_back(h.key);
    return out;
}

class AtomicBatchDocTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_atomic_batch_doc_test_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<Cask> open() {
        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        search::SearchLayerConfig scfg;
        scfg.analyzer_config.type = text::AnalyzerType::Whitespace;
        opts.search_config = scfg;
        opts.vector_dim = 4;
        // cosine：走「归一化缓冲移交」路径（vec_out 指向 vec_norm）。
        opts.vector_metric = meta::VectorMetric::kCosineNormalized;
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        EXPECT_TRUE(c) << (c ? "" : c.error().detail);
        return c ? *std::move(c) : nullptr;
    }

    // 同 atomic_batch_test：模拟崩溃且无新近 checkpoint——派生缓存全删，
    // 恢复必走 data fold 全量重建。另删 bm25.ckpt：close() 落的索引快照
    // 与 keydir 快照成对，真实掉电下同样不会覆盖到撕裂的批。
    void drop_derived() const {
        for (const auto& e : fs::directory_iterator(dir_)) {
            const std::string name = e.path().filename().string();
            if (e.path().extension() == ".hint" || name == "kv.keydir.ckpt" ||
                name == "bm25.ckpt" || name.rfind("kv.oki.", 0) == 0) {
                std::error_code ec;
                fs::remove(e.path(), ec);
            }
        }
    }

    fs::path max_data_file() const {
        std::uint64_t max_id = 0;
        fs::path found;
        constexpr std::string_view kSuffix = ".bitcask.data";
        for (const auto& e : fs::directory_iterator(dir_)) {
            const std::string name = e.path().filename().string();
            if (name.size() <= kSuffix.size() ||
                name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
                continue;
            const auto id = std::strtoull(
                name.substr(0, name.size() - kSuffix.size()).c_str(), nullptr, 10);
            if (id >= max_id) {
                max_id = id;
                found = e.path();
            }
        }
        return found;
    }

    static bool missing(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        return !r && r.error().kind == CaskError::kNotFound;
    }

    fs::path dir_;
};

constexpr float kVec[4] = {3.0F, 4.0F, 0.0F, 0.0F};

// 1：批内文档与 put_doc 存储相同、索引完整，重启后仍在。
TEST_F(AtomicBatchDocTest, PutDocInBatchMatchesPutDocAndIsIndexed) {
    const auto red = color_blob("red");
    DocInput d;
    d.text = bytes("apple pie");
    d.meta = red;
    d.vector = std::span<const float>(kVec, 4);
    d.fields = {{"title", bytes("crumble")}};
    {
        auto c = open();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put_doc(bytes("ref"), d, 1000));
        const std::vector<Cask::BatchOp> ops = {doc_op("doc1", d),
                                                put_op("ix/c/red/doc1", "")};
        ASSERT_TRUE(c->put_batch_atomic(ops, 1000));

        auto a = c->get_owned(bytes("doc1"));
        auto b = c->get_owned(bytes("ref"));
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        EXPECT_EQ(a->value, b->value);
        EXPECT_EQ(a->meta, b->meta);
        EXPECT_EQ(a->vector, b->vector);  // 同为归一化值 (0.6, 0.8, 0, 0)
        ASSERT_EQ(a->vector.size(), 4u);
        EXPECT_NEAR(a->vector[0], 0.6F, 1e-6);
        c->close();
    }
    for (int round = 0; round < 2; ++round) {
        if (round == 1) drop_derived();  // 第二轮：data fold 全量重建
        auto c = open();
        ASSERT_TRUE(c);
        c->flush_index();
        const auto red_f = color_filter("red");
        auto t = c->search_text("apple", 10, &red_f);
        ASSERT_TRUE(t);
        EXPECT_EQ(t->hits.size(), 2u) << "round " << round;
        auto f = c->search_fields("title:crumble", 10);
        ASSERT_TRUE(f);
        EXPECT_EQ(f->hits.size(), 2u) << "round " << round;
        auto v = c->search_vector(std::span<const float>(kVec, 4), 10, 0, &red_f);
        ASSERT_TRUE(v);
        EXPECT_EQ(v->hits.size(), 2u) << "round " << round;
        EXPECT_TRUE(c->get_owned(bytes("ix/c/red/doc1")));
        c->close();
    }
}

// 2：下游场景——meta c=red → c=blue，文档与索引项一批完成。
TEST_F(AtomicBatchDocTest, MetaChangeWithSecondaryIndexInOneBatch) {
    auto c = open();
    ASSERT_TRUE(c);
    const auto red = color_blob("red");
    const auto blue = color_blob("blue");
    DocInput d1;
    d1.text = bytes("apple");
    d1.meta = red;
    ASSERT_TRUE(c->put_batch_atomic(std::vector<Cask::BatchOp>{
        doc_op("pk", d1), put_op("ix/c/red/pk", "")}));

    DocInput d2;
    d2.text = bytes("apple");
    d2.meta = blue;
    ASSERT_TRUE(c->put_batch_atomic(std::vector<Cask::BatchOp>{
        doc_op("pk", d2), put_op("ix/c/blue/pk", ""), remove_op("ix/c/red/pk")}));

    EXPECT_TRUE(missing(*c, "ix/c/red/pk"));
    EXPECT_TRUE(c->get_owned(bytes("ix/c/blue/pk")));
    auto g = c->get_owned(bytes("pk"));
    ASSERT_TRUE(g);
    EXPECT_EQ(g->meta, blue);

    c->flush_index();
    const auto rf = color_filter("red");
    const auto bf = color_filter("blue");
    auto r = c->search_text("apple", 10, &rf);
    auto b = c->search_text("apple", 10, &bf);
    ASSERT_TRUE(r);
    ASSERT_TRUE(b);
    EXPECT_TRUE(r->hits.empty());
    EXPECT_EQ(keys_of(*b), std::vector<std::string>{"pk"});
    c->close();
}

// 3：撕裂批——文档与索引项一起不可见（keydir 与检索两侧）。
TEST_F(AtomicBatchDocTest, TornBatchDropsDocAndIndexEntryTogether) {
    const auto red = color_blob("red");
    DocInput d;
    d.text = bytes("apple");
    d.meta = red;
    d.vector = std::span<const float>(kVec, 4);
    {
        auto c = open();
        ASSERT_TRUE(c);
        ASSERT_TRUE(c->put(bytes("pre"), bytes("kept")));
        // 索引项排前、文档排末：掐尾 1 字节断的是文档成员。
        ASSERT_TRUE(c->put_batch_atomic(std::vector<Cask::BatchOp>{
            put_op("ix/c/red/pk", ""), doc_op("pk", d)}));
        ASSERT_TRUE(c->sync());
        // 崩溃时刻的盘面 = 库仍打开时的目录内容（不经 close——close 会落与
        // 完整 data 成对的索引快照/封口段，事后再掐 data 不是崩溃形态）。
        // 锁文件不拷：同进程 pid 存活，会被当成别的写者。
        const fs::path crash = dir_.string() + "_crash";
        std::error_code ec;
        fs::remove_all(crash, ec);
        fs::create_directories(crash);
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.path().filename().string().find("lock") != std::string::npos) continue;
            fs::copy(e.path(), crash / e.path().filename(),
                     fs::copy_options::recursive, ec);
            ASSERT_FALSE(ec) << e.path();
        }
        c->close();
        c.reset();
        fs::remove_all(dir_);
        fs::rename(crash, dir_);
    }
    drop_derived();
    const fs::path data = max_data_file();
    std::error_code ec;
    fs::resize_file(data, fs::file_size(data) - 1, ec);
    ASSERT_FALSE(ec);

    auto c = open();
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->get_owned(bytes("pre")));
    EXPECT_TRUE(missing(*c, "pk"));
    EXPECT_TRUE(missing(*c, "ix/c/red/pk"));
    c->flush_index();
    auto t = c->search_text("apple", 10);
    ASSERT_TRUE(t);
    EXPECT_TRUE(t->hits.empty());
    auto v = c->search_vector(std::span<const float>(kVec, 4), 10);
    ASSERT_TRUE(v);
    EXPECT_TRUE(v->hits.empty());
    c->close();
}

// 4：校验失败零副作用（meta 不升 v6、前序 op 不可见、字段名不 intern）。
TEST_F(AtomicBatchDocTest, ValidationFailuresHaveNoSideEffects) {
    auto c = open();
    ASSERT_TRUE(c);
    ASSERT_TRUE(c->put(bytes("seed"), bytes("x")));
    const auto v_before = meta::read_meta(dir_.string());
    ASSERT_TRUE(v_before);
    ASSERT_LT(v_before->version, 6);

    DocInput named;
    named.text = bytes("apple");
    named.fields = {{"never_interned", bytes("zzz")}};

    // doc 缺失。
    Cask::BatchOp no_doc{.type = Cask::BatchOp::Type::kPutDoc, .key = bytes("k")};
    auto r1 = c->put_batch_atomic(std::vector<Cask::BatchOp>{doc_op("a", named), no_doc});
    ASSERT_FALSE(r1);
    EXPECT_EQ(r1.error().kind, CaskError::kInvalidOption);

    // 向量维度错（排在带新字段名的文档之后——intern 必须尚未发生）。
    const float bad[3] = {1, 2, 3};
    DocInput wrong;
    wrong.text = bytes("x");
    wrong.vector = std::span<const float>(bad, 3);
    auto r2 = c->put_batch_atomic(std::vector<Cask::BatchOp>{doc_op("a", named),
                                                             doc_op("b", wrong)});
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().kind, CaskError::kInvalidOption);

    // 未知 op 类型。
    Cask::BatchOp unknown{.type = static_cast<Cask::BatchOp::Type>(7), .key = bytes("u")};
    auto r3 = c->put_batch_atomic(std::vector<Cask::BatchOp>{put_op("a", "1"), unknown});
    ASSERT_FALSE(r3);
    EXPECT_EQ(r3.error().kind, CaskError::kInvalidOption);

    EXPECT_TRUE(missing(*c, "a"));
    EXPECT_TRUE(missing(*c, "b"));
    auto v_after = meta::read_meta(dir_.string());
    ASSERT_TRUE(v_after);
    EXPECT_EQ(v_after->version, v_before->version);
    c->close();

    // field.schema 里不该出现被拒批的字段名。
    std::string schema;
    if (std::FILE* f = std::fopen((dir_ / "field.schema").string().c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) schema.append(buf, n);
        std::fclose(f);
    }
    EXPECT_EQ(schema.find("never_interned"), std::string::npos);
}

// 5：expiry_at 批内生效；TxnCask::commit 收 kPutDoc。
TEST_F(AtomicBatchDocTest, ExpiryAndTxnCommit) {
    auto c = open();
    ASSERT_TRUE(c);
    DocInput expired;
    expired.text = bytes("gone");
    expired.expiry_at = 1;  // 1970 年——早已过期
    const auto red = color_blob("red");
    DocInput live;
    live.text = bytes("apple");
    live.meta = red;

    TxnCask txn(c.get());
    const std::vector<TxnOp> ops = {
        {.type = TxnOp::Type::kPutDoc, .key = bytes("old"), .doc = &expired},
        {.type = TxnOp::Type::kPutDoc, .key = bytes("pk"), .doc = &live},
        {.type = TxnOp::Type::kPut, .key = bytes("ix/c/red/pk"), .value = bytes("")},
    };
    ASSERT_TRUE(txn.commit(ops));
    EXPECT_TRUE(missing(*c, "old"));
    auto g = c->get_owned(bytes("pk"));
    ASSERT_TRUE(g);
    EXPECT_EQ(g->meta, red);
    EXPECT_TRUE(c->get_owned(bytes("ix/c/red/pk")));

    // kPutDoc 缺 doc 经 TxnCask 同样被拒。
    const std::vector<TxnOp> bad = {{.type = TxnOp::Type::kPutDoc, .key = bytes("z")}};
    auto r = txn.commit(bad);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, CaskError::kInvalidOption);
    c->close();
}

}  // namespace
