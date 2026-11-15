// S43-9：目录级 IndexAuthority 注册表，保留原 multi-Cask/refcount/thread-pool
// 语义，但不再共享任何内存哈希索引。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bitcask/index_authority.hpp"

namespace bitcask { class IndexPool; }

namespace bitcask::oki {

enum class AcquireStatus { kCreated, kReady, kNotReady };
struct AcquireResult {
    AcquireStatus status;
    std::shared_ptr<IndexAuthority> authority;
};

class IndexRegistry {
public:
    IndexRegistry() = default;
    ~IndexRegistry();
    IndexRegistry(const IndexRegistry&) = delete;
    IndexRegistry& operator=(const IndexRegistry&) = delete;

    [[nodiscard]] bitcask::IndexPool* index_pool();
    [[nodiscard]] std::size_t stop_index_pool_if_idle();
    [[nodiscard]] AcquireResult acquire(std::string_view name);
    [[nodiscard]] AcquireResult query(std::string_view name) const;
    void release(std::string_view name);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t>
    saved_biggest_file_id(std::string_view name) const;

private:
    struct Slot {
        std::shared_ptr<IndexAuthority> authority;
        std::uint32_t refcount = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> entries_;
    std::unordered_map<std::string, std::uint32_t> saved_biggest_file_id_;
    std::unique_ptr<bitcask::IndexPool> index_pool_;
};

} // namespace bitcask::oki
