#include <gtest/gtest.h>

#include <sys/stat.h>  // S14-2 .vec 追加测试:stat inode 断言

#include <bitcask/cask.hpp>
#include <bitcask/keydir_registry.hpp>
#include <bitcask/search_checkpoint.hpp>  // S14-3 段 carry 断言
#include <bitcask/codec.hpp>
#include <bitcask/data_file.hpp>  // parse_data_tstamp（S13 测试枚举 data 文件）
#include <bitcask/hw_crc32.hpp>   // S32-M0 meta 手改字节重算 CRC
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {

// S6-P0-pre：open() 现强制非空 registry。测试/bench 共享一个进程内 registry——
// 各用例用唯一目录名，互不冲突；同用例内 open→close→reopen 经 refcount 归零
// 重新从盘加载，与旧 nullptr 行为等价。
inline bitcask::keydir::KeyDirRegistry& test_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::GetResult;
using bitcask::GetResultView;  // V6.1
using bitcask::search::SearchLayerConfig;
using bitcask::search::SearchHit;
using bitcask::text::AnalyzerType;

// S6-P0-pre 契约护栏：open() 强制非空 registry，传 nullptr 必返 kInvalidOption。
TEST(CaskRegistryContract, OpenWithNullRegistryReturnsInvalidOption) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "bitcask_null_registry_test";
    fs::remove_all(tmp);
    CaskOptions opts;
    opts.read_write = true;
    auto r = Cask::open(tmp.string(), opts, nullptr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, bitcask::CaskError::kInvalidOption);
    fs::remove_all(tmp);
}

class CaskDocValueTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        // V3.3:目录加测试名后缀——ctest 按 case 并行调度,共享固定目录
        // 会被并发 case 的 SetUp/TearDown 互相清掉(实测偶发互踩)。
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = std::filesystem::temp_directory_path() /
                  (std::string("bitcask_docvalue_test_") + info->name());
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    std::filesystem::path tmpdir_;
};

TEST_F(CaskDocValueTest, PutGetRoundTrip) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}};
    std::vector<std::byte> val{std::byte{'v'}, std::byte{'a'}, std::byte{'l'}, std::byte{'u'}, std::byte{'e'}};

    auto pr = (*c)->put(key, val, /*tstamp*/ 1000);
    ASSERT_TRUE(pr);

    auto gr = (*c)->get_owned(key);
    ASSERT_TRUE(gr);
    EXPECT_EQ(gr->value, val);
    EXPECT_EQ(gr->tstamp, 1000u);
    // alloc_ord 从 0 开始，第一次 put 的 ord = 0
    EXPECT_EQ(gr->ord, 0u);

    (*c)->close();
}

TEST_F(CaskDocValueTest, PutGetRoundTripMultipleKeys) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key1{std::byte{'a'}};
    std::vector<std::byte> val1{std::byte{1}};
    std::vector<std::byte> key2{std::byte{'b'}};
    std::vector<std::byte> val2{std::byte{2}, std::byte{3}};

    ASSERT_TRUE((*c)->put(key1, val1, 1000));
    ASSERT_TRUE((*c)->put(key2, val2, 1001));

    auto r1 = (*c)->get_owned(key1);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->value, val1);

    auto r2 = (*c)->get_owned(key2);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->value, val2);

    // ord 单调递增：key1 先写=0，key2 后写=1
    EXPECT_EQ(r1->ord, 0u);
    EXPECT_EQ(r2->ord, 1u);
    EXPECT_NE(r1->ord, r2->ord);

    (*c)->close();
}

TEST_F(CaskDocValueTest, OrdMonotonicallyIncreasing) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'k'}};
    std::vector<std::byte> val{std::byte{'v'}};
    // 用 -1 作为哨兵，因为第一次 ord = 0
    std::int64_t last_ord = -1;

    for (int i = 0; i < 5; ++i) {
        auto pr = (*c)->put(key, val, static_cast<std::uint32_t>(2000 + i));
        ASSERT_TRUE(pr);
        auto gr = (*c)->get_owned(key);
        ASSERT_TRUE(gr);
        EXPECT_GT(static_cast<std::int64_t>(gr->ord), last_ord);
        last_ord = static_cast<std::int64_t>(gr->ord);
    }

    (*c)->close();
}

TEST_F(CaskDocValueTest, RemoveAndReinsert) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'k'}};
    std::vector<std::byte> val1{std::byte{'a'}};
    std::vector<std::byte> val2{std::byte{'b'}};

    ASSERT_TRUE((*c)->put(key, val1, 1000));
    auto r1 = (*c)->get_owned(key);
    ASSERT_TRUE(r1);
    std::uint64_t ord1 = r1->ord;

    ASSERT_TRUE((*c)->remove(key, 2000));

    ASSERT_TRUE((*c)->put(key, val2, 3000));
    auto r2 = (*c)->get_owned(key);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->value, val2);
    EXPECT_GT(r2->ord, ord1);

    (*c)->close();
}

TEST_F(CaskDocValueTest, MetaFileCreatedOnOpen) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    EXPECT_TRUE(fs::exists(tmpdir_ / "bitcask.meta"));
    (*c)->close();
}

TEST_F(CaskDocValueTest, ModeMismatchKVVsSearch) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    (*c)->close();

    CaskOptions search_opts;
    search_opts.read_write = true;
    search_opts.enable_search = true;
    auto c2 = Cask::open(tmpdir_.string(), search_opts, &test_registry());
    ASSERT_FALSE(c2);
    EXPECT_EQ(c2.error().kind, bitcask::CaskError::kModeMismatch);
}

TEST_F(CaskDocValueTest, ModeMismatchSearchVsKV) {
    namespace fs = std::filesystem;
    CaskOptions search_opts;
    search_opts.read_write = true;
    search_opts.enable_search = true;
    auto c = Cask::open(tmpdir_.string(), search_opts, &test_registry());
    ASSERT_TRUE(c);
    (*c)->close();

    CaskOptions opts;
    opts.read_write = true;
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_FALSE(c2);
    EXPECT_EQ(c2.error().kind, bitcask::CaskError::kModeMismatch);
}

TEST_F(CaskDocValueTest, DocValueEncodingVerified) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
    std::vector<std::byte> val{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

    ASSERT_TRUE((*c)->put(key, val, 1000));
    (*c)->close();

    // 重新打开后 get 仍然能正确解码 DocValue
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);

    auto gr = (*c2)->get_owned(key);
    ASSERT_TRUE(gr);
    EXPECT_EQ(gr->value, val);
    // 恢复后 ord 保持不变（从 record header 读取）
    EXPECT_EQ(gr->ord, 0u);

    (*c2)->close();
}

// S16-1：DocMap 宿主服务——Cask 持有的 docmap_ 与 SearchLayer 借用的
// index() 必须是同一实例（所有权反转的结构性断言）。
TEST_F(CaskDocValueTest, DocmapIsHostOwnedSharedInstance) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    ASSERT_TRUE((*c)->has_search());
    ASSERT_NE((*c)->docmap(), nullptr);
    // S19-2：shim 退役——docmap 即宿主唯一身份表实例，改断言其可用性。
    EXPECT_NE((*c)->docmap().get(), nullptr);

    // 经宿主句柄可见的写入 == 经 SearchLayer 可见的写入（同一实例语义）。
    std::vector<std::byte> key{std::byte{'d'}, std::byte{'m'}};
    std::vector<std::byte> val{std::byte{'x'}};
    ASSERT_TRUE((*c)->put(key, val, 1000));
    (*c)->flush_index();
    EXPECT_TRUE((*c)->docmap()->is_live(0));
    EXPECT_TRUE((*c)->docmap()->is_live(0));
    (*c)->close();
    EXPECT_EQ((*c)->docmap(), nullptr);  // close 清宿主句柄
}

TEST_F(CaskDocValueTest, SearchTextAfterPut) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Ngram;
    sl_cfg.analyzer_config.min_n = 2;
    sl_cfg.analyzer_config.max_n = 3;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_TRUE((*c)->has_search());

    std::vector<std::byte> key{std::byte{'k'}};
    std::vector<std::byte> val{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

    ASSERT_TRUE((*c)->put(key, val, 1000));

    // S19-2：shim 退役——原「同步 on_write 直插」改经插件查询内核直读
    //（同一通路：Cask 门面即薄委托），put 的索引经 flush_index 排干。
    (*c)->flush_index();

    auto* tp = (*c)->text_plugin();
    ASSERT_NE(tp, nullptr);
    auto r1 = tp->search_text("hello", 10);
    ASSERT_TRUE(r1) << "search_text failed: SearchError="
                    << static_cast<int>(r1.error());
    EXPECT_EQ(r1->size(), 1u);

    auto sr = (*c)->search_text("hello", /*k*/ 10);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 1u);
    EXPECT_EQ(sr->hits[0].ord, 0u);

    (*c)->close();
}

// S13-D8+D11：扩展观测字段 + HNSW 参数透传（端到端冒烟：非默认 M/efc 下
// 建图、检索、观测计数正确）。
TEST_F(CaskDocValueTest, StatusExFieldsAndHnswParamPassthrough) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    opts.vector_dim = 4;
    opts.vector_metric = bitcask::meta::VectorMetric::kL2;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    sl_cfg.hnsw_m = 8;                 // S13-D11：非默认建图参数
    sl_cfg.hnsw_ef_construction = 64;
    opts.search_config = sl_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s2) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s2.data()), s2.size());
    };
    const float vecs[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (int i = 0; i < 3; ++i) {
        bitcask::DocInput doc;
        const std::string text = "doc number " + std::to_string(i);
        doc.text = bytes(text);
        doc.vector = std::span<const float>(vecs[i], 4);
        ASSERT_TRUE((*c)->put_doc(bytes("sx" + std::to_string(i)), doc, 1000));
    }
    (*c)->flush_index();

    // D11 冒烟：非默认参数下向量检索正确。
    const float q[4] = {1, 0, 0, 0};
    auto vr = (*c)->search_vector(std::span<const float>(q, 4), 1);
    ASSERT_TRUE(vr);
    ASSERT_EQ(vr->hits.size(), 1u);
    EXPECT_EQ(vr->hits[0].key, "sx0");

    // D8：文本搜索一次让缓存入一条，再看观测字段。
    auto tr = (*c)->search_text("doc", 10);
    ASSERT_TRUE(tr);
    auto st = (*c)->status();
    EXPECT_EQ(st.hnsw_nodes, 3u);
    EXPECT_GE(st.search_cache_entries, 1u);
    EXPECT_EQ(st.key_count, 3u);
    (*c)->close();
}

// S13-D7：日志回调——用「keydir 快照保存失败」（目录只读）确定性触发 warn。
TEST_F(CaskDocValueTest, LogHookFiresOnSnapshotFailure) {
    std::vector<std::string> logs;
    std::mutex logs_mu;

    CaskOptions opts;
    opts.read_write = true;
    opts.log_fn = [&](CaskOptions::LogLevel lvl, std::string_view msg) {
        std::lock_guard<std::mutex> g(logs_mu);
        logs.push_back(std::string(lvl == CaskOptions::LogLevel::kError
                                       ? "E:" : "W:") + std::string(msg));
    };
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    std::vector<std::byte> k{std::byte{'k'}}, v{std::byte{'v'}};
    ASSERT_TRUE((*c)->put(k, v, 1000));

    // 目录改只读：close 的 write_keydir_snapshot（temp 文件创建）失败 → warn。
    std::filesystem::permissions(tmpdir_,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    (*c)->close();
    std::filesystem::permissions(tmpdir_, std::filesystem::perms::owner_all);

    std::lock_guard<std::mutex> g(logs_mu);
    ASSERT_FALSE(logs.empty()) << "只读目录下 close 应触发快照保存失败上报";
    bool has_snap_warn = false;
    for (const auto& m : logs) {
        if (m.find("snapshot save failed") != std::string::npos) {
            has_snap_warn = true;
        }
    }
    EXPECT_TRUE(has_snap_warn) << "应包含 keydir snapshot 失败的 warn";
}

// S13-D9：布尔查询语言——括号嵌套 + 引号短语（树路径），扁平语法行为不变。
TEST_F(CaskDocValueTest, BoolSearchTreeSyntax) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto bytes = [](std::string_view s2) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s2.data()), s2.size());
    };
    auto put = [&](std::string_view key, std::string_view text) {
        bitcask::DocInput doc;
        doc.text = bytes(text);
        ASSERT_TRUE((*c)->put_doc(bytes(key), doc, 1000));
    };
    put("d1", "rust systems programming");
    put("d2", "rust web framework");
    put("d3", "go web service");
    put("d4", "python data science");

    auto keys_of = [](const bitcask::TextSearchResult& r) {
        std::vector<std::string> ks;
        for (const auto& h : r.hits) ks.push_back(h.key);
        std::sort(ks.begin(), ks.end());
        return ks;
    };

    // 括号组：(rust OR go) AND web → d2, d3。扁平语法无法表达。
    auto r1 = (*c)->bool_search("+(rust go) +web", 10);
    ASSERT_TRUE(r1);
    EXPECT_EQ(keys_of(*r1), (std::vector<std::string>{"d2", "d3"}));

    // 引号短语子句：必须包含 "rust systems" → 仅 d1。
    auto r2 = (*c)->bool_search("+\"rust systems\"", 10);
    ASSERT_TRUE(r2);
    EXPECT_EQ(keys_of(*r2), (std::vector<std::string>{"d1"}));

    // 短语排除 + 词：rust 文档中排除短语命中 → d2。
    auto r3 = (*c)->bool_search("+rust -\"rust systems\"", 10);
    ASSERT_TRUE(r3);
    EXPECT_EQ(keys_of(*r3), (std::vector<std::string>{"d2"}));

    // 嵌套组 + 组级排除：(web AND (rust OR go)) 排除 go → d2。
    auto r4 = (*c)->bool_search("+(+web +(rust go)) -go", 10);
    ASSERT_TRUE(r4);
    EXPECT_EQ(keys_of(*r4), (std::vector<std::string>{"d2"}));

    // 容错：未闭合引号取到尾。
    auto r5 = (*c)->bool_search("+\"rust systems", 10);
    ASSERT_TRUE(r5);
    EXPECT_EQ(keys_of(*r5), (std::vector<std::string>{"d1"}));

    // 回归：无新语法查询走扁平路径，语义不变（+rust +web → d2）。
    auto r6 = (*c)->bool_search("+rust +web", 10);
    ASSERT_TRUE(r6);
    EXPECT_EQ(keys_of(*r6), (std::vector<std::string>{"d2"}));
    (*c)->close();
}

// S13-D10：分页 offset——overfetch 后截断；offset 页与全量列表的对应切片一致。
TEST_F(CaskDocValueTest, SearchTextPaginationOffset) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto bytes = [](std::string_view s2) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s2.data()), s2.size());
    };
    // 5 篇含 "page"，词频递增 → 分数区分度。
    for (int i = 0; i < 5; ++i) {
        std::string text = "page";
        for (int r = 0; r < i; ++r) text += " page";
        text += " filler" + std::to_string(i);
        bitcask::DocInput doc;
        doc.text = bytes(text);
        ASSERT_TRUE((*c)->put_doc(bytes("pg" + std::to_string(i)), doc, 1000));
    }

    auto full = (*c)->search_text("page", 5);
    ASSERT_TRUE(full);
    ASSERT_EQ(full->hits.size(), 5u);

    auto page2 = (*c)->search_text("page", 2, nullptr, /*offset*/ 2);
    ASSERT_TRUE(page2);
    ASSERT_EQ(page2->hits.size(), 2u);
    EXPECT_EQ(page2->hits[0].key, full->hits[2].key);
    EXPECT_EQ(page2->hits[1].key, full->hits[3].key);

    auto beyond = (*c)->search_text("page", 10, nullptr, /*offset*/ 100);
    ASSERT_TRUE(beyond);
    EXPECT_TRUE(beyond->hits.empty());
    (*c)->close();
}

// S13-D5：per-key TTL——get/iter 过滤 + merge 回收（清 keydir）+ 无 TTL 回归。
TEST_F(CaskDocValueTest, PerKeyTtlExpiryAndMergeReclaim) {
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 512;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto bytes = [](std::string_view s2) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s2.data()), s2.size());
    };
    const auto now = static_cast<std::uint32_t>(::time(nullptr));

    // 三类：已过期 / 未来过期 / 无 TTL。
    ASSERT_TRUE((*c)->put(bytes("t_expired"), bytes("v1"), 1000, now - 10));
    ASSERT_TRUE((*c)->put(bytes("t_future"),  bytes("v2"), 1000, now + 3600));
    ASSERT_TRUE((*c)->put(bytes("t_none"),    bytes("v3"), 1000));

    EXPECT_FALSE((*c)->get_owned(bytes("t_expired")));   // 过期 = 不存在
    ASSERT_TRUE((*c)->get_owned(bytes("t_future")));
    ASSERT_TRUE((*c)->get_owned(bytes("t_none")));

    // 迭代器同语义：只见 2 条。
    auto it = (*c)->make_iter();
    auto sr = it->start();
    ASSERT_TRUE(sr);
    ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);
    std::size_t n = 0;
    while (true) {
        auto e = it->next();
        ASSERT_TRUE(e);
        if (!e->has_value()) break;
        ++n;
    }
    it->release();
    EXPECT_EQ(n, 2u);

    // merge：过期记录不搬运并清 keydir（records_expired 计数）。
    // 先滚出 sealed 文件再显式 merge 全部 sealed。
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE((*c)->put(bytes("fill" + std::to_string(i)),
                              bytes("ffffffffffffffff"), 1000));
    }
    auto nm = (*c)->needs_merge();
    std::vector<std::string> files = std::move(nm.files);
    if (files.empty()) {
        // 手动收集 sealed 文件（全 live 不触发策略）。
        for (const auto& de : std::filesystem::directory_iterator(tmpdir_)) {
            const auto name = de.path().filename().string();
            if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
                files.push_back(de.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        files.pop_back();  // 排除 active
    }
    auto mr = (*c)->merge(files);
    ASSERT_TRUE(mr);
    EXPECT_GE(mr->records_expired, 1u);
    EXPECT_FALSE((*c)->get_owned(bytes("t_expired")));
    ASSERT_TRUE((*c)->get_owned(bytes("t_future")));
    ASSERT_TRUE((*c)->get_owned(bytes("t_none")));
    (*c)->close();

    // 重启：过期 key 不复活（merge 已清）；其余仍在。
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);
    EXPECT_FALSE((*c2)->get_owned(bytes("t_expired")));
    ASSERT_TRUE((*c2)->get_owned(bytes("t_future")));
    ASSERT_TRUE((*c2)->get_owned(bytes("t_none")));
    (*c2)->close();
}

// S13-D6：备份——热拷贝到新目录，备份目录可独立 open；原库备份后继续可写。
TEST_F(CaskDocValueTest, BackupHotCopyAndReopen) {
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 512;  // 滚出多个 sealed 文件
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s2) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s2.data()), s2.size());
    };
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE((*c)->put(bytes("bku" + std::to_string(i)),
                              bytes("val" + std::to_string(i)), 1000));
    }

    const auto dst = tmpdir_.string() + "_backup";
    std::filesystem::remove_all(dst);
    ASSERT_TRUE((*c)->backup(dst));

    // 原库备份后仍可写（active writer 惰性重建）且旧数据可读。
    ASSERT_TRUE((*c)->put(bytes("after_backup"), bytes("x"), 2000));
    ASSERT_TRUE((*c)->get_owned(bytes("bku0")));
    (*c)->close();

    // 备份目录独立 open：50 个 key 全在，备份后写入的 key 不在。
    CaskOptions ro;
    ro.read_write = false;
    auto b = Cask::open(dst, ro, &test_registry());
    ASSERT_TRUE(b);
    for (int i = 0; i < 50; ++i) {
        auto r = (*b)->get_owned(bytes("bku" + std::to_string(i)));
        ASSERT_TRUE(r) << "backup missing key bku" << i;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->value.data()),
                              r->value.size()),
                  "val" + std::to_string(i));
    }
    EXPECT_FALSE((*b)->get_owned(bytes("after_backup")));
    (*b)->close();
    std::filesystem::remove_all(dst);
}

// S13-D1：put_batch——整批一次提交（flush 后才 apply keydir）。
TEST_F(CaskDocValueTest, PutBatchBasicAndPersistence) {
    CaskOptions opts;
    opts.read_write = true;
    opts.sync_every_n = 8;  // 走「整批一次组提交」路径
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s.data()), s.size());
    };

    constexpr int kN = 200;
    std::vector<std::string> keys, vals;
    keys.reserve(kN);
    vals.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        keys.push_back("bk" + std::to_string(i));
        vals.push_back("value_" + std::to_string(i));
    }
    std::vector<Cask::BatchItem> batch;
    batch.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        batch.push_back({bytes(keys[i]), bytes(vals[i])});
    }
    ASSERT_TRUE((*c)->put_batch(batch, 1000));

    // 全部可读且值正确。
    for (int i = 0; i < kN; ++i) {
        auto r = (*c)->get_owned(bytes(keys[i]));
        ASSERT_TRUE(r) << "batch key missing: " << keys[i];
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->value.data()),
                              r->value.size()),
                  vals[i]);
    }

    // 空批 no-op；校验失败零副作用（超大 key 的批整体拒绝，其余条目不写入）。
    ASSERT_TRUE((*c)->put_batch({}, 1000));
    std::string big_key(bitcask::format::kMaxKeySize + 1, 'x');
    std::vector<Cask::BatchItem> bad;
    bad.push_back({bytes("bk_new_never_written"), bytes("v")});
    bad.push_back({bytes(big_key), bytes("v")});
    auto br = (*c)->put_batch(bad, 1000);
    ASSERT_FALSE(br);
    EXPECT_EQ(br.error().kind, bitcask::CaskError::kKeyTooLarge);
    EXPECT_FALSE((*c)->get_owned(bytes("bk_new_never_written")))
        << "校验失败的批不应有任何副作用";

    // 批中覆盖已有 key：LWW 生效。
    std::vector<Cask::BatchItem> upd;
    upd.push_back({bytes(keys[0]), bytes("updated")});
    ASSERT_TRUE((*c)->put_batch(upd, 2000));
    auto u = (*c)->get_owned(bytes(keys[0]));
    ASSERT_TRUE(u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(u->value.data()),
                          u->value.size()),
              "updated");

    (*c)->close();

    // 重启后全部仍在（含覆盖后的值）。
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);
    for (int i = 1; i < kN; ++i) {
        auto r = (*c2)->get_owned(bytes(keys[i]));
        ASSERT_TRUE(r) << "batch key lost after reopen: " << keys[i];
    }
    auto u2 = (*c2)->get_owned(bytes(keys[0]));
    ASSERT_TRUE(u2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(u2->value.data()),
                          u2->value.size()),
              "updated");
    (*c2)->close();
}

// S13-D1：索引模式下 put_batch 的 Add 任务经异步管线正确入索引。
TEST_F(CaskDocValueTest, PutBatchIndexedSearchable) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    std::vector<Cask::BatchItem> batch;
    batch.push_back({bytes("bd0"), bytes("batch hello world")});
    batch.push_back({bytes("bd1"), bytes("batch hello there")});
    batch.push_back({bytes("bd2"), bytes("other text")});
    ASSERT_TRUE((*c)->put_batch(batch, 1000));

    auto sr = (*c)->search_text("hello", 10);  // 门面内部 flush 管线
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 2u);
    (*c)->close();
}

// S13-D4：前缀扫描——CaskIter::start / parallel_scan 的 key_prefix 过滤。
TEST_F(CaskDocValueTest, PrefixScanIterAndParallelScan) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s.data()), s.size());
    };
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE((*c)->put(bytes("user:" + std::to_string(i)),
                              bytes("u" + std::to_string(i)), 1000));
    }
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE((*c)->put(bytes("post:" + std::to_string(i)),
                              bytes("p" + std::to_string(i)), 1000));
    }
    // "user" 是 "user:*" 的前缀但也是完整 key——加一条验证边界（恰等于前缀）。
    ASSERT_TRUE((*c)->put(bytes("user:"), bytes("root"), 1000));

    // 迭代器前缀过滤：只出 "user:" 开头的 6 条。
    auto it = (*c)->make_iter();
    auto sr = it->start(-1, -1, 0, false, bytes("user:"));
    ASSERT_TRUE(sr);
    ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);
    std::size_t n = 0;
    while (true) {
        auto e = it->next();
        ASSERT_TRUE(e);
        if (!e->has_value()) break;
        const auto& key = (*e)->key;
        ASSERT_GE(key.size(), 5u);
        EXPECT_EQ(std::memcmp(key.data(), "user:", 5), 0);
        ++n;
    }
    it->release();
    EXPECT_EQ(n, 6u);

    // parallel_scan 前缀过滤：只访问 3 条 "post:"。
    std::atomic<std::size_t> visited{0};
    auto pr = (*c)->parallel_scan(
        2,
        [&](std::span<const std::byte> key, const bitcask::GetResultView&) {
            ASSERT_GE(key.size(), 5u);
            EXPECT_EQ(std::memcmp(key.data(), "post:", 5), 0);
            visited.fetch_add(1, std::memory_order_relaxed);
        },
        bytes("post:"));
    ASSERT_TRUE(pr);
    EXPECT_EQ(*pr, 3u);
    EXPECT_EQ(visited.load(), 3u);

    // 空前缀 = 全表（回归既有语义）。
    std::atomic<std::size_t> all{0};
    auto pr_all = (*c)->parallel_scan(
        2, [&](std::span<const std::byte>, const bitcask::GetResultView&) {
            all.fetch_add(1, std::memory_order_relaxed);
        });
    ASSERT_TRUE(pr_all);
    EXPECT_EQ(*pr_all, 9u);

    (*c)->close();
}

// S13-D3：search_text_highlight 门面方法（README 宣称已久，本轮补上）。
// 端到端：put 经异步管线索引 → Cask 门面高亮搜索 → 命中含片段；
// 关闭后 fail-fast 返回 kClosed（门面契约，绕过门面直调 SearchLayer 没有）。
TEST_F(CaskDocValueTest, SearchTextHighlightViaCaskFacade) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    const std::string_view text = "the quick brown fox jumps over the lazy dog";
    std::vector<std::byte> key{std::byte{'k'}};
    std::vector<std::byte> val(
        reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data()) + text.size());
    ASSERT_TRUE((*c)->put(key, val, 1000));

    // 门面内部 prepare_search() 会 flush 异步索引管线，无需手动 flush。
    auto hr = (*c)->search_text_highlight("quick fox", 10);
    ASSERT_TRUE(hr) << "search_text_highlight failed";
    ASSERT_EQ(hr->hits.size(), 1u);
    EXPECT_FALSE(hr->hits[0].highlights.empty())
        << "hit should carry highlight snippets";

    (*c)->close();
    auto closed = (*c)->search_text_highlight("quick", 10);
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().kind, bitcask::CaskError::kClosed);
}

// S8.6：put_doc 多字段 → search_fields 字段路由，端到端经 Cask（含异步 IndexTask）。
TEST_F(CaskDocValueTest, MultiFieldPutAndSearch) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s.data()), s.size());
    };

    // doc1: title="apple fruit", body="banana"
    bitcask::DocInput d1;
    d1.fields.push_back({"title", bytes("apple fruit")});
    d1.fields.push_back({"body", bytes("banana split")});
    ASSERT_TRUE((*c)->put_doc(bytes("doc1"), d1, 1000));

    // doc2: title="banana", body="apple"
    bitcask::DocInput d2;
    d2.fields.push_back({"title", bytes("banana bread")});
    d2.fields.push_back({"body", bytes("apple pie")});
    ASSERT_TRUE((*c)->put_doc(bytes("doc2"), d2, 1001));

    // title:apple 只命中 doc1（apple 在 doc1 title、doc2 body）。
    auto r = (*c)->search_fields("title:apple", 10);
    ASSERT_TRUE(r) << "search_fields failed";
    ASSERT_EQ(r->hits.size(), 1u);
    EXPECT_EQ(r->hits[0].ord, 0u);  // doc1 是第一个写入，ord=0

    // body:apple 只命中 doc2。
    auto r2 = (*c)->search_fields("body:apple", 10);
    ASSERT_TRUE(r2);
    ASSERT_EQ(r2->hits.size(), 1u);
    EXPECT_EQ(r2->hits[0].ord, 1u);

    (*c)->close();
}

TEST_F(CaskDocValueTest, DocTextIndexedAsDefaultInMultiFieldMode) {
    // S28-1: doc.text 在多字段模式下应索引为 kDefaultField
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto bytes = [](std::string_view s) {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(s.data()), s.size());
    };

    bitcask::DocInput d;
    d.text = bytes("unique alpha keywords");
    d.fields.push_back({"title", bytes("special beta terms")});
    ASSERT_TRUE((*c)->put_doc(bytes("k1"), d, 1000));

    // doc.text 的词项应进默认字段索引(S28-1 修复前丢失)
    auto r1 = (*c)->search_text("alpha", 10);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->hits.size(), 1u) << "doc.text 应进默认字段索引(S28-1)";

    // title 经 catch-all 也应命中默认字段
    auto r2 = (*c)->search_text("beta", 10);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->hits.size(), 1u) << "title 经 catch-all 应命中";

    // search_fields 验证命名字段仍独立可查
    auto r3 = (*c)->search_fields("title:special", 10);
    ASSERT_TRUE(r3);
    EXPECT_EQ(r3->hits.size(), 1u);

    (*c)->close();
}

TEST_F(CaskDocValueTest, SearchTextEmptyAfterRemove) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Ngram;
    sl_cfg.analyzer_config.min_n = 2;
    sl_cfg.analyzer_config.max_n = 3;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'k'}};
    std::vector<std::byte> val{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

    ASSERT_TRUE((*c)->put(key, val, 1000));
    ASSERT_TRUE((*c)->remove(key, 2000));

    auto sr = (*c)->search_text("hello", /*k*/ 10);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 0u);

    (*c)->close();
}

// === Merge + Search 集成测试 ===

class CaskMergeSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpdir_ = std::filesystem::temp_directory_path() /
            ("bitcask_merge_search_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    std::filesystem::path tmpdir_;

    CaskOptions make_search_opts() {
        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        SearchLayerConfig sl_cfg;
        sl_cfg.analyzer_config.type = AnalyzerType::Ngram;
        sl_cfg.analyzer_config.min_n = 2;
        sl_cfg.analyzer_config.max_n = 3;
        opts.search_config = sl_cfg;
        return opts;
    }
};

TEST_F(CaskMergeSearchTest, SearchSurvivesMerge) {
    auto opts = make_search_opts();
    opts.max_file_size = 32;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key_a{std::byte{'a'}};
    std::vector<std::byte> val_hello{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

    // max_file_size=32 且每条约 35 字节（23 header + 1 key + ~11 DocValue），
    // 每次 put 都会触发文件滚动
    ASSERT_TRUE((*c)->put(key_a, val_hello, 1000));

    auto sr = (*c)->search_text("hello", 10);
    ASSERT_TRUE(sr);
    ASSERT_EQ(sr->hits.size(), 1u);

    // 关闭后重新打开，恢复 SearchLayer
    (*c)->close();
    c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    sr = (*c)->search_text("hello", 10);
    ASSERT_TRUE(sr);
    ASSERT_EQ(sr->hits.size(), 1u);
    auto ord_before = sr->hits[0].ord;

    // 收集 data file 路径并显式传给 merge
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(tmpdir_)) {
        auto name = entry.path().filename().string();
        if (name.ends_with(".bitcask.data")) {
            files.push_back(entry.path().string());
        }
    }
    ASSERT_GE(files.size(), 1u);

    auto mr = (*c)->merge(files, 3000);
    ASSERT_TRUE(mr) << "merge failed: " << mr.error().detail;
    EXPECT_GT(mr->records_kept, 0u);

    // merge 后搜索结果不变（ord 保留）
    auto sr2 = (*c)->search_text("hello", 10);
    ASSERT_TRUE(sr2);
    ASSERT_EQ(sr2->hits.size(), 1u);
    EXPECT_EQ(sr2->hits[0].ord, ord_before);

    // get 仍然正确
    auto gr = (*c)->get_owned(key_a);
    ASSERT_TRUE(gr);
    EXPECT_EQ(gr->value, val_hello);

    (*c)->close();
}

TEST_F(CaskMergeSearchTest, MergeWithMultipleKeysAndSearch) {
    auto opts = make_search_opts();
    opts.max_file_size = 256;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> k1{std::byte{'x'}};
    std::vector<std::byte> k2{std::byte{'y'}};
    std::vector<std::byte> v_cat{std::byte{'c'}, std::byte{'a'}, std::byte{'t'}};
    std::vector<std::byte> v_dog{std::byte{'d'}, std::byte{'o'}, std::byte{'g'}};

    ASSERT_TRUE((*c)->put(k1, v_cat, 1000));
    ASSERT_TRUE((*c)->put(k2, v_dog, 1001));

    // merge
    auto mr = (*c)->merge({}, 2000);
    ASSERT_TRUE(mr);

    // 搜索 "cat" 和 "dog" 都能找到
    auto sr_cat = (*c)->search_text("cat", 10);
    ASSERT_TRUE(sr_cat);
    EXPECT_EQ(sr_cat->hits.size(), 1u);

    auto sr_dog = (*c)->search_text("dog", 10);
    ASSERT_TRUE(sr_dog);
    EXPECT_EQ(sr_dog->hits.size(), 1u);

    // ord 保持不同
    EXPECT_NE(sr_cat->hits[0].ord, sr_dog->hits[0].ord);

    (*c)->close();
}

TEST_F(CaskMergeSearchTest, MergeEliminatesDeletedDocs) {
    auto opts = make_search_opts();
    opts.max_file_size = 256;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'z'}};
    std::vector<std::byte> val{std::byte{'w'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'}, std::byte{'d'}};

    ASSERT_TRUE((*c)->put(key, val, 1000));

    auto sr1 = (*c)->search_text("world", 10);
    ASSERT_TRUE(sr1);
    EXPECT_EQ(sr1->hits.size(), 1u);

    // 删除后搜索为空
    ASSERT_TRUE((*c)->remove(key, 2000));
    auto sr2 = (*c)->search_text("world", 10);
    ASSERT_TRUE(sr2);
    EXPECT_EQ(sr2->hits.size(), 0u);

    // merge 后搜索仍然为空（不恢复已删除的文档）
    auto mr = (*c)->merge({}, 3000);
    ASSERT_TRUE(mr);

    auto sr3 = (*c)->search_text("world", 10);
    ASSERT_TRUE(sr3);
    EXPECT_EQ(sr3->hits.size(), 0u);

    (*c)->close();
}

TEST_F(CaskMergeSearchTest, MergeUpdatesOverwrittenKey) {
    auto opts = make_search_opts();
    opts.max_file_size = 256;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::vector<std::byte> key{std::byte{'m'}};
    std::vector<std::byte> val_old{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
    std::vector<std::byte> val_new{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}};

    ASSERT_TRUE((*c)->put(key, val_old, 1000));
    ASSERT_TRUE((*c)->put(key, val_new, 1001));

    // 搜索只找到 "new"
    auto sr = (*c)->search_text("new", 10);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 1u);

    // merge 后仍然只找到 "new"
    auto mr = (*c)->merge({}, 2000);
    ASSERT_TRUE(mr);

    auto sr2 = (*c)->search_text("new", 10);
    ASSERT_TRUE(sr2);
    EXPECT_EQ(sr2->hits.size(), 1u);

    // "old" 不应该找到
    auto sr_old = (*c)->search_text("old", 10);
    ASSERT_TRUE(sr_old);
    EXPECT_EQ(sr_old->hits.size(), 0u);

    (*c)->close();
}

class CaskUpgradeTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpdir_ = std::filesystem::temp_directory_path() /
                  ("cask_upgrade_test_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
        std::filesystem::create_directories(tmpdir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmpdir_, ec);
    }

    std::filesystem::path tmpdir_;
};

TEST_F(CaskUpgradeTest, UpgradeKVToIndex) {
    auto bytes = [](std::string_view s) {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(s.data()),
            reinterpret_cast<const std::byte*>(s.data()) + s.size());
    };

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);

        auto k1 = bytes("key1");
        auto v1 = bytes("hello world");
        auto r1 = (*c)->put(k1, v1);
        ASSERT_TRUE(r1);

        auto k2 = bytes("key2");
        auto v2 = bytes("hello again");
        auto r2 = (*c)->put(k2, v2);
        ASSERT_TRUE(r2);

        (*c)->close();
    }

    SearchLayerConfig search_cfg;
    search_cfg.analyzer_config.type = AnalyzerType::Ngram;
    search_cfg.analyzer_config.min_n = 2;
    search_cfg.analyzer_config.max_n = 3;
    auto upg = Cask::upgrade(tmpdir_.string(), search_cfg);
    ASSERT_TRUE(upg) << "upgrade failed";

    auto sr = (*upg)->search_text("hello", 10);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 2u);

    auto g1 = (*upg)->get_owned(bytes("key1"));
    ASSERT_TRUE(g1);
    EXPECT_EQ(g1->value, bytes("hello world"));

    (*upg)->close();

    {
        CaskOptions opts;
        opts.read_write = true;
        opts.enable_search = true;
        opts.search_config = search_cfg;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);

        auto sr2 = (*c)->search_text("hello", 10);
        ASSERT_TRUE(sr2);
        EXPECT_EQ(sr2->hits.size(), 2u);

        (*c)->close();
    }
}

TEST_F(CaskUpgradeTest, UpgradeFailsOnNonexistentDir) {
    SearchLayerConfig search_cfg;
    search_cfg.analyzer_config.type = AnalyzerType::Ngram;
    auto upg = Cask::upgrade("/tmp/no_such_bitcask_dir_12345", search_cfg);
    EXPECT_FALSE(upg);
}

TEST_F(CaskUpgradeTest, UpgradeFailsOnAlreadyIndexMode) {
    auto bytes = [](std::string_view s) {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(s.data()),
            reinterpret_cast<const std::byte*>(s.data()) + s.size());
    };

    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig search_cfg;
    search_cfg.analyzer_config.type = AnalyzerType::Ngram;
    opts.search_config = search_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    (*c)->put(bytes("k"), bytes("v"));
    (*c)->close();

    auto upg = Cask::upgrade(tmpdir_.string(), search_cfg);
    EXPECT_FALSE(upg);
}

TEST_F(CaskUpgradeTest, UpgradePreservesDeletes) {
    auto bytes = [](std::string_view s) {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(s.data()),
            reinterpret_cast<const std::byte*>(s.data()) + s.size());
    };

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);

        (*c)->put(bytes("keep"), bytes("kept value"));
        (*c)->put(bytes("remove"), bytes("removed value"));
        (*c)->remove(bytes("remove"));
        (*c)->close();
    }

    SearchLayerConfig search_cfg;
    search_cfg.analyzer_config.type = AnalyzerType::Ngram;
    search_cfg.analyzer_config.min_n = 2;
    search_cfg.analyzer_config.max_n = 3;
    auto upg = Cask::upgrade(tmpdir_.string(), search_cfg);
    ASSERT_TRUE(upg);

    auto g_keep = (*upg)->get_owned(bytes("keep"));
    ASSERT_TRUE(g_keep);

    auto g_rm = (*upg)->get_owned(bytes("remove"));
    EXPECT_FALSE(g_rm);

    auto sr = (*upg)->search_text("kept", 10);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 1u);

    auto sr_rm = (*upg)->search_text("removed", 10);
    ASSERT_TRUE(sr_rm);
    EXPECT_EQ(sr_rm->hits.size(), 0u);

    (*upg)->close();
}

// --- C1 延后 apply：merge 失败时 keydir 完全未动，所有 key 立即可见 ---
//
// 复现：写多个文件 → 给 merge 传一个不存在的输入路径触发 kInputOpenFailed →
// 验证：(1) merge 返回错误；(2) 失败后所有 key 仍可 get（keydir 未动）；
// (3) 值完全不变。改造前（fold 内即时 CAS），已 fold 的部分 key 会指向
// 已 cleanup 删掉的输出文件 → get() 失败，需重启 fold 才能恢复可见性。
TEST_F(CaskDocValueTest, MergeFailurePreservesKeyDirVisibility) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 256;  // 强制 roll 出多个 data 文件
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    constexpr int N = 20;
    std::map<std::vector<std::byte>, std::vector<std::byte>> expected;
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
        std::vector<std::byte> val(40, static_cast<std::byte>(i));
        ASSERT_TRUE(cask.put(key, val, static_cast<std::uint32_t>(1000 + i)));
        expected[key] = val;
    }
    ASSERT_GE(expected.size(), 20u);

    // 失败前快照：所有 key 应可见
    for (const auto& [k, v] : expected) {
        auto r = cask.get_owned(k);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->value, v);
    }

    // 收集 data 文件并构造无效 merge 输入（含不存在路径 → kInputOpenFailed）
    std::vector<std::pair<std::uint32_t, std::string>> files;
    for (const auto& de : fs::directory_iterator(tmpdir_)) {
        const auto name = de.path().filename().string();
        if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
            files.push_back({static_cast<std::uint32_t>(*t), de.path().string()});
        }
    }
    ASSERT_GE(files.size(), 2u) << "max_file_size 应已滚出多个文件";
    std::sort(files.begin(), files.end());
    std::vector<std::string> invalid_input = {
        files[0].second,
        (tmpdir_ / "9999999.bitcask.data").string(),  // 不存在
    };

    auto mr = cask.merge(invalid_input);
    EXPECT_FALSE(mr) << "含无效路径的 merge 应失败";

    // 关键验证：失败后所有 key 仍可 get，值完全不变
    for (const auto& [k, v] : expected) {
        auto r = cask.get_owned(k);
        ASSERT_TRUE(r) << "merge 失败后 key 仍应可见";
        EXPECT_EQ(r->value, v) << "值应不变";
    }

    cask.close();
}

// --- S13: fold 文件句柄快照 pin —— fold 跨越并发 merge 的 unlink 仍正确 ---
//
// 复现：小 max_file_size 滚出多个 data 文件 → 开 fold（next() 一次触发 pin）→
// 对 sealed 文件做 merge（relocate live record 到新文件 + unlink 旧文件）→
// 继续 fold 到结束。修复前 fold 会因旧文件被删、read_file 重新 open 失败而报错；
// 修复后从 pin 的 fd 读，仍能拿到全部 key 的正确 value。
TEST_F(CaskDocValueTest, FoldSurvivesConcurrentMergeUnlink) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 256;  // 强制 roll 出多个 data 文件
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    // 写 20 个不同 key（都 live），40 字节 value 撑大文件、散落到多个文件。
    constexpr int N = 20;
    std::map<std::vector<std::byte>, std::vector<std::byte>> expected;
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
        std::vector<std::byte> val(40, static_cast<std::byte>(i));
        ASSERT_TRUE(cask.put(key, val, static_cast<std::uint32_t>(1000 + i)));
        expected[key] = val;
    }

    // 收集所有 data 文件，按 file_id 排序；除最后一个（active）外都作 merge 输入。
    std::vector<std::pair<std::uint32_t, std::string>> files;
    for (const auto& de : fs::directory_iterator(tmpdir_)) {
        const auto name = de.path().filename().string();
        if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
            files.push_back({static_cast<std::uint32_t>(*t), de.path().string()});
        }
    }
    ASSERT_GE(files.size(), 2u) << "max_file_size 应已滚出多个文件";
    std::sort(files.begin(), files.end());
    std::vector<std::string> to_merge;
    for (std::size_t i = 0; i + 1 < files.size(); ++i) to_merge.push_back(files[i].second);

    // 开 fold，next() 一次 → 触发 pin_files（pin 住 sealed 文件的只读 fd）。
    auto it = cask.make_iter();
    auto sr = it->start();
    ASSERT_TRUE(sr);
    ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);
    auto first = it->next();
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->has_value());

    // fold 进行中触发 merge：relocate live record 到新文件并 unlink 掉 to_merge。
    auto mr = cask.merge(to_merge);
    ASSERT_TRUE(mr);
    for (const auto& p : to_merge) {
        EXPECT_FALSE(fs::exists(p)) << "merge 后旧文件应被 unlink：" << p;
    }

    // 继续 fold 到结束：收集所有 entry。没有 pin 的话这里会 read_file 失败报错。
    std::map<std::vector<std::byte>, std::vector<std::byte>> got;
    got[(*first)->key] = (*first)->value;
    while (true) {
        auto e = it->next();
        ASSERT_TRUE(e) << "fold next 在 merge unlink 后失败（pin 未生效？）";
        if (!e->has_value()) break;
        got[(*e)->key] = (*e)->value;
    }
    it->release();

    // fold 应原样看到全部 20 个 key 的正确 value（快照 + pin 双重保证）。
    EXPECT_EQ(got, expected);

    cask.close();
}

// S2:批量 merge 写。merge 输出走 DataFile::write_buffered（累积 1 MiB 一次
// pwrite），本测试写 > 3 MiB live 数据让 merge 输出跨阈值多次 flush——校验
// flush 边界两侧 offset 连续、merge 后每个 key 仍能按 keydir offset 读回正确
// value（offset 算错会 CRC 失败或读到错位字节）。含跨文件覆盖以验活性检查。
TEST_F(CaskDocValueTest, S2BatchedMergeManyRecordsRoundTrip) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 64 * 1024;  // 64 KiB → 滚出几十个 data 文件

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    constexpr int N = 3000;
    constexpr std::size_t kValLen = 1100;  // N*1100 ≈ 3.3 MiB live → 多次 flush
    auto make_key = [](int i) {
        return std::vector<std::byte>{static_cast<std::byte>(i & 0xFF),
                                      static_cast<std::byte>((i >> 8) & 0xFF)};
    };
    std::map<std::vector<std::byte>, std::vector<std::byte>> expected;

    std::uint32_t ts = 1000;
    // 第一轮：写入旧值（稍后覆盖，制造 stale 记录让活性检查介入）。
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> stale(kValLen, static_cast<std::byte>(0xEE));
        ASSERT_TRUE(cask.put(make_key(i), stale, ts++));
    }
    // 第二轮：覆盖成正确值（高 tstamp 必胜）。
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> val(kValLen, static_cast<std::byte>(i & 0xFF));
        val[0] = static_cast<std::byte>(i & 0xFF);
        val[1] = static_cast<std::byte>((i >> 8) & 0xFF);
        ASSERT_TRUE(cask.put(make_key(i), val, ts++));
        expected[make_key(i)] = val;
    }

    // 收集 data 文件，merge 除 active 外全部。
    std::vector<std::pair<std::uint32_t, std::string>> files;
    for (const auto& de : fs::directory_iterator(tmpdir_)) {
        if (auto t = bitcask::fileops::parse_data_tstamp(
                de.path().filename().string())) {
            files.push_back({static_cast<std::uint32_t>(*t), de.path().string()});
        }
    }
    ASSERT_GE(files.size(), 4u) << "应滚出多个文件";
    std::sort(files.begin(), files.end());
    std::vector<std::string> to_merge;
    for (std::size_t i = 0; i + 1 < files.size(); ++i) {
        to_merge.push_back(files[i].second);
    }

    auto mr = cask.merge(to_merge);
    ASSERT_TRUE(mr) << "batched merge failed";

    // merge 后逐 key 读回，校验 value 完整（offset 连续性的端到端验证）。
    for (const auto& [k, v] : expected) {
        auto gr = cask.get_owned(k);
        ASSERT_TRUE(gr) << "key missing after batched merge";
        EXPECT_EQ(gr->value, v) << "value corrupted after batched merge";
    }

    // 重开再验一次：从磁盘 fold/hint 重建 keydir 指向 merge 输出文件。
    cask.close();
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);
    for (const auto& [k, v] : expected) {
        auto gr = (*c2)->get_owned(k);
        ASSERT_TRUE(gr) << "key missing after reopen";
        EXPECT_EQ(gr->value, v) << "value corrupted after reopen";
    }
    (*c2)->close();
}

// P6:持有 mmap 命中的 GetResultView 期间,并发 merge unlink 掉该 view 映射的
// sealed 文件 → view 照常读、无 UAF/SIGBUS（map_holder_ shared_ptr 锚定映射,
// 即便文件被 unlink + 从 read_files_ 淘汰）。释放后映射随最后引用 munmap。
TEST_F(CaskDocValueTest, P6MmapViewSurvivesMergeUnlink) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    opts.max_file_size = 256;  // 滚出多个 sealed 文件
    constexpr int N = 20;
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < N; ++i) {
            std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
            std::vector<std::byte> val(40, static_cast<std::byte>(i));
            ASSERT_TRUE((*c)->put(key, val, static_cast<std::uint32_t>(1000 + i)));
        }
        (*c)->close();
    }
    // reopen:此后所有 data 文件均 sealed,get 经 read_file 按需 mmap。
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    std::vector<std::pair<std::uint32_t, std::string>> files;
    for (const auto& de : fs::directory_iterator(tmpdir_)) {
        if (auto t = bitcask::fileops::parse_data_tstamp(
                de.path().filename().string())) {
            files.push_back({static_cast<std::uint32_t>(*t), de.path().string()});
        }
    }
    ASSERT_GE(files.size(), 2u);

    // 持有 k0 的 view(mmap 命中其 sealed 文件)。
    std::vector<std::byte> k0{std::byte{'k'}, static_cast<std::byte>(0)};
    auto view = cask.get(k0);
    ASSERT_TRUE(view);
    const std::vector<std::byte> before(view->value.begin(), view->value.end());
    ASSERT_FALSE(before.empty());

    // merge 掉全部 sealed 文件 → unlink(含 k0 所在文件)+ 从 read_files_ 淘汰。
    std::vector<std::string> to_merge;
    for (const auto& f : files) to_merge.push_back(f.second);
    auto mr = cask.merge(to_merge);
    ASSERT_TRUE(mr);
    for (const auto& p : to_merge) {
        EXPECT_FALSE(fs::exists(p)) << "merge 后旧文件应被 unlink:" << p;
    }

    // 关键:持有的 view 仍可读(映射经 shared_ptr 续命,无 UAF/SIGBUS)。
    const std::vector<std::byte> after(view->value.begin(), view->value.end());
    EXPECT_EQ(after, before);

    // merge 后新 get 从重定位的新文件读,值一致。
    auto v2 = cask.get(k0);
    ASSERT_TRUE(v2);
    const std::vector<std::byte> fresh(v2->value.begin(), v2->value.end());
    EXPECT_EQ(fresh, before);

    cask.close();
}

// --- #1: FieldSchema 注册表 ---

// intern 确定性：同名同 id、新名递增；name_of 反查。
TEST(FieldSchema, InternDeterministicAndReverseLookup) {
    namespace fs = std::filesystem;
    auto path = (fs::temp_directory_path() / "bitcask_fieldschema_a.schema").string();
    fs::remove(path);

    bitcask::FieldSchema s;
    ASSERT_TRUE(s.open(path));

    auto title = s.intern("title");
    auto body  = s.intern("body");
    EXPECT_EQ(title, 0u);
    EXPECT_EQ(body, 1u);
    EXPECT_EQ(s.intern("title"), title);  // 幂等
    EXPECT_EQ(s.intern("body"), body);
    EXPECT_EQ(s.size(), 2u);

    EXPECT_EQ(s.name_of(0u), std::optional<std::string>("title"));
    EXPECT_EQ(s.name_of(1u), std::optional<std::string>("body"));
    EXPECT_EQ(s.name_of(99u), std::nullopt);

    fs::remove(path);
}

// 持久化：重开后 name↔id 映射不变（append-only 重放）。
TEST(FieldSchema, PersistsAcrossReopen) {
    namespace fs = std::filesystem;
    auto path = (fs::temp_directory_path() / "bitcask_fieldschema_b.schema").string();
    fs::remove(path);

    {
        bitcask::FieldSchema s;
        ASSERT_TRUE(s.open(path));
        EXPECT_EQ(s.intern("alpha"), 0u);
        EXPECT_EQ(s.intern("beta"), 1u);
        EXPECT_EQ(s.intern("gamma"), 2u);
    }
    {
        bitcask::FieldSchema s2;
        ASSERT_TRUE(s2.open(path));
        EXPECT_EQ(s2.size(), 3u);
        EXPECT_EQ(s2.intern("alpha"), 0u);   // 旧名字 id 不变
        EXPECT_EQ(s2.intern("beta"), 1u);
        EXPECT_EQ(s2.intern("delta"), 3u);   // 新名字接着分配
        EXPECT_EQ(s2.name_of(2u), std::optional<std::string>("gamma"));
    }
    fs::remove(path);
}

}  // namespace

// ── A4:keydir 段快照 + 尾部回放(doc/recovery-snapshot-design-zh.md)──────

namespace {
// S24 补（ASan 实证）：收 string_view——const string& 版对字面量会生成
// 临时 string，`doc.text = sv_bytes("...")` 存 span 后临时即亡（悬垂）。
// string_view 直接绑定字面量（静态存储）/具名缓冲，杜绝该类。注意仍不可
// 对 sv_bytes(std::string(...)) 的**存储**用法——span 源必须比使用点长寿。
std::span<const std::byte> sv_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
}  // namespace

// 快照路径 reopen 与全量 fold reopen 等价(键集/值/删除一致)。
TEST_F(CaskDocValueTest, KeydirSnapshotRoundTripEquivalence) {
    CaskOptions opts;
    opts.read_write = true;
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 500; ++i) {
            const std::string k = "k" + std::to_string(i);
            const std::string v = "v" + std::to_string(i * 7);
            ASSERT_TRUE((*c)->put(sv_bytes(k), sv_bytes(v)));
        }
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE((*c)->remove(sv_bytes("k" + std::to_string(i * 5))));
        }
        (*c)->close();
    }
    const auto snap = tmpdir_ / "kv.keydir.ckpt";
    ASSERT_TRUE(std::filesystem::exists(snap));

    auto collect = [&]() {
        std::map<std::string, std::string> m;
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        EXPECT_TRUE(c);
        for (int i = 0; i < 500; ++i) {
            const std::string k = "k" + std::to_string(i);
            auto g = (*c)->get_owned(sv_bytes(k));
            if (g) {
                m[k] = std::string(
                    reinterpret_cast<const char*>(g->value.data()),
                    g->value.size());
            }
        }
        (*c)->close();
        return m;
    };

    auto with_snap = collect();          // 快照快路径
    std::filesystem::remove(snap);
    auto full_fold = collect();          // 全量 fold(close 会重写快照)
    EXPECT_EQ(with_snap.size(), 400u);
    EXPECT_EQ(with_snap, full_fold);
}

// 陈旧快照:会话 2 的写/删必须经尾部回放可见。
TEST_F(CaskDocValueTest, KeydirSnapshotStaleTailReplay) {
    CaskOptions opts;
    opts.read_write = true;
    const auto snap = tmpdir_ / "kv.keydir.ckpt";
    const auto snap_old = tmpdir_ / "snap.old";

    {   // 会话 1
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 200; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("a" + std::to_string(i)),
                                  sv_bytes("old")));
        }
        (*c)->close();
    }
    std::filesystem::copy_file(snap, snap_old);

    {   // 会话 2:新增 + 覆写 + 删除
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("b" + std::to_string(i)),
                                  sv_bytes("new")));
        }
        ASSERT_TRUE((*c)->put(sv_bytes("a0"), sv_bytes("updated")));
        ASSERT_TRUE((*c)->remove(sv_bytes("a1")));
        (*c)->close();
    }
    // 用会话 1 的旧快照覆盖 → 模拟"快照落后于数据文件"(崩溃形态)。
    std::filesystem::copy_file(snap_old, snap,
        std::filesystem::copy_options::overwrite_existing);

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto g = (*c)->get_owned(sv_bytes(std::string("b42")));
    ASSERT_TRUE(g);  // 会话 2 新键:尾部回放恢复
    auto g2 = (*c)->get_owned(sv_bytes(std::string("a0")));
    ASSERT_TRUE(g2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(g2->value.data()),
                          g2->value.size()),
              "updated");
    EXPECT_FALSE((*c)->get_owned(sv_bytes(std::string("a1"))));  // 尾部墓碑生效
    (*c)->close();
}

// 损坏快照(位翻转/截断)→ 回退全量 fold,数据完好。
TEST_F(CaskDocValueTest, KeydirSnapshotCorruptFallsBackToFullFold) {
    CaskOptions opts;
    opts.read_write = true;
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 300; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("k" + std::to_string(i)),
                                  sv_bytes("v" + std::to_string(i))));
        }
        (*c)->close();
    }
    const auto snap = tmpdir_ / "kv.keydir.ckpt";

    // 位翻转 payload 中部。
    {
        std::FILE* f = std::fopen(snap.string().c_str(), "rb+");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 0, SEEK_END);
        const long mid = std::ftell(f) / 2;
        std::fseek(f, mid, SEEK_SET);
        int ch = std::fgetc(f);
        std::fseek(f, mid, SEEK_SET);
        std::fputc(ch ^ 0xFF, f);
        std::fclose(f);
    }
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 300; ++i) {
            EXPECT_TRUE((*c)->get_owned(sv_bytes("k" + std::to_string(i)))) << i;
        }
        (*c)->close();  // 重写好快照
    }
    // 截断注入。
    std::filesystem::resize_file(snap, std::filesystem::file_size(snap) / 3);
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 300; ++i) {
            EXPECT_TRUE((*c)->get_owned(sv_bytes("k" + std::to_string(i)))) << i;
        }
        (*c)->close();
    }
}

// ── A4-P3:search 模式快照快路径(Index sidecar 齐备后开门)──────────────

namespace {
CaskOptions p3_search_opts() {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = cfg;
    return opts;
}
}  // namespace

// 干净 close → reopen:三块快照齐备走快路径,搜索/读取完整。
TEST_F(CaskDocValueTest, SearchSnapshotFastReopen) {
    auto opts = p3_search_opts();
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 200; ++i) {
            const std::string k = "k" + std::to_string(i);
            const std::string v = "apple banana doc" + std::to_string(i);
            ASSERT_TRUE((*c)->put(sv_bytes(k), sv_bytes(v)));
        }
        (*c)->flush_index();
        ASSERT_TRUE((*c)->remove(sv_bytes(std::string("k7"))));
        (*c)->close();
    }
    ASSERT_TRUE(std::filesystem::exists(tmpdir_ / "bm25.ckpt"));

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto sr = (*c)->search_text("banana", 300);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 199u);  // k7 已删,live 经 sidecar 恢复
    auto g = (*c)->get_owned(sv_bytes(std::string("k42")));
    ASSERT_TRUE(g);
    EXPECT_FALSE((*c)->get_owned(sv_bytes(std::string("k7"))));
    (*c)->close();
}

// 陈旧 keydir 快照:尾部回放在 search 模式下同样生效(门按覆盖判定放行)。
TEST_F(CaskDocValueTest, SearchSnapshotStaleKeydirTailReplay) {
    auto opts = p3_search_opts();
    const auto snap = tmpdir_ / "kv.keydir.ckpt";
    const auto snap_old = tmpdir_ / "kd.old";
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("a" + std::to_string(i)),
                                  sv_bytes(std::string("alpha text"))));
        }
        (*c)->close();
    }
    std::filesystem::copy_file(snap, snap_old);
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 50; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("b" + std::to_string(i)),
                                  sv_bytes(std::string("beta text"))));
        }
        (*c)->close();
    }
    // keydir 快照回退到会话 1;bm25/sidecar 保持会话 2(覆盖更大 → 门过)。
    std::filesystem::copy_file(snap_old, snap,
        std::filesystem::copy_options::overwrite_existing);

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    EXPECT_TRUE((*c)->get_owned(sv_bytes(std::string("b25"))));   // 尾部回放
    auto sr = (*c)->search_text("beta", 100);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 50u);
    (*c)->close();
}

// sidecar 损坏 → 门关,全量 fold 兜底,数据与搜索完好。
TEST_F(CaskDocValueTest, SearchSnapshotCorruptSidecarFallsBack) {
    auto opts = p3_search_opts();
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 120; ++i) {
            ASSERT_TRUE((*c)->put(sv_bytes("k" + std::to_string(i)),
                                  sv_bytes(std::string("gamma text"))));
        }
        (*c)->close();
    }
    const auto sc = tmpdir_ / "bm25.ckpt";
    std::filesystem::resize_file(sc, std::filesystem::file_size(sc) / 2);

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto sr = (*c)->search_text("gamma", 200);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 120u);
    EXPECT_TRUE((*c)->get_owned(sv_bytes(std::string("k99"))));
    (*c)->close();
}

// ── V3.1:DocValue vector 段 + meta VectorConfig 打通 ────────────────────

namespace {
CaskOptions v31_opts(std::uint16_t dim) {
    auto opts = p3_search_opts();
    opts.vector_dim = dim;
    return opts;
}
}  // namespace

// 回归：Jieba 经工厂(AnalyzerFactory::create)创建的索引路径必须可用。
// 历史 bug：Jieba 自注册在独立 TU(jieba_analyzer.cpp)，bitcask_text 是
// STATIC 库且无外部符号引用该 TU → 链接器丢弃 → create(Jieba) 返回 nullptr
// → SearchLayer::analyzer_ 为空 → 首次带 text 的 put 解空指针段错误。
// 注册移到工厂同 TU 后修复；此测试经 Cask→IndexPool→SearchLayer 全路径守护。
#ifndef BITCASK_JIEBA_DICT_DIR
#define BITCASK_JIEBA_DICT_DIR "_deps/cppjieba-src/dict"
#endif
TEST_F(CaskDocValueTest, JiebaIndexModePutDoesNotCrash) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Jieba;
    cfg.analyzer_config.dict_path = BITCASK_JIEBA_DICT_DIR;
    opts.search_config = cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);   // analyzer 为空时 open 现在干净失败，不再带病打开
    const std::string text =
        "BM25 全文检索 — 用 {analyzer, ...} 打开即可启用。\n"
        "每次 put 自动索引；返回 {ok, [{Key, Ord, Score}, ...]}，按分数降序：";
    bitcask::DocInput doc;
    doc.text = sv_bytes(text);
    std::vector<std::byte> key{std::byte{'d'}, std::byte{'1'}};
    ASSERT_TRUE((*c)->put_doc(key, doc, 1000));
    (*c)->close();   // flush IndexPool worker（曾在此路径段错误）
    SUCCEED();
}

// P4：单写者组提交（{sync_strategy,{puts,N}} = sync_every_n）。写入在组提交
// 模式下不丢、不坏，close 的 force-flush 落最后一批；重开全部可读。
TEST_F(CaskDocValueTest, P4GroupCommitWritesSurviveReopen) {
    CaskOptions opts;
    opts.read_write = true;
    opts.sync_every_n = 3;  // 每 3 次写 fsync 一次
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < 10; ++i) {  // 10 % 3 = 1 条留给 close force-flush
            std::vector<std::byte> k{std::byte{'k'}, static_cast<std::byte>('0' + i)};
            std::vector<std::byte> v{std::byte{'v'}, static_cast<std::byte>('0' + i)};
            ASSERT_TRUE((*c)->put(k, v, 1000 + i));
        }
        (*c)->close();
    }
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 10; ++i) {
        std::vector<std::byte> k{std::byte{'k'}, static_cast<std::byte>('0' + i)};
        auto g = (*c)->get_owned(k);
        ASSERT_TRUE(g) << "missing key " << i;
        ASSERT_EQ(g->value.size(), 2u);
    }
    (*c)->close();
}

// 写入归一化 + get 透传 + 重开持久(cosine_normalized 默认度量)。
TEST_F(CaskDocValueTest, V31VectorRoundTripNormalized) {
    auto opts = v31_opts(4);
    const float raw[4] = {3.0f, 4.0f, 0.0f, 0.0f};   // 模长 5
    std::vector<std::byte> key{std::byte{'k'}};
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        bitcask::DocInput doc;
        const std::string text = "hello vec";
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(raw, 4);
        ASSERT_TRUE((*c)->put_doc(key, doc, 1000));

        auto g = (*c)->get_owned(key);
        ASSERT_TRUE(g);
        ASSERT_EQ(g->vector.size(), 4u);
        EXPECT_FLOAT_EQ(g->vector[0], 0.6f);
        EXPECT_FLOAT_EQ(g->vector[1], 0.8f);
        (*c)->close();
    }
    // 重开(快照路径)后向量仍在(data file 为 source of truth)。
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto g = (*c)->get_owned(key);
    ASSERT_TRUE(g);
    ASSERT_EQ(g->vector.size(), 4u);
    EXPECT_FLOAT_EQ(g->vector[0], 0.6f);
    EXPECT_FLOAT_EQ(g->vector[1], 0.8f);
    (*c)->close();
}

// P3b：{vector_quantized} 端到端——put 落盘 int8，get dequant 近似还原，
// 重开一致 ok / 不一致 mode_mismatch。
TEST_F(CaskDocValueTest, P3bQuantizedVectorRoundTripAndReopen) {
    auto opts = v31_opts(4);
    opts.vector_quantized = true;
    const float raw[4] = {3.0f, 4.0f, 0.0f, 0.0f};  // 模长5 → cosine 归一化 0.6,0.8,0,0
    std::vector<std::byte> key{std::byte{'k'}};
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        bitcask::DocInput doc;
        const std::string text = "hello vec";
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(raw, 4);
        ASSERT_TRUE((*c)->put_doc(key, doc, 1000));
        auto g = (*c)->get_owned(key);
        ASSERT_TRUE(g);
        ASSERT_EQ(g->vector.size(), 4u);
        EXPECT_NEAR(g->vector[0], 0.6f, 0.02f);  // int8 误差 ≤ 一个量化步长
        EXPECT_NEAR(g->vector[1], 0.8f, 0.02f);
        (*c)->close();
    }
    {  // 重开（quantized 一致）→ 向量仍可读
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto g = (*c)->get_owned(key);
        ASSERT_TRUE(g);
        ASSERT_EQ(g->vector.size(), 4u);
        EXPECT_NEAR(g->vector[1], 0.8f, 0.02f);
        (*c)->close();
    }
    // 重开但 vector_quantized 不一致 → mode_mismatch
    auto cbad = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());  // quantized=false
    ASSERT_FALSE(cbad);
    EXPECT_EQ(cbad.error().kind, bitcask::CaskError::kModeMismatch);
}

// P5b：{vector_inmem_int8} 端到端——开库建 int8-only HNSW，search_vector 命中
// 最近者；meta 持久化（offset[10]）；重开一致 ok / 不一致 mode_mismatch；
// kL2 拒绝；与 P3 落盘 int8 正交可组合。
TEST_F(CaskDocValueTest, P5bInmemInt8OpenSearchAndReopen) {
    auto opts = v31_opts(4);
    opts.vector_inmem_int8 = true;
    const std::string k1 = "k1", k2 = "k2", k3 = "k3";
    const float v1[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float v2[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    const float v3[4] = {0.7f, 0.7f, 0.0f, 0.0f};
    const float q[4]  = {2.0f, 0.0f, 0.0f, 0.0f};
    auto put_vec = [&](Cask& c, const std::string& key, const float* v) {
        bitcask::DocInput doc;
        const std::string text = "doc " + key;
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(v, 4);
        ASSERT_TRUE(c.put_doc(sv_bytes(key), doc, 1000));
    };
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        put_vec(**c, k1, v1);
        put_vec(**c, k2, v2);
        put_vec(**c, k3, v3);
        (*c)->flush_index();
        auto r = (*c)->search_vector(std::span<const float>(q, 4), 3);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 3u);
        EXPECT_EQ(r->hits[0].key, k1);  // 轴对齐向量量化无损 → cos=1.0 仍最近
        (*c)->close();
    }
    // meta 持久化:offset[10] == 1。
    {
        auto mc = bitcask::meta::read_meta(tmpdir_.string());
        ASSERT_TRUE(mc);
        EXPECT_TRUE(mc->vector_inmem_int8);
    }
    // 重开(inmem_int8 一致)→ 全量 fold 重建 int8-only 图,search 仍命中。
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto r = (*c)->search_vector(std::span<const float>(q, 4), 3);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 3u);
        EXPECT_EQ(r->hits[0].key, k1);
        (*c)->close();
    }
    // 重开但 inmem_int8 不一致(默认 false)→ mode_mismatch。
    auto cbad = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
    ASSERT_FALSE(cbad);
    EXPECT_EQ(cbad.error().kind, bitcask::CaskError::kModeMismatch);
}

// P5b:int8-only + kL2 → 干净拒绝(int8 距离仅 kDot)。
TEST_F(CaskDocValueTest, P5bInmemInt8RejectsL2) {
    auto opts = v31_opts(4);
    opts.vector_inmem_int8 = true;
    opts.vector_metric = bitcask::meta::VectorMetric::kL2;
    auto tmp = tmpdir_ / "i8l2";
    std::filesystem::create_directories(tmp);
    auto c = Cask::open(tmp.string(), opts, &test_registry());
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().kind, bitcask::CaskError::kInvalidOption);
}

// P5b:与 P3 落盘 int8 正交——盘 int8 + 内存 int8 同开,put/search 正常。
TEST_F(CaskDocValueTest, P5bInmemInt8ComposesWithQuantized) {
    auto opts = v31_opts(4);
    opts.vector_inmem_int8 = true;
    opts.vector_quantized = true;
    const float v1[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float q[4]  = {2.0f, 0.0f, 0.0f, 0.0f};
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    bitcask::DocInput doc;
    const std::string text = "doc k1";
    doc.text = sv_bytes(text);
    doc.vector = std::span<const float>(v1, 4);
    ASSERT_TRUE((*c)->put_doc(sv_bytes("k1"), doc, 1000));
    (*c)->flush_index();
    auto r = (*c)->search_vector(std::span<const float>(q, 4), 1);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->hits.size(), 1u);
    EXPECT_EQ(r->hits[0].key, "k1");
    (*c)->close();
}

// S32-M0:vector_engine 接线——meta 往返 + 旧全零解码 kHnsw + 未实现引擎
// open 干净拒绝 + 引擎不符 mode_mismatch。
TEST_F(CaskDocValueTest, S32M0VectorEngineMetaWiring) {
    // meta 往返：kIvfRq 写读保真（纯 meta 层，不经 Cask）。
    {
        auto tmp = tmpdir_ / "eng_rt";
        std::filesystem::create_directories(tmp);
        bitcask::meta::MetaConfig cfg;
        cfg.mode = bitcask::meta::Mode::kIndex;
        cfg.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
        cfg.vector_dim = 4;
        cfg.vector_engine = bitcask::meta::VectorEngine::kIvfRq;
        ASSERT_TRUE(bitcask::meta::write_meta(tmp.string(), cfg));
        auto rd = bitcask::meta::read_meta(tmp.string());
        ASSERT_TRUE(rd);
        EXPECT_EQ(rd->vector_engine, bitcask::meta::VectorEngine::kIvfRq);
    }
    // 建库默认引擎 → meta 持久化 kHnsw（保留字节零语义）。
    {
        auto c = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
        ASSERT_TRUE(c);
        (*c)->close();
        auto mc = bitcask::meta::read_meta(tmpdir_.string());
        ASSERT_TRUE(mc);
        EXPECT_EQ(mc->vector_engine, bitcask::meta::VectorEngine::kHnsw);
    }
    // 未实现引擎（kDiskann，S32-M5）：open 干净拒绝。kIvfRq 已落地
    // （S32-M3）→ 建库成功且 meta 持久化引擎标识。
    {
        auto tmp = tmpdir_ / "eng_dann";
        std::filesystem::create_directories(tmp);
        auto opts = v31_opts(4);
        opts.vector_engine = bitcask::meta::VectorEngine::kDiskann;
        auto c = Cask::open(tmp.string(), opts, &test_registry());
        ASSERT_FALSE(c);
        EXPECT_EQ(c.error().kind, bitcask::CaskError::kInvalidOption);
    }
    {
        auto tmp = tmpdir_ / "eng_ivf";
        std::filesystem::create_directories(tmp);
        auto opts = v31_opts(4);
        opts.vector_engine = bitcask::meta::VectorEngine::kIvfRq;
        auto c = Cask::open(tmp.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        (*c)->close();
        auto mc = bitcask::meta::read_meta(tmp.string());
        ASSERT_TRUE(mc);
        EXPECT_EQ(mc->vector_engine, bitcask::meta::VectorEngine::kIvfRq);
    }
    // 引擎不符：meta 声明 kIvfRq（模拟未来引擎写的库）、opts 默认 kHnsw
    // → kModeMismatch（绝不按 HNSW 静默误开别家引擎的库）。
    {
        auto tmp = tmpdir_ / "eng_mm";
        std::filesystem::create_directories(tmp);
        bitcask::meta::MetaConfig cfg;
        cfg.mode = bitcask::meta::Mode::kIndex;
        cfg.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
        cfg.vector_dim = 4;
        cfg.vector_engine = bitcask::meta::VectorEngine::kIvfRq;
        ASSERT_TRUE(bitcask::meta::write_meta(tmp.string(), cfg));
        auto c = Cask::open(tmp.string(), v31_opts(4), &test_registry());
        ASSERT_FALSE(c);
        EXPECT_EQ(c.error().kind, bitcask::CaskError::kModeMismatch);
    }
    // 未知引擎值：read_meta fail-fast（新读端才认识的值绝不静默过）。
    {
        auto tmp = tmpdir_ / "eng_bad";
        std::filesystem::create_directories(tmp);
        bitcask::meta::MetaConfig cfg;
        cfg.mode = bitcask::meta::Mode::kIndex;
        cfg.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
        cfg.vector_dim = 4;
        ASSERT_TRUE(bitcask::meta::write_meta(tmp.string(), cfg));
        const auto path = (tmp / "bitcask.meta").string();
        // 手改 offset[11] 为未知值 + 重算 CRC（CRC 覆盖 [0,14)）。
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        unsigned char hdr[18];
        ASSERT_EQ(std::fread(hdr, 1, sizeof(hdr), f), sizeof(hdr));
        hdr[11] = 99;
        const std::uint32_t crc = bitcask::hw::crc32(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(hdr), 14));
        std::memcpy(hdr + 14, &crc, 4);
        std::rewind(f);
        ASSERT_EQ(std::fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
        std::fclose(f);
        auto rd = bitcask::meta::read_meta(tmp.string());
        EXPECT_FALSE(rd) << "未知 vector_engine 必须 fail-fast";
    }
}

// S32-M3:IVF 引擎 Cask 全链路 e2e——建库(engine=kIvfRq)→ 写查 → 删除 →
// 重开(meta 引擎校验 + 组件链恢复)→ 检索等价。
TEST_F(CaskDocValueTest, S32M3IvfEngineEndToEnd) {
    auto opts = v31_opts(4);
    opts.vector_engine = bitcask::meta::VectorEngine::kIvfRq;
    const std::size_t kN = 24;
    auto vec_of = [](std::size_t i) {
        // 三簇轴对齐向量 + 小扰动（cosine 归一化由写入端做）。
        std::vector<float> v(4, 0.05f);
        v[i % 3] = 1.0f;
        v[3] = 0.01f * static_cast<float>(i);
        return v;
    };
    auto put_all = [&](Cask& c, std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            bitcask::DocInput doc;
            const std::string text = "ivf doc " + std::to_string(i);
            doc.text = sv_bytes(text);
            auto v = vec_of(i);
            doc.vector = std::span<const float>(v.data(), 4);
            ASSERT_TRUE(c.put_doc(sv_bytes("k" + std::to_string(i)), doc,
                                  1000));
        }
    };
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        put_all(**c, 0, kN);
        (*c)->flush_index();
        EXPECT_EQ((*c)->vector_plugin()->size(), kN);  // 探针:插入到达
        auto r = (*c)->search_vector(
            std::span<const float>(vec_of(5).data(), 4), 3);
        ASSERT_TRUE(r);
        ASSERT_GE(r->hits.size(), 1u);
        EXPECT_EQ(r->hits[0].key, "k5");  // 自匹配 top1
        // 删除后查询侧立即滤除。
        ASSERT_TRUE((*c)->remove(sv_bytes("k5"), 2000));
        (*c)->flush_index();
        auto r2 = (*c)->search_vector(
            std::span<const float>(vec_of(5).data(), 4), kN);
        ASSERT_TRUE(r2);
        for (const auto& h : r2->hits) EXPECT_NE(h.key, "k5");
        (*c)->close();
    }
    // meta 持久化引擎标识。
    {
        auto mc = bitcask::meta::read_meta(tmpdir_.string());
        ASSERT_TRUE(mc);
        EXPECT_EQ(mc->vector_engine, bitcask::meta::VectorEngine::kIvfRq);
    }
    // 以 HNSW 默认重开 → 引擎不符拒绝。
    {
        auto bad = Cask::open(tmpdir_.string(), v31_opts(4),
                              &test_registry());
        ASSERT_FALSE(bad);
        EXPECT_EQ(bad.error().kind, bitcask::CaskError::kModeMismatch);
    }
    // 正确引擎重开：组件恢复 + 检索等价 + 续写。
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto r = (*c)->search_vector(
            std::span<const float>(vec_of(7).data(), 4), 3);
        ASSERT_TRUE(r);
        ASSERT_GE(r->hits.size(), 1u);
        EXPECT_EQ(r->hits[0].key, "k7");
        auto rd = (*c)->search_vector(
            std::span<const float>(vec_of(5).data(), 4), kN);
        ASSERT_TRUE(rd);
        for (const auto& h : rd->hits) EXPECT_NE(h.key, "k5");  // 删除持久
        put_all(**c, kN, kN + 4);  // 续写进窗口
        (*c)->flush_index();
        auto r3 = (*c)->search_vector(
            std::span<const float>(vec_of(kN + 2).data(), 4), 3);
        ASSERT_TRUE(r3);
        ASSERT_GE(r3->hits.size(), 1u);
        EXPECT_EQ(r3->hits[0].key, "k" + std::to_string(kN + 2));
        (*c)->close();
    }
}

// LE flag-day 护栏:旧 v1(大端 legacy)meta 在 open 时被干净拒绝,而非静默把
// 大端字节读成小端 → 全 record CRC 失败 → 恢复成空库。bump kMetaVersion=2。
TEST_F(CaskDocValueTest, LegacyV1MetaRejectedCleanly) {
    // 手写一个 v1 meta:magic "BCME" + version=1 + mode=0(KV) + 余 12 字节零。
    unsigned char hdr[18] = {0};
    hdr[0] = 'B'; hdr[1] = 'C'; hdr[2] = 'M'; hdr[3] = 'E';
    hdr[4] = 1;  // version 1 = 大端 legacy 纪元
    hdr[5] = 0;  // mode = KV
    const auto path = (tmpdir_ / "bitcask.meta").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
    std::fclose(f);

    // read_meta 直接拒绝(version 不匹配 → 报错,不返回半读配置)。
    auto mc = bitcask::meta::read_meta(tmpdir_.string());
    EXPECT_FALSE(mc) << "v1 大端 legacy meta 必须被拒绝";

    // 经 Cask::open 也应失败(不静默读坏)。
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    EXPECT_FALSE(c) << "旧大端目录 open 必须失败,提示重建";
}

// S12：bitcask.meta v3 加 CRC32——往返保真 + 覆盖区被篡改则 fail-fast。
TEST_F(CaskDocValueTest, MetaV3CrcRoundTripAndCorruption) {
    bitcask::meta::MetaConfig cfg;
    cfg.mode = bitcask::meta::Mode::kIndex;
    cfg.vector_metric = bitcask::meta::VectorMetric::kL2;
    cfg.vector_dim = 8;
    ASSERT_TRUE(bitcask::meta::write_meta(tmpdir_.string(), cfg));

    auto rd = bitcask::meta::read_meta(tmpdir_.string());
    ASSERT_TRUE(rd);
    EXPECT_EQ(rd->mode, bitcask::meta::Mode::kIndex);
    EXPECT_EQ(rd->vector_dim, 8);
    EXPECT_EQ(rd->vector_metric, bitcask::meta::VectorMetric::kL2);

    // 篡改覆盖区内一字节（偏移 7 = vector dim 低字节）→ CRC 失配 → 拒绝。
    const auto path = (tmpdir_ / "bitcask.meta").string();
    std::FILE* mf = std::fopen(path.c_str(), "r+b");
    ASSERT_NE(mf, nullptr);
    std::fseek(mf, 7, SEEK_SET);
    int b = std::fgetc(mf);
    std::fseek(mf, 7, SEEK_SET);
    std::fputc(b ^ 0xFF, mf);
    std::fclose(mf);

    EXPECT_FALSE(bitcask::meta::read_meta(tmpdir_.string()))
        << "meta 覆盖区被篡改 → CRC 失配必须拒绝";
}

// v2（无 CRC 字段）向后兼容读取——旧库不破坏。
TEST_F(CaskDocValueTest, MetaV2BackwardCompatRead) {
    unsigned char hdr[18] = {0};
    hdr[0] = 'B'; hdr[1] = 'C'; hdr[2] = 'M'; hdr[3] = 'E';
    hdr[4] = 2;  // version 2 = 旧格式（无 CRC），保留区全零
    hdr[5] = 1;  // mode = Index（向量配置全零 = kNone/dim0，一致）
    const auto path = (tmpdir_ / "bitcask.meta").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(hdr, 1, sizeof(hdr), f), sizeof(hdr));
    std::fclose(f);

    auto rd = bitcask::meta::read_meta(tmpdir_.string());
    ASSERT_TRUE(rd) << "v2 无 CRC meta 必须向后兼容读取";
    EXPECT_EQ(rd->mode, bitcask::meta::Mode::kIndex);
}

// P9:read 句柄缓存上限——读 > cap 个文件后常驻句柄数 ≤ cap;淘汰后再读正确;
// cap=0 不限。
TEST_F(CaskDocValueTest, P9ReadHandleCapEvictsAndRereads) {
    constexpr int N = 16;
    {   // 小 max_file_size 滚出 ~N 个 sealed data 文件。
        CaskOptions w;
        w.read_write = true;
        w.max_file_size = 64;
        auto c = Cask::open(tmpdir_.string(), w, &test_registry());
        ASSERT_TRUE(c);
        for (int i = 0; i < N; ++i) {
            std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
            std::vector<std::byte> val(40, static_cast<std::byte>(i));
            ASSERT_TRUE((*c)->put(key, val, static_cast<std::uint32_t>(1000 + i)));
        }
        (*c)->close();
    }

    // 重开,cap=4。get_owned 逐个读(view 即取即弃 → 句柄空闲可淘汰)。
    CaskOptions r;
    r.read_write = false;
    r.max_read_handles = 4;
    auto c = Cask::open(tmpdir_.string(), r, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
        auto g = (*c)->get_owned(key);
        ASSERT_TRUE(g);
        ASSERT_EQ(g->value.size(), 40u);
        EXPECT_EQ(static_cast<unsigned char>(g->value[0]), static_cast<unsigned>(i & 0xFF));
        // 常驻句柄始终不超过 cap(软上限;空闲句柄被近似 LRU 淘汰)。
        EXPECT_LE((*c)->read_handle_count(), 4u);
    }
    // 淘汰后再读最早写的 key(其句柄早被淘汰)→ 重开仍正确。
    {
        std::vector<std::byte> k0{std::byte{'k'}, static_cast<std::byte>(0)};
        auto g = (*c)->get_owned(k0);
        ASSERT_TRUE(g);
        EXPECT_EQ(static_cast<unsigned char>(g->value[0]), 0u);
    }
    (*c)->close();

    // 显式不限:读完所有文件后常驻句柄数 = 文件数(> 4)。
    // （S12-1 后默认 0 = 自动上限，须用哨兵显式请求不限。）
    CaskOptions u;
    u.read_write = false;
    u.max_read_handles = CaskOptions::kUnlimitedReadHandles;
    auto c2 = Cask::open(tmpdir_.string(), u, &test_registry());
    ASSERT_TRUE(c2);
    for (int i = 0; i < N; ++i) {
        std::vector<std::byte> key{std::byte{'k'}, static_cast<std::byte>(i)};
        ASSERT_TRUE((*c2)->get_owned(key));
    }
    EXPECT_GT((*c2)->read_handle_count(), 4u);
    (*c2)->close();
}

// S12-1：read 句柄上限解析（纯函数，确定性）。
TEST(ReadHandleCap, ResolveSemantics) {
    using bitcask::Cask;
    using bitcask::CaskOptions;
    // 显式不限哨兵 → 0（evict 语义下不限），与 nofile 无关。
    EXPECT_EQ(Cask::resolve_read_handle_cap(CaskOptions::kUnlimitedReadHandles, 1024u), 0u);
    EXPECT_EQ(Cask::resolve_read_handle_cap(CaskOptions::kUnlimitedReadHandles, 16u), 0u);
    // 显式上限 → 原样。
    EXPECT_EQ(Cask::resolve_read_handle_cap(7u, 1024u), 7u);
    EXPECT_EQ(Cask::resolve_read_handle_cap(4096u, 1024u), 4096u);
    // 自动（0）→ 约一半，下限 64。
    EXPECT_EQ(Cask::resolve_read_handle_cap(0u, 1024u), 512u);
    EXPECT_EQ(Cask::resolve_read_handle_cap(0u, 2048u), 1024u);
    EXPECT_EQ(Cask::resolve_read_handle_cap(0u, 100u), 64u);   // 50 < 64 → 抬到下限
    EXPECT_EQ(Cask::resolve_read_handle_cap(0u, 0u), 64u);     // 极端：仍给下限
}

// S12-2：auto-compact 开启下，并发读者 + churn 写者经异步管线（reducer 线程内触发
// compact）无 data race、结果最终一致、posting 有界。TSan 下为主要护栏。
TEST_F(CaskDocValueTest, AutoCompactConcurrentReadersNoRace) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.auto_compact_dead_ratio = 0.5;  // 开
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    constexpr int K = 16;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto sr = (*c)->search_text("hello", 50);  // 只求不崩不 race
                (void)sr;
            }
        });
    }

    std::uint32_t ts = 1000;
    for (int round = 0; round < 200; ++round) {   // 200*16=3200 次写，≫ 阈值 → 多次触发
        for (int i = 0; i < K; ++i) {
            bitcask::DocInput doc;
            const std::string text = "hello world";
            doc.text = sv_bytes(text);
            ASSERT_TRUE((*c)->put_doc(sv_bytes("k" + std::to_string(i)), doc, ts++));
        }
    }
    stop.store(true);
    for (auto& th : readers) th.join();

    (*c)->flush_index();
    auto sr = (*c)->search_text("hello", 50);
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), static_cast<std::size_t>(K));   // 最终恰 K 篇 live
    EXPECT_LT((*c)->text_plugin()->total_postings(), 2000u);  // S27-3 步骤 5 复活
    (*c)->close();
}

// S13-F6 回归：ensure_vocab（wildcard 查询线程）与 reducer add_doc 新词插入
// 并发，加 merge 期 compact/ckpt 序列化（现经 RunFn 在 reducer 内执行）。
// 修复前 ensure_vocab 在查询线程遍历 tbb::concurrent_hash_map、merge 在调用
// 线程遍历——两者都与 reducer 插入并发（TBB 不支持，rehash 可致迭代器失效）。
// 本测试在 TSan 构建下作为竞态护栏；plain 构建下验证 vocab 增量维护的正确性
// （wildcard 能命中所有历史新词）。
TEST_F(CaskDocValueTest, VocabConcurrentNewTermsAndMergeNoRace) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    opts.max_file_size = 4096;  // 滚出多个文件给 merge
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                // wildcard 走 ensure_vocab（dirty 时重建词典侧表）。
                auto sr = (*c)->search_wildcard("term*", 20);
                (void)sr;
            }
        });
    }

    // 写者：每篇文档带一个此前未见过的新词 → 持续触发 vocab delta 记账。
    std::uint32_t ts = 1000;
    for (int i = 0; i < 400; ++i) {
        bitcask::DocInput doc;
        const std::string text = "term" + std::to_string(i) + " filler";
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("k" + std::to_string(i)), doc, ts++));
        if (i == 200) {
            // merge 与写/查并发：compact + ckpt 序列化经 RunFn 在 reducer 执行。
            auto mr = (*c)->merge({});
            ASSERT_TRUE(mr);
        }
    }
    stop.store(true);
    for (auto& th : readers) th.join();

    // 正确性：全部 400 个历史新词都必须进 vocab（wildcard 可枚举）。
    (*c)->flush_index();
    auto sr = (*c)->search_wildcard("term39?", 20);  // term390..term399
    ASSERT_TRUE(sr);
    EXPECT_EQ(sr->hits.size(), 10u);
    auto sr0 = (*c)->search_wildcard("term0", 5);    // 最早的词仍在
    ASSERT_TRUE(sr0);
    EXPECT_EQ(sr0->hits.size(), 1u);
    (*c)->close();
}

// 校验:dim 不符 / 未配置却带向量 / cosine 下零向量,全部拒绝。
TEST_F(CaskDocValueTest, V31VectorValidation) {
    std::vector<std::byte> key{std::byte{'k'}};
    const std::string text = "t";
    {
        auto c = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
        ASSERT_TRUE(c);
        bitcask::DocInput doc;
        doc.text = sv_bytes(text);
        const float wrong[2] = {1.0f, 2.0f};
        doc.vector = std::span<const float>(wrong, 2);
        auto r = (*c)->put_doc(key, doc, 1000);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, bitcask::CaskError::kInvalidOption);

        const float zeros[4] = {0, 0, 0, 0};
        doc.vector = std::span<const float>(zeros, 4);
        r = (*c)->put_doc(key, doc, 1000);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().kind, bitcask::CaskError::kInvalidOption);
        (*c)->close();
    }
    // 未配置向量的集合拒收向量(新目录,search 模式 dim=0)。
    auto tmp2 = tmpdir_ / "novec";
    std::filesystem::create_directories(tmp2);
    auto c2 = Cask::open(tmp2.string(), p3_search_opts(), &test_registry());
    ASSERT_TRUE(c2);
    bitcask::DocInput doc;
    doc.text = sv_bytes(text);
    const float v4[4] = {1, 0, 0, 0};
    doc.vector = std::span<const float>(v4, 4);
    auto r = (*c2)->put_doc(key, doc, 1000);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, bitcask::CaskError::kInvalidOption);
    (*c2)->close();
}

// 重开配置必须与 meta 一致:dim 改变 / 去掉向量配置 → kModeMismatch。
TEST_F(CaskDocValueTest, V31MetaVectorMismatchOnReopen) {
    {
        auto c = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
        ASSERT_TRUE(c);
        (*c)->close();
    }
    auto bad_dim = Cask::open(tmpdir_.string(), v31_opts(8), &test_registry());
    ASSERT_FALSE(bad_dim);
    EXPECT_EQ(bad_dim.error().kind, bitcask::CaskError::kModeMismatch);

    auto no_vec = Cask::open(tmpdir_.string(), p3_search_opts(), &test_registry());
    ASSERT_FALSE(no_vec);
    EXPECT_EQ(no_vec.error().kind, bitcask::CaskError::kModeMismatch);

    auto ok = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
    EXPECT_TRUE(ok);
    if (ok) (*ok)->close();
}

// ── V3.3:HNSW 并发化 + IndexPool 接线(端到端)──────────────────────────

// put_doc(带向量)→ IndexPool → HNSW;search_vector 命中最近者;
// remove 后 live 过滤;close+reopen(向量集合走全量 fold)恢复接线。
TEST_F(CaskDocValueTest, V33VectorSearchEndToEnd) {
    auto opts = v31_opts(4);
    const std::string k1 = "k1", k2 = "k2", k3 = "k3";
    const float v1[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float v2[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    const float v3[4] = {0.7f, 0.7f, 0.0f, 0.0f};   // 归一化后 (0.7071, 0.7071)
    const float q[4]  = {2.0f, 0.0f, 0.0f, 0.0f};   // 未归一化查询(引擎归一化)

    auto put_vec_doc = [&](Cask& c, const std::string& key, const float* v) {
        bitcask::DocInput doc;
        const std::string text = "doc " + key;
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(v, 4);
        ASSERT_TRUE(c.put_doc(sv_bytes(key), doc, 1000));
    };

    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        put_vec_doc(**c, k1, v1);
        put_vec_doc(**c, k2, v2);
        put_vec_doc(**c, k3, v3);
        (*c)->flush_index();

        auto r = (*c)->search_vector(std::span<const float>(q, 4), 3);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 3u);
        EXPECT_EQ(r->hits[0].key, k1);              // cos=1.0 最近
        EXPECT_NEAR(r->hits[0].score, 1.0, 1e-5);
        EXPECT_EQ(r->hits[1].key, k3);              // cos≈0.7071
        EXPECT_NEAR(r->hits[1].score, 0.7071, 1e-3);
        EXPECT_EQ(r->hits[2].key, k2);              // cos=0

        // remove k1 → live 过滤,死节点不出现(图内仍作路标)。
        ASSERT_TRUE((*c)->remove(sv_bytes(k1)));
        auto r2 = (*c)->search_vector(std::span<const float>(q, 4), 3);
        ASSERT_TRUE(r2);
        ASSERT_EQ(r2->hits.size(), 2u);
        EXPECT_EQ(r2->hits[0].key, k3);
        for (const auto& h : r2->hits) EXPECT_NE(h.key, k1);
        (*c)->close();
    }

    // reopen:V3.3 向量集合强制全量 fold(HNSW 持久化是 V3.5)——
    // 恢复路径 recover_doc(…, vector) 重建图;k1 的墓碑令其仍被滤掉。
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto r = (*c)->search_vector(std::span<const float>(q, 4), 3);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->hits.size(), 2u);
    EXPECT_EQ(r->hits[0].key, k3);
    EXPECT_NEAR(r->hits[0].score, 0.7071, 1e-3);
    EXPECT_EQ(r->hits[1].key, k2);
    for (const auto& h : r->hits) EXPECT_NE(h.key, k1);

    // 错误路径:零向量查询 → 空命中(非错误)。
    const float zq[4] = {0, 0, 0, 0};
    auto rz = (*c)->search_vector(std::span<const float>(zq, 4), 3);
    ASSERT_TRUE(rz);
    EXPECT_TRUE(rz->hits.empty());
    (*c)->close();

    // 无向量配置的 search 集合 → kInvalidOption;KV 集合 → kNoIndex。
    auto tmp2 = tmpdir_ / "novec33";
    std::filesystem::create_directories(tmp2);
    auto c2 = Cask::open(tmp2.string(), p3_search_opts(), &test_registry());
    ASSERT_TRUE(c2);
    auto bad = (*c2)->search_vector(std::span<const float>(q, 4), 3);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().kind, bitcask::CaskError::kInvalidOption);
    (*c2)->close();

    auto tmp3 = tmpdir_ / "kv33";
    std::filesystem::create_directories(tmp3);
    CaskOptions kv_opts;
    kv_opts.read_write = true;
    auto c3 = Cask::open(tmp3.string(), kv_opts, &test_registry());
    ASSERT_TRUE(c3);
    auto bad2 = (*c3)->search_vector(std::span<const float>(q, 4), 3);
    ASSERT_FALSE(bad2);
    EXPECT_EQ(bad2.error().kind, bitcask::CaskError::kNoIndex);
    (*c3)->close();
}

// ── V3.4:软删语义(机制在 Index.live_ + HNSW 结果侧过滤,此处证语义)──

// 覆写:同 key 重写新向量后,旧向量必须不可达——key 只能经新向量的
// 位置出现(旧 ord 翻死,新 ord 存活),且结果中恰出现一次。
TEST_F(CaskDocValueTest, V34OverwriteVectorMovesKey) {
    auto c = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
    ASSERT_TRUE(c);
    const float va[4] = {1.0f, 0.0f, 0.0f, 0.0f};   // k1 初版
    const float vb[4] = {0.0f, 0.0f, 1.0f, 0.0f};   // k1 覆写版(⊥ va)
    const float v2[4] = {0.6f, 0.8f, 0.0f, 0.0f};   // k2 固定
    auto put = [&](const std::string& key, const float* v) {
        bitcask::DocInput doc;
        const std::string text = "doc " + key;
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(v, 4);
        ASSERT_TRUE((*c)->put_doc(sv_bytes(key), doc, 1000));
    };
    put("k1", va);
    put("k2", v2);
    put("k1", vb);   // 覆写:旧 ord(va 处)翻死

    // 查 va 方向:k1 的旧向量若可达会以 score=1.0 居首——必须不发生。
    // 活集真值:k2·va=0.6 > k1(vb)·va=0。
    auto ra = (*c)->search_vector(std::span<const float>(va, 4), 3);
    ASSERT_TRUE(ra);
    ASSERT_EQ(ra->hits.size(), 2u);
    EXPECT_EQ(ra->hits[0].key, "k2");
    EXPECT_NEAR(ra->hits[0].score, 0.6, 1e-5);
    EXPECT_EQ(ra->hits[1].key, "k1");
    EXPECT_NEAR(ra->hits[1].score, 0.0, 1e-5);      // 经 vb 出现,绝非 1.0

    // 查 vb 方向:k1 经新向量以 1.0 居首。
    auto rb = (*c)->search_vector(std::span<const float>(vb, 4), 3);
    ASSERT_TRUE(rb);
    ASSERT_EQ(rb->hits.size(), 2u);
    EXPECT_EQ(rb->hits[0].key, "k1");
    EXPECT_NEAR(rb->hits[0].score, 1.0, 1e-5);
    (*c)->close();
}

// 死区导航:删掉查询近邻的一半(形成包住查询的死壳),搜索仍须凑满 k
// 且与活集暴力真值一致——证"死节点留作图内路标"参与导航而不污染结果。
TEST_F(CaskDocValueTest, V34DeadZoneNavigation) {
    constexpr std::size_t kDim = 8, kN = 300, kDead = 150, kTopK = 10;
    auto c = Cask::open(tmpdir_.string(), v31_opts(kDim), &test_registry());
    ASSERT_TRUE(c);

    std::mt19937 rng(0x5EED34);  // 固定种子可复现
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<std::vector<float>> vecs(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        auto& v = vecs[i];
        v.resize(kDim);
        double sq = 0.0;
        for (auto& x : v) { x = nd(rng); sq += static_cast<double>(x) * x; }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (auto& x : v) x *= inv;
        char key[8];
        std::snprintf(key, sizeof key, "d%03zu", i);
        bitcask::DocInput doc;
        const std::string text = "filler";
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(v.data(), kDim);
        ASSERT_TRUE((*c)->put_doc(sv_bytes(std::string(key)), doc, 1000));
    }
    std::vector<float> q(kDim);
    { double sq = 0.0;
      for (auto& x : q) { x = nd(rng); sq += static_cast<double>(x) * x; }
      const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
      for (auto& x : q) x *= inv; }

    // 暴力真值排序(按内积降序)。
    auto dot = [&](const std::vector<float>& v) {
        double s = 0.0;
        for (std::size_t d = 0; d < kDim; ++d) s += static_cast<double>(q[d]) * v[d];
        return s;
    };
    std::vector<std::size_t> order(kN);
    for (std::size_t i = 0; i < kN; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return dot(vecs[a]) > dot(vecs[b]); });

    // 删掉最近的 kDead 个 → 查询被死壳包围。
    std::set<std::string> dead;
    for (std::size_t r = 0; r < kDead; ++r) {
        char key[8];
        std::snprintf(key, sizeof key, "d%03zu", order[r]);
        ASSERT_TRUE((*c)->remove(sv_bytes(std::string(key))));
        dead.insert(key);
    }
    // 活集真值 top-k = 排序中第 kDead 起的 kTopK 个。
    std::set<std::string> truth;
    for (std::size_t r = kDead; r < kDead + kTopK; ++r) {
        char key[8];
        std::snprintf(key, sizeof key, "d%03zu", order[r]);
        truth.insert(key);
    }

    auto res = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                   kTopK, /*ef=*/256);
    ASSERT_TRUE(res);
    ASSERT_EQ(res->hits.size(), kTopK);              // 死壳没有压垮凑满 k
    std::size_t overlap = 0;
    for (const auto& h : res->hits) {
        EXPECT_EQ(dead.count(h.key), 0u) << "死文档泄入结果: " << h.key;
        overlap += truth.count(h.key);
    }
    // ef=256 对 300 节点近乎穷举;留 1 个并列容差。
    EXPECT_GE(overlap, kTopK - 1)
        << "活集 top-k 重合 " << overlap << "/" << kTopK;
    (*c)->close();
}

// ── V3.5:HNSW 快照(BCVS)并入 A4 covers 门 + merge 重建 ──────────────────

namespace {

// 固定种子归一化高斯向量组(cosine 集合的标准合成形态)。
std::vector<std::vector<float>> v35_make_vecs(std::size_t n, std::size_t dim,
                                              std::uint64_t seed) {
    std::mt19937 rng(static_cast<unsigned>(seed));
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::vector<std::vector<float>> out(n);
    for (auto& v : out) {
        v.resize(dim);
        double sq = 0.0;
        for (auto& x : v) { x = nd(rng); sq += static_cast<double>(x) * x; }
        const auto inv = static_cast<float>(1.0 / std::sqrt(sq));
        for (auto& x : v) x *= inv;
    }
    return out;
}

void v35_put(Cask& c, const std::string& key, const std::vector<float>& v) {
    bitcask::DocInput doc;
    const std::string text = "filler " + key;
    doc.text = sv_bytes(text);
    doc.vector = std::span<const float>(v.data(), v.size());
    ASSERT_TRUE(c.put_doc(sv_bytes(key), doc, 1000));
}

std::vector<std::string> v35_hit_keys(const bitcask::TextSearchResult& r) {
    std::vector<std::string> keys;
    keys.reserve(r.hits.size());
    for (const auto& h : r.hits) keys.push_back(h.key);
    return keys;
}

}  // namespace

// 三件套 1:干净 close → reopen 走快路径,search_vector 与 close 前一致。
// 快路径实证:把目录复制一份并删光 data/hint 文件——没有 data 可 fold,
// 搜索仍正确 ⟹ 结果只可能来自快照(keydir + search.ckpt)。
// 再删 search.ckpt 对比全量 fold 的等价结果(A4 既有断言范式)。
TEST_F(CaskDocValueTest, V35SnapshotFastReopen) {
    constexpr std::size_t kDim = 8;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(50, kDim, 0xB35F);
    auto q = v35_make_vecs(1, kDim, 0xBEEF)[0];
    auto key_of = [](std::size_t i) {
        char k[8];
        std::snprintf(k, sizeof k, "v%02zu", i);
        return std::string(k);
    };

    std::vector<std::string> expect_keys;
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < 40; ++i) v35_put(**c, key_of(i), vecs[i]);
        (*c)->flush_index();
        // 中部删除(墓碑占 ord 但不收尾——尾部 ord 须是向量文档,
        // 否则 covers/floor 门保守关闭,快路径无从谈起)。
        for (std::size_t i = 5; i < 30; i += 6) {
            ASSERT_TRUE((*c)->remove(sv_bytes(key_of(i))));
        }
        for (std::size_t i = 40; i < 50; ++i) v35_put(**c, key_of(i), vecs[i]);
        (*c)->flush_index();
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 10u);
        expect_keys = v35_hit_keys(*r);
        (*c)->close();
    }
    ASSERT_TRUE(std::filesystem::exists(tmpdir_ / "bm25.ckpt"));

    auto search_in = [&](const std::string& dir) {
        std::vector<std::string> keys;
        auto c = Cask::open(dir, opts, &test_registry());
        EXPECT_TRUE(c);
        if (!c) return keys;
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        EXPECT_TRUE(r);
        if (r) keys = v35_hit_keys(*r);
        (*c)->close();
        return keys;
    };

    // (a) 快照齐备的 reopen。
    EXPECT_EQ(search_in(tmpdir_.string()), expect_keys);

    // (b) 快路径实证:复制目录、删光 data/hint/lock,只剩快照仍可检索。
    const auto snaponly = tmpdir_.parent_path() /
                          (tmpdir_.filename().string() + "_snaponly");
    std::error_code ec;
    std::filesystem::remove_all(snaponly, ec);
    std::filesystem::copy(tmpdir_, snaponly, ec);
    ASSERT_FALSE(ec);
    for (const auto& e : std::filesystem::directory_iterator(snaponly)) {
        const auto name = e.path().filename().string();
        if (name.ends_with(".data") || name.ends_with(".hint") ||
            name.ends_with(".lock")) {
            std::filesystem::remove(e.path(), ec);
        }
    }
    EXPECT_EQ(search_in(snaponly.string()), expect_keys);
    std::filesystem::remove_all(snaponly, ec);

    // (c) 删 search.ckpt → 全量 fold,结果等价;close 后快照重新落盘。
    std::filesystem::remove(tmpdir_ / "bm25.ckpt");
    EXPECT_EQ(search_in(tmpdir_.string()), expect_keys);
    EXPECT_TRUE(std::filesystem::exists(tmpdir_ / "bm25.ckpt"));
}

// 三件套 2:快照旧于 data 尾部(崩溃形态)——回退 keydir+search.ckpt 到
// 会话 1,会话 2 的向量文档须经尾部回放重插,新旧都可检索。
TEST_F(CaskDocValueTest, V35StaleTailReplay) {
    constexpr std::size_t kDim = 8;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(35, kDim, 0x57A1E);
    const auto kd_snap = tmpdir_ / "kv.keydir.ckpt";
    const auto hs_snap = tmpdir_ / "bm25.ckpt";
    const auto kd_old = tmpdir_ / "kd.old";
    const auto hs_old = tmpdir_ / "hs.old";

    {   // 会话 1:20 个向量文档。
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < 20; ++i) {
            v35_put(**c, "a" + std::to_string(i), vecs[i]);
        }
        (*c)->close();
    }
    std::filesystem::copy_file(kd_snap, kd_old);
    std::filesystem::copy_file(hs_snap, hs_old);

    {   // 会话 2:再写 15 个。
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 20; i < 35; ++i) {
            v35_put(**c, "b" + std::to_string(i), vecs[i]);
        }
        (*c)->close();
    }
    // 回退 keydir+hnsw 到会话 1(一致的旧快照对;bm25/sidecar 保持新——
    // 覆盖更大,门照过)。会话 2 的文件不在旧水位表 → 从 0 回放。
    std::filesystem::copy_file(kd_old, kd_snap,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(hs_old, hs_snap,
        std::filesystem::copy_options::overwrite_existing);

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto q = v35_make_vecs(1, kDim, 0xFACE)[0];
    auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                 35, /*ef=*/512);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->hits.size(), 35u);   // 旧 20 + 尾部回放重插的 15 全可达
    std::set<std::string> got;
    for (const auto& h : r->hits) got.insert(h.key);
    for (std::size_t i = 0; i < 20; ++i) {
        EXPECT_EQ(got.count("a" + std::to_string(i)), 1u) << i;
    }
    for (std::size_t i = 20; i < 35; ++i) {
        EXPECT_EQ(got.count("b" + std::to_string(i)), 1u) << i;
    }
    (*c)->close();
}

// 三件套 3:search.ckpt 损坏(位翻转)→ 整体拒绝 + 回退全量 fold,
// 不崩、结果正确;再 close 后快照恢复健康。
TEST_F(CaskDocValueTest, V35CorruptFallsBack) {
    constexpr std::size_t kDim = 8;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(30, kDim, 0xC0552);
    auto q = v35_make_vecs(1, kDim, 0xDeed)[0];

    std::vector<std::string> expect_keys;
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < 30; ++i) {
            v35_put(**c, "k" + std::to_string(i), vecs[i]);
        }
        (*c)->flush_index();
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        ASSERT_TRUE(r);
        expect_keys = v35_hit_keys(*r);
        (*c)->close();
    }
    const auto snap = tmpdir_ / "bm25.ckpt";
    ASSERT_TRUE(std::filesystem::exists(snap));

    {   // 位翻转 payload 中部(CRC 必炸 → load 整体拒绝)。
        std::FILE* f = std::fopen(snap.string().c_str(), "rb+");
        ASSERT_NE(f, nullptr);
        std::fseek(f, 0, SEEK_END);
        const long mid = std::ftell(f) / 2;
        std::fseek(f, mid, SEEK_SET);
        int ch = std::fgetc(f);
        std::fseek(f, mid, SEEK_SET);
        std::fputc(ch ^ 0xFF, f);
        std::fclose(f);
    }
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        ASSERT_TRUE(r);
        EXPECT_EQ(v35_hit_keys(*r), expect_keys);   // 全量 fold 重建等价
        (*c)->close();                              // 快照重写恢复健康
    }
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        ASSERT_TRUE(r);
        EXPECT_EQ(v35_hit_keys(*r), expect_keys);
        (*c)->close();
    }
}

// merge 重建:删一半 → merge → RebuildHnsw(IndexPool worker)物理清死,
// hnsw_size() 收敛到活文档数;结果与删除后一致;close+reopen 仍一致。
TEST_F(CaskDocValueTest, V35MergeRebuildEvictsDead) {
    constexpr std::size_t kDim = 8, kN = 50;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(kN, kDim, 0x4EAD);
    auto q = v35_make_vecs(1, kDim, 0xF00D)[0];
    auto key_of = [](std::size_t i) {
        char k[8];
        std::snprintf(k, sizeof k, "m%02zu", i);
        return std::string(k);
    };

    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < kN; ++i) v35_put(**c, key_of(i), vecs[i]);
        (*c)->flush_index();
        for (std::size_t i = 1; i < kN; i += 2) {   // 删奇数位,留 25 活
            ASSERT_TRUE((*c)->remove(sv_bytes(key_of(i))));
        }
        (*c)->close();   // 放掉 active writer,merge 可吃全部文件
    }

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    (*c)->flush_index();
    // merge 前:图含全部 50 节点(死节点只是结果侧滤除)。
    EXPECT_EQ((*c)->vector_plugin()->size(), kN);
    auto before = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                      10, /*ef=*/256);
    ASSERT_TRUE(before);
    ASSERT_EQ(before->hits.size(), 10u);
    for (const auto& h : before->hits) {
        EXPECT_NE(std::stoul(h.key.substr(1)) % 2, 1u) << h.key;
    }

    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
        if (e.path().filename().string().ends_with(".bitcask.data")) {
            files.push_back(e.path().string());
        }
    }
    ASSERT_GE(files.size(), 1u);
    auto mr = (*c)->merge(files, 3000);
    ASSERT_TRUE(mr) << mr.error().detail;
    (*c)->flush_index();   // 等 RebuildHnsw 任务被 worker 消化

    // 物理清死:图节点数 == 活文档数。
    EXPECT_EQ((*c)->vector_plugin()->size(), kN / 2);
    auto after = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
    ASSERT_TRUE(after);
    EXPECT_EQ(v35_hit_keys(*after), v35_hit_keys(*before));
    (*c)->close();

    // 重开(merge 后快照/数据均只剩活集)仍一致。
    auto c2 = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c2);
    auto r2 = (*c2)->search_vector(std::span<const float>(q.data(), kDim),
                                   10, /*ef=*/256);
    ASSERT_TRUE(r2);
    EXPECT_EQ(v35_hit_keys(*r2), v35_hit_keys(*before));
    EXPECT_EQ((*c2)->vector_plugin()->size(), kN / 2);
    (*c2)->close();
}

// 并发:merge 触发的 RebuildHnsw(worker 换图指针)与多读者 search_vector
// 并发,期间主线程继续 put(任务排在 Rebuild 之后)。TSan 是裁判;
// 语义断言:全程查询不出死文档,重建后图 = 活集 + 新增。
TEST_F(CaskDocValueTest, V35ConcurrentSearchDuringRebuild) {
    constexpr std::size_t kDim = 16, kN = 400;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(kN + 40, kDim, 0xCC35);
    auto key_of = [](std::size_t i) {
        char k[8];
        std::snprintf(k, sizeof k, "c%03zu", i);
        return std::string(k);
    };

    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < kN; ++i) v35_put(**c, key_of(i), vecs[i]);
        (*c)->flush_index();
        for (std::size_t i = 1; i < kN; i += 2) {
            ASSERT_TRUE((*c)->remove(sv_bytes(key_of(i))));
        }
        (*c)->close();
    }

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::atomic<bool> done{false};
    std::atomic<bool> dead_leaked{false};
    std::atomic<std::uint64_t> queries{0};
    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t]() {
            std::mt19937 rng(0x5EED0000u + static_cast<unsigned>(t));
            std::normal_distribution<float> nd(0.0f, 1.0f);
            std::vector<float> q(kDim);
            while (!done.load(std::memory_order_acquire)) {
                for (auto& x : q) x = nd(rng);
                // 直走 SearchLayer(避开 Cask::search_vector 的 flush 串行
                // 化)——与 worker 的重建/插入真并发。
                auto r = (*c)->vector_plugin()->search(
                    std::span<const float>(q.data(), kDim), 10, 64,
                    nullptr);
                if (!r) continue;
                for (const auto& h : *r) {
                    const auto idx = std::stoul(h.key.substr(1));
                    if (idx < kN && idx % 2 == 1) {
                        dead_leaked.store(true, std::memory_order_relaxed);
                    }
                }
                queries.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
        if (e.path().filename().string().ends_with(".bitcask.data")) {
            files.push_back(e.path().string());
        }
    }
    auto mr = (*c)->merge(files, 3000);   // 末尾提交 RebuildHnsw(异步)
    ASSERT_TRUE(mr) << mr.error().detail;
    // 重建在 worker 排队/执行中,主线程继续写——新任务排在 Rebuild 后。
    for (std::size_t i = kN; i < kN + 40; ++i) {
        v35_put(**c, key_of(i), vecs[i]);
    }
    (*c)->flush_index();
    done.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    EXPECT_FALSE(dead_leaked.load());
    EXPECT_GT(queries.load(), 0u);
    // 重建清死 + 新增 40:图 = 200 活 + 40。
    EXPECT_EQ((*c)->vector_plugin()->size(), kN / 2 + 40);
    (*c)->close();
}

// ── V3.6:search_hybrid RRF 融合(hnsw-design §4)─────────────────────────

namespace {

// 两路排名已知的固定小语料(dim=4,whitespace 分词)。
//   查询:text "x",vec (1,0,0,0)。
//   key  text(BM25 rank)        vec(归一化后)   (vec rank,cos)
//   d1   "x x x"  (rank1,tf=3)  (0.6,0.8,0,0)   (rank3,0.6)
//   d2   "x x y"  (rank2,tf=2)  (0.8,0.6,0,0)   (rank2,0.8)
//   d3   "x y y"  (rank3,tf=1)  (1,0,0,0)       (rank1,1.0)
//   d4   "z z z"  (无命中)       (0,0,0,1)       (rank4,0.0,单路文档)
// 四篇 doclen 同为 3 → BM25 单调于 tf,文本排名确定。
void v36_put_corpus(Cask& c) {
    struct Row { const char* key; const char* text; float v[4]; };
    static constexpr Row rows[] = {
        {"d1", "x x x", {0.6f, 0.8f, 0.0f, 0.0f}},
        {"d2", "x x y", {0.8f, 0.6f, 0.0f, 0.0f}},
        {"d3", "x y y", {1.0f, 0.0f, 0.0f, 0.0f}},
        {"d4", "z z z", {0.0f, 0.0f, 0.0f, 1.0f}},
    };
    for (const auto& r : rows) {
        bitcask::DocInput doc;
        const std::string text = r.text;
        doc.text = sv_bytes(text);
        doc.vector = std::span<const float>(r.v, 4);
        ASSERT_TRUE(c.put_doc(sv_bytes(std::string(r.key)), doc, 1000));
    }
}

constexpr double v36_rrf(int rank) { return 1.0 / (60.0 + rank); }

}  // namespace

// RRF 真值逐位断言(含精确平局的 ord 序):
//   d1 = 1/61 + 1/63(text r1 + vec r3)
//   d2 = 1/62 + 1/62
//   d3 = 1/63 + 1/61(与 d1 **精确平局**:同两项之和,浮点可交换)
//   d4 = 1/64        (仅 vec 路,照常累加该路项)
//   序:d1(ord 小)→ d3 → d2 → d4。
TEST_F(CaskDocValueTest, V36HybridRrfFusion) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);
    const float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    auto r = (*c)->search_hybrid("x", std::span<const float>(q, 4), 10);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->hits.size(), 4u);
    EXPECT_EQ(r->hits[0].key, "d1");   // 平局,ord 0 < ord 2
    EXPECT_EQ(r->hits[1].key, "d3");
    EXPECT_EQ(r->hits[2].key, "d2");
    EXPECT_EQ(r->hits[3].key, "d4");
    EXPECT_DOUBLE_EQ(r->hits[0].score, v36_rrf(1) + v36_rrf(3));
    EXPECT_DOUBLE_EQ(r->hits[1].score, v36_rrf(3) + v36_rrf(1));
    EXPECT_DOUBLE_EQ(r->hits[0].score, r->hits[1].score);  // 锁死精确平局
    EXPECT_DOUBLE_EQ(r->hits[2].score, v36_rrf(2) + v36_rrf(2));
    EXPECT_DOUBLE_EQ(r->hits[3].score, v36_rrf(4));
    EXPECT_LT(r->hits[0].ord, r->hits[1].ord);             // 平局序 = ord 升序

    // k 截断:top-2 = 平局对(d1, d3)。
    auto r2 = (*c)->search_hybrid("x", std::span<const float>(q, 4), 2);
    ASSERT_TRUE(r2);
    ASSERT_EQ(r2->hits.size(), 2u);
    EXPECT_EQ(r2->hits[0].key, "d1");
    EXPECT_EQ(r2->hits[1].key, "d3");
    (*c)->close();
}

// 单路退化:text 空 → 等价纯向量(RRF 重打分 1/61..);vec 空 → 纯文本。
TEST_F(CaskDocValueTest, V36HybridSingleLeg) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);
    const float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // 纯向量:BM25 路空 → 排名 = search_vector 序(d3,d2,d1,d4)。
    auto rv = (*c)->search_hybrid("", std::span<const float>(q, 4), 10);
    ASSERT_TRUE(rv);
    ASSERT_EQ(rv->hits.size(), 4u);
    const char* vexp[] = {"d3", "d2", "d1", "d4"};
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(rv->hits[i].key, vexp[i]);
        EXPECT_DOUBLE_EQ(rv->hits[i].score, v36_rrf(i + 1));
    }

    // 纯文本:vec 路空 → 排名 = search_text 序(d1,d2,d3),d4 无命中。
    auto rt = (*c)->search_hybrid("x", std::span<const float>{}, 10);
    ASSERT_TRUE(rt);
    ASSERT_EQ(rt->hits.size(), 3u);
    const char* texp[] = {"d1", "d2", "d3"};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(rt->hits[i].key, texp[i]);
        EXPECT_DOUBLE_EQ(rt->hits[i].score, v36_rrf(i + 1));
    }
    (*c)->close();
}

// 边界:无向量配置 → kInvalidOption;KV → kNoIndex;维度不符 →
// kInvalidOption;两路都空 → kInvalidOption。
TEST_F(CaskDocValueTest, V36HybridErrors) {
    const float q[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    // 无向量配置的 search 集合。
    auto tmp_novec = tmpdir_ / "novec36";
    std::filesystem::create_directories(tmp_novec);
    auto c1 = Cask::open(tmp_novec.string(), p3_search_opts(), &test_registry());
    ASSERT_TRUE(c1);
    auto e1 = (*c1)->search_hybrid("x", std::span<const float>(q, 4), 10);
    ASSERT_FALSE(e1);
    EXPECT_EQ(e1.error().kind, bitcask::CaskError::kInvalidOption);
    (*c1)->close();

    // KV 集合(无 search_)。
    auto tmp_kv = tmpdir_ / "kv36";
    std::filesystem::create_directories(tmp_kv);
    CaskOptions kv_opts;
    kv_opts.read_write = true;
    auto c2 = Cask::open(tmp_kv.string(), kv_opts, &test_registry());
    ASSERT_TRUE(c2);
    auto e2 = (*c2)->search_hybrid("x", std::span<const float>(q, 4), 10);
    ASSERT_FALSE(e2);
    EXPECT_EQ(e2.error().kind, bitcask::CaskError::kNoIndex);
    (*c2)->close();

    // 向量集合:维度不符 / 双空。
    auto c3 = Cask::open(tmpdir_.string(), v31_opts(4), &test_registry());
    ASSERT_TRUE(c3);
    v36_put_corpus(**c3);
    const float wrong[2] = {1.0f, 0.0f};
    auto e3 = (*c3)->search_hybrid("x", std::span<const float>(wrong, 2), 10);
    ASSERT_FALSE(e3);
    EXPECT_EQ(e3.error().kind, bitcask::CaskError::kInvalidOption);

    auto e4 = (*c3)->search_hybrid("", std::span<const float>{}, 10);
    ASSERT_FALSE(e4);
    EXPECT_EQ(e4.error().kind, bitcask::CaskError::kInvalidOption);
    (*c3)->close();
}

// ── S7-4:批量文本搜索（inter-query 并发入口）──────────────────────────

// 批量结果必须与逐条 search_text 字节等价（保序 + 各槽独立），并发安全（TSan）。
TEST_F(CaskDocValueTest, S74SearchTextBatchMatchesSerial) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);

    // 含重复 "x"（并发写同一缓存键，压 cache 写锁）+ 无命中查询。
    std::vector<std::string> qstr = {"x", "y", "z", "nomatch",
                                     "x", "y", "x", "z"};
    std::vector<std::string_view> q(qstr.begin(), qstr.end());

    // oracle：逐条串行 search_text。
    std::vector<std::vector<std::string>> expect;
    for (const auto& s : qstr) {
        auto r = (*c)->search_text(s, 10);
        ASSERT_TRUE(r);
        std::vector<std::string> keys;
        for (const auto& h : r->hits) keys.push_back(h.key);
        expect.push_back(std::move(keys));
    }

    auto batch = (*c)->search_text_batch(q, 10);
    ASSERT_EQ(batch.size(), q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        ASSERT_TRUE(batch[i]) << "query " << i << " = " << qstr[i];
        std::vector<std::string> keys;
        for (const auto& h : batch[i]->hits) keys.push_back(h.key);
        EXPECT_EQ(keys, expect[i]) << "query " << i << " = " << qstr[i];
    }
    (*c)->close();
}

// S11-W2：并发**多模式搜索 + 并发写**同一 Cask handle 的安全护栏（通用 C++ 库）。
// 验证读路径（text/phrase/bool/fields/vector/hybrid 各模式并发）+ 写路径（put_doc/
// remove，W1 后内部 write_mu_）在同一 handle 上并发零 data race。结果因并发写不可
// 严格断言（near-real-time），只断言「不崩、查询不返回错误」——TSan 负责抓竞态。
TEST_F(CaskDocValueTest, W2ConcurrentSearchAndWriteNoRace) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);

    std::atomic<bool> stop{false};
    std::atomic<bool> ok{true};
    const float qv[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    std::vector<std::thread> ts;
    // 4 个读线程：轮转 6 种搜索模式。
    for (int r = 0; r < 4; ++r) {
        ts.emplace_back([&, r] {
            int n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::span<const float> v(qv, 4);
                switch ((r + n) % 6) {
                    case 0: if (!(*c)->search_text("x", 10)) ok = false; break;
                    case 1: if (!(*c)->search_phrase("x y", 10)) ok = false; break;
                    case 2: if (!(*c)->bool_search("x y", 10)) ok = false; break;
                    case 3: if (!(*c)->search_fields("x", 10)) ok = false; break;
                    case 4: if (!(*c)->search_vector(v, 10)) ok = false; break;
                    case 5: if (!(*c)->search_hybrid("x", v, 10)) ok = false; break;
                }
                if (++n >= 200) break;
            }
        });
    }
    // 2 个写线程：并发 put_doc（新键）+ 间隔 remove。
    for (int w = 0; w < 2; ++w) {
        ts.emplace_back([&, w] {
            for (int i = 0; i < 150; ++i) {
                bitcask::DocInput doc;
                std::string text = "x y z w" + std::to_string(i);
                doc.text = sv_bytes(text);
                const float dv[4] = {0.5f, 0.5f, 0.5f, 0.5f};
                doc.vector = std::span<const float>(dv, 4);
                std::string key = "w" + std::to_string(w) + "_" + std::to_string(i);
                if (!(*c)->put_doc(sv_bytes(key), doc,
                                   static_cast<std::uint32_t>(3000 + i))) {
                    ok = false; return;
                }
                if (i % 7 == 6) {
                    std::string old = "w" + std::to_string(w) + "_" +
                                      std::to_string(i - 6);
                    (void)(*c)->remove(sv_bytes(old));  // 删除可因时序无目标，忽略返回
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    stop.store(true);
    EXPECT_TRUE(ok.load()) << "并发搜索/写出现错误返回";

    // 收尾一致性：flush 后搜索仍正常返回。
    (*c)->flush_index();
    auto final_r = (*c)->search_text("x", 100);
    ASSERT_TRUE(final_r);
    (*c)->close();
}

// H1（s13-review §P1）：close 与在途写并发的防御性收敛护栏。写路径的
// Add/Delete 提交现已移出 write_mu_ 临界区（队列背压不再冻结全部写者），
// close() 靠 WriteOpGate 等在途写者（含其锁外提交尾段）排空后才注销
// index lane——否则锁外 submit_index_task 会解引用已 erase 的 lane（UAF）。
// 契约上 close 时刻不应有在途写（caller 责任），本测试验证违约时的收敛
// 行为：不崩、close 后写路径 kClosed fail-fast。TSan 负责抓 gate 的同步。
TEST_F(CaskDocValueTest, H1CloseConvergesWithInflightWriters) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    std::atomic<bool> ok{true};
    std::vector<std::thread> ts;
    for (int w = 0; w < 4; ++w) {
        ts.emplace_back([&, w] {
            for (int i = 0; i < 400; ++i) {
                bitcask::DocInput doc;
                std::string text = "alpha beta gamma " + std::to_string(i);
                doc.text = sv_bytes(text);
                std::string key =
                    "h1_" + std::to_string(w) + "_" + std::to_string(i);
                auto r = (*c)->put_doc(sv_bytes(key), doc,
                                       static_cast<std::uint32_t>(4000 + i));
                if (!r) {
                    // close 之后必须是 kClosed fail-fast，而非崩溃/其它错误。
                    if (r.error().kind != bitcask::CaskError::kClosed) ok = false;
                    return;
                }
                if (i % 5 == 4) {
                    // 混入 remove（Delete 同样走锁外提交路径）。
                    auto d = (*c)->remove(sv_bytes(key));
                    if (!d && d.error().kind != bitcask::CaskError::kClosed) {
                        ok = false;
                        return;
                    }
                }
            }
        });
    }
    // 写者先跑起来，然后并发 close——close 内部等 WriteOpGate 归零。
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    (*c)->close();
    for (auto& t : ts) t.join();
    EXPECT_TRUE(ok.load()) << "close 并发下写路径出现非 kClosed 错误";

    // close 返回后库不可写：kClosed fail-fast。
    bitcask::DocInput doc;
    doc.text = sv_bytes("post");
    auto after = (*c)->put_doc(sv_bytes("h1_after"), doc, 9000);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().kind, bitcask::CaskError::kClosed);
}


// ── checkpoint()（s13-review §P1 后续①②）：崩溃恢复窗口收敛 ─────────────

namespace {
// 模拟崩溃现场：句柄仍打开（不 close）时把库目录按字节复制——等价于崩溃
// 瞬间的磁盘状态（data 已 pwrite 可见；没有 close 才会补存的 ckpt/快照）。
// 复制品里删掉 lock 文件：本进程 pid 仍活，stale 检测不会回收，需手动
// 清理才能以 read_write 重新打开。
std::filesystem::path crash_image(const std::filesystem::path& src,
                                  const std::string& tag) {
    namespace fs = std::filesystem;
    auto dst = fs::temp_directory_path() /
               (src.filename().string() + "_crash_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    fs::remove(dst / "bitcask.write.lock", ec);
    fs::remove(dst / "bitcask.merge.lock", ec);
    return dst;
}
}  // namespace

// ②：运行中 checkpoint() 落盘 ckpt + keydir 快照；崩溃镜像重开后，ckpt
// 覆盖区 + fold 尾部重放合起来恢复全部文档（重叠区由 ord 自门幂等）。
TEST_F(CaskDocValueTest, CheckpointCrashImageRecoversAllDocs) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto put_n = [&](int from, int to) {
        for (int i = from; i < to; ++i) {
            bitcask::DocInput doc;
            std::string text = "apple banana n" + std::to_string(i);
            doc.text = sv_bytes(text);
            ASSERT_TRUE((*c)->put_doc(sv_bytes("ck_" + std::to_string(i)), doc,
                                      static_cast<std::uint32_t>(1000 + i)));
        }
    };
    put_n(0, 120);
    ASSERT_TRUE((*c)->checkpoint());
    EXPECT_TRUE(fs::exists(tmpdir_ / "bm25.ckpt"));
    EXPECT_TRUE(fs::exists(tmpdir_ / "kv.keydir.ckpt"));
    put_n(120, 160);  // ckpt 之后的尾部增量，崩溃时只存在于 data 文件

    auto img = crash_image(tmpdir_, "ckpt");
    (*c)->close();

    auto r = Cask::open(img.string(), opts, &test_registry());
    ASSERT_TRUE(r);
    auto hits = (*r)->search_text("banana", 200);
    ASSERT_TRUE(hits);
    EXPECT_EQ(hits->hits.size(), 160u)
        << "ckpt 覆盖区(120) + fold 尾部重放(40) 应全部可检索";
    auto g = (*r)->get_owned(sv_bytes(std::string("ck_159")));
    EXPECT_TRUE(g);
    (*r)->close();
    std::error_code ec;
    fs::remove_all(img, ec);
}

// ②：checkpoint 与并发写零竞态（keydir 快照屏障 + RunFn 有界等待 +
// WriteOpGate）。结果不可严格断言（near-real-time），只断言不崩、
// checkpoint 全部成功——TSan 负责抓竞态。S14-1：同时开启小文件 roll +
// 自动 checkpoint，让手动 RunFn、自动 RunFn 与并发写在 reducer 上交错。
TEST_F(CaskDocValueTest, CheckpointConcurrentWithWrites) {
    auto opts = v31_opts(0);
    opts.max_file_size = 8 * 1024;        // 频繁 roll → 自动 ckpt 路径参战
    opts.auto_checkpoint_min_docs = 50;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    std::atomic<bool> ok{true};
    std::vector<std::thread> ts;
    for (int w = 0; w < 2; ++w) {
        ts.emplace_back([&, w] {
            for (int i = 0; i < 300; ++i) {
                bitcask::DocInput doc;
                std::string text = "melon kiwi n" + std::to_string(i);
                doc.text = sv_bytes(text);
                std::string key =
                    "cw_" + std::to_string(w) + "_" + std::to_string(i);
                if (!(*c)->put_doc(sv_bytes(key), doc,
                                   static_cast<std::uint32_t>(5000 + i))) {
                    ok = false;
                    return;
                }
            }
        });
    }
    for (int k = 0; k < 5; ++k) {
        if (!(*c)->checkpoint()) {
            ok = false;
            break;
        }
    }
    for (auto& t : ts) t.join();
    EXPECT_TRUE(ok.load()) << "并发写下 checkpoint 出现错误返回";
    ASSERT_TRUE((*c)->checkpoint());  // 静止后收尾一份
    EXPECT_TRUE(std::filesystem::exists(tmpdir_ / "bm25.ckpt"));
    (*c)->close();
}

// S14-1：roll 封口点自动 checkpoint——无 close/merge/手动调用，写满若干
// 文件后成对 ckpt 文件自动出现（RunFn 在 reducer 落盘，flush_index 排干
// 即确定完成）；崩溃镜像重开可完整恢复。
TEST_F(CaskDocValueTest, AutoCheckpointOnRoll) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    opts.max_file_size = 8 * 1024;       // 小文件强制频繁 roll
    opts.auto_checkpoint_min_docs = 50;  // 低阈值
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 400; ++i) {
        bitcask::DocInput doc;
        std::string text =
            "lemon peach n" + std::to_string(i) + " " + std::string(64, 'x');
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("ac_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(3000 + i)));
    }
    (*c)->flush_index();  // 排干 reducer → 在途自动 ckpt RunFn 必已执行
    EXPECT_TRUE(fs::exists(tmpdir_ / "bm25.ckpt"));
    EXPECT_TRUE(fs::exists(tmpdir_ / "kv.keydir.ckpt"));
    {
        auto live = (*c)->search_text("lemon", 500);
        ASSERT_TRUE(live);
        ASSERT_EQ(live->hits.size(), 400u) << "活索引在崩溃镜像前就不完整";
    }

    auto img = crash_image(tmpdir_, "auto");
    (*c)->close();
    auto r = Cask::open(img.string(), opts, &test_registry());
    ASSERT_TRUE(r);
    auto hits = (*r)->search_text("lemon", 500);
    ASSERT_TRUE(hits);
    EXPECT_EQ(hits->hits.size(), 400u)
        << "ckpt 覆盖区 + fold 尾部重放应恢复全部文档（水位成对性）";
    (*r)->close();
    std::error_code ec;
    fs::remove_all(img, ec);
}

// S14-2/S14-4：.vec 与 delta 链的引擎级验证——第二次 checkpoint 走 delta
// （search.ckpt.d1，base 与 .vec 均不动：向量随 delta 内联）；close 强制
// 全量 base，此时 .vec 才**追加**（inode 不变、尺寸精确增长）。崩溃镜像
// 重开：base + 链重放恢复全部向量。
TEST_F(CaskDocValueTest, CheckpointVecPayloadAppends) {
    namespace fs = std::filesystem;
    constexpr std::size_t kDim = 8;
    auto opts = v31_opts(kDim);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto vec_i = [&](int i) {
        std::mt19937 rng(static_cast<std::uint32_t>(i) * 2654435761u + 11u);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        std::vector<float> v(kDim);
        for (auto& x : v) x = nd(rng);
        return v;
    };
    auto put_range = [&](int from, int to) {
        for (int i = from; i < to; ++i) {
            bitcask::DocInput doc;
            std::string text = "vecdoc n" + std::to_string(i);
            doc.text = sv_bytes(text);
            auto v = vec_i(i);
            doc.vector = std::span<const float>(v.data(), v.size());
            ASSERT_TRUE((*c)->put_doc(sv_bytes("vk_" + std::to_string(i)), doc,
                                      static_cast<std::uint32_t>(6000 + i)));
        }
    };

    put_range(0, 120);
    ASSERT_TRUE((*c)->checkpoint());  // 首存：全量 base（.vec 收养 inode）
    // S17-2:per-component sidecar is sibling to vec.ckpt（vec.vec / vec.qc8）。
    const std::string vec_path = (tmpdir_ / "vec.vec").string();
    struct stat st1;
    ASSERT_EQ(::stat(vec_path.c_str(), &st1), 0);

    put_range(120, 160);
    ASSERT_TRUE((*c)->checkpoint());  // 二存：delta——base 与 .vec 均不动
    // S17-2:per-component delta——vec 组件写 .d1。
    EXPECT_TRUE(fs::exists(tmpdir_ / "vec.ckpt.d1"));
    EXPECT_TRUE(fs::exists(tmpdir_ / "docmap.ckpt.d1"));
    struct stat st2;
    ASSERT_EQ(::stat(vec_path.c_str(), &st2), 0);
    EXPECT_EQ(st1.st_ino, st2.st_ino);
    EXPECT_EQ(st1.st_size, st2.st_size) << "delta 保存不应触碰 .vec";

    auto img = crash_image(tmpdir_, "vecapp");

    // close 强制全量 base → 此时 .vec 走 S14-2 追加（inode 不变、精确增长）。
    (*c)->close();
    struct stat st3;
    ASSERT_EQ(::stat(vec_path.c_str(), &st3), 0);
    EXPECT_EQ(st1.st_ino, st3.st_ino) << "close 的 base 保存应追加 .vec 而非重写";
    EXPECT_EQ(static_cast<std::size_t>(st3.st_size - st1.st_size),
              40u * kDim * sizeof(float));
    EXPECT_FALSE(fs::exists(tmpdir_ / "vec.ckpt.d1"))
        << "base 落成后 delta 链应被清扫";
    EXPECT_FALSE(fs::exists(tmpdir_ / "docmap.ckpt.d1"));

    auto r = Cask::open(img.string(), opts, &test_registry());
    ASSERT_TRUE(r);
    // 追加区（链重放的 ord 120-159）与 base 区（0-119）抽查 self-top1。
    for (int i : {0, 119, 120, 159}) {
        auto q = vec_i(i);
        auto hits = (*r)->search_vector(
            std::span<const float>(q.data(), q.size()), 1);
        ASSERT_TRUE(hits) << "i=" << i;
        ASSERT_FALSE(hits->hits.empty()) << "i=" << i;
        EXPECT_EQ(hits->hits[0].key, "vk_" + std::to_string(i)) << "i=" << i;
    }
    (*r)->close();
    std::error_code ec;
    fs::remove_all(img, ec);
}

// S14-3/S14-4：脏标记驱动 delta 段选择性 + base 不动性。三阶段：
//   A 文本+向量 → ckpt#1（全量 base）；
//   B 纯文本增量 → ckpt#2 = d1：含 bm25.default/docmap delta、**无 hnsw delta**，
//     base 文件逐字节不动；
//   C 纯向量增量 → ckpt#3 = d2：含 hnsw/docmap delta、**无 bm25.default delta**；
//   崩溃镜像重开：base + 链重放合成完整状态（文本与向量全可检索）。
TEST_F(CaskDocValueTest, RecoverPreservesNamedFields) {
    namespace fs = std::filesystem;
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig sl_cfg;
    sl_cfg.analyzer_config.type = AnalyzerType::Whitespace;
    opts.search_config = sl_cfg;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto put_fields_doc = [&](int i) {
        bitcask::DocInput doc;  // 纯命名字段：text 空、无向量
        std::string title = "alpha t" + std::to_string(i);
        std::string body  = "beta b" + std::to_string(i);
        doc.fields.push_back({"title", sv_bytes(title)});
        doc.fields.push_back({"body", sv_bytes(body)});
        ASSERT_TRUE((*c)->put_doc(sv_bytes("nf_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(8000 + i)));
    };

    for (int i = 0; i < 30; ++i) put_fields_doc(i);
    // 分支镜像 A：无任何 ckpt → 重开走全量 fold 重建。
    auto img_full = crash_image(tmpdir_, "nf_full");
    ASSERT_TRUE((*c)->checkpoint());
    for (int i = 30; i < 50; ++i) put_fields_doc(i);
    // 分支镜像 B：ckpt 覆盖 [0,30)，[30,50) 是增量窗口（fold 重放）。
    auto img_incr = crash_image(tmpdir_, "nf_incr");
    (*c)->close();

    auto verify = [&](const fs::path& dir, std::size_t expect) {
        auto r = Cask::open(dir.string(), opts, &test_registry());
        ASSERT_TRUE(r) << dir;
        auto t = (*r)->search_fields("title:alpha", 100);
        ASSERT_TRUE(t);
        EXPECT_EQ(t->hits.size(), expect) << dir << " title:alpha";
        auto b = (*r)->search_fields("body:beta", 100);
        ASSERT_TRUE(b);
        EXPECT_EQ(b->hits.size(), expect) << dir << " body:beta";
        // catch-all：字段值词条在默认域可全文命中（docmap live 亦经此验证）。
        auto ca = (*r)->search_text("alpha", 100);
        ASSERT_TRUE(ca);
        EXPECT_EQ(ca->hits.size(), expect) << dir << " catch-all";
        (*r)->close();
    };
    verify(img_full, 30u);
    verify(img_incr, 50u);

    std::error_code ec;
    fs::remove_all(img_full, ec);
    fs::remove_all(img_incr, ec);
}

// S14-4：delta 窗口的删除/覆盖语义——removals 日志 + 按 ord 交错重放。
// 覆盖四种棘手形态：跨窗删除（杀 base 行）、窗口内覆盖写（新行自动杀旧）、
// 写后删（行不入 delta + removal 杀 base/前版）、删后重写（removal 先于
// 新行应用，不误杀）。另验多 delta 链、重开后续链（d3）、close 链坍缩。
TEST_F(CaskDocValueTest, AutoCheckpointDisabledByDefault) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    opts.max_file_size = 8 * 1024;  // 有 roll，但阈值未设
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 200; ++i) {
        bitcask::DocInput doc;
        std::string text =
            "quince fig n" + std::to_string(i) + " " + std::string(64, 'x');
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("ad_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(4000 + i)));
    }
    (*c)->flush_index();
    EXPECT_FALSE(fs::exists(tmpdir_ / "bm25.ckpt"));
    EXPECT_FALSE(fs::exists(tmpdir_ / "kv.keydir.ckpt"));
    (*c)->close();
}

// ①：崩溃镜像（无任何 ckpt/快照）重开触发重建，重分析量超过阈值
// （kPostRecoveryCkptMinDocs=1000）时 open 内立即回存 checkpoint——
// 重建成果落盘，再崩不再全价重付。
TEST_F(CaskDocValueTest, OpenAfterCrashResavesCheckpoint) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 1500; ++i) {  // > 回存阈值 1000
        bitcask::DocInput doc;
        std::string text = "cherry grape n" + std::to_string(i);
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("rs_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(2000 + i)));
    }
    // 不 checkpoint、不 close——镜像里没有任何 ckpt/快照
    // （新建库首次 open 重分析量 0，不触发回存）。
    auto img = crash_image(tmpdir_, "resave");
    (*c)->close();
    ASSERT_FALSE(fs::exists(img / "bm25.ckpt"));

    {
        auto r = Cask::open(img.string(), opts, &test_registry());
        ASSERT_TRUE(r);
        // ①：重建（1500 docs > 阈值）完成后 open 已回存两份成对文件。
        EXPECT_TRUE(fs::exists(img / "bm25.ckpt"));
        EXPECT_TRUE(fs::exists(img / "kv.keydir.ckpt"));
        auto hits = (*r)->search_text("cherry", 2000);
        ASSERT_TRUE(hits);
        EXPECT_EQ(hits->hits.size(), 1500u);
        (*r)->close();
    }
    std::error_code ec;
    fs::remove_all(img, ec);
}

// S17-6①：manifest 损坏 → 全量 fold 兜底，全部文档仍可恢复。
TEST_F(CaskDocValueTest, S17ManifestCorruptionFallsBackToFullFold) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
        ("s17_manifest_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    auto opts = v31_opts(0);

    bitcask::keydir::KeyDirRegistry reg1;
    {
        auto c = Cask::open(dir.string(), opts, &reg1);
        ASSERT_TRUE(c);
        for (int i = 0; i < 20; ++i) {
            bitcask::DocInput doc;
            std::string text = "apple banana n" + std::to_string(i);
            std::string key = "mc_" + std::to_string(i);
            doc.text = sv_bytes(text);
            ASSERT_TRUE((*c)->put_doc(sv_bytes(key), doc,
                                      static_cast<std::uint32_t>(1000 + i)));
        }
        (*c)->flush_index();
        ASSERT_TRUE((*c)->checkpoint());
        ASSERT_TRUE(fs::exists(dir / "index.manifest"));
        (*c)->close();
    }

    fs::remove(dir / "index.manifest");
    fs::remove(dir / "kv.keydir.ckpt");

    bitcask::keydir::KeyDirRegistry reg2;
    auto r = Cask::open(dir.string(), opts, &reg2);
    ASSERT_TRUE(r);
    auto h = (*r)->search_text("banana", 100);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->hits.size(), 20u);
    (*r)->close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// 边界：空批量 → 空；单条 → 走快路径（不进池）仍正确。
TEST_F(CaskDocValueTest, S74SearchTextBatchEdgeCases) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);

    auto empty = (*c)->search_text_batch({}, 10);
    EXPECT_TRUE(empty.empty());

    std::string one = "x";
    std::vector<std::string_view> q1{one};
    auto b1 = (*c)->search_text_batch(q1, 10);
    ASSERT_EQ(b1.size(), 1u);
    ASSERT_TRUE(b1[0]);
    EXPECT_EQ(b1[0]->hits.size(), 3u);  // d1,d2,d3 命中 "x"
    (*c)->close();
}

// 向量批量：批量 == 逐条 search_vector oracle，并发安全（TSan）。
TEST_F(CaskDocValueTest, S74SearchVectorBatchMatchesSerial) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);

    static const float qa[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const float qb[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static const float qc[4] = {0.6f, 0.8f, 0.0f, 0.0f};
    std::vector<std::span<const float>> q = {
        std::span<const float>(qa, 4), std::span<const float>(qb, 4),
        std::span<const float>(qc, 4), std::span<const float>(qa, 4),
        std::span<const float>(qb, 4), std::span<const float>(qc, 4)};

    std::vector<std::vector<std::string>> expect;
    for (const auto& vq : q) {
        auto r = (*c)->search_vector(vq, 10);
        ASSERT_TRUE(r);
        std::vector<std::string> keys;
        for (const auto& h : r->hits) keys.push_back(h.key);
        expect.push_back(std::move(keys));
    }
    auto batch = (*c)->search_vector_batch(q, 10);
    ASSERT_EQ(batch.size(), q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        ASSERT_TRUE(batch[i]) << "vec query " << i;
        std::vector<std::string> keys;
        for (const auto& h : batch[i]->hits) keys.push_back(h.key);
        EXPECT_EQ(keys, expect[i]) << "vec query " << i;
    }
    (*c)->close();
}

// hybrid 批量：批量 == 逐条 search_hybrid oracle，并发安全（TSan）。
TEST_F(CaskDocValueTest, S74SearchHybridBatchMatchesSerial) {
    auto opts = v31_opts(4);
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v36_put_corpus(**c);

    static const float qv[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    std::span<const float> vsp(qv, 4);
    using HQ = Cask::HybridQuery;
    std::vector<HQ> q = {
        HQ{"x", vsp}, HQ{"y", vsp}, HQ{"", vsp},          // 纯向量退化
        HQ{"x", std::span<const float>{}},                // 纯文本退化
        HQ{"x", vsp}, HQ{"z", vsp}};

    std::vector<std::vector<std::string>> expect;
    for (const auto& hq : q) {
        auto r = (*c)->search_hybrid(hq.text, hq.vec, 10);
        ASSERT_TRUE(r);
        std::vector<std::string> keys;
        for (const auto& h : r->hits) keys.push_back(h.key);
        expect.push_back(std::move(keys));
    }
    auto batch = (*c)->search_hybrid_batch(q, 10);
    ASSERT_EQ(batch.size(), q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        ASSERT_TRUE(batch[i]) << "hybrid query " << i;
        std::vector<std::string> keys;
        for (const auto& h : batch[i]->hits) keys.push_back(h.key);
        EXPECT_EQ(keys, expect[i]) << "hybrid query " << i;
    }
    (*c)->close();
}

// ── V4:单域 merge 三项变更 ──────────────────────────────────────────────

// V4.1:删除率触发策略。写入 20 篇文档,删除 12 篇(60%),设置
// deletion_rate_trigger=50 → needs_merge 返回 true。对照组:不删除时
// 即使 threshold 设置很低,没有碎片文件也不会触发。
TEST_F(CaskMergeSearchTest, DeletionRateTrigger) {
    auto opts = make_search_opts();
    opts.max_file_size = 32;  // 小文件,多次滚动
    opts.policy.deletion_rate_trigger = 50;  // 50% 死文档即触发

    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        // 写 20 篇文档,每个 put 因 max_file_size 小而滚动新文件
        for (std::size_t i = 0; i < 20; ++i) {
            char k[8], v[16];
            std::snprintf(k, sizeof k, "k%02zu", i);
            std::snprintf(v, sizeof v, "doc%02zu text", i);
            // S24 补：直接绑定局部 char 缓冲（本迭代内存活）——原
            // std::string(v) 临时在语句末即亡，存下的 span 悬垂。
            auto val = sv_bytes(std::string_view(v));
            auto key = sv_bytes(std::string_view(k));
            ASSERT_TRUE((*c)->put(key, val, 1000 + static_cast<std::uint32_t>(i)));
        }
        (*c)->close();
    }

    // 对照组:不删任何文档 → needs_merge 不触发(文件小但无碎片)
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto nm = (*c)->needs_merge(3000);
        // 文件小且碎片低,deletion_rate = 0% < 50%,不应触发
        // (frag/dead_bytes trigger 也可能不满足,取决于文件大小)
        EXPECT_FALSE(nm.needs);
        (*c)->close();
    }

    // 删除 12 篇(60%)
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < 20; i += 2) {
            // 删除偶数位(0,2,4,...,18) = 10 篇
            char k[8];
            std::snprintf(k, sizeof k, "k%02zu", i);
            ASSERT_TRUE((*c)->remove(sv_bytes(std::string(k)), 2000));
        }
        // 额外删 2 篇(k01, k03)凑到 12/20 = 60%
        for (std::size_t i : {1uz, 3uz}) {
            char k[8];
            std::snprintf(k, sizeof k, "k%02zu", i);
            ASSERT_TRUE((*c)->remove(sv_bytes(std::string(k)), 2001));
        }
        (*c)->close();
    }

    // 删除 60% → needs_merge 应触发(deletion_rate=60% >= 50%)
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        auto nm = (*c)->needs_merge(3000);
        EXPECT_TRUE(nm.needs) << "deletion_rate=60% should trigger merge";
        EXPECT_FALSE(nm.files.empty()) << "should have files to merge";
        (*c)->close();
    }
}

// V4.2:merge 后 search.ckpt 应被保存,重开时走快照路径而非全量 fold。
// 验证方法:merge 前删除一半节点,merge 后 close,再 open 后图大小
// 应等于活节点数(不是全部节点数),证明 snap 被使用(而非全量重建)。
TEST_F(CaskDocValueTest, V4HnswSnapSavedAtMerge) {
    constexpr std::size_t kDim = 8, kN = 40;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(kN, kDim, 0xBEEF);
    auto q = v35_make_vecs(1, kDim, 0xF00D)[0];
    auto key_of = [](std::size_t i) {
        char k[8];
        std::snprintf(k, sizeof k, "hs%02zu", i);
        return std::string(k);
    };

    // 写入 40 篇向量文档
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < kN; ++i) v35_put(**c, key_of(i), vecs[i]);
        (*c)->flush_index();
        (*c)->close();
    }

    // 删除一半(偶数位)
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        for (std::size_t i = 0; i < kN; i += 2) {
            ASSERT_TRUE((*c)->remove(sv_bytes(key_of(i))));
        }
        (*c)->close();
    }

    // merge:V4 改为同步 rebuild + save search.ckpt
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
            if (e.path().filename().string().ends_with(".bitcask.data")) {
                files.push_back(e.path().string());
            }
        }
        ASSERT_GE(files.size(), 1u);
        auto mr = (*c)->merge(files, 3000);
        ASSERT_TRUE(mr) << mr.error().detail;

        // merge 后图大小 = 活节点数(同步 rebuild 已完成)
        EXPECT_EQ((*c)->vector_plugin()->size(), kN / 2);
        (*c)->close();
    }

    // 关键验证:重开时 search.ckpt 存在,图大小 = 活节点数(非全量 fold 重建)
    {
        auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
        ASSERT_TRUE(c);
        // search.ckpt 被 merge 保存,open 时走快照路径,图大小 = kN/2
        EXPECT_EQ((*c)->vector_plugin()->size(), kN / 2);
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim),
                                     10, /*ef=*/256);
        ASSERT_TRUE(r);
        // 搜索结果不应包含已删除的偶数位
        for (const auto& h : r->hits) {
            auto idx = std::stoul(h.key.substr(2));
            EXPECT_EQ(idx % 2, 1u) << "deleted key " << h.key << " in results";
        }
        (*c)->close();
    }

    // 验证 search.ckpt 文件确实存在
    {
        namespace fs = std::filesystem;
        EXPECT_TRUE(fs::exists(tmpdir_ / "bm25.ckpt"))
            << "search.ckpt should exist after merge";
    }
}

// ── V5:metadata filter 集成测试 ────────────────────────────────────────

namespace {
std::vector<std::byte> make_meta_blob(
    std::initializer_list<std::pair<std::string, bitcask::meta::MetaValue>> kvs) {
    std::vector<bitcask::meta::MetaEntry> entries;
    for (auto& [k, v] : kvs) {
        entries.push_back({k, std::move(v)});
    }
    std::vector<std::byte> blob;
    bitcask::meta::encode_meta(blob, entries);
    return blob;
}

void v5_put(Cask& c, const std::string& key, const std::string& text,
            std::span<const float> vec, std::span<const std::byte> meta) {
    bitcask::DocInput doc;
    doc.text = sv_bytes(text);
    doc.vector = vec;
    doc.meta = meta;
    ASSERT_TRUE(c.put_doc(sv_bytes(key), doc, 1000));
}

std::unique_ptr<bitcask::meta::MetaFilter> make_eq_filter(
    std::initializer_list<std::pair<std::string, bitcask::meta::MetaValue>> conds) {
    auto f = std::make_unique<bitcask::meta::MetaFilter>();
    for (auto& [k, v] : conds) {
        f->conditions.push_back({k, bitcask::meta::MetaOp::Eq, std::move(v), {}});
    }
    return f;
}
}  // namespace

// V5.1:文本搜索 + metadata filter
TEST_F(CaskDocValueTest, V5SearchTextWithMetaFilter) {
    auto opts = p3_search_opts();
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    auto m_tech = make_meta_blob({{"category", std::string("tech")}, {"year", std::int64_t(2024)}});
    auto m_sport = make_meta_blob({{"category", std::string("sport")}, {"year", std::int64_t(2023)}});
    auto m_tech2 = make_meta_blob({{"category", std::string("tech")}, {"year", std::int64_t(2023)}});

    v5_put(**c, "d1", "machine learning algorithms", {}, m_tech);
    v5_put(**c, "d2", "football world cup final", {}, m_sport);
    v5_put(**c, "d3", "deep learning neural networks", {}, m_tech2);
    v5_put(**c, "d4", "basketball playoff game", {}, m_sport);
    v5_put(**c, "d5", "cloud computing architecture", {}, m_tech);
    v5_put(**c, "d6", "swimming competition results", {}, m_sport);
    (*c)->flush_index();

    {
        auto r = (*c)->search_text("learning", 10);
        ASSERT_TRUE(r);
        EXPECT_GE(r->hits.size(), 2u);
    }

    {
        auto filter = make_eq_filter({{"category", std::string("tech")}});
        auto r = (*c)->search_text("learning", 10, filter.get());
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 2u);
        for (auto& h : r->hits) {
            EXPECT_TRUE(h.key == "d1" || h.key == "d3") << h.key;
        }
    }

    {
        auto filter = make_eq_filter({{"year", std::int64_t(2024)}});
        auto r = (*c)->search_text("learning", 10, filter.get());
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 1u);
        EXPECT_EQ(r->hits[0].key, "d1");
    }

    {
        auto filter = make_eq_filter({{"category", std::string("sport")}});
        auto r = (*c)->search_text("learning", 10, filter.get());
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 0u);
    }

    (*c)->close();
}

// V5.2:向量搜索 + metadata filter
TEST_F(CaskDocValueTest, V5SearchVectorWithMetaFilter) {
    constexpr std::size_t kDim = 8, kN = 6;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(kN, kDim, 0xBEE5);
    auto q = v35_make_vecs(1, kDim, 0xF00D)[0];

    auto m_a = make_meta_blob({{"group", std::string("a")}});
    auto m_b = make_meta_blob({{"group", std::string("b")}});

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (std::size_t i = 0; i < kN; ++i) {
        auto key = "v" + std::to_string(i);
        auto& meta = (i % 2 == 0) ? m_a : m_b;
        v5_put(**c, key, "doc " + key,
               std::span<const float>(vecs[i].data(), kDim), meta);
    }
    (*c)->flush_index();

    {
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 6, 256);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 6u);
    }

    {
        auto filter = make_eq_filter({{"group", std::string("a")}});
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 6, 256, filter.get());
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 3u);
        for (auto& h : r->hits) {
            auto idx = std::stoul(h.key.substr(1));
            EXPECT_EQ(idx % 2, 0u) << "expected group a, got " << h.key;
        }
    }

    {
        auto filter = make_eq_filter({{"group", std::string("b")}});
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 6, 256, filter.get());
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 3u);
        for (auto& h : r->hits) {
            auto idx = std::stoul(h.key.substr(1));
            EXPECT_EQ(idx % 2, 1u) << "expected group b, got " << h.key;
        }
    }

    {
        auto filter = make_eq_filter({{"group", std::string("c")}});
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 6, 256, filter.get());
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 0u);
    }

    (*c)->close();
}

// V5.3:无 meta 的文档在 filter 时被排除
TEST_F(CaskDocValueTest, V5NoMetaFilteredOut) {
    constexpr std::size_t kDim = 8;
    auto opts = v31_opts(kDim);
    auto vecs = v35_make_vecs(4, kDim, 0xCAFE);
    auto q = v35_make_vecs(1, kDim, 0xF00D)[0];
    auto m = make_meta_blob({{"tag", std::string("ok")}});

    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    v5_put(**c, "v0", "doc v0", std::span<const float>(vecs[0].data(), kDim), m);
    bitcask::DocInput doc1;
    doc1.text = sv_bytes("doc v1");
    doc1.vector = std::span<const float>(vecs[1].data(), kDim);
    ASSERT_TRUE((*c)->put_doc(sv_bytes("v1"), doc1, 1000));
    v5_put(**c, "v2", "doc v2", std::span<const float>(vecs[2].data(), kDim), m);
    bitcask::DocInput doc3;
    doc3.text = sv_bytes("doc v3");
    doc3.vector = std::span<const float>(vecs[3].data(), kDim);
    ASSERT_TRUE((*c)->put_doc(sv_bytes("v3"), doc3, 1000));
    (*c)->flush_index();

    {
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 10, 256);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->hits.size(), 4u);
    }

    {
        auto filter = make_eq_filter({{"tag", std::string("ok")}});
        auto r = (*c)->search_vector(std::span<const float>(q.data(), kDim), 10, 256, filter.get());
        ASSERT_TRUE(r);
        ASSERT_EQ(r->hits.size(), 2u);
        for (auto& h : r->hits) {
            EXPECT_TRUE(h.key == "v0" || h.key == "v2") << h.key;
        }
    }

    (*c)->close();
}

// V6.1:GetResultView 生命周期合约——给 TSan 验证零拷贝路径下没有
// 悬空访问、double-free、data race。重点：
//   1. span 生命周期 = GetResultView 实例生命周期,move 后旧 view 不能再
//      deref（编译期禁止,运行期持有 mvd-out 后的旧实例就是越界）
//   2. to_owned() 后 GetResult 完全独立于 view,可任意析构后者
//   3. 反复 put / get 在同一目录不泄漏 ReadRecord 内部 vector
//   4. view 暴露的 span.data() 在 view 析构前可安全 memcpy
TEST_F(CaskDocValueTest, V61GetResultViewLifecycle) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    const std::string payload = "tsan-lifecycle-payload-1234567890";
    std::vector<std::byte> key{std::byte{'t'}, std::byte{'s'}, std::byte{'n'}};
    std::vector<std::byte> val{payload.size(), std::byte{0}};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        val[i] = static_cast<std::byte>(payload[i]);
    }
    ASSERT_TRUE((*c)->put(key, val, 1000));

    // (1) get 返回 GetResultView,spans 指向 pread 缓冲。
    auto v1 = (*c)->get_owned(key);  // 用 owned 拷贝原始值,做 baseline
    ASSERT_TRUE(v1);
    auto gv = (*c)->get(key);
    ASSERT_TRUE(gv);
    EXPECT_EQ(gv->tstamp, 1000u);
    EXPECT_EQ(gv->ord, v1->ord);
    ASSERT_EQ(gv->value.size(), v1->value.size());
    // span → bytes 拷贝,验证 v1 (owned) 和 gv (view) 一致
    {
        std::vector<std::byte> got(gv->value.begin(), gv->value.end());
        EXPECT_EQ(got, v1->value);
    }

    // (2) move 构造:新 view 持有 storage,旧 view 不能再 deref。
    //   仅做运行时校验(编译期 = delete 已禁止复制);move 后仍可读新 view。
    GetResultView moved(std::move(*gv));
    ASSERT_EQ(moved.value.size(), v1->value.size());
    {
        std::vector<std::byte> got(moved.value.begin(), moved.value.end());
        EXPECT_EQ(got, v1->value);
    }
    // gv 已被 move-from,spans 已空(storage 已被搬走,空向量 → decode
    // 在 move-ctor 里被跳过)。这里只校验 moved 仍可正常读。
    EXPECT_TRUE(moved.ord == v1->ord);

    // (3) to_owned() 出独立副本,跟 view 解耦:析构 view 后副本仍可读。
    GetResult owned = moved.to_owned();
    EXPECT_EQ(owned.value, v1->value);
    EXPECT_EQ(owned.tstamp, v1->tstamp);
    EXPECT_EQ(owned.ord, v1->ord);
    // 强制析构 view(RAII)
    {
        GetResultView tmp = std::move(moved);
        (void)tmp;
    }
    // owned 仍持有完整数据
    EXPECT_EQ(owned.value, v1->value);

    // (4) 反复 put / get 不应泄漏 ReadRecord 内部 vector;同时校验
    //     新 span 不指向旧 storage(避免假阳性)。
    for (int i = 0; i < 50; ++i) {
        std::vector<std::byte> k{std::byte{'k'}};
        k.push_back(static_cast<std::byte>('0' + (i % 10)));
        std::vector<std::byte> v{payload.size(), std::byte{0}};
        for (std::size_t j = 0; j < payload.size(); ++j) {
            v[j] = static_cast<std::byte>(payload[j]);
        }
        ASSERT_TRUE((*c)->put(k, v, 2000 + static_cast<std::uint32_t>(i)));
        auto g = (*c)->get(k);
        ASSERT_TRUE(g);
        std::vector<std::byte> got(g->value.begin(), g->value.end());
        EXPECT_EQ(got, v);
    }

    (*c)->close();
}

// V6.1:并发读 view 不应触发 data race——get() 路径全程无锁,pread 本身
// thread-safe；TSan 应保持静默。仅当 vector_dim 配置开启时
// 才有额外 search 路径参与,这里只跑纯 KV。
TEST_F(CaskDocValueTest, V61GetResultViewConcurrentReaders) {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);

    constexpr int kKeys = 64;
    for (int i = 0; i < kKeys; ++i) {
        std::vector<std::byte> k{std::byte{'k'}};
        // 唯一 key:k<i>——避免 i=0 (sum=0) 与 i=26 之类的碰撞。
        std::string s = "k" + std::to_string(i);
        for (char ch : s) k.push_back(static_cast<std::byte>(ch));
        std::vector<std::byte> v(64, std::byte{static_cast<std::byte>((i + 1) & 0xFF)});
        ASSERT_TRUE((*c)->put(k, v, 3000 + static_cast<std::uint32_t>(i)));
    }

    constexpr int kThreads = 4;
    constexpr int kIters = 200;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t]() {
            for (int it = 0; it < kIters; ++it) {
                int idx = (t * kIters + it) % kKeys;
                std::vector<std::byte> k{std::byte{'k'}};
                std::string s = "k" + std::to_string(idx);
                for (char ch : s) k.push_back(static_cast<std::byte>(ch));
                auto g = (*c)->get(k);
                if (!g) { errors.fetch_add(1); continue; }
                if (g->value.size() != 64) { errors.fetch_add(1); continue; }
                std::uint32_t sum = 0;
                for (auto b : g->value) {
                    sum += static_cast<std::uint8_t>(b);
                }
                // value 字节恒为 (idx+1),sum == 64*(idx+1)
                std::uint32_t expected = 64u * static_cast<std::uint32_t>(idx + 1);
                if (sum != expected) { errors.fetch_add(1); }
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(errors.load(), 0);

    (*c)->close();
}

// V6.1.4 batch fold:next_batch 一次取最多 max_n 条 entry。
// 在 5 条数据上验证 3+2+EOI 三段边界 + 内容正确。iter 的顺序
// 不保证按 key 字典序（实测按 keydir 内部顺序），所以用 map 收集后再核对。
TEST(V61BatchFold, NextBatchReturnsMultiple) {
    namespace fs = std::filesystem;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    auto tmpdir = fs::temp_directory_path() /
                  (std::string("bitcask_v61batchfold_") + info->name());
    std::error_code ec;
    fs::remove_all(tmpdir, ec);
    fs::create_directories(tmpdir, ec);

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    auto& cask = **c;

    // 5 个 key/value（key1..key5 → "v1".."v5"）。
    std::map<std::vector<std::byte>, std::vector<std::byte>> expected;
    for (int i = 1; i <= 5; ++i) {
        std::vector<std::byte> k{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}};
        for (char ch : std::to_string(i)) k.push_back(static_cast<std::byte>(ch));
        std::vector<std::byte> v{std::byte{'v'}};
        for (char ch : std::to_string(i)) v.push_back(static_cast<std::byte>(ch));
        ASSERT_TRUE(cask.put(k, v, 1000u + static_cast<std::uint32_t>(i)));
        expected[k] = v;
    }

    // 启 iter。
    auto it = cask.make_iter();
    auto sr = it->start();
    ASSERT_TRUE(sr);
    ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);

    // 用 map 收集 next_batch 返回的内容（顺序无关）。
    std::map<std::vector<std::byte>, std::vector<std::byte>> got;

    // 第一次：next_batch(3) → 3 条。
    auto r1 = it->next_batch(3);
    ASSERT_TRUE(r1);
    ASSERT_EQ(r1->size(), 3u);
    for (const auto& e : *r1) {
        EXPECT_FALSE(e.is_tombstone);
        got[e.key] = e.value;
    }

    // 第二次：next_batch(3) → 只剩 2 条（EOI 触发 break）。
    auto r2 = it->next_batch(3);
    ASSERT_TRUE(r2);
    ASSERT_EQ(r2->size(), 2u);
    for (const auto& e : *r2) {
        EXPECT_FALSE(e.is_tombstone);
        got[e.key] = e.value;
    }

    // 第三次：next_batch(3) → 空 vector = EOI。
    auto r3 = it->next_batch(3);
    ASSERT_TRUE(r3);
    EXPECT_TRUE(r3->empty());

    it->release();
    cask.close();

    EXPECT_EQ(got, expected);
    fs::remove_all(tmpdir, ec);
}

// S31.5(下游 zhwiki 反馈跟进):自动 checkpoint 的锚点改为 ord 增量本身
// ——无数据文件 roll、无 close,写满阈值后 ckpt 也必须自动落盘。分词是
// 恢复重放的主成本,窗口必须钳在 min_docs 内,不能依赖字节锚点(大文档
// 语料下 roll 周期与 analyze 工作量完全脱钩)。
TEST_F(CaskDocValueTest, S31AutoCheckpointByOrdDeltaWithoutRoll) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    opts.auto_checkpoint_min_docs = 16;  // 小阈值:60 篇内必然多次跨越
    // max_file_size 保持默认(大)——全程**不发生 roll**,锚点只剩 ord 增量。
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 60; ++i) {
        bitcask::DocInput doc;
        std::string text = "autockpt corpus n" + std::to_string(i);
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("ak_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(1000 + i)));
    }
    // 排干 reducer(fire-and-forget 的 ckpt RunFn 在 reducer 序列内执行)。
    (*c)->flush_index();
    // 关键断言:无 roll 无 close,ckpt 文件已在盘上。
    EXPECT_TRUE(fs::exists(tmpdir_ / "bm25.ckpt"))
        << "ord 增量锚点未触发自动 checkpoint";
    EXPECT_TRUE(fs::exists(tmpdir_ / "index.manifest"));

    // crash-image:未 close 的镜像重开——已 ckpt 的部分免重放,尾部由
    // fold 补,全量可检索。
    auto img = crash_image(tmpdir_, "autockpt");
    (*c)->close();
    auto r = Cask::open(img.string(), opts, &test_registry());
    ASSERT_TRUE(r);
    auto hits = (*r)->search_text("autockpt", 100);
    ASSERT_TRUE(hits);
    EXPECT_EQ(hits->hits.size(), 60u);
    (*r)->close();
    std::error_code ec;
    fs::remove_all(img, ec);
}

// 对照:auto_checkpoint_min_docs=0(关闭)→ 无 roll 无 close 恒不落 ckpt
// (旧行为可回退)。
TEST_F(CaskDocValueTest, S31AutoCheckpointDisabledNoFiles) {
    namespace fs = std::filesystem;
    auto opts = v31_opts(0);
    opts.auto_checkpoint_min_docs = 0;
    auto c = Cask::open(tmpdir_.string(), opts, &test_registry());
    ASSERT_TRUE(c);
    for (int i = 0; i < 40; ++i) {
        bitcask::DocInput doc;
        std::string text = "noauto n" + std::to_string(i);
        doc.text = sv_bytes(text);
        ASSERT_TRUE((*c)->put_doc(sv_bytes("na_" + std::to_string(i)), doc,
                                  static_cast<std::uint32_t>(1000 + i)));
    }
    (*c)->flush_index();
    EXPECT_FALSE(fs::exists(tmpdir_ / "bm25.ckpt"));
    (*c)->close();
}
