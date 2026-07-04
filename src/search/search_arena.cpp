// Search 池实现（S19-1 自 search_layer.cpp 平移，行为不变）。

#include "bitcask/search_arena.hpp"

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <thread>

namespace bitcask::search {

namespace {
// S7：进程级共享的「有界 Search 池」——所有 Cask 共用一个 task_arena
//（非每 Cask 一个）。用途 = inter-query 并发：多条独立查询进池并发跑
//（稳赚，无单查询两路并行的均衡/唤醒摊销问题）。并发上限由 TBB market
// 封顶（≈hardware_concurrency），与索引/恢复期 TBB 工作隔离。
// 故意泄漏（never-destroyed）：规避静态析构与 TbbLifetime::finalize 的顺序坑；
// task_arena 仅是调度上下文、不持有线程（线程来自全局 market），泄漏成本可忽略。
tbb::task_arena& search_arena() {
    static tbb::task_arena* arena = [] {
        unsigned hc = std::thread::hardware_concurrency();
        int slots = static_cast<int>(hc > 1 ? hc : 2);
        return new tbb::task_arena(slots);
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

}  // namespace bitcask::search
