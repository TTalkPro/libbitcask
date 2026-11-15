#include "bitcask/index_registry.hpp"

#include <algorithm>

#include "bitcask/thread_limits.hpp"
#include "bitcask/thread_pool.hpp"

namespace bitcask::oki {

IndexRegistry::~IndexRegistry() = default;

bitcask::IndexPool* IndexRegistry::index_pool() {
    std::scoped_lock lock(mutex_);
    if (!index_pool_) {
        const auto lim = bitcask::freeze_thread_limits();
        const int workers = static_cast<int>(
            bitcask::resolve_thread_count(lim.index_workers));
        index_pool_ = std::make_unique<bitcask::IndexPool>(workers);
    }
    return index_pool_.get();
}

std::size_t IndexRegistry::stop_index_pool_if_idle() {
    std::scoped_lock lock(mutex_);
    if (!entries_.empty()) return entries_.size();
    index_pool_.reset();
    return 0;
}

AcquireResult IndexRegistry::acquire(std::string_view name) {
    std::scoped_lock lock(mutex_);
    std::string key(name);
    if (auto it = entries_.find(key); it != entries_.end()) {
        if (!it->second.authority->is_ready())
            return {AcquireStatus::kNotReady, nullptr};
        ++it->second.refcount;
        return {AcquireStatus::kReady, it->second.authority};
    }
    auto authority = std::make_shared<IndexAuthority>();
    if (auto saved = saved_biggest_file_id_.find(key);
        saved != saved_biggest_file_id_.end()) {
        (void)authority->increment_file_id_at_least(saved->second);
    }
    entries_.emplace(key, Slot{authority, 1});
    return {AcquireStatus::kCreated, authority};
}

AcquireResult IndexRegistry::query(std::string_view name) const {
    std::scoped_lock lock(mutex_);
    auto it = entries_.find(std::string(name));
    if (it == entries_.end() || !it->second.authority->is_ready())
        return {AcquireStatus::kNotReady, nullptr};
    return {AcquireStatus::kReady, it->second.authority};
}

void IndexRegistry::release(std::string_view name) {
    std::scoped_lock lock(mutex_);
    auto it = entries_.find(std::string(name));
    if (it == entries_.end() || --it->second.refcount > 0) return;
    auto& saved = saved_biggest_file_id_[std::string(name)];
    saved = std::max(saved, it->second.authority->biggest_file_id() + 1);
    entries_.erase(it);
}

std::size_t IndexRegistry::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

std::optional<std::uint32_t>
IndexRegistry::saved_biggest_file_id(std::string_view name) const {
    std::scoped_lock lock(mutex_);
    auto it = saved_biggest_file_id_.find(std::string(name));
    return it == saved_biggest_file_id_.end() ? std::nullopt
                                             : std::optional(it->second);
}

} // namespace bitcask::oki
