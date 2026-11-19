// Unit tests for the in-memory Index side tables (doc/vector-db-design-zh.md §3.1).

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "bitcask/index.hpp"
#include "bitcask/meta_codec.hpp"

using bitcask::index::DocLoc;
using bitcask::index::DocSlot;
using bitcask::index::Index;

namespace {

DocSlot slot(std::uint32_t file_id, std::uint64_t off, std::uint32_t sz,
             std::uint32_t ts = 0) {
    return DocSlot{DocLoc{.offset = off, .file_id = file_id, .total_sz = sz},
                   ts, /*doc_len*/ 0};
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> b;
    b.reserve(s.size());
    for (const char c : s) {
        b.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return b;
}

// S42-4：reference model——ord/ext/live/doc_len/meta 全量对拍。
struct RefDoc {
    std::uint64_t ord = 0;
    std::uint32_t doc_len = 0;
    std::vector<std::byte> meta;
};

void verify_model(const Index& idx,
                  const std::unordered_map<std::string, RefDoc>& model,
                  const std::vector<std::uint64_t>& dead_ords,
                  bool check_meta) {
    for (const auto& [key, d] : model) {
        auto hit = idx.get(key);
        ASSERT_TRUE(hit.has_value()) << key;
        EXPECT_EQ(hit->ord, d.ord) << key;
        EXPECT_EQ(idx.ord_of(key), std::optional<std::uint64_t>(d.ord)) << key;
        EXPECT_TRUE(idx.is_live(d.ord)) << key;
        EXPECT_EQ(idx.doc_len(d.ord), d.doc_len) << key;
        auto e = idx.ord_to_ext(d.ord);
        ASSERT_TRUE(e.has_value());
        EXPECT_EQ(*e, key);
        if (check_meta) {
            EXPECT_EQ(idx.meta_blob(d.ord), d.meta) << key;
        }
    }
    for (const auto ord : dead_ords) {
        EXPECT_FALSE(idx.is_live(ord)) << ord;
    }
}

}  // namespace

TEST(Index, AllocOrdMonotonic) {
    Index idx;
    EXPECT_EQ(idx.alloc_ord(), 0u);
    EXPECT_EQ(idx.alloc_ord(), 1u);
    EXPECT_EQ(idx.alloc_ord(), 2u);
    EXPECT_EQ(idx.info().next_ord, 3u);
}

TEST(Index, InsertThenGet) {
    Index idx;
    const std::uint64_t ord = idx.alloc_ord();
    idx.put_doc("doc-1", ord, slot(1, 100, 50, 42));

    auto s = idx.get("doc-1");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->loc.file_id, 1u);
    EXPECT_EQ(s->loc.offset, 100u);
    EXPECT_EQ(s->loc.total_sz, 50u);
    EXPECT_EQ(s->tstamp, 42u);
    EXPECT_TRUE(idx.is_live(ord));

    auto info = idx.info();
    EXPECT_EQ(info.live_docs, 1u);

    EXPECT_FALSE(idx.get("missing").has_value());
}

TEST(Index, UpdateSoftDeletesOldOrd) {
    Index idx;
    const std::uint64_t o1 = idx.alloc_ord();
    idx.put_doc("doc-1", o1, slot(1, 0, 10));

    const std::uint64_t o2 = idx.alloc_ord();
    idx.put_doc("doc-1", o2, slot(1, 10, 20));

    // 旧 ord 软删，新 ord 存活。
    EXPECT_FALSE(idx.is_live(o1));
    EXPECT_TRUE(idx.is_live(o2));

    // get 返回最新版本。
    auto s = idx.get("doc-1");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->loc.offset, 10u);
    EXPECT_EQ(s->loc.total_sz, 20u);

    // 同一文档更新，存活文档数不变。
    EXPECT_EQ(idx.info().live_docs, 1u);
    // 历史 ord 数 = 2（含已死）。
    EXPECT_EQ(idx.info().total_ords, 2u);
}

TEST(Index, RemoveErasesAndSoftDeletes) {
    Index idx;
    const std::uint64_t o1 = idx.alloc_ord();
    idx.put_doc("doc-1", o1, slot(1, 0, 10));

    const std::uint64_t tomb = idx.alloc_ord();
    EXPECT_TRUE(idx.remove("doc-1", tomb));

    EXPECT_FALSE(idx.get("doc-1").has_value());
    EXPECT_FALSE(idx.is_live(o1));
    EXPECT_EQ(idx.info().live_docs, 0u);
}

TEST(Index, RemoveMissingReturnsFalse) {
    Index idx;
    EXPECT_FALSE(idx.remove("nope", idx.alloc_ord()));
    EXPECT_EQ(idx.info().live_docs, 0u);
}

TEST(Index, ReinsertAfterRemove) {
    Index idx;
    idx.put_doc("doc-1", idx.alloc_ord(), slot(1, 0, 10));
    idx.remove("doc-1", idx.alloc_ord());

    const std::uint64_t o = idx.alloc_ord();
    idx.put_doc("doc-1", o, slot(2, 0, 30));
    auto s = idx.get("doc-1");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->loc.file_id, 2u);
    EXPECT_EQ(idx.info().live_docs, 1u);
}

TEST(Index, OrdToExt) {
    Index idx;
    const std::uint64_t o = idx.alloc_ord();
    idx.put_doc("hello", o, slot(1, 0, 10));
    auto e = idx.ord_to_ext(o);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(*e, "hello");
    EXPECT_FALSE(idx.ord_to_ext(9999).has_value());
}

// S40 D2:ord_to_ext 兑现 DocTable 契约「已删返回 nullopt」——覆盖写退休的
// 旧 ord、remove 的当前 ord 都不再返回残留 ext(ord2ext 改 view 后不变量 O:
// 槽非空 ⟺ live)。长 key(>15B,走堆)让 ASan 能抓到悬垂 view。
TEST(Index, OrdToExtDeadOrdReturnsNullopt) {
    Index idx;
    const std::string k(40, 'x');
    idx.put_doc(k, 0, slot(1, 0, 10));
    idx.put_doc(k, 1, slot(1, 10, 10));  // 覆盖写:ord 0 退休
    EXPECT_FALSE(idx.ord_to_ext(0).has_value());
    ASSERT_TRUE(idx.ord_to_ext(1).has_value());
    EXPECT_EQ(*idx.ord_to_ext(1), k);

    ASSERT_TRUE(idx.remove(k, 2));  // 节点 erase:所有槽都不得再指向它
    EXPECT_FALSE(idx.ord_to_ext(0).has_value());
    EXPECT_FALSE(idx.ord_to_ext(1).has_value());

    idx.put_doc(k, 3, slot(1, 20, 10));  // 删后再写:新节点
    ASSERT_TRUE(idx.ord_to_ext(3).has_value());
    EXPECT_EQ(*idx.ord_to_ext(3), k);
    std::size_t live = 0;
    idx.for_each_live([&](std::uint64_t ord, std::string_view ext,
                          const DocSlot&) {
        EXPECT_EQ(ord, 3u);
        EXPECT_EQ(ext, k);
        ++live;
    });
    EXPECT_EQ(live, 1u);
}

// D3:remove 命中时 key 从 ext2ord_ 节点 move 进删除日志(省一次拷贝);
// 未命中照旧拷贝入账;低于窗口水位的旧墓碑两条路径都不入。
TEST(Index, RemoveLogsKeyOnHitAndMiss) {
    Index idx;
    const std::string k(40, 'r');  // 堆串:move 后源节点摘除,ASan 可见误用
    idx.put_doc(k, 5, slot(1, 0, 10));
    idx.begin_delta_window(3);
    ASSERT_TRUE(idx.remove(k, 6));            // 命中
    ASSERT_FALSE(idx.remove("absent", 7));    // 未命中仍入账
    ASSERT_FALSE(idx.remove("stale", 2));     // 低于水位:不入
    idx.put_doc(k, 8, slot(1, 10, 10));       // 删后再写:新节点可用
    ASSERT_TRUE(idx.ord_to_ext(8).has_value());
    EXPECT_EQ(*idx.ord_to_ext(8), k);

    const auto log = idx.removals_snapshot();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].first, k);
    EXPECT_EQ(log[0].second, 6u);
    EXPECT_EQ(log[1].first, "absent");
    EXPECT_EQ(log[1].second, 7u);
}

// 恢复路径：按 ord 序回放磁盘 record（显式 ord，不经 alloc_ord）。
// put_doc/remove 内部推进 next_ord，后写自动覆盖旧版本。
TEST(Index, ReplayWithExplicitOrds) {
    Index idx;
    idx.put_doc("a", 0, slot(1, 0,  10));
    idx.put_doc("b", 1, slot(1, 10, 10));
    idx.put_doc("a", 2, slot(1, 20, 15));   // update a
    idx.remove("b", 3);                      // delete b

    EXPECT_EQ(idx.info().next_ord, 4u);      // 推到 max ord + 1
    EXPECT_EQ(idx.info().live_docs, 1u);     // 只剩 a

    auto a = idx.get("a");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->loc.offset, 20u);           // a 的最新版本
    EXPECT_FALSE(idx.get("b").has_value());

    EXPECT_FALSE(idx.is_live(0));            // a 旧版本
    EXPECT_TRUE(idx.is_live(2));             // a 新版本
    EXPECT_FALSE(idx.is_live(1));            // b（已删）

    // 之后正常 alloc_ord 接着 next_ord 走。
    EXPECT_EQ(idx.alloc_ord(), 4u);
}

// ---- S42：live-only 形态测试（chunk 数组已退役）----

TEST(Index, LargeOrdGapOps) {
    Index idx;
    constexpr std::uint64_t N = 1ull << 16;  // 旧 chunk 边界；S42 起只是普通 ord 间隙

    idx.put_doc("low",  0,     slot(1, 0,  10));
    idx.put_doc("high", N + 5, slot(2, 100, 20));

    EXPECT_TRUE(idx.is_live(0));
    EXPECT_TRUE(idx.is_live(N + 5));
    EXPECT_FALSE(idx.is_live(1));
    EXPECT_FALSE(idx.is_live(N));  // 间隙 ord 不占任何行

    EXPECT_EQ(idx.get("low")->loc.file_id, 1u);
    EXPECT_EQ(idx.get("high")->loc.file_id, 2u);

    EXPECT_EQ(*idx.ord_to_ext(0), "low");
    EXPECT_EQ(*idx.ord_to_ext(N + 5), "high");

    // 中间 64K 死 ord 不占行：live-only 稳态随 live docs 而非历史 ord。
    EXPECT_EQ(idx.info().live_docs, 2u);
}

TEST(Index, CompactChunksIsNoOp) {
    Index idx;
    constexpr std::uint64_t N = 1ull << 16;

    idx.put_doc("a", 0,     slot(1, 0, 10));
    idx.put_doc("b", 1,     slot(1, 10, 10));
    idx.put_doc("c", N,     slot(1, 20, 10));
    idx.put_doc("d", N + 1, slot(1, 30, 10));

    idx.remove("c", idx.alloc_ord());
    idx.remove("d", idx.alloc_ord());

    // S42：写路径已淘汰死行，compact 退化为兼容 no-op。
    auto freed = idx.compact_chunks();
    EXPECT_EQ(freed, 0u);
    EXPECT_EQ(idx.info().chunks_allocated, 0u);
    EXPECT_EQ(idx.info().chunks_freed, 0u);

    EXPECT_TRUE(idx.is_live(0));
    EXPECT_TRUE(idx.is_live(1));
    EXPECT_FALSE(idx.is_live(N));
    EXPECT_FALSE(idx.is_live(N + 1));
}

TEST(Index, ForEachLiveAcrossLargeOrdGap) {
    Index idx;
    constexpr std::uint64_t N = 1ull << 16;

    idx.put_doc("a", 0,     slot(1, 0, 10));
    idx.put_doc("b", N,     slot(1, 10, 10));
    idx.put_doc("c", N + 1, slot(1, 20, 10));
    idx.remove("b", idx.alloc_ord());

    std::vector<std::pair<std::uint64_t, std::string>> seen;
    idx.for_each_live([&](std::uint64_t ord, std::string_view ext,
                          const DocSlot&) {
        seen.emplace_back(ord, ext);
    });

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].first, 0u);
    EXPECT_EQ(seen[0].second, "a");
    EXPECT_EQ(seen[1].first, N + 1);
    EXPECT_EQ(seen[1].second, "c");
}

// ---- S42-2：delta journal（live-only、窗口推进清空）----

TEST(Index, DeltaJournalTracksLiveOnly) {
    Index idx;
    idx.put_doc("k", 0, slot(1, 0, 10));
    idx.begin_delta_window(0);  // 窗口从 0 起；journal 清空
    EXPECT_TRUE(idx.delta_live_snapshot().empty());

    idx.put_doc("k", 1, slot(1, 10, 10));  // 覆盖写：journal 0 淘汰 → 1
    auto js = idx.delta_live_snapshot();
    ASSERT_EQ(js.size(), 1u);
    EXPECT_EQ(js[0], 1u);

    idx.put_doc("other", 2, slot(2, 0, 10));
    js = idx.delta_live_snapshot();
    ASSERT_EQ(js.size(), 2u);
    EXPECT_EQ(js[0], 1u);
    EXPECT_EQ(js[1], 2u);

    ASSERT_TRUE(idx.remove("k", 3));
    js = idx.delta_live_snapshot();
    ASSERT_EQ(js.size(), 1u);
    EXPECT_EQ(js[0], 2u);

    idx.begin_delta_window(4);  // 推进窗口 = journal 整体作废
    EXPECT_TRUE(idx.delta_live_snapshot().empty());

    idx.put_doc("old", 3, slot(1, 20, 10));  // ord < 新窗口水位：不入 journal
    EXPECT_TRUE(idx.delta_live_snapshot().empty());

    idx.put_doc("new", 5, slot(1, 30, 10));
    js = idx.delta_live_snapshot();
    ASSERT_EQ(js.size(), 1u);
    EXPECT_EQ(js[0], 5u);
}

// ---- S51-8：fill_eval_meta 批量版与逐个 eval_meta 逐项一致 ----

TEST(Index, FillEvalMetaMatchesEvalMeta) {
    namespace meta = bitcask::meta;
    const auto blob_of = [](const std::string& c) {
        std::vector<meta::MetaEntry> entries{{"c", meta::MetaValue(c)}};
        std::vector<std::byte> blob;
        meta::encode_meta(blob, entries);
        return blob;
    };
    Index idx;
    // ord 0..5：red / blue / 无 meta / red（随后被覆盖写淘汰）/ red（随后删除）/ red
    idx.put_doc("a", 0, slot(1, 0, 10));  idx.set_meta(0, blob_of("red"));
    idx.put_doc("b", 1, slot(1, 10, 10)); idx.set_meta(1, blob_of("blue"));
    idx.put_doc("c", 2, slot(1, 20, 10));
    idx.put_doc("d", 3, slot(1, 30, 10)); idx.set_meta(3, blob_of("red"));
    idx.put_doc("e", 4, slot(1, 40, 10)); idx.set_meta(4, blob_of("red"));
    idx.put_doc("d", 5, slot(1, 50, 10)); idx.set_meta(5, blob_of("red"));
    ASSERT_TRUE(idx.remove("e", 6));

    meta::MetaFilter f;
    f.conditions.push_back({"c", meta::MetaOp::Eq, meta::MetaValue("red"), {}});

    const std::vector<std::uint64_t> ords{0, 1, 2, 3, 4, 5, 999, 0};
    std::vector<char> got(ords.size(), 7);
    idx.fill_eval_meta(ords, f, got);
    const std::vector<char> want{1, 0, 0, 0, 0, 1, 0, 1};
    for (std::size_t i = 0; i < ords.size(); ++i) {
        EXPECT_EQ(got[i], want[i]) << "ord=" << ords[i];
        EXPECT_EQ(got[i] != 0, idx.eval_meta(ords[i], f)) << "ord=" << ords[i];
    }

    // 经基类默认实现（逐个调用）结果相同——外部 DocTable 实现者不覆写也成立。
    struct Fallback final : bitcask::bm25::DocTable {
        const Index& i;
        explicit Fallback(const Index& x) : i(x) {}
        bool is_live(std::uint64_t o) const override { return i.is_live(o); }
        std::uint32_t doc_len(std::uint64_t o) const override { return i.doc_len(o); }
        std::optional<std::string> ord_to_ext(std::uint64_t o) const override {
            return i.ord_to_ext(o);
        }
        bool eval_meta(std::uint64_t o, const meta::MetaFilter& mf) const override {
            return i.eval_meta(o, mf);
        }
        std::optional<std::uint64_t> ord_of(std::string_view e) const override {
            return i.ord_of(e);
        }
    } fb(idx);
    std::vector<char> got2(ords.size(), 7);
    fb.fill_eval_meta(ords, f, got2);
    EXPECT_EQ(got2, want);

    // 空批：不触碰 out。
    idx.fill_eval_meta({}, f, {});
}

// ---- S42：meta 挂 live 行，覆盖写/删除同步淘汰 ----

TEST(Index, OverwriteClearsOldMeta) {
    Index idx;
    idx.put_doc("k", 0, slot(1, 0, 10));
    const auto blob1 = as_bytes("meta-1");
    idx.set_meta(0, blob1);
    EXPECT_EQ(idx.meta_blob(0), blob1);

    idx.put_doc("k", 1, slot(1, 10, 10));  // 覆盖写：旧 ord 的 meta 淘汰
    EXPECT_TRUE(idx.meta_blob(0).empty());
    EXPECT_TRUE(idx.meta_blob(1).empty());  // 新行未挂 meta

    const auto blob2 = as_bytes("meta-2");
    idx.set_meta(1, blob2);
    EXPECT_EQ(idx.meta_blob(1), blob2);

    idx.set_meta(1, {});  // 空 blob = 清除
    EXPECT_TRUE(idx.meta_blob(1).empty());

    idx.set_meta(999, blob2);  // 未登记/死 ord：no-op，不复活
    EXPECT_TRUE(idx.meta_blob(999).empty());

    ASSERT_TRUE(idx.remove("k", 2));
    EXPECT_TRUE(idx.meta_blob(1).empty());  // 删除淘汰 meta
}

// ---- S42-1：热点覆盖写稳态（内存形态回归）----

TEST(Index, HotOverwriteKeepsLiveOnlyShape) {
    Index idx;
    constexpr std::uint64_t kWrites = 100000;
    for (std::uint64_t i = 0; i < kWrites; ++i) {
        idx.put_doc("hot", i, slot(1, i * 8, 8));
        if (i % 2 == 1) idx.set_doc_len(i, 7);
    }

    auto info = idx.info();
    EXPECT_EQ(info.live_docs, 1u);          // 1 key 写 100K 次 → 仍 1 行
    EXPECT_EQ(info.total_ords, kWrites);
    EXPECT_EQ(info.chunks_allocated, 0u);   // chunk 数组已退役
    EXPECT_EQ(idx.compact_chunks(), 0u);

    EXPECT_TRUE(idx.is_live(kWrites - 1));
    EXPECT_FALSE(idx.is_live(0));
    EXPECT_FALSE(idx.is_live(kWrites - 2));
    EXPECT_EQ(idx.doc_len(0), 0u);          // 死 ord 不再返回残留 doc_len
    EXPECT_EQ(idx.ord_to_ext(0), std::nullopt);

    auto hit = idx.get("hot");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->ord, kWrites - 1);
    EXPECT_EQ(hit->doc_len, 7u);            // 最后一次写入为奇数 ord → 已回填
}

// ---- S42-4：随机操作流对拍 reference model（含 checkpoint roundtrip）----

TEST(Index, RandomOpsMatchReferenceModel) {
    Index idx;
    std::mt19937_64 rng(42);
    std::vector<std::string> keys;
    for (int i = 0; i < 16; ++i) keys.push_back("key-" + std::to_string(i));
    std::unordered_map<std::string, RefDoc> model;
    std::vector<std::uint64_t> dead_ords;
    std::uint64_t next = 0;
    std::uniform_int_distribution<std::size_t> key_pick(0, keys.size() - 1);
    std::uniform_int_distribution<int> op(0, 5);

    auto do_put = [&](const std::string& k) {
        const std::uint64_t ord = next++;
        idx.put_doc(k, ord,
                    slot(1, ord * 16, 8, static_cast<std::uint32_t>(ord)));
        if (auto it = model.find(k); it != model.end()) {
            dead_ords.push_back(it->second.ord);
        }
        model[k] = RefDoc{ord, 0u, {}};
    };
    auto do_remove = [&](const std::string& k) {
        auto it = model.find(k);
        const std::uint64_t tomb = next++;
        ASSERT_EQ(idx.remove(k, tomb), it != model.end());
        if (it != model.end()) {
            dead_ords.push_back(it->second.ord);
            model.erase(it);
        }
    };

    for (int step = 0; step < 400; ++step) {
        const std::string& k = keys[key_pick(rng)];
        switch (op(rng)) {
        case 0:
        case 1:
            do_put(k);
            break;
        case 2:
            do_remove(k);
            break;
        case 3: {
            auto it = model.find(k);
            if (it == model.end()) break;
            const std::uint32_t len = static_cast<std::uint32_t>(step % 97 + 1);
            idx.set_doc_len(it->second.ord, len);
            it->second.doc_len = len;
            break;
        }
        case 4: {
            auto it = model.find(k);
            if (it == model.end()) break;
            if (step % 3 == 0) {
                idx.set_meta(it->second.ord, {});
                it->second.meta.clear();
            } else {
                const auto blob = as_bytes("meta-" + std::to_string(step));
                idx.set_meta(it->second.ord, blob);
                it->second.meta = blob;
            }
            break;
        }
        default:
            verify_model(idx, model, dead_ords, /*check_meta=*/true);
            break;
        }
    }
    verify_model(idx, model, dead_ords, /*check_meta=*/true);

    // checkpoint roundtrip：ord/ext/live/doc_len 保持；meta 走独立 section，
    // 纯 sidecar 不含 meta（与旧实现一致）→ idx2 对拍时关闭 meta 校验。
    std::vector<std::uint8_t> buf;
    ASSERT_TRUE(idx.serialize_docmap(buf, next));
    Index idx2;
    auto covers = idx2.deserialize_docmap(
        std::span<const std::uint8_t>(buf.data(), buf.size()));
    ASSERT_TRUE(covers.has_value());
    EXPECT_EQ(*covers, next);
    verify_model(idx2, model, dead_ords, /*check_meta=*/false);
}

// ---- S21-2 A2：BCIS sidecar v2（行 gap+vbyte）----

#include <span>

#include "bitcask/codec.hpp"

namespace {

void bcis_le32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
void bcis_le64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}

}  // namespace

// v2 roundtrip：serialize → 全新 Index deserialize → 行字段一致。
TEST(Index, SidecarV2RoundTrip) {
    Index idx;
    idx.put_doc("doc-a", 0, slot(1, 100, 50, 1111));
    idx.put_doc("doc-b", 5, slot(2, 4096, 80, 2222));
    idx.remove("doc-a", 6);
    std::vector<std::uint8_t> buf;
    ASSERT_TRUE(idx.serialize_docmap(buf, /*covers*/ 7));

    Index idx2;
    auto covers = idx2.deserialize_docmap(
        std::span<const std::uint8_t>(buf.data(), buf.size()));
    ASSERT_TRUE(covers.has_value());
    EXPECT_EQ(*covers, 7u);
    EXPECT_FALSE(idx2.get("doc-a").has_value()) << "已删行不应出现";
    auto s = idx2.get("doc-b");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->ord, 5u);
    EXPECT_EQ(s->loc.file_id, 2u);
    EXPECT_EQ(s->loc.offset, 4096u);
    EXPECT_EQ(s->loc.total_sz, 80u);
    EXPECT_EQ(s->tstamp, 2222u);
}

// v1（定宽行，u32-tstamp 纪元）：64 位时间戳 flag-day 后读端仅收 v3——
// 旧版本必须被干净拒收（返回 nullopt → 组件退 fold），绝不按新布局误读。
TEST(Index, SidecarV1CompatRead) {
    std::vector<std::uint8_t> b;
    bcis_le32(b, 0x42434953);  // "BCIS"
    bcis_le32(b, 1);           // v1
    bcis_le64(b, 3);           // covers
    bcis_le64(b, 1);           // rows
    bcis_le64(b, 2);           // ord
    const char* ext = "v1doc";
    b.push_back(5); b.push_back(0);  // klen u16
    b.insert(b.end(), ext, ext + 5);
    bcis_le32(b, 7);           // file_id
    bcis_le64(b, 512);         // offset
    bcis_le32(b, 64);          // total_sz
    bcis_le32(b, 3333);        // tstamp
    bcis_le32(b, 12);          // doc_len
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(b.data() + 8), b.size() - 8));
    bcis_le32(b, crc);

    Index idx;
    auto covers = idx.deserialize_docmap(
        std::span<const std::uint8_t>(b.data(), b.size()));
    EXPECT_FALSE(covers.has_value())
        << "u32 纪元 v1 sidecar 必须被干净拒收（退 fold 重建）";
    EXPECT_FALSE(idx.get("v1doc").has_value()) << "拒收后不得残留半解析状态";
}
