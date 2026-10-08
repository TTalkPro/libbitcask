// Search 池实现（S19-1 自 search_layer.cpp 平移，行为不变）。

#include "bitcask/search_arena.hpp"
#include "bitcask/thread_limits.hpp"  // 6.6.0

#include <atomic>

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>

namespace bitcask::search {

namespace {
// S7：进程级共享的「有界 Search 池」——所有 Cask 共用一个 task_arena
//（非每 Cask 一个）。用途 = inter-query 并发：多条独立查询进池并发跑
//（稳赚，无单查询两路并行的均衡/唤醒摊销问题）。并发上限由 TBB market
// 封顶（≈hardware_concurrency），与索引/恢复期 TBB 工作隔离。
// 故意泄漏（never-destroyed）：规避静态析构与 TbbLifetime::finalize 的顺序坑；
// task_arena 仅是调度上下文、不持有线程（线程来自全局 market），泄漏成本可忽略。
//
// 6.7.0：池对象仍不析构，但 release_search_arena() 可 terminate 掉它对 TBB
// 运行时的引用（task_arena 持有 public 引用，不释放则 tbb::finalize 必败）；
// terminate 后 execute 会按原槽数自动重新 initialize。
std::atomic<bool> g_arena_built{false};

tbb::task_arena& search_arena() {
    // 6.6.0：槽数取进程级上限（set_thread_limits；首次使用即冻结）。
    static tbb::task_arena* arena = [] {
        const auto lim = bitcask::freeze_thread_limits();
        const int slots = static_cast<int>(
            bitcask::resolve_thread_count(lim.search_slots));
        auto* a = new tbb::task_arena(slots);
        g_arena_built.store(true, std::memory_order_release);
        return a;
    }();
    return *arena;
}
}  // namespace

// S7-4: inter-query 并发入口。n 条独立查询并发跑共享有界 Search 池。
void parallel_for_queries(std::size_t n,
                          const std::function<void(std::size_t)>& body) {
    if (n == 0) return;
    if (n == 1) { body(0); return; }  // 单条直跑，不进池（零开销快路径）
    // grainsize=1 在此正确：每 item 是一条完整重查询（与 BOW 的小 posting 不同）。
    search_arena().execute([&] {
        tbb::parallel_for(std::size_t{0}, n,
                          [&](std::size_t i) { body(i); });
    });
}

void release_search_arena() {
    // 未建过就别为了 terminate 去建（建池会冻结 thread limits）。
    if (!g_arena_built.load(std::memory_order_acquire)) return;
    search_arena().terminate();
}

}  // namespace bitcask::search
