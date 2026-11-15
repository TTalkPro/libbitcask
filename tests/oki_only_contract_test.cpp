// S43-0：OKI-only 改造前的外部语义守门测试。
//
// 这里不测实现细节，只锁住 S43 承诺不静默改变的契约：
//   - 点查 lifecycle（missing/put/overwrite/remove/reinsert/reopen）；
//   - Level B 下 OKI 冷视图与 public get 的存在性/ord 对齐；
//   - atomic batch 的批后最终状态；
//   - 删除 hint 后 data 仍是持久真相，重开语义不变。
//
// 崩溃注入、并发矩阵和 benchmark 由 S43-0 后续工作补齐。

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <functional>
#include <cstdint>
#include <optional>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <ctime>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/byte_order.hpp>
#include <bitcask/codec.hpp>
#include <bitcask/vbyte.hpp>
#include <bitcask/index_registry.hpp>
#include <bitcask/oki_run.hpp>
#include <bitcask/oki_state.hpp>
#include <bitcask/detail/scanner.hpp>

namespace {

namespace fs = std::filesystem;
namespace codec = bitcask::codec;
using bitcask::Cask;
using bitcask::CaskError;
using bitcask::CaskOptions;
using bitcask::oki::OkiManifest;
using bitcask::oki::read_manifest;
using bitcask::oki::OkiRunReader;
using bitcask::oki::OkiRunWriter;
using bitcask::oki::RowLoc;
using bitcask::oki::OkiState;

bitcask::oki::IndexRegistry& test_registry() {
    static bitcask::oki::IndexRegistry registry;
    return registry;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string as_string(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

class OkiOnlyContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = fs::temp_directory_path() /
               (std::string("bitcask_oki_only_contract_") + info->name());
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<Cask> open_rw(std::size_t level_b_entries = 128) {
        CaskOptions opts;
        opts.read_write = true;
        opts.keydir_cache_entries = level_b_entries;
        opts.log_fn = [](auto, std::string_view msg) {
            std::cerr << "LOG: " << msg << '\n';
        };
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        EXPECT_TRUE(opened)
            << (opened ? "" : opened.error().detail);
        return opened ? *std::move(opened) : nullptr;
    }

    // S43 会停写 hint。这里显式删除已封口 hint，确保守门测试不依赖它。
    void remove_hints() const {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir_, ec)) {
            if (entry.path().extension() == ".hint") {
                fs::remove(entry.path(), ec);
            }
        }
    }

    static std::optional<std::string> get_str(Cask& c, std::string_view key) {
        auto r = c.get_owned(bytes(key));
        if (!r) {
            EXPECT_EQ(r.error().kind, CaskError::kNotFound)
                << r.error().detail;
            return std::nullopt;
        }
        return as_string(r->value);
    }

    static void expect_value(
        Cask& c, std::string_view key, std::optional<std::string_view> want) {
        const auto got = get_str(c, key);
        ASSERT_EQ(got.has_value(), want.has_value()) << "key=" << key;
        if (want.has_value()) {
            ASSERT_EQ(*got, *want) << "key=" << key;
        }
    }

    // S43-4 之前这是迁移对拍；S43-4 之后这变成“公共 get 必须直接来自 OKI”
    // 的行为守门。
    static void expect_oki_matches_public(Cask& c, std::string_view key) {
        const auto view = c.get_owned(bytes(key));
        const auto cold = c.index_authority().state().locate(key);
        ASSERT_NE(cold.status, OkiState::LocateStatus::kUnavailable)
            << "key=" << key;
        if (!view) {
            ASSERT_EQ(view.error().kind, CaskError::kNotFound)
                << "key=" << key;
            EXPECT_TRUE(cold.status == OkiState::LocateStatus::kMiss ||
                        (cold.status == OkiState::LocateStatus::kHit &&
                         cold.tomb))
                << "key=" << key;
            return;
        }
        ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit)
            << "key=" << key;
        EXPECT_FALSE(cold.tomb) << "key=" << key;
        EXPECT_EQ(cold.ord, view->ord) << "key=" << key;
    }

    static std::vector<OkiRunReader::Entry> read_run_rows(
        const fs::path& run) {
        auto reader = OkiRunReader::open(run.string());
        EXPECT_TRUE(reader.has_value());
        std::vector<OkiRunReader::Entry> rows;
        if (!reader.has_value()) return rows;
        auto cur = reader->begin();
        OkiRunReader::Entry row;
        while (true) {
            auto has = cur.next(row);
            if (!has.has_value()) {
                ADD_FAILURE() << static_cast<int>(has.error());
                return {};
            }
            if (!*has) break;
            rows.push_back(row);
        }
        return rows;
    }

    // 注入矩阵不能停在 raw CRC 破坏：重写一个结构合法的 BCOK v3 run，
    // 并同步刷新 manifest，保证 mutation 真正到达 OKI/data 语义裁决层。
    static bool rewrite_v3_run(
        const fs::path& run,
        const std::vector<OkiRunReader::Entry>& rows,
        const std::function<void(OkiRunReader::Entry&)>& mutate,
        std::size_t mutated_index) {
        auto fixed = rows;
        if (mutated_index >= fixed.size()) return false;
        mutate(fixed[mutated_index]);
        auto w = OkiRunWriter::create(
            run.string(), /*block_target=*/96,
            bitcask::oki::kRunVersion3, fixed.size());
        if (!w.has_value()) return false;
        for (const auto& row : fixed) {
            const RowLoc loc = row.loc;
            auto added = w->add(
                bytes(row.key), row.ord, row.tomb,
                row.has_loc ? &loc : nullptr);
            if (!added.has_value()) return false;
        }
        return w->finish(/*fsync_dir=*/true).has_value();
    }

    static bool refresh_manifest(
        const fs::path& dir, const OkiManifest& manifest) {
        return bitcask::oki::write_manifest(dir.string(), manifest);
    }

    // S43-9：OKI 点查就是唯一读路径；不再有 resident-key 逐出。
    static void force_cold(Cask& c, std::string_view key) {
        (void)c;
        (void)key;
    }

    static fs::path sole_run(const fs::path& dir) {
        fs::path run;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().filename().string().starts_with(
                    "kv.oki.seg-")) {
                EXPECT_TRUE(run.empty()) << "expected a single OKI run";
                run = entry.path();
            }
        }
        return run;
    }

    // 生成后立即重开，既丢掉旧块缓存，也模拟旧进程/旧读端打开。
    std::unique_ptr<Cask> reopen_rw(std::size_t level_b_entries = 128) {
        auto c = open_rw(level_b_entries);
        EXPECT_TRUE(c);
        return c;
    }

    fs::path dir_;

    // S43-1 测试辅助：解析 BCOK v2/v3 首个数据块，返回指定 key 的 loc 字段中
    // `offset` vbyte 的物理偏移。生产读端不受影响；只用于构造“OKI 指向
    // 另一条完好记录”的索引损坏场景。
    static std::optional<std::uint64_t> v2_loc_offset_position(
        const fs::path& run, std::string_view want_key) {
        std::ifstream in(run, std::ios::binary);
        if (!in) return std::nullopt;
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        if (raw.size() < 40) return std::nullopt;
        const auto* b = reinterpret_cast<const std::byte*>(raw.data());
        const std::span<const std::byte> bytes_all(b, raw.size());
        if (raw[0] != 'B' || raw[1] != 'C' || raw[2] != 'O' ||
            raw[3] != 'K') {
            return std::nullopt;
        }

        // BCOK v2/v3 trailer: entry_count | index_off | bloom_off | crc | magic。
        const std::size_t index_off_pos = raw.size() - 24;
        const auto index_off_bytes =
            bytes_all.subspan(index_off_pos, 8);
        std::uint64_t index_off = 0;
        for (unsigned i = 0; i < 8; ++i) {
            index_off |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(index_off_bytes[i])) << (8 * i);
        }
        if (index_off <= 8 || index_off > raw.size()) return std::nullopt;

        std::span<const std::byte> block =
            bytes_all.subspan(8, index_off - 8);
        std::size_t pos = 0;
        std::string prev_key;
        while (pos < block.size()) {
            const auto shared = codec::vbyte_read_checked(block, pos);
            if (!shared) return std::nullopt;
            const auto suffix_len = codec::vbyte_read_checked(block, shared->second);
            if (!suffix_len) return std::nullopt;
            if (suffix_len->first > block.size() - suffix_len->second) {
                return std::nullopt;
            }
            std::string key(prev_key, 0, static_cast<std::size_t>(shared->first));
            const auto suffix_begin = suffix_len->second;
            key.append(reinterpret_cast<const char*>(
                           block.data() + suffix_begin),
                       static_cast<std::size_t>(suffix_len->first));
            pos = static_cast<std::size_t>(suffix_begin + suffix_len->first);

            const auto ord = codec::vbyte_read_checked(block, pos);
            if (!ord) return std::nullopt;
            pos = static_cast<std::size_t>(ord->second);
            if (pos >= block.size()) return std::nullopt;
            const auto flags =
                std::to_integer<std::uint8_t>(block[pos++]);
            constexpr std::uint8_t kTomb = 0x01;
            constexpr std::uint8_t kLoc = 0x02;
            if ((flags & ~static_cast<std::uint8_t>(kTomb | kLoc)) != 0) {
                return std::nullopt;
            }
            if ((flags & kLoc) == 0) {
                if (key == want_key) return std::nullopt;
                continue;
            }

            const auto file_id = codec::vbyte_read_checked(block, pos);
            if (!file_id) return std::nullopt;
            const auto total_sz =
                codec::vbyte_read_checked(block, file_id->second);
            if (!total_sz) return std::nullopt;
            const std::size_t offset_pos =
                static_cast<std::size_t>(total_sz->second);
            const auto offset = codec::vbyte_read_checked(block, offset_pos);
            if (!offset) return std::nullopt;
            const auto tstamp =
                codec::vbyte_read_checked(block, offset->second);
            if (!tstamp) return std::nullopt;
            if (key == want_key) return 8 + offset_pos;
            pos = static_cast<std::size_t>(tstamp->second);
            prev_key = std::move(key);
        }
        return std::nullopt;
    }

    // v3 的 loc 错指向仍属索引语义损坏，而不是数据块位翻转。测试修改块内
    // vbyte 后重算该块的 CRC（单块小 run：索引末 4B 即 block_crc），保证
    // 请求真的到达 Cask::read_doc 的 key 核对门。
    static bool fix_v3_first_block_crc(const fs::path& run) {
        std::fstream io(run, std::ios::binary | std::ios::in | std::ios::out);
        if (!io) return false;
        io.seekg(0, std::ios::end);
        const auto size = io.tellg();
        if (size < 40) return false;
        std::vector<char> raw(static_cast<std::size_t>(size));
        io.seekg(0, std::ios::beg);
        if (!io.read(raw.data(), size)) return false;
        const auto* b = reinterpret_cast<const std::byte*>(raw.data());
        const std::span<const std::byte> all(b, raw.size());
        const std::size_t index_off_pos = raw.size() - 24;
        const std::size_t bloom_off_pos = raw.size() - 16;
        const std::uint64_t index_off =
            bitcask::le_load_u64(all.subspan(index_off_pos, 8).data());
        const std::uint64_t bloom_off =
            bitcask::le_load_u64(all.subspan(bloom_off_pos, 8).data());
        if (index_off <= 8 || index_off >= bloom_off) return false;

        // 精确解析第一条 sparse index entry 的 block_crc 位置。
        const std::span<const std::byte> idx =
            all.subspan(static_cast<std::size_t>(index_off),
                        static_cast<std::size_t>(bloom_off - index_off));
        const std::uint32_t block_count = bitcask::le_load_u32(idx.data());
        if (block_count == 0) return false;
        std::size_t ip = 4;
        const auto klen = codec::vbyte_read_checked(idx, ip);
        if (!klen) return false;
        ip = klen->second;
        if (ip + klen->first + 12 > idx.size()) return false;
        const std::uint64_t first_off =
            bitcask::le_load_u64(idx.data() + ip + klen->first);
        if (first_off != 8) return false;
        const std::size_t crc_pos =
            static_cast<std::size_t>(ip + klen->first + 8);

        const std::span<const std::byte> block =
            all.subspan(8, static_cast<std::size_t>(index_off - 8));
        const std::uint32_t crc = bitcask::codec::crc32(block);
        std::byte crc_bytes[4];
        bitcask::le_store_u32(crc_bytes, crc);
        const std::size_t block_crc_abs =
            static_cast<std::size_t>(index_off + crc_pos);
        io.seekp(static_cast<std::streamoff>(block_crc_abs), std::ios::beg);
        io.write(reinterpret_cast<const char*>(crc_bytes), 4);
        // v3 metadata CRC 也覆盖 sparse index（其中含 block_crc），所以
        // 改完索引后必须同步重算 trailer metadata CRC；否则新开 Reader 会
        // 正确地拒收。
        std::vector<char> updated(raw.size());
        io.seekg(0, std::ios::beg);
        if (!io.read(updated.data(), static_cast<std::streamsize>(raw.size()))) {
            return false;
        }
        const auto* ub =
            reinterpret_cast<const std::byte*>(updated.data());
        const std::span<const std::byte> uall(ub, updated.size());
        const std::size_t trailer_start = updated.size() - 32;
        std::uint32_t meta_crc = 0;
        meta_crc = bitcask::codec::crc32_update(
            meta_crc, uall.subspan(0, 8));
        meta_crc = bitcask::codec::crc32_update(
            meta_crc,
            uall.subspan(static_cast<std::size_t>(index_off),
                         static_cast<std::size_t>(bloom_off - index_off)));
        meta_crc = bitcask::codec::crc32_update(
            meta_crc, uall.subspan(static_cast<std::size_t>(bloom_off),
                                   trailer_start - static_cast<std::size_t>(bloom_off)));
        meta_crc = bitcask::codec::crc32_update(
            meta_crc, uall.subspan(trailer_start, 24));
        std::byte meta_crc_bytes[4];
        bitcask::le_store_u32(meta_crc_bytes, meta_crc);
        io.seekp(static_cast<std::streamoff>(trailer_start + 24),
                 std::ios::beg);
        io.write(reinterpret_cast<const char*>(meta_crc_bytes), 4);
        io.flush();
        return io.good();
    }

    // S43-2 收口对拍：manifest 里的 v4 元数据必须是同一个静止点的
    // authority 账本派生值。fstats 按 file_id 排序后逐字段比较。
    static void expect_v4_matches_authority(
        Cask& c, const OkiManifest& manifest) {
        ASSERT_TRUE(manifest.metadata.has_value());
        const auto& g = *manifest.metadata;
        const auto& kd = c.index_authority();
        const auto info = kd.info();
        EXPECT_EQ(g.next_ord, kd.peek_next_ord());
        EXPECT_EQ(g.biggest_file_id, kd.biggest_file_id());
        EXPECT_EQ(g.next_file_id, kd.biggest_file_id() + 1);
        EXPECT_EQ(g.active_keys, info.key_count);
        EXPECT_EQ(g.live_bytes, info.key_bytes);

        std::uint64_t oldest = 0;
        std::uint64_t newest = 0;
        for (const auto& f : kd.fstats()) {
            if (f.live_keys == 0) continue;
            if (f.oldest_tstamp != 0) {
                oldest = oldest == 0 ? f.oldest_tstamp
                                     : std::min(oldest, f.oldest_tstamp);
            }
            newest = std::max(newest, f.newest_tstamp);
        }
        EXPECT_EQ(g.oldest_tstamp, oldest);
        EXPECT_EQ(g.newest_tstamp, newest);

        auto mfstats = manifest.fstats;
        auto kdfstats = kd.fstats();
        ASSERT_EQ(mfstats.size(), kdfstats.size());
        std::sort(mfstats.begin(), mfstats.end(),
                  [](const auto& a, const auto& b) {
                      return a.file_id < b.file_id;
                  });
        std::sort(kdfstats.begin(), kdfstats.end(),
                  [](const auto& a, const auto& b) {
                      return a.file_id < b.file_id;
                  });
        for (std::size_t i = 0; i < mfstats.size(); ++i) {
            const auto& m = mfstats[i];
            const auto& k = kdfstats[i];
            ASSERT_EQ(m.file_id, k.file_id) << "fstats index=" << i;
            EXPECT_EQ(m.pending_delete, k.pending_delete)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.live_keys, k.live_keys)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.total_keys, k.total_keys)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.live_bytes, k.live_bytes)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.total_bytes, k.total_bytes)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.oldest_tstamp, k.oldest_tstamp)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.newest_tstamp, k.newest_tstamp)
                << "file_id=" << k.file_id;
            EXPECT_EQ(m.expiration_epoch, k.expiration_epoch)
                << "file_id=" << k.file_id;
        }
    }

    // 显式 merge 需要 sealed data 文件；返回按 file_id 排序且排除当前
    // active writer 的输入表。
    std::vector<std::string> sealed_data_files() const {
        std::vector<std::pair<std::uint32_t, std::string>> files;
        for (const auto& entry : fs::directory_iterator(dir_)) {
            const auto name = entry.path().filename().string();
            const auto id = bitcask::fileops::parse_data_tstamp(name);
            if (id.has_value()) files.emplace_back(*id, entry.path().string());
        }
        EXPECT_FALSE(files.empty());
        if (files.empty()) return {};
        std::sort(files.begin(), files.end());
        files.pop_back();  // active writer
        std::vector<std::string> out;
        out.reserve(files.size());
        for (const auto& [id, path] : files) out.push_back(path);
        return out;
    }
};

TEST_F(OkiOnlyContractTest, PointLifecycleSurvivesReopenWithoutHints) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        expect_value(*c, "absent", std::nullopt);

        ASSERT_TRUE((*c).put(bytes("k"), bytes("v1")));
        expect_value(*c, "k", "v1");
        const auto ord1 = (*c).get_owned(bytes("k"))->ord;

        ASSERT_TRUE((*c).put(bytes("k"), bytes("v2")));
        expect_value(*c, "k", "v2");
        const auto ord2 = (*c).get_owned(bytes("k"))->ord;
        EXPECT_GT(ord2, ord1);
        expect_oki_matches_public(*c, "k");

        ASSERT_TRUE((*c).remove(bytes("k")));
        expect_value(*c, "k", std::nullopt);
        expect_oki_matches_public(*c, "k");

        ASSERT_TRUE((*c).put(bytes("k"), bytes("v3")));
        expect_value(*c, "k", "v3");
        expect_oki_matches_public(*c, "k");
        (*c).close();
    }

    remove_hints();
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        expect_value(*c, "k", "v3");
        expect_value(*c, "absent", std::nullopt);
        expect_oki_matches_public(*c, "k");
    }
}

// S43-8：新路径不生成 hint；旧目录里的合法/损坏 hint 都是可删除缓存，
// 可写 open 惰性清理后必须完整走 data 恢复。
TEST_F(OkiOnlyContractTest, ReopenRemovesLegacyHintsAndUsesDataOnly) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("legacy"), bytes("live")));
        ASSERT_TRUE((*c).put(bytes("gone"), bytes("stale")));
        ASSERT_TRUE((*c).remove(bytes("gone")));
        (*c).close();
    }

    std::size_t produced_hints = 0;
    for (const auto& entry : fs::directory_iterator(dir_)) {
        if (entry.path().extension() == ".hint") ++produced_hints;
    }
    EXPECT_EQ(produced_hints, 0u);

    const auto data_file = dir_ / "1.bitcask.data";
    ASSERT_TRUE(fs::exists(data_file));
    {
        std::ofstream corrupt(data_file.string() + ".hint",
                              std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(corrupt);
        corrupt << "not-a-bitcask-hint";
    }
    {
        std::ofstream orphan(dir_ / "999.bitcask.hint",
                             std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(orphan);
        orphan << "orphan";
    }

    auto c = open_rw();
    ASSERT_TRUE(c);
    expect_value(*c, "legacy", "live");
    expect_value(*c, "gone", std::nullopt);
    expect_oki_matches_public(*c, "legacy");

    std::size_t remaining_hints = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() == ".hint") ++remaining_hints;
    }
    EXPECT_FALSE(ec);
    EXPECT_EQ(remaining_hints, 0u);
}

// S43-5：v2 tombstone 的 shadow file_id 是“删除前权威位置”的派生元数据，
// 必须来自 OKI probe，而不是 KeyDir 哈希缓存。这里先固化活行，再删除并
// 扫 data 记录验证 4 字节 shadow 与 OKI 记录的旧 file_id 一致。
TEST_F(OkiOnlyContractTest, RemoveShadowFileIdComesFromOkiProbe) {
    CaskOptions opts;
    opts.read_write = true;
    opts.tombstone_version = 2;
    opts.keydir_cache_entries = 128;
    auto opened = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(opened);
    auto c = *std::move(opened);
    ASSERT_TRUE(c);
    ASSERT_TRUE((*c).put(bytes("shadow-key"), bytes("live")));
    ASSERT_TRUE((*c).checkpoint());

    const auto prior = (*c).index_authority().state().probe_for_write("shadow-key");
    ASSERT_EQ(prior.status, OkiState::LocateStatus::kHit);
    ASSERT_FALSE(prior.tomb);
    ASSERT_TRUE(prior.has_loc);
    const auto old_file_id = prior.loc.file_id;
    ASSERT_NE(old_file_id, 0u);

    ASSERT_TRUE((*c).remove(bytes("shadow-key")));

    auto entries = bitcask::fileops::scan_dir(dir_.string());
    ASSERT_TRUE(entries.has_value());
    bool found = false;
    for (const auto& e : *entries) {
        std::ifstream in(e.data_path, std::ios::binary);
        ASSERT_TRUE(in) << e.data_path;
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        const auto* base = reinterpret_cast<const std::byte*>(raw.data());
        std::size_t pos = 0;
        while (pos < raw.size()) {
            const auto rec = codec::decode_data_record(
                std::span<const std::byte>(base + pos, raw.size() - pos));
            ASSERT_TRUE(rec.has_value()) << e.data_path << " at " << pos;
            if (rec->type == bitcask::format::RecordType::kTombstone &&
                as_string(rec->key) == "shadow-key") {
                found = true;
                ASSERT_EQ(rec->value.size(), 4u);
                std::uint32_t shadow = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    shadow |= static_cast<std::uint32_t>(
                        std::to_integer<std::uint8_t>(rec->value[i]))
                        << (8 * i);
                }
                EXPECT_EQ(shadow, old_file_id);
            }
            pos += rec->total_size;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(OkiOnlyContractTest, AtomicBatchFinalStateSurvivesReopenWithoutHints) {
    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        auto put_result = (*c).put(bytes("victim"), bytes("old"));
        ASSERT_TRUE(put_result) << put_result.error().detail;

        const std::vector<Cask::BatchOp> ops = {
            {.type = Cask::BatchOp::Type::kPut,
             .key = bytes("batch-a"),
             .value = bytes("va")},
            {.type = Cask::BatchOp::Type::kPut,
             .key = bytes("batch-b"),
             .value = bytes("vb")},
            {.type = Cask::BatchOp::Type::kRemove,
             .key = bytes("victim")},
        };
        ASSERT_TRUE((*c).put_batch_atomic(ops));

        expect_value(*c, "batch-a", "va");
        expect_value(*c, "batch-b", "vb");
        expect_value(*c, "victim", std::nullopt);
        (*c).close();
    }

    remove_hints();
    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        expect_value(*c, "batch-a", "va");
        expect_value(*c, "batch-b", "vb");
        expect_value(*c, "victim", std::nullopt);
    }
}

// 批内同 key 多次写只允许最终成员进入逻辑索引/账务；物理 data 记录与
// ord 序保持不变，崩溃重放按文件序自然收敛到同一最终态。
TEST_F(OkiOnlyContractTest, AtomicBatchDuplicateKeysApplyOnlyFinalOp) {
    CaskOptions opts;
    opts.read_write = true;
    opts.keydir_cache_entries = 128;
    opts.max_file_size = 1;  // 每个 put 成员都落独立 data 文件，暴露跨文件账
    auto opened = Cask::open(dir_.string(), opts, &test_registry());
    ASSERT_TRUE(opened);
    auto c = *std::move(opened);
    ASSERT_TRUE(c);
    ASSERT_TRUE((*c).put(bytes("base-put-remove"), bytes("base-v"), 1000));
    ASSERT_TRUE((*c).put(bytes("base-remove-put"), bytes("base-v"), 1001));
    ASSERT_TRUE((*c).sync());

    const std::vector<Cask::BatchOp> ops = {
        {.type = Cask::BatchOp::Type::kPut,
         .key = bytes("put-remove-put"), .value = bytes("first")},
        {.type = Cask::BatchOp::Type::kRemove,
         .key = bytes("put-remove-put")},
        {.type = Cask::BatchOp::Type::kPut,
         .key = bytes("put-remove-put"), .value = bytes("final")},
        {.type = Cask::BatchOp::Type::kRemove,
         .key = bytes("remove-put")},
        {.type = Cask::BatchOp::Type::kPut,
         .key = bytes("remove-put"), .value = bytes("alive")},
        {.type = Cask::BatchOp::Type::kPut,
         .key = bytes("put-remove"), .value = bytes("temporary")},
        {.type = Cask::BatchOp::Type::kRemove,
         .key = bytes("put-remove")},
        {.type = Cask::BatchOp::Type::kPut,
         .key = bytes("base-put-remove"), .value = bytes("new")},
    };
    ASSERT_TRUE((*c).put_batch_atomic(ops, 2000));

    expect_value(*c, "put-remove-put", "final");
    expect_value(*c, "remove-put", "alive");
    expect_value(*c, "put-remove", std::nullopt);
    expect_value(*c, "base-remove-put", "base-v");
    expect_value(*c, "base-put-remove", "new");

    // 逻辑账必须和单次最终应用一致；这里用真实提交点把 v4 fstats 落盘。
    ASSERT_TRUE((*c).sync());
    auto commit = (*c).index_authority().capture_commit_snapshot();
    ASSERT_TRUE(commit.has_value());
    ASSERT_TRUE((*c).index_authority().state().flush(
        dir_.string(), {}, std::move(commit)));
    const auto manifest = read_manifest(dir_.string());
    ASSERT_TRUE(manifest.has_value());
    expect_v4_matches_authority(*c, *manifest);

    // 重放持久真相仍得到同一最终态（物理重复记录不能复活中间态）。
    remove_hints();
    (*c).close();
    auto reopened = reopen_rw(/*level_b_entries=*/0);
    ASSERT_TRUE(reopened);
    expect_value(*reopened, "put-remove-put", "final");
    expect_value(*reopened, "remove-put", "alive");
    expect_value(*reopened, "put-remove", std::nullopt);
    expect_value(*reopened, "base-remove-put", "base-v");
    expect_value(*reopened, "base-put-remove", "new");
    ASSERT_TRUE((*reopened).sync());
    auto reopen_commit = (*reopened).index_authority().capture_commit_snapshot();
    ASSERT_TRUE(reopen_commit.has_value());
    ASSERT_TRUE((*reopened).index_authority().state().flush(
        dir_.string(), {}, std::move(reopen_commit)));
    const auto reopened_manifest = read_manifest(dir_.string());
    ASSERT_TRUE(reopened_manifest.has_value());
    expect_v4_matches_authority(*reopened, *reopened_manifest);
    EXPECT_EQ(reopened_manifest->metadata->active_keys, 4u);
    (*reopened).close();
}

// S43-5：group commit 的硬不变量是“队列序 = ord 序 = 文件字节序”。
// 并发提交后物理 data 必须严格递增；OKI 按 key 固化后，每行必须指向
// 同一 ord。否则 LWW、恢复水位和 index reducer 的 ord 门都会失真。
TEST_F(OkiOnlyContractTest, GroupCommitPreservesOrdAndMemdeltaOrder) {
    constexpr int kThreads = 8;
    constexpr int kPutsPerThread = 180;
    std::unordered_map<std::string, std::uint64_t> physical_ords;
    std::unordered_map<std::string, std::string> physical_values;
    {
        CaskOptions opts;
        opts.read_write = true;
        opts.sync_every_n = 1;  // 每个 leader batch 后都触发提交点
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(opened);
        auto c = *std::move(opened);
        ASSERT_TRUE(c);

        std::atomic<bool> ok{true};
        std::atomic<int> arrived{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> writers;
        writers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&, t] {
                ++arrived;
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < kPutsPerThread; ++i) {
                    const std::string key =
                        "gc-" + std::to_string(t) + "-" + std::to_string(i);
                    const std::string value = "v-" + key;
                    if (!(*c).put(bytes(key), bytes(value), 2000)) {
                        ok.store(false, std::memory_order_release);
                        return;
                    }
                }
            });
        }
        while (arrived.load() < kThreads) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& w : writers) w.join();
        ASSERT_TRUE(ok.load());

        // 先在源目录直接读持久真相。文件按 file_id 升序、文件内按字节序；
        // ord 递增就是 group commit 队列序未被打乱的直接证据。
        ASSERT_TRUE((*c).sync());
        auto entries = bitcask::fileops::scan_dir(dir_.string());
        ASSERT_TRUE(entries.has_value());
    std::optional<std::uint64_t> prev_ord;
        for (const auto& entry : *entries) {
            std::ifstream in(entry.data_path, std::ios::binary);
            ASSERT_TRUE(in) << entry.data_path;
            const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
            const auto* base = reinterpret_cast<const std::byte*>(raw.data());
            std::size_t pos = 0;
            while (pos < raw.size()) {
                const auto rec = codec::decode_data_record(
                    std::span<const std::byte>(base + pos, raw.size() - pos));
                ASSERT_TRUE(rec.has_value())
                    << entry.data_path << " at " << pos;
                ASSERT_EQ(rec->type, bitcask::format::RecordType::kDoc);
                EXPECT_TRUE(!prev_ord.has_value() || rec->ord > *prev_ord)
                    << "file=" << entry.data_path << " pos=" << pos;
                prev_ord = rec->ord;
                const std::string key = as_string(rec->key);
                EXPECT_FALSE(physical_ords.contains(key)) << key;
                physical_ords.emplace(key, rec->ord);
                const auto doc = codec::decode_doc_value(rec->value);
                ASSERT_TRUE(doc.has_value()) << key;
                physical_values.emplace(key, as_string(doc->text));
                pos += rec->total_size;
            }
        }
        EXPECT_EQ(physical_ords.size(), kThreads * kPutsPerThread);

        // 用同一提交快照固化 memdelta；run 内 ord 递增证明到达序同样保序。
        auto commit = (*c).index_authority().capture_commit_snapshot();
        ASSERT_TRUE(commit.has_value());
        ASSERT_TRUE((*c).index_authority().state().flush(
            dir_.string(), {}, std::move(commit)));
        (*c).close();
    }

    const auto manifest = read_manifest(dir_.string());
    ASSERT_TRUE(manifest.has_value());
    ASSERT_FALSE(manifest->runs.empty());
    std::size_t oki_rows = 0;
    for (const auto& run : manifest->runs) {
        auto reader = OkiRunReader::open(
            (dir_ / ("kv.oki.seg-" + std::to_string(run.gen))).string());
        ASSERT_TRUE(reader.has_value());
        auto cursor = reader->begin();
        OkiRunReader::Entry row;
        while (true) {
            auto has = cursor.next(row);
            ASSERT_TRUE(has.has_value());
            if (!*has) break;
            ++oki_rows;

            const std::string key = as_string(bytes(row.key));
            auto it = physical_ords.find(key);
            ASSERT_NE(it, physical_ords.end()) << key;
            EXPECT_EQ(row.ord, it->second) << key;
            EXPECT_FALSE(row.tomb) << key;
            ASSERT_TRUE(row.has_loc) << key;
        }
    }
    EXPECT_EQ(oki_rows, physical_ords.size());

    // 重开后逐 key 对拍 public get：ord/value 不能因乱序应用而串台。
    auto reopened = reopen_rw();
    ASSERT_TRUE(reopened);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPutsPerThread; ++i) {
            const std::string key =
                "gc-" + std::to_string(t) + "-" + std::to_string(i);
            auto got = (*reopened).get_owned(bytes(key));
            ASSERT_TRUE(got.has_value()) << key;
            EXPECT_EQ(as_string(got->value), physical_values[key]) << key;
            EXPECT_EQ(got->ord, physical_ords[key]) << key;
        }
    }
    (*reopened).close();
}

// 同文件覆盖、跨文件覆盖、被逐出 key 的覆盖/remove 是 fstats 最容易
// 出现 ±1 漂移的路径；每个场景都在静止后与 BCOM v4 manifest 精确对拍。
TEST_F(OkiOnlyContractTest, PutAndProbeFstatsParityAcrossFiles) {
    {
        CaskOptions opts;
        opts.read_write = true;
        opts.keydir_cache_entries = 0;
        opts.max_file_size = 8 * 1024 * 1024;
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(opened);
        auto c = *std::move(opened);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("same-file"), bytes("v1"), 1000));
        ASSERT_TRUE((*c).put(bytes("same-file"), bytes("v2"), 1001));
        ASSERT_TRUE((*c).sync());
        auto commit = (*c).index_authority().capture_commit_snapshot();
        ASSERT_TRUE(commit.has_value());
        ASSERT_TRUE((*c).index_authority().state().flush(
            dir_.string(), {}, std::move(commit)));
        const auto m = read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(m->fstats.size(), 1u);
        EXPECT_EQ(m->fstats.front().live_keys, 1u);
        EXPECT_EQ(m->fstats.front().total_keys, 2u);
        EXPECT_GT(m->fstats.front().total_bytes,
                  m->fstats.front().live_bytes);
        expect_v4_matches_authority(*c, *m);
        (*c).close();
    }

    {
        std::error_code reset_ec;
        fs::remove_all(dir_, reset_ec);
        fs::create_directories(dir_, reset_ec);
        CaskOptions opts;
        opts.read_write = true;
        opts.keydir_cache_entries = 0;
        opts.max_file_size = 1;  // 两次写入必然跨文件
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(opened);
        auto c = *std::move(opened);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("cross-file"), bytes("v1"), 1000));
        ASSERT_TRUE((*c).put(bytes("cross-file"), bytes("v2"), 1001));
        ASSERT_TRUE((*c).remove(bytes("cross-file"), 1002));
        ASSERT_TRUE((*c).sync());
        auto commit = (*c).index_authority().capture_commit_snapshot();
        ASSERT_TRUE(commit.has_value());
        ASSERT_TRUE((*c).index_authority().state().flush(
            dir_.string(), {}, std::move(commit)));
        const auto m = read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(m->metadata->active_keys, 0u);
        ASSERT_EQ(m->metadata->live_bytes, 0u);
        for (const auto& f : m->fstats) {
            EXPECT_EQ(f.live_keys, 0u) << f.file_id;
            EXPECT_EQ(f.live_bytes, 0u) << f.file_id;
        }
        expect_v4_matches_authority(*c, *m);
        (*c).close();
    }

    // Level B 缓存可逐出时，hash miss + OKI live 的覆盖/remove 必须按
    // OKI probe 的旧 loc 退账，而不是把 key 重复记成新 key。
    {
        std::error_code reset_ec;
        fs::remove_all(dir_, reset_ec);
        fs::create_directories(dir_, reset_ec);
        CaskOptions opts;
        opts.read_write = true;
        opts.keydir_cache_entries = 1;
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(opened);
        auto c = *std::move(opened);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("cold-live"), bytes("v"), 1000));
        const auto before_remove = (*c).index_authority().info().key_count;
        ASSERT_TRUE((*c).remove(bytes("cold-live"), 2000));
        ASSERT_TRUE((*c).sync());
        auto commit = (*c).index_authority().capture_commit_snapshot();
        ASSERT_TRUE(commit.has_value());
        ASSERT_TRUE((*c).index_authority().state().flush(
            dir_.string(), {}, std::move(commit)));
        const auto m = read_manifest(dir_.string());
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->metadata->active_keys, before_remove - 1);
        expect_v4_matches_authority(*c, *m);
        (*c).close();
    }
}

TEST_F(OkiOnlyContractTest, OkiColdViewMatchesPublicGetAcrossReopen) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        for (int i = 0; i < 16; ++i) {
            const auto key = "key-" + std::to_string(i);
            ASSERT_TRUE((*c).put(bytes(key), bytes("first-" + key)));
            expect_oki_matches_public(*c, key);
        }
        ASSERT_TRUE((*c).put(bytes("key-7"), bytes("updated-key-7")));
        ASSERT_TRUE((*c).remove(bytes("key-11")));
        for (int i = 0; i < 16; ++i) {
            expect_oki_matches_public(*c, "key-" + std::to_string(i));
        }
        (*c).close();
    }

    remove_hints();
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        for (int i = 0; i < 16; ++i) {
            expect_oki_matches_public(*c, "key-" + std::to_string(i));
        }
        expect_value(*c, "key-7", "updated-key-7");
        expect_value(*c, "key-11", std::nullopt);
    }
}

TEST_F(OkiOnlyContractTest, BCOMv4CommitSnapshotSurvivesDataOnlyReopen) {
    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("alive"), bytes("1")));
        ASSERT_TRUE((*c).put(bytes("gone"), bytes("22")));
        (*c).close();
    }

    auto m = read_manifest(dir_.string());
    ASSERT_TRUE(m.has_value());
    ASSERT_TRUE(m->metadata.has_value());
    EXPECT_EQ(m->metadata->active_keys, 2u);
    EXPECT_EQ(m->metadata->live_bytes, 9u);  // KeyDir 口径 = 活 key 字节
    EXPECT_GT(m->metadata->next_file_id, m->metadata->biggest_file_id);

    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).remove(bytes("gone")));
        ASSERT_TRUE((*c).put(bytes("other"), bytes("333")));
        (*c).close();
    }
    remove_hints();
    m = read_manifest(dir_.string());
    ASSERT_TRUE(m.has_value());
    ASSERT_TRUE(m->metadata.has_value());
    EXPECT_EQ(m->metadata->active_keys, 2u);
    EXPECT_EQ(m->metadata->live_bytes, 10u);

    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        expect_value(*c, "alive", "1");
        expect_value(*c, "gone", std::nullopt);
        expect_value(*c, "other", "333");
        const auto& commit = (*c).index_authority().state().manifest_metadata();
        ASSERT_TRUE(commit.has_value());
        // v4 是上一个提交点；open 期 tail/墓碑高水位允许正向超出。
        EXPECT_LE(commit->next_ord, (*c).index_authority().peek_next_ord());
        EXPECT_EQ(commit->active_keys, (*c).index_authority().info().key_count);
    }
}

TEST_F(OkiOnlyContractTest, BCOMv4RebuildMatchesPreviousSnapshotAndSweepsRuns) {
    std::size_t old_runs = 0;
    {
        auto c = open_rw(/*level_b_entries=*/0);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("same"), bytes("state")));
        ASSERT_TRUE((*c).put(bytes("still"), bytes("there")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.path().filename().string().starts_with("kv.oki.seg-")) {
                ++old_runs;
            }
        }
        (*c).close();
    }
    const auto expected = read_manifest(dir_.string());
    ASSERT_TRUE(expected.has_value());
    ASSERT_TRUE(expected->metadata.has_value());
    ASSERT_EQ(expected->metadata->active_keys, 2u);

    // manifest 损坏是 S43-3 的一级公民 fallback：这里只锁 S43-2 的对拍
    // 语义——data/BCKS 重建出的 v4 元数据必须和上一个提交点一致。
    fs::remove(dir_ / "kv.oki.manifest");
    std::size_t orphan_runs = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        if (e.path().filename().string().starts_with("kv.oki.seg-")) {
            ++orphan_runs;
        }
    }
    ASSERT_GT(orphan_runs, 0u);
    {
        std::ofstream half_spill(dir_ / "kv.oki.spill-99-0",
                                 std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(half_spill.good());
        half_spill.write("BCOK", 4);
        half_spill.write("\0\0\0", 3);  // 崩溃遗留的半截 spill 残件
        ASSERT_TRUE(half_spill.good());
    }

    auto c = open_rw(/*level_b_entries=*/0);
    ASSERT_TRUE(c);
    const auto& rebuilt = (*c).index_authority().state().manifest_metadata();
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ(*rebuilt, *expected->metadata);
    expect_value(*c, "same", "state");
    expect_value(*c, "still", "there");
    (*c).close();

    std::size_t new_runs = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
        if (e.path().filename().string().starts_with("kv.oki.seg-")) {
            ++new_runs;
        }
    }
    EXPECT_LE(new_runs, old_runs);
    EXPECT_FALSE(fs::exists(dir_ / "kv.oki.spill-99-0"))
        << "rebuild must sweep half-written spill residue";
}

// K4 的可复现版：run 已经 fsync/校验，但 manifest 原子发布失败。
// 失败必须只留下一个被 flush 清掉的孤儿 run；内存 manifest 和 memdelta
// 都不能回退，下一次重试用同一 delta 收敛到新 commit point。
TEST_F(OkiOnlyContractTest, ManifestCommitFailureKeepsDeltaAndPreviousState) {
    {
        auto c = open_rw(/*level_b_entries=*/128);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("committed"), bytes("old")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const auto before = read_manifest(dir_.string());
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->runs.size(), 1u);
    ASSERT_TRUE(before->metadata.has_value());
    ASSERT_EQ(before->metadata->active_keys, 1u);

    {
        auto c = open_rw(/*level_b_entries=*/128);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("pending"), bytes("new")));

        // atomic_write_bytes 的 manifest tmp 名是固定后缀；目录化后 fopen
        // 失败位于 run commit 之后。这不是实现细节依赖：注入点正是
        // “run 已 durable，manifest 尚未发布”的 K4 状态。
        const fs::path blocker = dir_ / "kv.oki.manifest.tmp";
        std::error_code ec;
        fs::create_directory(blocker, ec);
        ASSERT_TRUE(ec || fs::is_directory(blocker));

        std::size_t runs_before_failure = 0;
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.path().filename().string().starts_with("kv.oki.seg-")) {
                ++runs_before_failure;
            }
        }
        EXPECT_FALSE((*c).index_authority().state().flush(dir_.string()));
        EXPECT_EQ((*c).index_authority().state().delta_rows(), 1u);
        EXPECT_EQ((*c).index_authority().state().manifest_metadata(),
                  before->metadata);
        expect_value(*c, "committed", "old");
        expect_value(*c, "pending", "new");

        fs::remove(blocker, ec);
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        EXPECT_EQ((*c).index_authority().state().delta_rows(), 0u);
        expect_value(*c, "committed", "old");
        expect_value(*c, "pending", "new");

        std::size_t runs_after_retry = 0;
        for (const auto& e : fs::directory_iterator(dir_)) {
            if (e.path().filename().string().starts_with("kv.oki.seg-")) {
                ++runs_after_retry;
            }
        }
        EXPECT_EQ(runs_after_retry, runs_before_failure + 1);
        (*c).close();
    }

    const auto after = read_manifest(dir_.string());
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->runs.size(), 2u);
    ASSERT_TRUE(after->metadata.has_value());
    EXPECT_EQ(after->metadata->active_keys, 2u);
    EXPECT_EQ(after->wm, after->runs.back().cover_ord);
}

// merge 的搬迁/退账/TTL 都落在 fstats 上。v4 收口要求 merge 成功后
// manifest 元数据与 trim 后的 KeyDir 完全一致，而不是停留在 trim 前的
// 中间快照；旧输入 fstats 必须消失，输出文件只保留真实 live 统计。
TEST_F(OkiOnlyContractTest, MergeAndTtlFstatsMatchManifestAfterTrim) {
    const std::uint32_t now =
        static_cast<std::uint32_t>(::time(nullptr));
    std::uint32_t merged_biggest = 0;
    {
        CaskOptions opts;
        opts.read_write = true;
        opts.keydir_cache_entries = 128;
        opts.max_file_size = 512;
        auto opened = Cask::open(dir_.string(), opts, &test_registry());
        ASSERT_TRUE(opened);
        auto& c = **opened;

        // 一条到期、一条覆盖死值、一条仍活。足够填充让 max_file_size
        // 产生多个 sealed 输入，保证 merge 有 relocation 和退账路径。
        ASSERT_TRUE(c.put(bytes("expired"), bytes("dead"), 1000,
                             now - 10));
        ASSERT_TRUE(c.put(bytes("overwritten"), bytes("old"), 1000));
        ASSERT_TRUE(c.put(bytes("alive"), bytes("live-value"), 1000));
        ASSERT_TRUE(c.put(bytes("overwritten"), bytes("new-value"), 1000));
        const std::string pad(96, 'f');
        for (int i = 0; i < 24; ++i) {
            ASSERT_TRUE(c.put(bytes("fill" + std::to_string(i)),
                                 bytes(pad), 1000));
        }
        ASSERT_TRUE(c.sync());
        ASSERT_TRUE(c.index_authority().state().flush(dir_.string()));

        auto files = sealed_data_files();
        ASSERT_FALSE(files.empty());
        auto ms = c.merge(files, /*now_sec=*/now + 1);
        ASSERT_TRUE(ms) << ms.error().detail;
        merged_biggest = c.index_authority().biggest_file_id();
        EXPECT_GT(ms->records_expired, 0u);
        expect_value(c, "expired", std::nullopt);
        expect_value(c, "overwritten", "new-value");
        expect_value(c, "alive", "live-value");
        EXPECT_EQ(c.index_authority().info().key_count, 26u);

        // merge 收尾先 pre-unlink flush，再 trim 旧输入，最后
        // write_keydir_snapshot。这里专门锁住最后一步必须把 trim 后的
        // v4 快照发布出来。
        const auto fresh = read_manifest(dir_.string());
        ASSERT_TRUE(fresh.has_value());
        ASSERT_EQ(fresh->metadata->biggest_file_id, merged_biggest);
        expect_v4_matches_authority(c, *fresh);
        for (const auto& path : files) {
            const auto id = *bitcask::fileops::parse_data_tstamp(
                fs::path(path).filename().string());
            const auto f = std::find_if(
                fresh->fstats.begin(), fresh->fstats.end(),
                [&](const auto& e) { return e.file_id == id; });
            EXPECT_EQ(f, fresh->fstats.end())
                << "merged input fstats survived: file_id=" << id;
        }
        auto close_snap = c.index_authority().capture_commit_snapshot();
        ASSERT_TRUE(close_snap.has_value());
        EXPECT_EQ(c.index_authority().biggest_file_id(), merged_biggest);
        ASSERT_EQ(close_snap->metadata.biggest_file_id, merged_biggest);
        c.close();
    }

    {
        const auto closed_manifest = read_manifest(dir_.string());
        ASSERT_TRUE(closed_manifest.has_value());
        ASSERT_TRUE(closed_manifest->metadata.has_value());
        EXPECT_EQ(closed_manifest->metadata->biggest_file_id,
                  merged_biggest);
    }

    // reopen 后 BCOM v4 恢复的 fstats 仍和 authority 账本精确一致；TTL 删除不因
    // 重启复活。
    auto c = reopen_rw(/*level_b_entries=*/0);
    ASSERT_TRUE(c);
    expect_value(*c, "expired", std::nullopt);
    expect_value(*c, "overwritten", "new-value");
    // 开库会立刻分配 active file_id，但 fstats 只有首个持久 record 后才
    // 存在；这里推进到下一个真实提交点再做对拍。
    ASSERT_TRUE(c->put(bytes("post-reopen"), bytes("v"), 1000));
    ASSERT_TRUE(c->sync());
    auto reopen_commit = c->index_authority().capture_commit_snapshot();
    ASSERT_TRUE(reopen_commit.has_value());
    ASSERT_TRUE(c->index_authority().state().flush(
        dir_.string(), {}, std::move(reopen_commit)));
    const auto manifest = read_manifest(dir_.string());
    ASSERT_TRUE(manifest.has_value());
    expect_v4_matches_authority(*c, *manifest);

    // pending_delete 是 expiration_epoch 的派生布尔：set_pending_delete
    // 记录当前 fold epoch；随后 epoch 推进后，下一次 v4 提交必须准确
    // 发布 true（以及完整 expiration_epoch，而不是只存布尔）。
    auto& kd = c->index_authority();
    {
        auto it = c->make_iter();
        ASSERT_TRUE(it->start(0, -1, 0).has_value());
        it->release();
    }
    const auto target_id = kd.biggest_file_id();
    EXPECT_EQ(kd.state().manifest_metadata()->biggest_file_id, target_id);
    EXPECT_FALSE(kd.state().manifest_fstats().empty());
    kd.set_pending_delete(target_id);
    const auto pending_epoch = kd.peek_next_ord();
    {
        auto it = c->make_iter();
        ASSERT_TRUE(it->start(0, -1, 0).has_value());
        it->release();
    }
    auto pending_commit = kd.capture_commit_snapshot();
    ASSERT_TRUE(pending_commit.has_value());
    ASSERT_TRUE(kd.state().flush(dir_.string(), {},
                               std::move(pending_commit)));
    const auto pending_manifest = read_manifest(dir_.string());
    ASSERT_TRUE(pending_manifest.has_value());
    ASSERT_TRUE(pending_manifest->metadata.has_value());
    const auto pending = std::find_if(
        pending_manifest->fstats.begin(), pending_manifest->fstats.end(),
        [&](const auto& f) { return f.file_id == target_id; });
    ASSERT_NE(pending, pending_manifest->fstats.end());
    EXPECT_TRUE(pending->pending_delete);
    EXPECT_EQ(pending->expiration_epoch, pending_epoch);
    expect_v4_matches_authority(*c, *pending_manifest);
}

// 健康旧目录强制降回 BCOM v3 后，读端不得伪造空 v4 元数据；下一次
// 真实 flush/迁移点必须用当时的 authority 账本派生值发布 v4。
TEST_F(OkiOnlyContractTest, ForcedV3ManifestMigratesOnNextFlush) {
    {
        auto c = open_rw(/*level_b_entries=*/128);
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("old-a"), bytes("1")));
        ASSERT_TRUE((*c).put(bytes("old-b"), bytes("22")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const auto v4 = read_manifest(dir_.string());
    ASSERT_TRUE(v4.has_value());
    ASSERT_TRUE(v4->metadata.has_value());
    ASSERT_EQ(v4->metadata->active_keys, 2u);
    ASSERT_EQ(v4->fstats.size(), 1u);

    // 显式构造 v3 目录：保留 run 描述与 wm，只丢弃 v4 元数据段。
    auto v3 = *v4;
    v3.metadata.reset();
    v3.fstats.clear();
    ASSERT_TRUE(refresh_manifest(dir_, v3));
    const auto downgraded = read_manifest(dir_.string());
    ASSERT_TRUE(downgraded.has_value());
    ASSERT_FALSE(downgraded->metadata.has_value());
    ASSERT_TRUE(downgraded->fstats.empty());

    auto c = open_rw(/*level_b_entries=*/128);
    ASSERT_TRUE(c);
    expect_value(*c, "old-a", "1");
    expect_value(*c, "old-b", "22");
    const auto& opened = (*c).index_authority().state().manifest_metadata();
    ASSERT_FALSE(opened.has_value())
        << "v3 open must not fabricate empty metadata";

    ASSERT_TRUE((*c).put(bytes("migrated"), bytes("new")));
    ASSERT_TRUE((*c).sync());
    auto commit = (*c).index_authority().capture_commit_snapshot();
    ASSERT_TRUE(commit.has_value());
    ASSERT_TRUE((*c).index_authority().state().flush(
        dir_.string(), {}, std::move(commit)));

    const auto migrated = read_manifest(dir_.string());
    ASSERT_TRUE(migrated.has_value());
    ASSERT_TRUE(migrated->metadata.has_value());
    ASSERT_FALSE(migrated->fstats.empty());
    EXPECT_EQ(migrated->metadata->active_keys, 3u);
    expect_v4_matches_authority(*c, *migrated);
    (*c).close();

    const auto reopened = read_manifest(dir_.string());
    ASSERT_TRUE(reopened.has_value());
    ASSERT_TRUE(reopened->metadata.has_value());
    EXPECT_EQ(*reopened->metadata, *migrated->metadata);
}

// held row 不允许推进 runs 覆盖水位；但它所在那次提交的 v4 全局快照
// 可以包含 next_ord/active key/fstats。恢复语义是：wm 不含未持久 loc，
// 行继续留在 memdelta；持久水位越过 loc 后，下一次 flush 才进入 run。
TEST_F(OkiOnlyContractTest, HeldRowAdvancesMetadataButNotRunWatermark) {
    auto c = open_rw(/*level_b_entries=*/128);
    ASSERT_TRUE(c);
    ASSERT_TRUE((*c).put(bytes("held"), bytes("tail")));
    ASSERT_TRUE((*c).sync());
    const auto located = (*c).index_authority().state().locate("held");
    ASSERT_EQ(located.status, OkiState::LocateStatus::kHit);
    ASSERT_TRUE(located.has_loc);
    const auto file_id = located.loc.file_id;

    // 人为把 active data 的持久水位钳在 0，复现“row 已有 loc 但 caller
    // 尚不可引用”的提交输入；OkiState 必须把它持留。
    auto commit = (*c).index_authority().capture_commit_snapshot();
    ASSERT_TRUE(commit.has_value());
    ASSERT_TRUE((*c).index_authority().state().flush(
        dir_.string(),
        std::span<const std::pair<std::uint32_t, std::uint64_t>>(
            {{file_id, 0}}),
        std::move(commit)));
    EXPECT_EQ((*c).index_authority().state().delta_rows(), 1u);
    EXPECT_EQ((*c).index_authority().state().run_count(), 0u);
    const auto held_manifest = read_manifest(dir_.string());
    ASSERT_TRUE(held_manifest.has_value());
    ASSERT_TRUE(held_manifest->metadata.has_value());
    EXPECT_EQ(held_manifest->wm, 0u);
    EXPECT_EQ(held_manifest->metadata->next_ord,
              (*c).index_authority().peek_next_ord());
    EXPECT_EQ(held_manifest->metadata->active_keys, 1u);
    expect_value(*c, "held", "tail");

    // 数据已 sync；用真实文件尺寸做水位后，同一行收敛进唯一 run。
    const auto data_path =
        bitcask::fileops::mk_data_filename(dir_.string(), file_id);
    const auto durable_bytes = fs::file_size(data_path);
    ASSERT_GT(durable_bytes, 0u);
    commit = (*c).index_authority().capture_commit_snapshot();
    ASSERT_TRUE(commit.has_value());
    ASSERT_TRUE((*c).index_authority().state().flush(
        dir_.string(),
        std::span<const std::pair<std::uint32_t, std::uint64_t>>(
            {{file_id, durable_bytes}}),
        std::move(commit)));
    EXPECT_EQ((*c).index_authority().state().delta_rows(), 0u);
    EXPECT_EQ((*c).index_authority().state().run_count(), 1u);
    const auto committed = read_manifest(dir_.string());
    ASSERT_TRUE(committed.has_value());
    ASSERT_TRUE(committed->metadata.has_value());
    EXPECT_EQ(committed->wm, (*c).index_authority().peek_next_ord());
    EXPECT_EQ(committed->metadata->next_ord,
              (*c).index_authority().peek_next_ord());
    EXPECT_EQ(committed->metadata->active_keys, 1u);
    expect_v4_matches_authority(*c, *committed);
    expect_value(*c, "held", "tail");
    (*c).close();
}

TEST_F(OkiOnlyContractTest, GetRejectsLocatedRecordWithForeignKey) {
    fs::path run;
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        // 两个 record 的 offset/total_size 都保持单字节 vbyte，便于测试
        // 精确把 k1 的 OKI loc 改指向 k2 的完好记录。
        ASSERT_TRUE((*c).put(bytes("a"), bytes("1111")));
        ASSERT_TRUE((*c).put(bytes("z"), bytes("2222")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    for (const auto& entry : fs::directory_iterator(dir_)) {
        if (entry.path().filename().string().starts_with("kv.oki.seg-")) {
            run = entry.path();
            break;
        }
    }
    ASSERT_FALSE(run.empty());

    {
        // 先正常 open（v3 小区域元数据校验），再在运行期破坏已加载 run 引用的
        // 底层字节。这模拟块级/loc 损坏穿透开库校验的场景。
        auto c = open_rw();
        ASSERT_TRUE(c);
        const auto pos = v2_loc_offset_position(run, "a");
        ASSERT_TRUE(pos.has_value());
        EXPECT_LT(*pos, fs::file_size(run));
        const auto z_pos = v2_loc_offset_position(run, "z");
        ASSERT_TRUE(z_pos.has_value());
        std::fstream out(run, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(out.good());
        out.seekp(static_cast<std::streamoff>(*pos));
        std::ifstream in(run, std::ios::binary);
        ASSERT_TRUE(in.good());
        in.seekg(static_cast<std::streamoff>(*z_pos));
        char encoded{};
        ASSERT_TRUE(in.read(&encoded, 1));
        out.write(&encoded, 1);
        out.flush();
        ASSERT_TRUE(out.good());
        {
            std::ifstream in_version(run, std::ios::binary);
            char hdr[8]{};
            ASSERT_TRUE(in_version.read(hdr, sizeof hdr));
            const auto version = bitcask::le_load_u32(
                reinterpret_cast<const std::byte*>(hdr + 4));
            if (version == 3) {
                ASSERT_TRUE(fix_v3_first_block_crc(run));
            }
        }

        // 用索引自身定位结果证明这个 get 不是普通 miss：a 的 offset 已经
        // 是 z 的完好记录位置。这把失败原因锁定为 S43-1 要拦截的场景。
        // 重开确保没有旧块缓存掩盖运行期注入；v3 小区域元数据校验必须放行
        // 这类「块 CRC 已同步修复」的索引 loc 损坏。
        (*c).close();
        c = open_rw();
        ASSERT_TRUE(c);
        const auto cold = (*c).index_authority().state().locate("a");
        ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit);
        ASSERT_FALSE(cold.tomb);
        ASSERT_TRUE(cold.has_loc);

        auto r = (*c).get_owned(bytes("a"));
        ASSERT_FALSE(r);
        ASSERT_EQ(r.error().kind, CaskError::kIndexCorrupt)
            << r.error().detail;
    }
}

TEST_F(OkiOnlyContractTest, RuntimeBadBlockTriggersControlledOkiRebuild) {
    fs::path run;
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("lost"), bytes("after-rebuild")));
        ASSERT_TRUE((*c).put(bytes("keeper"), bytes("still-here")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        for (const auto& entry : fs::directory_iterator(dir_)) {
            if (entry.path().filename().string().starts_with("kv.oki.seg-")) {
                run = entry.path();
                break;
            }
        }
        ASSERT_FALSE(run.empty());
        (*c).close();
    }

    auto c = open_rw();
    ASSERT_TRUE(c);
    const auto original_run = run.filename();
    {
        std::ifstream in(run, std::ios::binary);
        char hdr[8]{};
        ASSERT_TRUE(in.read(hdr, sizeof hdr));
        const auto version = bitcask::le_load_u32(
            reinterpret_cast<const std::byte*>(hdr + 4));
        ASSERT_EQ(version, 3u);
    }
    // 先驱逐目标行，确保首次读走冷 OKI；随后翻转数据块字节。坏块 CRC
    // 会在首读时失败，read_doc 的 kUnavailable 分支必须触发 data 重建。
    {
        std::fstream out(run, std::ios::binary | std::ios::in |
                                  std::ios::out);
        ASSERT_TRUE(out.good());
        out.seekp(12, std::ios::beg);  // 首块第 5 字节，稳定破坏 vbyte 结构
        const char flipped = static_cast<char>(0xff);
        out.write(&flipped, 1);
        out.flush();
        ASSERT_TRUE(out.good());
    }

    // 丢弃运行期可能已缓存的好块；v3 open 只校验元数据，坏数据块仍会
    // 延迟到冷首读暴露。
    (*c).close();
    c = open_rw();
    ASSERT_TRUE(c);

    // 先直接锁定故障分类：坏 run 必须是 kUnavailable，而不是 miss。
    const auto corrupted = (*c).index_authority().state().locate("lost");
    ASSERT_EQ(corrupted.status, OkiState::LocateStatus::kUnavailable);
    // 触发读本身应在受控重建成功后恢复；不返回错值，也不把坏块当 miss。
    auto direct = (*c).get_owned(bytes("lost"));
    ASSERT_TRUE(direct) << (direct ? "" : direct.error().detail);
    ASSERT_EQ(as_string(direct->value), "after-rebuild");
    expect_value(*c, "keeper", "still-here");
    // 受控重建必须换新的 BCOK gen，并清掉损坏的旧 run；不得只是复用
    // 块缓存后“看起来能读”。
    bool original_gone = true;
    bool replacement_found = false;
    for (const auto& entry : fs::directory_iterator(dir_)) {
        const auto name = entry.path().filename();
        if (name == original_run) original_gone = false;
        if (name.string().starts_with("kv.oki.seg-")) replacement_found = true;
    }
    EXPECT_TRUE(original_gone);
    EXPECT_TRUE(replacement_found);
    const auto cold = (*c).index_authority().state().locate("lost");
    ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit);
    const auto keeper_cold = (*c).index_authority().state().locate("keeper");
    ASSERT_EQ(keeper_cold.status, OkiState::LocateStatus::kHit);
    EXPECT_GT(keeper_cold.ord, 0);
}

TEST_F(OkiOnlyContractTest, SemanticKeyMutationCannotExposeForeignValue) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("a"), bytes("belongs-to-a")));
        ASSERT_TRUE((*c).put(bytes("z"), bytes("belongs-to-z")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const fs::path run = sole_run(dir_);
    ASSERT_FALSE(run.empty());
    const auto rows = read_run_rows(run);
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].key, "a");

    // a -> b 仍保持 run 严格升序。这个 corruption 对 OKI 是合法 key；
    // 唯一能拦截错值的门是 data record 的 key 核对。
    ASSERT_TRUE(rewrite_v3_run(run, rows, [](OkiRunReader::Entry& row) {
        row.key = "b";
    }, 0));
    OkiManifest m;
    m.runs.push_back({1, rows.back().ord + 1,
                      bitcask::oki::kRunVersion3});
    m.wm = rows.back().ord + 1;
    m.level_b = true;
    ASSERT_TRUE(refresh_manifest(dir_, m));

    auto c = reopen_rw();
    ASSERT_TRUE(c);
    force_cold(*c, "b");
    const auto cold = (*c).index_authority().state().locate("b");
    ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit);
    ASSERT_FALSE(cold.tomb);
    ASSERT_TRUE(cold.has_loc);
    auto r = (*c).get_owned(bytes("b"));
    ASSERT_FALSE(r);
    ASSERT_EQ(r.error().kind, CaskError::kIndexCorrupt);
    expect_value(*c, "z", "belongs-to-z");
}

TEST_F(OkiOnlyContractTest, SemanticOrdMutationIsExplicitIndexCorruption) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("ord-key"), bytes("ord-value")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const fs::path run = sole_run(dir_);
    ASSERT_FALSE(run.empty());
    const auto rows = read_run_rows(run);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_TRUE(rewrite_v3_run(run, rows, [](OkiRunReader::Entry& row) {
        ++row.ord;
    }, 0));
    OkiManifest m;
    m.runs.push_back({1, rows[0].ord + 1,
                      bitcask::oki::kRunVersion3});
    m.wm = rows[0].ord + 1;
    m.level_b = true;
    ASSERT_TRUE(refresh_manifest(dir_, m));

    auto c = reopen_rw();
    ASSERT_TRUE(c);
    force_cold(*c, "ord-key");
    const auto cold = (*c).index_authority().state().locate("ord-key");
    ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit);
    ASSERT_EQ(cold.ord, rows[0].ord + 1);
    auto r = (*c).get_owned(bytes("ord-key"));
    ASSERT_FALSE(r);
    ASSERT_EQ(r.error().kind, CaskError::kIndexCorrupt);
}

TEST_F(OkiOnlyContractTest, SemanticTombMutationNeverExposesStaleValue) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("tomb-key"), bytes("must-not-leak")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const fs::path run = sole_run(dir_);
    ASSERT_FALSE(run.empty());
    const auto rows = read_run_rows(run);
    ASSERT_EQ(rows.size(), 1u);
    // 置 tomb 后行合法地失去 loc。该 mutation 会产生“索引权威删除”；
    // 只要 data record 未被返回，就不构成错值。当前持久真相重建可恢复。
    ASSERT_TRUE(rewrite_v3_run(run, rows, [](OkiRunReader::Entry& row) {
        row.tomb = true;
        row.has_loc = false;
        row.loc = RowLoc{};
    }, 0));
    OkiManifest m;
    m.runs.push_back({1, rows[0].ord + 1,
                      bitcask::oki::kRunVersion3});
    m.wm = rows[0].ord + 1;
    m.level_b = true;
    ASSERT_TRUE(refresh_manifest(dir_, m));

    auto c = reopen_rw();
    ASSERT_TRUE(c);
    force_cold(*c, "tomb-key");
    const auto cold = (*c).index_authority().state().locate("tomb-key");
    ASSERT_EQ(cold.status, OkiState::LocateStatus::kHit);
    ASSERT_TRUE(cold.tomb);
    expect_value(*c, "tomb-key", std::nullopt);
}

TEST_F(OkiOnlyContractTest, MixedV1V2V3RunsDegradeToControlledRebuild) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("in-v3"), bytes("a-value")));
        ASSERT_TRUE((*c).put(bytes("in-v2"), bytes("b-value")));
        ASSERT_TRUE((*c).put(bytes("in-v1"), bytes("c-value")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const auto rows = read_run_rows(sole_run(dir_));
    ASSERT_EQ(rows.size(), 3u);
    ASSERT_EQ(rows[0].key, "in-v1");
    ASSERT_EQ(rows[1].key, "in-v2");
    ASSERT_EQ(rows[2].key, "in-v3");

    // key 升序是 in-v1/in-v2/in-v3；gen 升序故意反向，让最高 gen 的
    // in-v1 只有 v1 行格式，从而把 mixed 降级暴露给 Level B locate。
    const std::vector<std::uint64_t> gens{3, 2, 1};
    const std::vector<std::uint32_t> versions{
        bitcask::oki::kRunVersion,
        bitcask::oki::kRunVersion2,
        bitcask::oki::kRunVersion3};
    std::uint64_t max_ord = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto w = OkiRunWriter::create(
            bitcask::oki::mk_run_filename(dir_.string(), gens[i]),
            /*block_target=*/96, versions[i], 1);
        ASSERT_TRUE(w.has_value());
        const RowLoc loc = rows[i].loc;
        // v1 行格式本就没有位置字段；in-v1 位于最高 gen，必然毒化点查。
        ASSERT_TRUE(w->add(bytes(rows[i].key), rows[i].ord, rows[i].tomb,
                           versions[i] == bitcask::oki::kRunVersion
                               ? nullptr : &loc)
                        .has_value());
        ASSERT_TRUE(w->finish(/*fsync_dir=*/true).has_value());
        max_ord = std::max(max_ord, rows[i].ord);
    }

    OkiManifest m;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        m.runs.push_back({gens[i], max_ord + 1,
                          static_cast<std::uint8_t>(versions[i])});
    }
    m.wm = max_ord + 1;
    m.level_b = true;
    ASSERT_TRUE(refresh_manifest(dir_, m));

    auto c = reopen_rw();
    ASSERT_TRUE(c);
    // v3/v2 冷路径可直接读；v1 在 Level B 点查视图中明确不可用。
    auto before = (*c).index_authority().state().locate("in-v1");
    ASSERT_EQ(before.status, OkiState::LocateStatus::kUnavailable);
    force_cold(*c, "in-v3");
    expect_value(*c, "in-v3", "a-value");
    force_cold(*c, "in-v2");
    expect_value(*c, "in-v2", "b-value");
    force_cold(*c, "in-v1");
    auto r = (*c).get_owned(bytes("in-v1"));
    ASSERT_TRUE(r) << (r ? "" : r.error().detail);
    ASSERT_EQ(as_string(r->value), "c-value");

    // Level B 不能长期停留在 mixed 毒化态：受控重建必须淘汰三个输入，
    // 并产出一个带位置字段、可点查的 v3 全量 run。
    ASSERT_EQ((*c).index_authority().state().run_count(), 1u);
    const auto replacement = sole_run(dir_);
    ASSERT_FALSE(replacement.empty());
    std::ifstream hdr(replacement, std::ios::binary);
    char magic[8]{};
    ASSERT_TRUE(hdr.read(magic, sizeof magic));
    EXPECT_EQ(bitcask::le_load_u32(
                  reinterpret_cast<const std::byte*>(magic + 4)),
              bitcask::oki::kRunVersion3);
    force_cold(*c, "in-v1");
    expect_value(*c, "in-v1", "c-value");
    expect_value(*c, "in-v2", "b-value");
    expect_value(*c, "in-v3", "a-value");
}

TEST_F(OkiOnlyContractTest, UnrecognizedRunVersionDegradesToDataRebuild) {
    {
        auto c = open_rw();
        ASSERT_TRUE(c);
        ASSERT_TRUE((*c).put(bytes("legacy"), bytes("from-data")));
        ASSERT_TRUE((*c).sync());
        ASSERT_TRUE((*c).index_authority().state().flush(dir_.string()));
        (*c).close();
    }
    const fs::path run = sole_run(dir_);
    ASSERT_FALSE(run.empty());
    std::ifstream in(run, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    ASSERT_EQ(raw[0], 'B');
    ASSERT_EQ(raw[1], 'C');
    ASSERT_EQ(raw[2], 'O');
    ASSERT_EQ(raw[3], 'K');
    std::byte version[4];
    bitcask::le_store_u32(version, 99);
    std::fstream out(run, std::ios::binary | std::ios::in |
                              std::ios::out);
    ASSERT_TRUE(out.good());
    out.seekp(4, std::ios::beg);
    out.write(reinterpret_cast<const char*>(version), 4);
    out.flush();
    ASSERT_TRUE(out.good());

    // 旧读端遇到未来 run 版本会拒收 OKI，而不是猜语义。当前实现等价
    // 降级：索引不可信时回到 data 重建，恢复后重写 v3。
    auto c = reopen_rw();
    ASSERT_TRUE(c);
    expect_value(*c, "legacy", "from-data");
    const auto replacement = sole_run(dir_);
    std::ifstream check(replacement, std::ios::binary);
    char hdr[8]{};
    ASSERT_TRUE(check.read(hdr, sizeof hdr));
    EXPECT_EQ(bitcask::le_load_u32(
                  reinterpret_cast<const std::byte*>(hdr + 4)),
              bitcask::oki::kRunVersion3);
}

}  // namespace
