// S75（反馈 2026-11-23-keydir-scan 第 3 条）：CaskIter::next_batch_view 回归。
//
// 契约（本文件锁住）：
//   1. 与 owned 版（next / next_batch）逐字段一致——同一快照、同一过滤；
//   2. 借用的切片跨 Cask::close() 仍可读（映射被钉住 / 记录留在迭代器里）；
//   3. owned 与 view 两种推进调用可以混用，拼起来仍是同一份快照；
//   4. 墓碑行的 key 落迭代器内的扁平缓冲——批内缓冲增长（realloc）不得让
//      先填的 key 悬垂（整批填完再回填 span）；
//   5. 重开后（封口文件：Linux 走映射、Windows 走 pread 兜底）照常；
//   6. 空库 / 未 start / max_n == 0 交空批，不报错。
//
// 平台分工：Windows 上活跃文件没有 growing map，刚写完的库全部走 pread +
// ReadRecord holder；映射零拷贝路径（value 直达文件映射）由 Linux CI 覆盖。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "bitcask/cask.hpp"
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

std::string to_str(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
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

struct IterOpts {
    bool see_tombstones = false;
    bool want_meta = false;
    std::string prefix;
    const meta::MetaFilter* filter = nullptr;
};

template <class T>
void fail_if_err(const std::expected<T, CaskFault>& r) {
    if (!r) ADD_FAILURE() << r.error().detail;
}

// 一条 entry 的值拷贝（对拍用——view 的切片只在当批有效，必须当场拷出）。
struct Rec {
    std::string key;
    std::string value;
    std::string meta;
    std::uint64_t tstamp = 0;
    std::uint32_t file_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t total_sz = 0;
    bool tomb = false;
    std::uint64_t ord = 0;
};

class IterBatchViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string name =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = fs::temp_directory_path() / ("bitcask_iter_batch_view_" + name);
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        open();
    }
    void TearDown() override {
        if (cask_) cask_->close();
        cask_.reset();
        fs::remove_all(dir_);
    }

    void open() {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(c) << c.error().detail;
        cask_ = std::move(*c);
    }

    // 值长度拉开档次：从 1 B 到 300 B，覆盖映射读与 pread 兜底的小 / 大值。
    static std::string value_of(int i) {
        const std::size_t n = static_cast<std::size_t>(i) % 300 + 1;
        return std::string(n, static_cast<char>('a' + i % 26));
    }
    static std::string plain_key(int i) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "k%04d", i);
        return buf;
    }

    // 300 个普通 key，其中前 100 个里每 7 个删 1 个（OKI 墓碑行）。
    void populate_plain() {
        for (int i = 0; i < 300; ++i) {
            const auto v = value_of(i);
            ASSERT_TRUE(cask_->put(sv_bytes(plain_key(i)), sv_bytes(v), 1000 + i));
        }
        for (int i = 0; i < 100; i += 7) {
            ASSERT_TRUE(cask_->remove(sv_bytes(plain_key(i)), 2000 + i));
        }
    }

    // 40 篇带 meta 的文档（前缀 m），每 4 篇 1 篇 tag=keep。
    void populate_docs() {
        for (int i = 0; i < 40; ++i) {
            DocInput doc;
            const std::string text = "doc text number " + std::to_string(i);
            doc.text = sv_bytes(text);
            const auto blob = tag_blob(i % 4 == 0 ? "keep" : "drop");
            doc.meta = std::span<const std::byte>(blob);
            ASSERT_TRUE(cask_->put_doc(sv_bytes("m" + std::to_string(i)), doc,
                                       3000 + i));
        }
    }

    std::unique_ptr<CaskIter> make_iter(const IterOpts& o) {
        auto it = cask_->make_iter();
        it->set_want_meta(o.want_meta);
        it->set_filter(o.filter);
        EXPECT_TRUE(it->start(-1, -1, 0, o.see_tombstones, sv_bytes(o.prefix)));
        return it;
    }

    std::vector<Rec> drain_owned(const IterOpts& o) {
        std::vector<Rec> out;
        auto it = make_iter(o);
        while (true) {
            auto e = it->next();
            fail_if_err(e);
            if (!e || !*e) break;
            out.push_back(rec_of(**e));
        }
        return out;
    }

    std::vector<Rec> drain_view(std::size_t batch, const IterOpts& o) {
        std::vector<Rec> out;
        auto it = make_iter(o);
        while (true) {
            auto b = it->next_batch_view(batch);
            fail_if_err(b);
            if (!b || b->empty()) break;
            for (const auto& e : *b) out.push_back(rec_of(e));
        }
        return out;
    }

    static Rec rec_of(const CaskIter::Entry& e) {
        Rec r;
        r.key = to_str(e.key);
        r.value = to_str(e.value);
        r.meta = to_str(e.meta);
        r.tstamp = e.tstamp;
        r.file_id = e.file_id;
        r.offset = e.offset;
        r.total_sz = e.total_sz;
        r.tomb = e.is_tombstone;
        r.ord = e.ord;
        return r;
    }
    static Rec rec_of(const CaskIter::EntryView& e) {
        Rec r;
        r.key = to_str(e.key);
        r.value = to_str(e.value);
        r.meta = to_str(e.meta);
        r.tstamp = e.tstamp;
        r.file_id = e.file_id;
        r.offset = e.offset;
        r.total_sz = e.total_sz;
        r.tomb = e.is_tombstone;
        r.ord = e.ord;
        return r;
    }

    static void expect_same(const std::vector<Rec>& want,
                            const std::vector<Rec>& got) {
        ASSERT_EQ(got.size(), want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            SCOPED_TRACE("entry " + std::to_string(i));
            EXPECT_EQ(got[i].key, want[i].key);
            EXPECT_EQ(got[i].value, want[i].value);
            EXPECT_EQ(got[i].meta, want[i].meta);
            EXPECT_EQ(got[i].tstamp, want[i].tstamp);
            EXPECT_EQ(got[i].file_id, want[i].file_id);
            EXPECT_EQ(got[i].offset, want[i].offset);
            EXPECT_EQ(got[i].total_sz, want[i].total_sz);
            EXPECT_EQ(got[i].tomb, want[i].tomb);
            EXPECT_EQ(got[i].ord, want[i].ord);
        }
    }

    fs::path dir_;
    std::unique_ptr<Cask> cask_;
};

// 墓碑行的 key 长度参差，逼扁平缓冲在一批之内多次增长。
TEST_F(IterBatchViewTest, ScratchKeyGrowthWithinBatchKeepsEarlierKeys) {
    for (int i = 0; i < 200; ++i) {
        std::string key(3 + i, 'x');  // 3..202 字节，互不相同
        key.push_back(static_cast<char>('0' + i % 10));
        ASSERT_TRUE(cask_->put(sv_bytes(key), sv_bytes("v"), 10));
        ASSERT_TRUE(cask_->remove(sv_bytes(key), 20));
    }
    const IterOpts o{.see_tombstones = true};
    const auto want = drain_owned(o);
    // 自证非空：see_tombstones 须透传到归并层（否则墓碑在归并层被抵消，
     // 下面的对拍会空转通过）。
    ASSERT_EQ(want.size(), 200u);
    for (const auto& r : want) ASSERT_TRUE(r.tomb);
    auto it = make_iter(o);
    auto batch = it->next_batch_view(256);  // 全部墓碑进同一批
    fail_if_err(batch);
    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        SCOPED_TRACE("entry " + std::to_string(i));
        EXPECT_EQ(to_str((*batch)[i].key), want[i].key);
        EXPECT_TRUE((*batch)[i].is_tombstone);
        EXPECT_TRUE((*batch)[i].value.empty());
    }
}

TEST_F(IterBatchViewTest, ParityWithOwnedAcrossBatchSizes) {
    populate_plain();
    populate_docs();
    for (const bool tombs : {false, true}) {
        const IterOpts o{.see_tombstones = tombs};
        const auto want = drain_owned(o);
        ASSERT_FALSE(want.empty());
        if (tombs) {
            EXPECT_EQ(std::count_if(want.begin(), want.end(),
                                    [](const Rec& r) { return r.tomb; }),
                      15);
        } else {
            EXPECT_EQ(want.size(), 300u - 15 + 40);
        }
        for (const std::size_t batch : {1u, 3u, 8u, 64u, 1000u}) {
            SCOPED_TRACE("batch " + std::to_string(batch));
            expect_same(want, drain_view(batch, o));
        }
    }
}

TEST_F(IterBatchViewTest, ParityWithMetaFilterAndPrefix) {
    populate_plain();
    populate_docs();
    const auto keep = tag_filter("keep");
    for (const IterOpts& o : std::vector<IterOpts>{
             IterOpts{.want_meta = true},
             IterOpts{.want_meta = true, .filter = &keep},
             IterOpts{.filter = &keep},
             IterOpts{.want_meta = true, .prefix = "m"},
             IterOpts{.want_meta = true, .prefix = "m3", .filter = &keep},
         }) {
        const auto want = drain_owned(o);
        expect_same(want, drain_view(4, o));
    }
    // 过滤确实生效：只留 keep 的那 10 篇，且 meta 非 filter 时为空。
    const IterOpts filtered{.filter = &keep};
    const auto kept = drain_view(4, filtered);
    ASSERT_EQ(kept.size(), 10u);
    for (const auto& r : kept) {
        EXPECT_TRUE(r.meta.empty());  // want_meta 关 ⇒ meta 恒空
    }
    const IterOpts filtered_with_meta{.want_meta = true, .filter = &keep};
    for (const auto& r : drain_view(4, filtered_with_meta)) {
        EXPECT_EQ(r.meta, to_str(tag_blob("keep")));
    }
}

TEST_F(IterBatchViewTest, ViewsStayReadableAfterClose) {
    populate_plain();
    for (int i = 0; i < 30; ++i) {  // 批里也要有墓碑行（key 在迭代器的缓冲里）
        const auto k = plain_key(500 + i);
        ASSERT_TRUE(cask_->put(sv_bytes(k), sv_bytes("v"), 10));
        ASSERT_TRUE(cask_->remove(sv_bytes(k), 20));
    }
    const IterOpts o{.see_tombstones = true};
    const auto want = drain_owned(o);
    auto it = make_iter(o);
    auto b = it->next_batch_view(16);
    fail_if_err(b);
    ASSERT_TRUE(b);
    ASSERT_EQ(b->size(), 16u);
    // close 之后再读 value——验证借用切片不依赖 Cask 的打开状态。Cask 对象
    // 本身须活过迭代器（裸 parent_ 契约），所以这里只 close、不 reset。
    std::vector<const CaskIter::EntryView*> views;
    for (const auto& e : *b) views.push_back(&e);
    cask_->close();
    std::vector<Rec> seen;
    for (const auto* e : views) seen.push_back(rec_of(*e));
    it.reset();
    // 只取了一批 16 条：对拍前缀（want 本身是全量）。
    expect_same(std::vector<Rec>(want.begin(), want.begin() + 16), seen);
}

TEST_F(IterBatchViewTest, MixingOwnedAndViewAdvancesSameSnapshot) {
    populate_plain();
    const IterOpts o{.see_tombstones = true};
    const auto want = drain_owned(o);
    std::vector<Rec> got;
    auto it = make_iter(o);
    int round = 0;
    while (true) {
        if (round++ % 3 == 0) {
            auto e = it->next();  // owned 推进：上一批 view 按契约失效
            fail_if_err(e);
            if (!e || !*e) break;
            got.push_back(rec_of(**e));
        } else if (round % 3 == 1) {
            auto b = it->next_batch_view(5);
            fail_if_err(b);
            if (!b || b->empty()) break;
            for (const auto& e : *b) got.push_back(rec_of(e));
        } else {
            auto v = it->next_batch(2);
            fail_if_err(v);
            if (!v || v->empty()) break;
            for (const auto& e : *v) got.push_back(rec_of(e));
        }
    }
    expect_same(want, got);
}

TEST_F(IterBatchViewTest, WorksAfterReopen) {
    populate_plain();
    cask_->close();
    open();
    const IterOpts o{.see_tombstones = true};
    expect_same(drain_owned(o), drain_view(7, o));
}

TEST_F(IterBatchViewTest, EmptyNotStartedAndZeroMaxN) {
    // 空库
    expect_same(drain_owned(IterOpts{}), drain_view(8, IterOpts{}));
    populate_plain();
    // 未 start：空批，不报错
    {
        auto it = cask_->make_iter();
        auto b = it->next_batch_view(8);
        ASSERT_TRUE(b);
        EXPECT_TRUE(b->empty());
    }
    // max_n == 0：空批
    {
        auto it = make_iter(IterOpts{});
        auto b = it->next_batch_view(0);
        ASSERT_TRUE(b);
        EXPECT_TRUE(b->empty());
        auto rest = it->next_batch_view(8);  // 之前的空调用不推进
        ASSERT_TRUE(rest);
        EXPECT_EQ(rest->size(), 8u);
    }
}

}  // namespace
