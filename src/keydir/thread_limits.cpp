// 6.6.0：进程级线程数上限。语义见 include/bitcask/thread_limits.hpp。

#include "bitcask/thread_limits.hpp"

#include <memory>
#include <mutex>
#include <thread>

#include <oneapi/tbb/global_control.h>

namespace bitcask {

namespace {

struct State {
    std::mutex   mu;
    ThreadLimits limits;
    bool         frozen = false;
    // search_slots 非 0 时持有的 TBB 并行度上限。进程生命期内不析构（见
    // state() 的泄漏说明）；重设（冻结前）时替换——global_control 析构即
    // 撤销其约束，新实例立即生效。
    std::unique_ptr<tbb::global_control> tbb_cap;
};

// 故意泄漏（never-destroyed）：与 search_arena() 同一理由——规避静态析构
// 与 TBB 运行时拆除（TbbLifetime::finalize）的先后坑。
State& state() {
    static State* s = new State();
    return *s;
}

bool same(const ThreadLimits& a, const ThreadLimits& b) noexcept {
    return a.index_workers == b.index_workers &&
           a.search_slots == b.search_slots;
}

}  // namespace

bool set_thread_limits(const ThreadLimits& limits) {
    auto& s = state();
    std::lock_guard lk(s.mu);
    if (s.frozen) return same(s.limits, limits);
    if (limits.search_slots != s.limits.search_slots) {
        s.tbb_cap.reset();
        if (limits.search_slots != 0) {
            s.tbb_cap = std::make_unique<tbb::global_control>(
                tbb::global_control::max_allowed_parallelism,
                limits.search_slots);
        }
    }
    s.limits = limits;
    return true;
}

ThreadLimits thread_limits() {
    auto& s = state();
    std::lock_guard lk(s.mu);
    return s.limits;
}

ThreadLimits freeze_thread_limits() {
    auto& s = state();
    std::lock_guard lk(s.mu);
    s.frozen = true;
    return s.limits;
}

std::size_t resolve_thread_count(std::size_t requested) noexcept {
    if (requested != 0) return requested;
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 1 ? hc : 2;
}

}  // namespace bitcask
