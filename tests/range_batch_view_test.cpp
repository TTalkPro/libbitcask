// S75-3（反馈 2026-11-23-keydir-scan 第 3 条，range 半边）：
// CaskRangeIter::next_batch_view 回归。
//
// 契约（本文件锁住）：
//   1. 与 owned 版（next）逐字段一致——同一快照、同一 lo/hi / filter / want_meta，
//      惰性与预取（自适应串行 / 显式并行）各条路径都一样；
//   2. 与 next() 可以混用：预取缓冲共用，拼起来不丢不乱序；
//   3. 借用的切片跨 Cask::close() 仍可读（映射被钉住 / 记录留在迭代器里），
//      close 之后再推进报 kClosed；
//   4. 快照版本被 merge 合并掉的 key 退回 public get、交当前值，key 落迭代器的
//      扁平缓冲——批内缓冲增长（realloc）不得让先填的 key 悬垂；
//   5. 重开后（封口文件：Linux 走映射、Windows 走 pread 兜底）照常；
//   6. 空库 / max_n == 0 交空批，不报错。
//
// 平台分工：Windows 上活跃文件没有 growing map，刚写完的库全部走 pread 兜底；
// 映射零拷贝路径由 Linux CI 覆盖。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/index_registry.hpp"
#include "bitcask/meta_codec.hpp"

using namespace bitcask;
namespace fs = std::filesystem;

namespace {

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry reg;
    return reg;
}

std::span<const std::byte> sv_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string to_str(std::span<const std::byte> s) {
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::vector<std::byte> tag_blob(const std::string& tag) {
    std::vector<meta::MetaEntry> entries{{"tag", meta::MetaValue(tag)}};
    std::vector<std::byte> blob;
    meta::encode_meta(blob, entries);
    return blob;
}

meta::MetaFilter tag_filter(const std::string& tag) {
    meta::MetaFilter f;
    f.conditions.push_back({"tag", meta::MetaOp::Eq, meta::MetaValue(tag), {}});
    return f;
}

// 一条 entry 的值拷贝（对拍用——view 的切片只在当批有效，必须当场拷出）。
struct Rec {
    std::string key;
    std::string value;
    std::string meta;
    std::uint64_t tstamp = 0;
    std::uint64_t ord = 0;
};

Rec rec_of(const CaskRangeIter::Entry& e) {
    return {to_str(e.key), to_str(e.value), to_str(e.meta), e.tstamp, e.ord};
}
Rec rec_of(const CaskRangeIter::EntryView& e) {
    return {to_str(e.key), to_str(e.value), to_str(e.meta), e.tstamp, e.ord};
}

void expect_same(const std::vector<Rec>& want, const std::vector<Rec>& got) {
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        SCOPED_TRACE("entry " + std::to_string(i));
        EXPECT_EQ(got[i].key, want[i].key);
        EXPECT_EQ(got[i].value, want[i].value);
        EXPECT_EQ(got[i].meta, want[i].meta);
        EXPECT_EQ(got[i].tstamp, want[i].tstamp);
        EXPECT_EQ(got[i].ord, want[i].ord);
    }
}

std::vector<Rec> drain_owned(CaskRangeIter& it) {
    std::vector<Rec> out;
    while (true) {
        auto e = it.next();
        if (!e) {
            ADD_FAILURE() << e.error().detail;
            break;
        }
        if (!*e) break;
        out.push_back(rec_of(**e));
    }
    return out;
}

std::vector<Rec> drain_view(CaskRangeIter& it, std::size_t batch) {
    std::vector<Rec> out;
    while (true) {
        auto b = it.next_batch_view(batch);
        if (!b) {
            ADD_FAILURE() << b.error().detail;
            break;
        }
        if (b->empty()) break;
        EXPECT_LE(b->size(), batch);
        for (const auto& e : *b) out.push_back(rec_of(e));
    }
    return out;
}

class RangeBatchViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string name =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = fs::temp_directory_path() / ("bitcask_range_batch_view_" + name);
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        if (cask_) cask_->close();
        cask_.reset();
        fs::remove_all(dir_);
    }

    void open(std::uint64_t max_file_size = 0) {
        CaskOptions opts;
        opts.read_write = true;
        if (max_file_size != 0) opts.max_file_size = max_file_size;
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        cask_ = std::move(*c);
    }

    static std::string key_of(int i) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "k%04d", i);
        return buf;
    }
    // 值长度拉开档次：1～300 B。
    static std::string value_of(int i) {
        return std::string(static_cast<std::size_t>(i) % 300 + 1,
                           static_cast<char>('a' + i % 26));
    }

    // 300 个普通 key（前 100 个里每 7 个删 1 个）+ 40 篇带 meta 的文档
    // （前缀 m，每 4 篇 1 篇 tag=keep）。
    void populate() {
        for (int i = 0; i < 300; ++i) {
            ASSERT_TRUE(cask_->put(sv_bytes(key_of(i)), sv_bytes(value_of(i)),
                                   static_cast<std::uint64_t>(1000 + i)));
        }
        for (int i = 0; i < 100; i += 7) {
            ASSERT_TRUE(cask_->remove(sv_bytes(key_of(i)),
                                      static_cast<std::uint64_t>(2000 + i)));
        }
        for (int i = 0; i < 40; ++i) {
            DocInput doc;
            const std::string text = "doc text number " + std::to_string(i);
            doc.text = sv_bytes(text);
            const auto blob = tag_blob(i % 4 == 0 ? "keep" : "drop");
            doc.meta = std::span<const std::byte>(blob);
            ASSERT_TRUE(cask_->put_doc(sv_bytes("m" + std::to_string(i)), doc,
                                       static_cast<std::uint64_t>(3000 + i)));
        }
    }

    std::unique_ptr<CaskRangeIter> make(const RangeOptions& ro) {
        auto it = cask_->make_range_iter(ro);
        EXPECT_TRUE(it) << (it ? "" : it.error().detail);
        return it ? std::move(*it) : nullptr;
    }

    // 各条取值路径：惰性、预取（自适应，热缓存下多半串行）、预取显式 4 线程。
    struct Path {
        std::size_t prefetch;
        std::size_t threads;
    };
    static constexpr Path kPaths[] = {{0, 0}, {16, 0}, {256, 0}, {256, 4}};

    void check_parity(RangeOptions ro, const char* what) {
        for (const auto& p : kPaths) {
            ro.prefetch = p.prefetch;
            ro.prefetch_threads = p.threads;
            auto ref = make(ro);
            ASSERT_TRUE(ref);
            const auto want = drain_owned(*ref);
            ASSERT_FALSE(want.empty()) << what;
            for (std::size_t batch : {std::size_t{1}, std::size_t{7},
                                      std::size_t{64}, std::size_t{1000}}) {
                SCOPED_TRACE(std::string(what) + " prefetch=" + std::to_string(p.prefetch) +
                             " threads=" + std::to_string(p.threads) +
                             " batch=" + std::to_string(batch));
                auto it = make(ro);
                ASSERT_TRUE(it);
                expect_same(want, drain_view(*it, batch));
            }
        }
    }

    fs::path dir_;
    std::unique_ptr<Cask> cask_;
};

TEST_F(RangeBatchViewTest, ParityWithOwnedAcrossPathsAndBatchSizes) {
    open();
    populate();
    check_parity(RangeOptions{}, "full");

    RangeOptions window;
    window.lo = sv_bytes("k0050");
    window.hi = sv_bytes("k0250");
    check_parity(window, "window");

    const auto f = tag_filter("keep");
    RangeOptions docs;
    docs.lo = sv_bytes("m");
    docs.hi = sv_bytes("n");
    docs.want_meta = true;
    docs.filter = &f;
    check_parity(docs, "meta+filter");
    // filter 生效：40 篇里只有 10 篇 keep，且带 meta
    auto it = make(docs);
    ASSERT_TRUE(it);
    const auto got = drain_view(*it, 64);
    EXPECT_EQ(got.size(), 10u);
    for (const auto& r : got) EXPECT_FALSE(r.meta.empty()) << r.key;
}

TEST_F(RangeBatchViewTest, MixingOwnedAndViewKeepsOrder) {
    open();
    populate();
    for (const auto& p : kPaths) {
        SCOPED_TRACE("prefetch=" + std::to_string(p.prefetch));
        RangeOptions ro;
        ro.prefetch = p.prefetch;
        ro.prefetch_threads = p.threads;
        auto ref = make(ro);
        ASSERT_TRUE(ref);
        const auto want = drain_owned(*ref);

        auto it = make(ro);
        ASSERT_TRUE(it);
        std::vector<Rec> got;
        for (bool use_view = false;; use_view = !use_view) {
            if (use_view) {
                auto b = it->next_batch_view(5);
                ASSERT_TRUE(b) << b.error().detail;
                if (b->empty()) break;
                for (const auto& e : *b) got.push_back(rec_of(e));
            } else {
                auto e = it->next();
                ASSERT_TRUE(e) << e.error().detail;
                if (!*e) break;
                got.push_back(rec_of(**e));
            }
        }
        expect_same(want, got);
    }
}

TEST_F(RangeBatchViewTest, ViewsStayReadableAfterClose) {
    open();
    populate();
    for (const auto& p : kPaths) {
        SCOPED_TRACE("prefetch=" + std::to_string(p.prefetch));
        RangeOptions ro;
        ro.prefetch = p.prefetch;
        ro.prefetch_threads = p.threads;
        auto it = make(ro);
        ASSERT_TRUE(it);
        auto b = it->next_batch_view(50);
        ASSERT_TRUE(b) << b.error().detail;
        ASSERT_EQ(b->size(), 50u);
        std::vector<Rec> before;
        for (const auto& e : *b) before.push_back(rec_of(e));

        cask_->close();
        std::vector<Rec> after;
        for (const auto& e : *b) after.push_back(rec_of(e));
        expect_same(before, after);

        auto more = it->next_batch_view(50);
        ASSERT_FALSE(more.has_value());
        EXPECT_EQ(more.error().kind, CaskError::kClosed);
        it.reset();
        cask_.reset();
        open();  // 下一条路径
    }
}

// 快照版本被 merge 合并掉（迭代期间两次 merge：第一次合并、第二次删掉上一代
// 输入文件）⇒ 退回 public get 交当前值，key 借不到记录、落扁平缓冲。key 长度 1～200 B（大多超出 SSO）全塞在一批里，逼缓冲在批内多次增长。
TEST_F(RangeBatchViewTest, SupersededKeysFallBackToGetAndKeepKeys) {
    open(/*max_file_size=*/4096);  // 小文件：写入滚出多个封口文件，merge 才有输入
    constexpr int kKeys = 120;
    const auto long_key = [](int i) {
        char head[16];
        std::snprintf(head, sizeof head, "L%03d", i);
        return std::string(head) + std::string(static_cast<std::size_t>(i * 37 % 200),
                                               static_cast<char>('a' + i % 26));
    };
    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE(cask_->put(sv_bytes(long_key(i)), sv_bytes("old" + std::to_string(i))));
    }
    // 每条路径各一个迭代器（快照建在改写之前），owned 对照一个。
    std::vector<std::unique_ptr<CaskRangeIter>> views;
    for (const auto& p : kPaths) {
        RangeOptions ro;
        ro.prefetch = p.prefetch;
        ro.prefetch_threads = p.threads;
        views.push_back(make(ro));
        ASSERT_TRUE(views.back());
    }
    auto ref = make(RangeOptions{});
    ASSERT_TRUE(ref);

    for (int i = 0; i < kKeys; ++i) {
        ASSERT_TRUE(cask_->put(sv_bytes(long_key(i)), sv_bytes("new" + std::to_string(i))));
    }
    // 封口活跃文件后显式合并全部数据文件（files 为空时走 needs_merge 的缺省门槛，
    // 这点数据量不会触发）：旧版本全是死记录，被丢弃、所在文件被删。
    ASSERT_TRUE(cask_->close_write_file());
    std::vector<std::string> files;
    for (const auto& de : fs::directory_iterator(dir_)) {
        if (fileops::parse_data_tstamp(de.path().filename().string())) {
            files.push_back(de.path().string());
        }
    }
    ASSERT_GE(files.size(), 2u) << "max_file_size 没让写入滚出多个文件";
    auto m = cask_->merge(files);
    ASSERT_TRUE(m) << m.error().detail;
    // 输入文件先进退休队列，到下一个落点才删（B4：按代延迟，不看在途读者）。
    // 再来一次 merge——入口先清上一代——这是长迭代期间撞上两次 merge 的真实形态，
    // 之后快照里的旧版本才真正读不到。
    auto m2 = cask_->merge();
    ASSERT_TRUE(m2) << m2.error().detail;
    for (const auto& f : files) {
        EXPECT_FALSE(fs::exists(f)) << f << " 第二次 merge 后应已删除";
    }

    const auto want = drain_owned(*ref);
    ASSERT_EQ(want.size(), static_cast<std::size_t>(kKeys));
    std::size_t current = 0;
    for (const auto& r : want) current += r.value.rfind("new", 0) == 0 ? 1u : 0u;
    // 合并掉的旧版本按 S76 的例外交当前值；至少要真走到这条路，测试才有意义。
    ASSERT_GT(current, 0u) << "merge 没有合并掉任何快照版本，没覆盖到退回 get 的路径";

    for (std::size_t i = 0; i < views.size(); ++i) {
        SCOPED_TRACE("prefetch=" + std::to_string(kPaths[i].prefetch) +
                     " threads=" + std::to_string(kPaths[i].threads));
        expect_same(want, drain_view(*views[i], /*batch=*/1000));
    }
}

TEST_F(RangeBatchViewTest, WorksAfterReopen) {
    open();
    populate();
    cask_->close();
    cask_.reset();
    open();
    check_parity(RangeOptions{}, "reopened");
}

TEST_F(RangeBatchViewTest, EmptyAndZeroMaxN) {
    open();
    auto it = make(RangeOptions{});
    ASSERT_TRUE(it);
    auto b = it->next_batch_view(16);
    ASSERT_TRUE(b) << b.error().detail;
    EXPECT_TRUE(b->empty());

    populate();
    auto it2 = make(RangeOptions{});
    ASSERT_TRUE(it2);
    auto z = it2->next_batch_view(0);
    ASSERT_TRUE(z) << z.error().detail;
    EXPECT_TRUE(z->empty());
    auto one = it2->next_batch_view(1);  // max_n == 0 不推进
    ASSERT_TRUE(one) << one.error().detail;
    ASSERT_EQ(one->size(), 1u);
    EXPECT_EQ(to_str((*one)[0].key), "k0001");  // k0000 已删
}

}  // namespace
