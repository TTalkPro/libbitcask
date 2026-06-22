#include <gtest/gtest.h>

#include <bitcask/cask.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bitcask::Cask;
using bitcask::CaskOptions;

class CrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tmpdir_ = std::filesystem::temp_directory_path() /
                  (std::string("bitcask_crash_recovery_test_") + info->name());
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

    static std::string value_for(int i) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "val%04d_fixed", i);
        return std::string(buf);
    }

    static std::vector<std::byte> bytes(std::string_view s) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        return std::vector<std::byte>(p, p + s.size());
    }

    std::filesystem::path max_data_file() const {
        std::uint64_t max_id = 0;
        std::filesystem::path found;
        for (const auto& e : std::filesystem::directory_iterator(tmpdir_)) {
            if (!e.is_regular_file()) continue;
            const std::string name = e.path().filename().string();
            constexpr std::string_view kSuffix = ".bitcask.data";
            if (name.size() <= kSuffix.size()) continue;
            if (name.compare(name.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
                continue;
            }
            const std::string id_str =
                name.substr(0, name.size() - kSuffix.size());
            char* end = nullptr;
            const unsigned long id = std::strtoul(id_str.c_str(), &end, 10);
            if (end != id_str.c_str() + id_str.size()) continue;
            if (id >= max_id) {
                max_id = id;
                found = e.path();
            }
        }
        return found;
    }

    std::filesystem::path tmpdir_;
};

TEST_F(CrashRecoveryTest, MidPutRestartFoldsCorrectly) {
    constexpr int kN = 50;

    const pid_t child = fork();
    ASSERT_NE(child, -1) << "fork failed";

    if (child == 0) {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        if (!c) {
            std::fprintf(stderr, "child open failed: %s\n",
                         c.error().detail.c_str());
            _exit(1);
        }
        for (int i = 0; i < kN; ++i) {
            auto pr = (*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                static_cast<std::uint32_t>(1000 + i));
            if (!pr) {
                std::fprintf(stderr, "child put failed: %s\n",
                             pr.error().detail.c_str());
                _exit(1);
            }
            auto sr = (*c)->sync();
            if (!sr) {
                std::fprintf(stderr, "child sync failed: %s\n",
                             sr.error().detail.c_str());
                _exit(1);
            }
        }
        _exit(0);
    }

    int status = 0;
    const pid_t waited = waitpid(child, &status, 0);
    ASSERT_NE(waited, -1);
    ASSERT_TRUE(WIFEXITED(status) || WIFSIGNALED(status));

    CaskOptions opts;
    opts.read_write = true;
    auto c = Cask::open(tmpdir_.string(), opts);
    ASSERT_TRUE(c) << "reopen after crash failed: " << c.error().detail;

    auto it = (*c)->make_iter();
    auto start = it->start();
    ASSERT_TRUE(start);

    std::map<std::string, std::string> recovered;
    while (true) {
        auto entry = it->next();
        ASSERT_TRUE(entry);
        if (!*entry) break;
        std::string key(reinterpret_cast<const char*>((*entry)->key.data()),
                        (*entry)->key.size());
        std::string value(
            reinterpret_cast<const char*>((*entry)->value.data()),
            (*entry)->value.size());
        recovered.emplace(std::move(key), std::move(value));
    }

    EXPECT_GE(recovered.size(), 1u);
    EXPECT_EQ(recovered.size(), static_cast<std::size_t>(kN))
        << "all sync'd puts should be recoverable";

    for (const auto& [k, v] : recovered) {
        ASSERT_GE(k.size(), 3u);
        const int idx = std::atoi(k.substr(3).c_str());
        EXPECT_EQ(v, value_for(idx))
            << "corruption detected for key " << k;
    }

    EXPECT_EQ(recovered.count(key_for(0)), 1u);

    (*c)->close();
}

TEST_F(CrashRecoveryTest, TornWriteTruncated) {
    constexpr int kN = 10;
    constexpr std::string_view kGarbage =
        "GARBAGE_DATA_NOT_A_VALID_RECORD_HEADER_THIS_IS_LONGER_THAN_100_BYTES_"
        "AND_WILL_NEVER_LOOK_LIKE_A_REAL_BITCASK_RECORD";
    constexpr std::size_t kGarbageBytes = 100;
    static_assert(kGarbage.size() >= kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c);
        for (int i = 0; i < kN; ++i) {
            ASSERT_TRUE((*c)->put(bytes(key_for(i)), bytes(value_for(i)),
                                  static_cast<std::uint32_t>(2000 + i)));
        }
        ASSERT_TRUE((*c)->sync());
        (*c)->close();
    }

    const std::filesystem::path data_path = max_data_file();
    ASSERT_FALSE(data_path.empty()) << "no data file found after clean close";

    const std::filesystem::path hint_path =
        data_path.parent_path() /
        (data_path.stem().string() + ".hint");

    std::error_code ec;
    std::filesystem::remove(hint_path, ec);

    const std::uint64_t valid_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec) << "failed to stat data file";

    {
        std::ofstream out(data_path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        out.write(kGarbage.data(), static_cast<std::streamsize>(kGarbageBytes));
        ASSERT_TRUE(out.good());
    }

    const std::uint64_t corrupted_size =
        std::filesystem::file_size(data_path, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(corrupted_size, valid_size + kGarbageBytes);

    {
        CaskOptions opts;
        opts.read_write = true;
        auto c = Cask::open(tmpdir_.string(), opts);
        ASSERT_TRUE(c) << "reopen with torn tail failed: " << c.error().detail;

        const std::uint64_t repaired_size =
            std::filesystem::file_size(data_path, ec);
        ASSERT_FALSE(ec);
        EXPECT_EQ(repaired_size, valid_size)
            << "torn tail was not truncated to last_valid_end";

        for (int i = 0; i < kN; ++i) {
            auto gr = (*c)->get_owned(bytes(key_for(i)));
            ASSERT_TRUE(gr) << "key " << key_for(i) << " missing after reopen";
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(
                                           gr->value.data()),
                                       gr->value.size()),
                      value_for(i));
        }

        (*c)->close();
    }
}

}  // namespace
