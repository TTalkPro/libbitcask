#include "bitcask/index_manifest.hpp"
#include "support/test_paths.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using bitcask::ComponentId;
using bitcask::Manifest;
using bitcask::ManifestEntry;

namespace {

class IndexManifestTest : public ::testing::Test {
protected:
    fs::path tmp = fs::temp_directory_path() / ("manifest_test_" + std::to_string(bitcask::test::test_pid()));
    std::string path = (tmp / "index.manifest").string();

    void SetUp() override { fs::create_directories(tmp); }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    Manifest sample() const {
        Manifest m;
        m[ComponentId::kDocmap] = {100, 2, 300};
        m[ComponentId::kBm25]   = {100, 2, 250};
        m[ComponentId::kVec]    = {100, 0, 100};
        return m;
    }
};

TEST_F(IndexManifestTest, SerializeDeserializeRoundtrip) {
    auto orig = sample();
    auto raw = bitcask::serialize_manifest(orig);
    ASSERT_EQ(raw.size(), bitcask::kManifestSize);

    auto opt = bitcask::deserialize_manifest(raw.data(), raw.size());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->entries, orig.entries);
}

TEST_F(IndexManifestTest, FileWriteReadRoundtrip) {
    auto orig = sample();
    ASSERT_TRUE(bitcask::write_manifest(path, orig));

    auto opt = bitcask::read_manifest(path);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->entries, orig.entries);
}

TEST_F(IndexManifestTest, MinChainWatermark) {
    auto m = sample();
    EXPECT_EQ(m.min_chain_watermark(), 100u);

    m[ComponentId::kVec].chain_watermark = 0;
    EXPECT_EQ(m.min_chain_watermark(), 0u);
}

TEST_F(IndexManifestTest, CorruptionDetected) {
    ASSERT_TRUE(bitcask::write_manifest(path, sample()));

    auto raw = bitcask::serialize_manifest(sample());
    ASSERT_GE(raw.size(), 20u);
    raw[15] = static_cast<std::byte>(static_cast<unsigned char>(raw[15]) ^ 0xFF);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(raw.data()),
                static_cast<std::streamsize>(raw.size()));
    }
    EXPECT_FALSE(bitcask::read_manifest(path).has_value());
}

TEST_F(IndexManifestTest, TruncatedRejected) {
    auto raw = bitcask::serialize_manifest(sample());
    raw.resize(raw.size() - 1);
    EXPECT_FALSE(
        bitcask::deserialize_manifest(raw.data(), raw.size()).has_value());
}

TEST_F(IndexManifestTest, WrongMagicRejected) {
    auto raw = bitcask::serialize_manifest(sample());
    raw[0] = static_cast<std::byte>('X');
    EXPECT_FALSE(
        bitcask::deserialize_manifest(raw.data(), raw.size()).has_value());
}

TEST_F(IndexManifestTest, MissingFileReturnsNullopt) {
    EXPECT_FALSE(
        bitcask::read_manifest((tmp / "nonexistent").string()).has_value());
}

TEST_F(IndexManifestTest, ZeroManifestRoundtrip) {
    Manifest zero;
    auto raw = bitcask::serialize_manifest(zero);
    auto opt = bitcask::deserialize_manifest(raw.data(), raw.size());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->min_chain_watermark(), 0u);
}

}  // namespace
