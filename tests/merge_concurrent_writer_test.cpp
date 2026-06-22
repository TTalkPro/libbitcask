#include <gtest/gtest.h>

#include <bitcask/cask.hpp>
#include <bitcask/data_file.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <latch>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bitcask::Cask;
using bitcask::CaskOptions;

class MergeConcurrentWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    tmpdir_ = std::filesystem::temp_directory_path() /
              (std::string("bitcask_merge_concurrent_writer_test_") +
               info->name());
    std::error_code ec;
    std::filesystem::remove_all(tmpdir_, ec);
    std::filesystem::create_directories(tmpdir_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmpdir_, ec);
  }

  static std::string key_for(int i) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "key%04d", i);
    return std::string(buf);
  }

  static std::vector<std::byte> value_for(int i) {
    return std::vector<std::byte>(40, static_cast<std::byte>(i));
  }

  static std::vector<std::byte> bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return std::vector<std::byte>(p, p + s.size());
  }

  std::pair<std::uint32_t, std::filesystem::path> max_data_file() const {
    std::uint32_t max_id = 0;
    std::filesystem::path found;
    for (const auto& e :
         std::filesystem::directory_iterator(tmpdir_)) {
      if (!e.is_regular_file()) continue;
      const std::string name = e.path().filename().string();
      if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
        if (*t >= max_id) {
          max_id = static_cast<std::uint32_t>(*t);
          found = e.path();
        }
      }
    }
    return {max_id, found};
  }

  static std::vector<std::byte> read_file_bytes(
      const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<std::byte> out;
    std::string buf((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    out.reserve(buf.size());
    for (char c : buf) out.push_back(static_cast<std::byte>(c));
    return out;
  }

  std::filesystem::path tmpdir_;
};

TEST_F(MergeConcurrentWriterTest, ConcurrentMergeWithActiveReader) {
  constexpr int kN = 50;

  CaskOptions opts;
  opts.read_write = true;
  opts.max_file_size = 256;
  auto c = Cask::open(tmpdir_.string(), opts);
  ASSERT_TRUE(c);
  auto& cask = **c;

  std::map<std::string, std::vector<std::byte>> expected;
  for (int i = 0; i < kN; ++i) {
    auto key = key_for(i);
    auto val = value_for(i);
    ASSERT_TRUE(cask.put(bytes(key), val, static_cast<std::uint32_t>(1000 + i)));
    expected[key] = std::move(val);
  }

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> iterations{0};
  std::atomic<std::size_t> mismatches{0};
  std::atomic<std::size_t> not_found{0};

  std::latch started(2);

  std::thread reader([&]() {
    started.count_down();
    started.wait();

    std::mt19937 rng(20250622);
    std::uniform_int_distribution<int> dist(0, kN - 1);

    while (!stop.load(std::memory_order_acquire)) {
      const int i = dist(rng);
      auto r = cask.get_owned(bytes(key_for(i)));
      ++iterations;
      if (!r) {
        ++not_found;
        continue;
      }
      const auto& expected_val = expected[key_for(i)];
      if (r->value != expected_val) {
        ++mismatches;
      }
    }
  });

  started.count_down();
  started.wait();

  auto mr = cask.merge({});
  ASSERT_TRUE(mr) << "merge failed: " << mr.error().detail;

  stop.store(true, std::memory_order_release);
  reader.join();

  EXPECT_GT(iterations.load(), 0u);
  EXPECT_EQ(not_found.load(), 0u);
  EXPECT_EQ(mismatches.load(), 0u);

  for (const auto& [k, v] : expected) {
    auto r = cask.get_owned(bytes(k));
    ASSERT_TRUE(r) << "post-merge key " << k << " missing";
    EXPECT_EQ(r->value, v) << "post-merge key " << k << " value mismatch";
  }

  cask.close();
}

TEST_F(MergeConcurrentWriterTest, ConcurrentMergePreservesActiveFile) {
  constexpr int kN = 20;

  CaskOptions opts;
  opts.read_write = true;
  opts.max_file_size = 256;
  auto c = Cask::open(tmpdir_.string(), opts);
  ASSERT_TRUE(c);
  auto& cask = **c;

  for (int i = 0; i < kN; ++i) {
    ASSERT_TRUE(
        cask.put(bytes(key_for(i)), value_for(i),
                 static_cast<std::uint32_t>(1000 + i)));
  }

  // 收集所有 data 文件，按 file_id 排序；最后一个（最大 file_id）是 active
  // writer 正在写的文件，不应被 merge 选中。前面的是 sealed 候选。
  // 显式传 files 绕过 needs_merge（全 live 记录不会触发任何 trigger）。
  std::vector<std::pair<std::uint32_t, std::string>> files;
  for (const auto& de : std::filesystem::directory_iterator(tmpdir_)) {
    const auto name = de.path().filename().string();
    if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
      files.push_back({static_cast<std::uint32_t>(*t), de.path().string()});
    }
  }
  ASSERT_GE(files.size(), 2u) << "max_file_size=256 应已滚出多个文件";
  std::sort(files.begin(), files.end());
  const auto active_path = files.back().second;
  const std::uint32_t active_id_before = files.back().first;
  const auto active_content_before = read_file_bytes(active_path);

  // merge 候选 = 除 active 外的全部 sealed 文件
  std::vector<std::string> to_merge;
  for (std::size_t i = 0; i + 1 < files.size(); ++i) {
    to_merge.push_back(files[i].second);
  }
  for (const auto& p : to_merge) {
    EXPECT_NE(p, active_path) << "active 文件不应在 merge 候选中";
  }

  auto mr = cask.merge(to_merge);
  ASSERT_TRUE(mr) << "merge failed: " << mr.error().detail;
  EXPECT_GT(mr->records_kept, 0u);
  const std::uint32_t merge_output_id = mr->output_file_id;

  // active 文件应未被 merge 触碰（内容字节相同）
  ASSERT_TRUE(std::filesystem::exists(active_path));
  const auto active_content_after = read_file_bytes(active_path);
  EXPECT_EQ(active_content_after, active_content_before);

  // merge 后新 put 应进新 file_id（>merge_output_id，!=active_id_before）
  const std::string new_key = "key_new_post_merge";
  const std::vector<std::byte> new_value(40, std::byte{0xAB});
  ASSERT_TRUE(cask.put(bytes(new_key), new_value, 9000));

  auto it = cask.make_iter();
  auto sr = it->start();
  ASSERT_TRUE(sr);
  ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);

  std::uint32_t new_key_file_id = 0;
  while (true) {
    auto e = it->next();
    ASSERT_TRUE(e) << "iteration failed after post-merge put";
    if (!e->has_value()) break;
    const auto& entry = e->value();
    if (entry.value == new_value) {
      new_key_file_id = entry.file_id;
    }
  }
  it->release();

  ASSERT_GT(new_key_file_id, 0u);
  EXPECT_GT(new_key_file_id, merge_output_id);
  EXPECT_NE(new_key_file_id, active_id_before);

  cask.close();
}

TEST_F(MergeConcurrentWriterTest, MergeFailureLeavesKeydirConsistent) {
  constexpr int kN = 30;

  CaskOptions opts;
  opts.read_write = true;
  opts.max_file_size = 256;
  auto c = Cask::open(tmpdir_.string(), opts);
  ASSERT_TRUE(c);
  auto& cask = **c;

  std::map<std::vector<std::byte>, std::vector<std::byte>> expected;
  for (int i = 0; i < kN; ++i) {
    std::vector<std::byte> key = bytes(key_for(i));
    std::vector<std::byte> val = value_for(i);
    ASSERT_TRUE(cask.put(key, val, static_cast<std::uint32_t>(1000 + i)));
    expected[key] = std::move(val);
  }

  auto it = cask.make_iter();
  auto sr = it->start();
  ASSERT_TRUE(sr);
  ASSERT_EQ(*sr, bitcask::keydir::StartIterResult::kOk);
  auto first = it->next();
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->has_value());

  std::vector<std::pair<std::uint32_t, std::string>> files;
  for (const auto& de :
       std::filesystem::directory_iterator(tmpdir_)) {
    const auto name = de.path().filename().string();
    if (auto t = bitcask::fileops::parse_data_tstamp(name)) {
      files.push_back(
          {static_cast<std::uint32_t>(*t), de.path().string()});
    }
  }
  ASSERT_GE(files.size(), 3u);
  std::sort(files.begin(), files.end());
  std::vector<std::string> invalid_input = {
      files[0].second,
      (tmpdir_ / "9999999.bitcask.data").string(),
  };

  auto mr = cask.merge(invalid_input);
  EXPECT_FALSE(mr);

  std::map<std::vector<std::byte>, std::vector<std::byte>> got;
  got[(*first)->key] = (*first)->value;
  while (true) {
    auto e = it->next();
    ASSERT_TRUE(e) << "fold next failed after a merge failure";
    if (!e->has_value()) break;
    got[(*e)->key] = (*e)->value;
  }
  it->release();
  EXPECT_EQ(got, expected);

  for (const auto& [k, v] : expected) {
    auto r = cask.get_owned(k);
    ASSERT_TRUE(r) << "key missing after failed merge";
    EXPECT_EQ(r->value, v) << "value corrupted after failed merge";
  }

  cask.close();
}

}
