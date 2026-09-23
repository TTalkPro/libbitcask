// 6.6.0：进程级线程数上限（feedback 2026-09-23 / keel 转 coxswain：C API
// 收不了线程数，`enable_search` 首次开库起 hardware_concurrency 条线程）。
//
// 形状是**进程级**而非每库：两处线程本来就是进程共享的——
//   · 索引池 IndexPool：挂在 KeyDirRegistry 上（C API 是进程级单例 registry），
//     首个 search 库 open 时建池、起 index_workers 条 map worker + 1 条 reducer；
//   · Search 池：进程级 static tbb::task_arena（search_arena.cpp）。
// 另外本库经 TBB 全局 arena 跑的并行段（恢复期批内 prepare、查询内短语 /
// 通配 / 模糊 / HNSW 重排的 parallel_for）的 worker 来自 TBB market，数量
// 默认 ≈ hardware_concurrency - 1；search_slots 非 0 时顺带用
// tbb::global_control 把它封到 search_slots - 1——否则「压到 4 条」的宿主
// 在首次恢复或首条大查询后照样看到十几条 TBB worker。
//
// 语义：「第一个开 search 库的人定终身」。set 须在首个 search 库打开之前调；
// 之后（池已按旧值建好）再调且值不同 → 返回 false，**不静默忽略**。与当前
// 生效值相同的重复设置视为成功（幂等）。0 = 缺省（hardware_concurrency，
// 至少 2），即 6.5.0 及以前的行为。
//
// 注意：tbb::global_control 是 TBB 运行时进程级的：宿主若与本库共用同一份
// TBB 动态库，它自己的 TBB 并行也会被同一上限约束。
//
// 线程安全：全部函数内部加锁，可并发调用。

#pragma once

#include <cstddef>

namespace bitcask {

struct ThreadLimits {
    std::size_t index_workers = 0;  // 索引池 map worker 数；0 = 缺省
    std::size_t search_slots  = 0;  // Search 池并发槽数（兼 TBB worker 上限）；0 = 缺省
};

// 设置进程级上限。已冻结（首个 search 库已建池）且与生效值不同 → false，
// 不改任何状态。
[[nodiscard]] bool set_thread_limits(const ThreadLimits& limits);

// 当前登记值（原样，0 未解析）。
[[nodiscard]] ThreadLimits thread_limits();

// 线程池建池点调用：冻结并返回登记值（此后 set 只接受相同值）。
[[nodiscard]] ThreadLimits freeze_thread_limits();

// 0 → max(hardware_concurrency, 2)；非 0 原样返回。
[[nodiscard]] std::size_t resolve_thread_count(std::size_t requested) noexcept;

}  // namespace bitcask
