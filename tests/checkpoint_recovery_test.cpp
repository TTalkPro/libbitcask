#include <gtest/gtest.h>

#include <bitcask/cask.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bitcask::Cask;
using bitcask::CaskOptions;
using bitcask::search::SearchLayerConfig;
using bitcask::text::AnalyzerType;

class CheckpointRecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    tmpdir_ = std::filesystem::temp_directory_path() /
              (std::string("bitcask_checkpoint_recovery_test_") + info->name());
    std::error_code ec;
    std::filesystem::remove_all(tmpdir_, ec);
    std::filesystem::create_directories(tmpdir_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmpdir_, ec);
  }

  static std::span<const std::byte> sv_bytes(std::string_view s) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(s.data()), s.size());
  }

  static std::vector<std::byte> bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
  }

  static std::string key_for(int i) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "key%04d", i);
    return std::string(buf);
  }

  static std::string value_for(int i) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "val%04d_fixed", i);
    return std::string(buf);
  }

  static std::vector<float> vector_for(int i, std::uint16_t dim) {
    std::vector<float> v(static_cast<std::size_t>(dim), 0.0f);
    const std::size_t idx0 = static_cast<std::size_t>(i) % dim;
    const std::size_t idx1 = (static_cast<std::size_t>(i) + 1) % dim;
    v[idx0] = 1.0f;
    v[idx1] = 0.5f;
    return v;
  }

  static void corrupt_file_bytes(const std::filesystem::path& path,
                                 std::size_t offset,
                                 const std::vector<std::byte>& garbage) {
    std::fstream f(path,
                   std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "cannot open " << path;
    f.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    f.write(reinterpret_cast<const char*>(garbage.data()),
            static_cast<std::streamsize>(garbage.size()));
    ASSERT_TRUE(f.good()) << "failed to corrupt " << path;
  }

  static CaskOptions make_search_options(std::uint16_t dim) {
    CaskOptions opts;
    opts.read_write = true;
    opts.enable_search = true;
    SearchLayerConfig cfg;
    cfg.analyzer_config.type = AnalyzerType::Ngram;
    cfg.analyzer_config.min_n = 2;
    cfg.analyzer_config.max_n = 3;
    opts.search_config = cfg;
    opts.vector_dim = dim;
    opts.vector_metric = bitcask::meta::VectorMetric::kCosineNormalized;
    return opts;
  }

  std::filesystem::path tmpdir_;
};

// P14e: search.ckpt corruption (CRC mismatch) must fall back to full fold of
// data files. Open succeeds, all docs remain searchable, and a new checkpoint
// is rewritten on the next close.
TEST_F(CheckpointRecoveryTest, CorruptSearchCheckpointFallsBackToFullFold) {
  namespace fs = std::filesystem;
  constexpr int kN = 10;
  constexpr int kDim = 128;
  const auto search_ckpt = tmpdir_ / "search.ckpt";
  const auto opts = make_search_options(kDim);

  {
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "initial open failed: " << c.error().detail;

    for (int i = 0; i < kN; ++i) {
      bitcask::DocInput doc;
      doc.text = sv_bytes("hello world foo bar");
      doc.vector = std::span<const float>(vector_for(i, kDim));
      ASSERT_TRUE((*c)->put_doc(sv_bytes(key_for(i)), doc,
                                static_cast<std::uint32_t>(1000 + i)))
          << "put_doc failed for " << i;
    }
    (*c)->flush_index();
    (*c)->close();
  }

  ASSERT_TRUE(fs::exists(search_ckpt)) << "search.ckpt was not written";

  {
    const auto sz = fs::file_size(search_ckpt);
    ASSERT_GT(sz, 8u) << "search.ckpt too small to corrupt";
    const std::vector<std::byte> garbage{
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    const std::size_t off = std::max<std::size_t>(4, sz / 2);
    corrupt_file_bytes(search_ckpt, off, garbage);
  }

  {
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen with corrupt search.ckpt failed: "
                   << c.error().detail;

    auto text_res = (*c)->search_text("hello", 20);
    ASSERT_TRUE(text_res) << "search_text failed after fallback";
    EXPECT_GE(text_res->hits.size(), 1u)
        << "no text hits after fallback fold";

    const auto qvec = vector_for(0, kDim);
    auto vec_res = (*c)->search_vector(
        std::span<const float>(qvec.data(), qvec.size()), 5);
    ASSERT_TRUE(vec_res) << "search_vector failed after fallback";
    EXPECT_GE(vec_res->hits.size(), 1u)
        << "no vector hits after fallback fold";
    EXPECT_LE(vec_res->hits.size(), 5u);

    for (int i = 0; i < kN; ++i) {
      auto gr = (*c)->get_owned(sv_bytes(key_for(i)));
      ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after fallback";
    }

    (*c)->close();
  }

  EXPECT_TRUE(fs::exists(search_ckpt))
      << "search.ckpt was not rewritten after recovery";
  EXPECT_GT(fs::file_size(search_ckpt), 0u);
}

// P14e: kv.keydir.ckpt corruption must fall back to full fold of data files.
TEST_F(CheckpointRecoveryTest, CorruptKeydirCheckpointFallsBackToFullFold) {
  namespace fs = std::filesystem;
  constexpr int kN = 20;
  const auto keydir_ckpt = tmpdir_ / "kv.keydir.ckpt";

  {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c);

    for (int i = 0; i < kN; ++i) {
      ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                            static_cast<std::uint32_t>(1000 + i)));
    }
    (*c)->close();
  }

  ASSERT_TRUE(fs::exists(keydir_ckpt)) << "kv.keydir.ckpt was not written";

  {
    const auto sz = fs::file_size(keydir_ckpt);
    ASSERT_GT(sz, 8u) << "kv.keydir.ckpt too small to corrupt";
    const std::vector<std::byte> garbage{
        std::byte{0xCA}, std::byte{0xFE}, std::byte{0xBA}, std::byte{0xBE},
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    const std::size_t off = std::max<std::size_t>(4, sz / 2);
    corrupt_file_bytes(keydir_ckpt, off, garbage);
  }

  {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen with corrupt kv.keydir.ckpt failed: "
                   << c.error().detail;

    for (int i = 0; i < kN; ++i) {
      auto gr = (*c)->get_owned(bytes(key_for(i)));
      ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after fallback";
      EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(gr->value.data()),
                                 gr->value.size()),
                value_for(i));
    }

    (*c)->close();
  }
}

// P14e: missing checkpoint must behave identically to a corrupt one.
TEST_F(CheckpointRecoveryTest, MissingCheckpointFallsBackToFullFold) {
  namespace fs = std::filesystem;
  constexpr int kN = 20;
  const auto keydir_ckpt = tmpdir_ / "kv.keydir.ckpt";

  {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c);

    for (int i = 0; i < kN; ++i) {
      ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                            static_cast<std::uint32_t>(1000 + i)));
    }
    (*c)->close();
  }

  ASSERT_TRUE(fs::exists(keydir_ckpt)) << "kv.keydir.ckpt was not written";
  {
    std::error_code ec;
    ASSERT_TRUE(fs::remove(keydir_ckpt, ec));
  }

  {
    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen with missing kv.keydir.ckpt failed: "
                   << c.error().detail;

    for (int i = 0; i < kN; ++i) {
      auto gr = (*c)->get_owned(bytes(key_for(i)));
      ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after fallback";
      EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(gr->value.data()),
                                 gr->value.size()),
                value_for(i));
    }

    (*c)->close();
  }
}

// P14e: if search.ckpt.prev exists, corruption of the current search.ckpt may
// either load the previous generation or fall back to full fold. Either is
// acceptable; open must succeed and at least the older generation's docs must
// be searchable.
TEST_F(CheckpointRecoveryTest,
       CorruptSearchCheckpointPrevGenerationFallback) {
  namespace fs = std::filesystem;
  constexpr int kFirst = 5;
  constexpr int kSecond = 5;
  constexpr int kDim = 128;
  const auto search_ckpt = tmpdir_ / "search.ckpt";
  const auto search_prev = tmpdir_ / "search.ckpt.prev";
  const auto opts = make_search_options(kDim);

  {
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c);
    for (int i = 0; i < kFirst; ++i) {
      bitcask::DocInput doc;
      doc.text = sv_bytes("hello world foo bar");
      doc.vector = std::span<const float>(vector_for(i, kDim));
      ASSERT_TRUE((*c)->put_doc(sv_bytes(key_for(i)), doc,
                                static_cast<std::uint32_t>(1000 + i)));
    }
    (*c)->flush_index();
    (*c)->close();
  }

  {
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c);
    for (int i = kFirst; i < kFirst + kSecond; ++i) {
      bitcask::DocInput doc;
      doc.text = sv_bytes("hello world foo bar");
      doc.vector = std::span<const float>(vector_for(i, kDim));
      ASSERT_TRUE((*c)->put_doc(sv_bytes(key_for(i)), doc,
                                static_cast<std::uint32_t>(2000 + i)));
    }
    (*c)->flush_index();
    (*c)->close();
  }

  ASSERT_TRUE(fs::exists(search_ckpt));
  ASSERT_TRUE(fs::exists(search_prev))
      << "second close did not create search.ckpt.prev";

  {
    const auto sz = fs::file_size(search_ckpt);
    ASSERT_GT(sz, 8u);
    const std::vector<std::byte> garbage{
        std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const std::size_t off = std::max<std::size_t>(4, sz / 2);
    corrupt_file_bytes(search_ckpt, off, garbage);
  }

  {
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen with corrupt current search.ckpt failed: "
                   << c.error().detail;

    auto text_res = (*c)->search_text("hello", 20);
    ASSERT_TRUE(text_res) << "search_text failed after prev/fold fallback";
    EXPECT_GE(text_res->hits.size(), 1u)
        << "no text hits after prev/fold fallback";

    for (int i = 0; i < kFirst; ++i) {
      auto gr = (*c)->get_owned(sv_bytes(key_for(i)));
      EXPECT_TRUE(gr) << "first-generation key " << key_for(i)
                      << " missing after prev/fold fallback";
    }

    (*c)->close();
  }
}

}
