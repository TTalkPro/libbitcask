// TBB 线程池封装：per-Cask 的 Index Pool + Search Pool。
//
// Index Pool（2 线程 + TBB task_group）— S6-P2 流水线：
//   1) dispatcher_ 线程：从 IndexTaskQueue 拉任务
//      - Add + 非空 fields：丢给 tbb::task_group 并行跑 map_fn_
//      - Add + 空 fields（单 text 走 on_write）：构造 OnWriteEntry 直接入 reorder
//      - Delete / Skip / RebuildHnsw：构造对应 entry 直接入 reorder
//      - 收到 Sentinel：等 map_group_ 排空 → 设 got_sentinel_ → 通知 reducer 退出
//   2) reducer_ 线程：从 reorder buffer 按 ord 严格升序取 entry，调用 reduce_fn_
//      - 单一写者约束（I3）：HNSW/索引的 apply 都在此线程串行
//      - 异常路径：try/catch 包裹，失败时调 error_fn_，仍推进 ord（避免 stall）
//   使用 std::thread 而非 TBB task_arena，避免 concurrency=1 时
//   主线程与 worker 争用同一 slot 导致 flush() 死锁。
// Search Pool（N 线程 unbounded）：执行 BM25 并行搜索（T6 阶段启用）。
//
// === 生命周期 ===
//   1. Cask::open() → 创建 IndexPool
//   2. put/delete → push IndexTask 到队列
//   3. Index Pool worker 异步消费（T3 阶段实现）
//   4. Cask::close() → stop() + join
//   5. NIF on_unload → tbb::finalize() 确保所有线程退出
//
// === 线程安全 ===
//   - IndexTaskQueue：基于 tbb::concurrent_bounded_queue，多生产者单消费者安全。
//   - stop()：设置原子标志后推入 sentinel，dispatcher/reducer 安全退出。
//   - flush()：等待 pending_ 计数归零 + applied_ord_ 追上 submitted_ord_hwm_，
//     保证所有已提交任务已被 reducer 消费并 apply。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <oneapi/tbb/concurrent_queue.h>  // concurrent_bounded_queue（oneTBB 已并入此头）
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>     // S6-P2: parallel map 调度（避免 task_group 与非 TBB 线程的小对象池错配）
#include <oneapi/tbb/task_arena.h>       // S6-P2: this_task_arena::isolate for TSan compatibility

#include "bitcask/search_layer.hpp"  // S6-P2: ReduceJob 用于 ReorderEntry

#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#endif

namespace bitcask {

// 索引操作类型。
enum class IndexOp : std::uint8_t {
    Add,         // 文档写入（分词 + put_doc + add_doc）
    Delete,      // 文档删除（remove_doc + index.remove）
    // V3.5:merge 后 HNSW 重建(物理清死)。S6-P2: 现在携带 ord（由
    // keydir_->alloc_ord 分配），通过 reorder buffer 与 Add/Delete 同序串行
    // apply，维护 HNSW 单写者约束。
    RebuildHnsw,
    // S6-P1: ord 空洞填充标记。alloc_ord 后若该 ord 不进索引（如 write_and_keydir
    // 重试路径浪费了原始 ord），单写线程发 Skip 填洞，使 reorder buffer 的
    // next_apply_ord 不永久 stall。P2 并行 map 下保证 ord 序重建。
    Skip,
    Sentinel,    // 停止信号，dispatcher 收到后等 map 排空并通知 reducer 退出
};

// S6-P3: per-库车道（lane）。共享池按 lib 路由任务到各自的 reorder buffer。
// 前置声明——完整定义在 MapFn/ReduceFn 之后（依赖它们）。
struct IndexLane;

// 索引任务：put/delete 路径提交到 Index Pool 的异步任务。
// key / text 必须拥有独立存储（非 string_view），因为原始数据在 put()
// 返回后可能被释放。两者合并进单个 buf（= key ⧺ text，key_len 划界），
// 一次分配替代原先 key/text 两个 string。
struct IndexTask {
    IndexOp              op;
    // S6-P3: 路由车道（submit 时由 pool 填入；Sentinel 为 nullptr）。
    // 裸指针安全性：lane 的生命周期由 in_flight 计数守护——unregister 先
    // flush(in_flight==0) 才注销，届时队列/reorder 中已无引用本 lane 的任务。
    IndexLane*           lane = nullptr;
    std::string          buf;          // key ⧺ text 合并存储
    std::uint64_t        ord       = 0;
    std::uint32_t        key_len   = 0;
    std::uint32_t        file_id   = 0;
    std::uint64_t        offset    = 0;
    std::uint32_t        total_sz  = 0;
    std::uint32_t        tstamp    = 0;
    std::uint32_t        doc_len   = 0; // token 总数（BM25 统计用）
    // S8.6 多字段：非空时走 on_write_fields；text 字段保留兼容单字段路径。
    std::vector<std::pair<std::string, std::string>> fields;
    // V3.3:Add 任务的文档向量(已归一化;空 = 无)。worker 转交
    // SearchLayer::on_vector → HNSW insert。
    std::vector<float> vec;
    // V5:文档结构化 meta blob(可为空)。worker 转交 Index::set_meta,
    // 与 put_doc 同 unique_lock 路径——filter 读取时 meta 与定位/live
    // 已原子一致。make() 不接管:caller 在构造后按需 assign。
    std::vector<std::byte> meta;

    [[nodiscard]] std::string_view key() const noexcept {
        return std::string_view(buf).substr(0, key_len);
    }
    [[nodiscard]] std::string_view text() const noexcept {
        return std::string_view(buf).substr(key_len);
    }

    // 唯一构造入口（Sentinel 除外）：key+text 一次分配进 buf。
    static IndexTask make(IndexOp op_, std::string_view key_,
                          std::uint64_t ord_, std::string_view text_,
                          std::uint32_t file_id_, std::uint64_t offset_,
                          std::uint32_t total_sz_, std::uint32_t tstamp_,
                          std::uint32_t doc_len_,
                          std::vector<std::pair<std::string, std::string>>
                              fields_ = {}) {
        IndexTask t;
        t.op = op_;
        t.buf.reserve(key_.size() + text_.size());
        t.buf.append(key_);
        t.buf.append(text_);
        t.key_len  = static_cast<std::uint32_t>(key_.size());
        t.ord      = ord_;
        t.file_id  = file_id_;
        t.offset   = offset_;
        t.total_sz = total_sz_;
        t.tstamp   = tstamp_;
        t.doc_len  = doc_len_;
        t.fields   = std::move(fields_);
        return t;
    }

    // 停止信号构造入口。显式初始化 op（其余成员走默认成员初始化），
    // 避免聚合初始化 IndexTask{IndexOp::Sentinel} 触发
    // -Wmissing-field-initializers（buf/fields/vec/meta 无默认成员初始化）。
    static IndexTask sentinel() {
        IndexTask t;
        t.op = IndexOp::Sentinel;
        return t;
    }
};

// 索引任务队列：多生产者（put/delete 线程）→ 单消费者（Index Pool worker）。
// 背压通过 tbb::concurrent_bounded_queue 的 bounded capacity 实现。
class IndexTaskQueue {
public:
    explicit IndexTaskQueue(std::size_t capacity = 10240)
    {
        queue_.set_capacity(capacity);
    }

    // C1:tbb 队列内部以 fence 同步,TSan 不建模独立 fence——producer
    // 构造的任务 payload 与 consumer 的读取会被误报 data race(任务
    // 字符串上的假阳性族)。用 TSan 标注 API 把移交的 happens-before
    // 显式告知 runtime;非 TSan 构建为空操作。
    void push(IndexTask task) {
        annotate_release();
        queue_.push(std::move(task));
    }

    IndexTask pop() {
        IndexTask task;
        queue_.pop(task);
        annotate_acquire();
        return task;
    }

    bool try_pop(IndexTask& task) {
        if (!queue_.try_pop(task)) return false;
        annotate_acquire();
        return true;
    }

    std::size_t size() const { return queue_.size(); }

private:
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    void annotate_release() { __tsan_release(&queue_); }
    void annotate_acquire() { __tsan_acquire(&queue_); }
#else
    void annotate_release() {}
    void annotate_acquire() {}
#endif

    tbb::concurrent_bounded_queue<IndexTask> queue_;
};

// ---- S6-P2: Reorder buffer entry types ----
// dispatcher/TBB map 把构造好的 entry 塞进 reorder buffer，reducer 按 ord 序
// 取出来 apply。variant 涵盖所有 IndexOp 类型。
struct ReduceEntry {
    search::ReduceJob job;             // map_analyze 产出（meta/vec 复用 ReduceJob）
    std::vector<std::byte> meta;       // 跨线程持有 owning
    std::vector<float>    vec;
};
struct OnWriteEntry {
    std::string           key;
    std::uint64_t         ord = 0;
    std::string           text;
    std::uint32_t         file_id = 0, total_sz = 0, tstamp = 0;
    std::uint64_t         offset  = 0;
    std::vector<std::byte> meta;
    std::vector<float>    vec;
};
struct DeleteEntry {
    std::string  key;
    std::uint64_t ord = 0;
};
struct SkipEntry {};
struct RebuildEntry {};

using ReorderEntry = std::variant<ReduceEntry, OnWriteEntry, DeleteEntry,
                                  SkipEntry, RebuildEntry>;

// ---- S6-P2: Pipeline callbacks ----
// MapFn（并行 TBB）：IndexTask → ReduceEntry。Add + 非空 fields 走此路径。
// ReduceFn（串行 reducer）：ReorderEntry → apply（含 on_write/on_delete/
//   reduce_apply/rebuild_hnsw 全部分支）。
// ErrorFn：异常计数器（best-effort 失败上报）。
using MapFn    = std::function<ReduceEntry(const IndexTask&)>;
using ReduceFn = std::function<void(ReorderEntry&)>;
using ErrorFn  = std::function<void()>;

// S6-P3: 库句柄。registry 共享池按 LibId 路由。
using LibId = std::uint64_t;

// S6-P3: per-库车道。共享池里每个 open 的 search 库注册一条 lane，持有
// 自己的回调 + reorder buffer + ord 水位。库间互不干扰（各自 lane），
// 库内 ord 序由单 reducer 串行保证（I2/I3）。
//
// 生命周期：register_lib 创建（shared_ptr 入 lanes_），unregister_lib
// 先 flush 再从 lanes_ 移除。reducer 在 apply 前对持有的 lane 拷一份
// shared_ptr，使 apply 期间 lane 不被并发 unregister 释放（UAF 防护）。
struct IndexLane {
    LibId    id = 0;
    // 回调（register 时注入；reducer 线程与 TBB map 线程读，const 调用安全）
    MapFn    map_fn;
    ReduceFn reduce_fn;
    ErrorFn  error_fn;

    // reorder buffer（受 IndexPool::reorder_mu_ 保护——与所有 lane 共享一把锁）
    std::map<std::uint64_t, ReorderEntry> pending;
    std::uint64_t next_apply_ord = 0;   // 下一个应 apply 的 ord（仅 reducer 推进）

    // ord 水位（atomic；flush 无锁读）。语义同 P2 的 per-pool 版本，下沉到 lane。
    std::atomic<std::uint64_t> submitted_ord_hwm{0};  // 已 submit 的最大 ord
    std::atomic<std::uint64_t> applied_ord{0};        // reducer 已 apply 到的连续 ord
    std::atomic<std::size_t>   in_flight{0};          // 本 lane 在途任务数（队列+reorder）
};

// per-registry 的共享索引线程管理器（S6-P3）。
// S6-P2: 拆成 dispatcher（pop 任务 + 分发到 TBB 或 reorder）+ reducer（按 ord
// 序 apply）双线程架构。tbb::task_group 跑 map_fn_；reorder buffer 用 mutex
// + cv 协调两端。
// T6 阶段会增加 Search Pool（基于 TBB task_arena）。
class IndexPool {
public:
    explicit IndexPool(int concurrency = 1, std::size_t queue_capacity = 10240)
        : queue_(queue_capacity)
    {
        // stopped_/pending_/next_apply_ord_ 走默认成员初始化（false/0/0），
        // 不在此重复列出——避免与声明顺序不一致触发 -Wreorder。
        (void)concurrency;
    }

    ~IndexPool() { stop(); }

    IndexPool(const IndexPool&) = delete;
    IndexPool& operator=(const IndexPool&) = delete;

    // ===== S6-P3: 多 lib 共享池 API =====

    // 注册一条库车道：注入回调 + reorder buffer 起始 ord，返回稳定句柄。
    // init_ord = cask::open 时 keydir「已分配最大 ord + 1」，reducer 跳过
    // [0, init_ord) 区间（disk fold 恢复已建索引）。首次注册时惰性启动
    // dispatcher/reducer 线程（registry 共享池：线程数与库数解耦 → G2）。
    IndexLane* register_lib(MapFn map_fn, ReduceFn reduce_fn, ErrorFn error_fn,
                            std::uint64_t init_ord = 0) {
        IndexLane* raw = nullptr;
        {
            std::lock_guard<std::mutex> lk(reorder_mu_);
            auto lane = std::make_shared<IndexLane>();
            lane->id            = next_lib_id_++;
            lane->map_fn        = std::move(map_fn);
            lane->reduce_fn     = std::move(reduce_fn);
            lane->error_fn      = std::move(error_fn);
            lane->next_apply_ord = init_ord;
            raw = lane.get();
            lanes_.emplace(lane->id, std::move(lane));
        }
        ensure_started();
        return raw;
    }

    // 注销一条库车道：先 flush 排空（保证 in_flight==0 ⇒ 队列/reorder 中
    // 无引用本 lane 的任务），再从 lanes_ 移除。不停池（其它库仍在用）。
    void unregister_lib(IndexLane* lane) {
        if (!lane) return;
        flush(lane);
        std::lock_guard<std::mutex> lk(reorder_mu_);
        lanes_.erase(lane->id);
    }

    // 提交任务到指定车道。背压由有界队列提供（满则 push 阻塞 put 线程）。
    void submit(IndexLane* lane, IndexTask task) {
        if (!lane || stopped_.load(std::memory_order_acquire)) return;
        // RebuildHnsw 携带 ord（merge 路径 alloc_ord），纳入 hwm 跟踪；
        // Sentinel 不携带 ord，跳过（且 Sentinel 不走本路径）。
        if (task.op != IndexOp::Sentinel) {
            auto prev = lane->submitted_ord_hwm.load(std::memory_order_relaxed);
            while (task.ord > prev &&
                   !lane->submitted_ord_hwm.compare_exchange_weak(
                       prev, task.ord,
                       std::memory_order_seq_cst,
                       std::memory_order_relaxed)) {
                // prev updated by CAS failure; retry
            }
            lane->in_flight.fetch_add(1, std::memory_order_relaxed);
        }
        task.lane = lane;
        queue_.push(std::move(task));
    }

    // 等待指定车道追平：该 lane 全部已 submit 的索引事件已完全 apply。
    void flush(IndexLane* lane) {
        if (!lane) return;
        std::unique_lock<std::mutex> lk(flush_mu_);
        flush_cv_.wait(lk, [lane] {
            return lane->in_flight.load(std::memory_order_acquire) == 0
                && lane->applied_ord.load(std::memory_order_acquire) >=
                   lane->submitted_ord_hwm.load(std::memory_order_acquire);
        });
    }

    // ===== 向后兼容 facade（单 lane；现有测试 + 单元基准用）=====
    // set_initial_ord → start 注册「默认车道」时的起始 ord。
    void set_initial_ord(std::uint64_t init_ord) {
        pending_initial_ord_ = init_ord;
    }
    // start：注册默认车道（惰性启动线程），后续 submit/flush/水位读默认走它。
    void start(MapFn map_fn, ReduceFn reduce_fn, ErrorFn error_fn) {
        default_lane_ = register_lib(std::move(map_fn), std::move(reduce_fn),
                                     std::move(error_fn), pending_initial_ord_);
    }
    void submit(IndexTask task) { submit(default_lane_, std::move(task)); }
    void flush() { flush(default_lane_); }

    // 停整池（registry 析构 / facade 测试收尾）。所有库车道都被排空。
    void stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
            return;
        }
        queue_.push(IndexTask::sentinel());
        // joinable() guard 兼容「start() 从未调用」场景：默认构造的
        // std::thread 不 joinable，跳过 join——Cask::open 失败回滚时
        // IndexPool 可能在 start() 前被析构，此 guard 防止 UB。
        if (dispatcher_.joinable()) dispatcher_.join();
        // S6-P2: dispatcher 退出时已串行排干所有 in-flight map（dispatcher
        // 单线程 + parallel_for 同步）。无需额外 wait。
        if (reducer_.joinable())    reducer_.join();
    }

    bool is_stopped() const {
        return stopped_.load(std::memory_order_acquire);
    }

    IndexTaskQueue&       queue()       { return queue_; }
    const IndexTaskQueue& queue() const { return queue_; }

    // facade 测试用——默认车道的水位（reducer 已处理到的最大 ord）。
    std::uint64_t applied_ord() const {
        return default_lane_
                   ? default_lane_->applied_ord.load(std::memory_order_acquire)
                   : 0;
    }
    std::uint64_t submitted_ord_hwm() const {
        return default_lane_
                   ? default_lane_->submitted_ord_hwm.load(std::memory_order_acquire)
                   : 0;
    }

private:
    // ---- Dispatcher + TBB map + reorder buffer (S6-P2) ----

    // 路由一条 IndexTask：Add + fields → TBB map；其它 → 直接构造 entry 推
    // reorder buffer。RebuildHnsw 是同步屏障点（必须等所有 in-flight map
    // 跑完才能插入到正确 ord 位置）。
    void dispatch_one(IndexTask task) {
        if (task.op == IndexOp::Add && !task.fields.empty()) {
            // S6-P2: 并行 map 用 tbb::parallel_for(0,1,lambda) —— 单元素
            // range，TBB 调度一个 task 到 worker 线程执行。task 的小对象
            // 分配在 TBB arena 内，释放也在 arena 内，避开 task_group
            // 从 std::thread 调 run() 的 thread_data 错配（TSan 严苛检查
            // 下会触发 small_object_pool destroy 断言 m_private_counter<0）。
            // TSan 限制：lambda 不能是 mutable（TBB 内部 const 调）。
            // 所有可变状态通过捕获指针 + lambda 内局部变量实现。
            IndexLane* lane = task.lane;
            MapFn     map_fn   = lane->map_fn;
            ErrorFn   error_fn = lane->error_fn;
            std::mutex*                       mu = &reorder_mu_;
            std::condition_variable*          cv = &reorder_cv_;
            std::map<std::uint64_t, ReorderEntry>* pending = &lane->pending;
            std::uint64_t                     ord  = task.ord;
            // 显式构造 local task 对象并调用 operator() —— 避免 parallel_for
            // 内部的 start_for 分配（TSan-strict 模式下与 std::thread 调
            // parallel_for 的 thread_data 错配）。并行语义由 TBB 调度器
            // 自然处理。
            auto map_task = [t = std::move(task), map_fn = std::move(map_fn),
                             error_fn = std::move(error_fn), ord, mu, cv, pending]() {
                ReduceEntry entry;
                try {
                    entry = map_fn(t);
                } catch (...) {
                    if (error_fn) error_fn();
                    entry = ReduceEntry{};
                }
                ReorderEntry re{std::move(entry)};
                {
                    std::lock_guard<std::mutex> lk(*mu);
                    pending->emplace(ord, std::move(re));
                }
                cv->notify_one();
            };
            // TSan 兼容：tbb::parallel_for(0, 0) 不会分配 start_for（早退）；
            // tbb::parallel_for(0, 1) 会分配，可能与 caller thread_data 错配。
            // 解法：直接调 tbb::this_task_arena::isolate + parallel_for。
            tbb::this_task_arena::isolate([&] {
                tbb::parallel_for(std::size_t{0}, std::size_t{1},
                                  [&](std::size_t) { map_task(); });
            });
        } else if (task.op == IndexOp::RebuildHnsw) {
            // Sync barrier：必须等所有 in-flight map job 完成，才能把
            // RebuildEntry 插到正确 ord 位置（ord 序由 reducer 保证）。
            flush_parallel_map();
            push_reorder(task.lane, task.ord, ReorderEntry{RebuildEntry{}});
        } else if (task.op == IndexOp::Delete) {
            DeleteEntry de;
            de.key = std::string(task.key());
            de.ord = task.ord;
            push_reorder(task.lane, task.ord, ReorderEntry{std::move(de)});
        } else if (task.op == IndexOp::Skip) {
            push_reorder(task.lane, task.ord, ReorderEntry{SkipEntry{}});
        } else {
            // on_write（单 text 路径）：构造 OnWriteEntry 跨线程交给 reducer
            // —— task 持有的是 owning 副本（buf/meta/vec），在 dispatcher
            // 线程构造 entry 时复制到 entry 字段，然后 push 后 task 析构。
            OnWriteEntry owe;
            owe.key      = std::string(task.key());
            owe.ord      = task.ord;
            owe.text     = std::string(task.text());
            owe.file_id  = task.file_id;
            owe.offset   = task.offset;
            owe.total_sz = task.total_sz;
            owe.tstamp   = task.tstamp;
            owe.meta     = std::move(task.meta);
            owe.vec      = std::move(task.vec);
            push_reorder(task.lane, task.ord, ReorderEntry{std::move(owe)});
        }
    }

    // S6-P2: RebuildHnsw barrier 替代 map_group_.wait()。当前实现：空
    // parallel_for(0, 0)，无任务立即返回——in-flight map 任务由 parallel_for
    // 自身隐式同步（其返回时所有任务已 complete）。但仅在「dispatcher 串行
    // 处理任务」的语义下成立：若某时刻 dispatcher 已 fork 但 TBB worker 尚未
    // 返回，parallel_for 会等它完成才返回 dispatch_one。
    void flush_parallel_map() {
        tbb::parallel_for(std::size_t{0}, std::size_t{0},
                          [](std::size_t) {});
    }

    void push_reorder(IndexLane* lane, std::uint64_t ord, ReorderEntry entry) {
        {
            std::lock_guard<std::mutex> lk(reorder_mu_);
            lane->pending.emplace(ord, std::move(entry));
        }
        reorder_cv_.notify_one();
    }

    // 惰性启动 dispatcher + reducer（首次 register_lib 调）。幂等。
    // reducer 先启动，确保在 dispatcher 推 entry 前已在 reorder_cv_ 等待。
    void ensure_started() {
        std::lock_guard<std::mutex> lk(start_mu_);
        if (started_) return;
        started_ = true;
        reducer_    = std::thread([this] { reducer_loop(); });
        dispatcher_ = std::thread([this] { dispatcher_loop(); });
    }

    void dispatcher_loop() {
        // 关键：必须先 pop 再检查 stopped_。否则 start() 之后立即 stop()
        // 会把 stopped_ 置位，然后才轮到本线程被调度 → 顶部检查就退出、
        // 留下的 sentinel 永远不被消费 → reducer 永远卡在 cv 上。
        while (true) {
            auto task = queue_.pop();
            if (task.op == IndexOp::Sentinel) {
                // Shutdown：dispatcher 单线程串行处理所有任务，循环回到
                // 顶部 pop 必然看到 sentinel 之前所有 parallel_for 已返
                // 回（parallel_for 同步），无需显式 barrier。drain 残留
                // 任务后设 got_sentinel_ 唤醒 reducer。
                IndexTask remaining;
                while (queue_.try_pop(remaining)) {
                    if (remaining.op == IndexOp::Sentinel) continue;
                    dispatch_one(std::move(remaining));
                }
                {
                    std::lock_guard<std::mutex> lk(reorder_mu_);
                    got_sentinel_ = true;
                }
                reorder_cv_.notify_one();
                break;
            }
            dispatch_one(std::move(task));
            // 严格 shutdown 路径必须经 Sentinel；外部 stop() 总是推
            // 一个 Sentinel 进来，所以即使本循环迭代完了所有在飞任务，
            // 下一个 pop 必拿到 sentinel 退出。无需在此读 stopped_。
        }
    }

    // S6-P3: 单 reducer 扫描所有 lane，对每条 lane 严格按其 next_apply_ord
    // 升序 apply（库内 I2/I3：HNSW/索引变更全在此线程 → 单写者）。一条 lane
    // 队头被慢分词卡住时，reducer 改服务其它 lane（无跨库队头阻塞）。
    //
    // lane 生命周期：在 unlock 前拷一份 shared_ptr，使 apply 期间该 lane 不
    // 被并发 unregister_lib 释放（UAF 防护）。apply 后只触碰 atomics +
    // flush 通知，relock 后不再引用旧 lane → break 重扫拿新迭代器。
    void reducer_loop() {
        std::unique_lock<std::mutex> lk(reorder_mu_);
        while (true) {
            reorder_cv_.wait(lk, [this] {
                return any_lane_ready_locked() ||
                       (got_sentinel_ && all_lanes_empty_locked());
            });

            // 退出：sentinel 收到 + 所有 lane buffer 已空
            if (got_sentinel_ && all_lanes_empty_locked()) break;

            // 轮扫各 lane，apply 其连续 ord。任一次 apply 后 break 重扫
            // （unlock 期间 lanes_ 可能因 unregister 失效迭代器）。
            bool progressed = true;
            while (progressed) {
                progressed = false;
                for (auto& [id, lane_sp] : lanes_) {
                    std::shared_ptr<IndexLane> lane = lane_sp;  // 锁下拷活 lane
                    if (lane->pending.count(lane->next_apply_ord) == 0) continue;
                    auto node  = lane->pending.extract(lane->next_apply_ord);
                    auto& entry = node.mapped();
                    lk.unlock();

                    try {
                        lane->reduce_fn(entry);
                    } catch (...) {
                        if (lane->error_fn) lane->error_fn();
                    }

                    lane->applied_ord.store(lane->next_apply_ord,
                                            std::memory_order_release);
                    ++lane->next_apply_ord;
                    dec_in_flight(lane.get());  // 必然执行：异常也推进 ord

                    lk.lock();
                    progressed = true;
                    break;  // 迭代器可能已失效 → 重扫
                }
            }
        }
    }

    // 谓词（持 reorder_mu_ 调）：任一 lane 的下一个 ord 已就绪。
    bool any_lane_ready_locked() const {
        for (const auto& [id, lane] : lanes_) {
            if (lane->pending.count(lane->next_apply_ord) > 0) return true;
        }
        return false;
    }
    // 谓词（持 reorder_mu_ 调）：所有 lane 的 reorder buffer 均空。
    bool all_lanes_empty_locked() const {
        for (const auto& [id, lane] : lanes_) {
            if (!lane->pending.empty()) return false;
        }
        return true;
    }

    // lane->in_flight 减 1；归 0 时持锁 notify 唤醒 flush(lane)/unregister。
    void dec_in_flight(IndexLane* lane) {
        if (lane->in_flight.fetch_sub(1, std::memory_order_release) == 1) {
            std::lock_guard<std::mutex> lk(flush_mu_);
            flush_cv_.notify_all();
        }
    }

    // ---- 成员 ----

    // S6-P2: 双线程 pipeline
    std::thread     dispatcher_;
    std::thread     reducer_;
    // 实际用 tbb::parallel_for(0, 1, lambda) 调度单元素并行 map。原先
    // 用 tbb::task_group 时从 std::thread（dispatcher）调 run()，小对象
    // 分配走 dispatcher 线程的 thread_data，TBB worker 释放 → 线程退出时
    // small_object_pool destroy 断言 m_private_counter<0。parallel_for
    // 把内部 task 分配交给 TBB 调度器所在的 arena，避开此问题。
    // RebuildHnsw barrier 用 flush_parallel_map() 串行调用 parallel_for
    // （无任务时即立即返回，等价于 wait）。

    // S6-P3: 惰性启动（首次 register_lib）。start_mu_ 保护 started_ 与建线程。
    std::mutex                       start_mu_;
    bool                             started_ = false;

    // S6-P3: 库车道表。reorder_mu_ 同时保护 lanes_ 容器与各 lane 的 pending/
    // next_apply_ord（register/unregister 改容器，dispatcher/TBB push pending，
    // reducer 扫描+drain，全在此锁下）。shared_ptr 让 reducer 拷活 lane 防 UAF。
    std::unordered_map<LibId, std::shared_ptr<IndexLane>> lanes_;
    LibId                            next_lib_id_ = 0;

    // facade（单 lane）状态：start() 注册的默认车道 + 其起始 ord。
    IndexLane*                       default_lane_ = nullptr;
    std::uint64_t                    pending_initial_ord_ = 0;

    // Reorder 协调（全 lane 共享一把锁 + cv；apply 极轻，P4 可分片）
    std::mutex                       reorder_mu_;
    std::condition_variable          reorder_cv_;
    bool                             got_sentinel_ = false;  // 退出信号（整池）

    // 控制
    std::atomic<bool>                stopped_{false};
    std::mutex                       flush_mu_;
    std::condition_variable          flush_cv_;
    IndexTaskQueue                   queue_;
};

// TBB 生命周期管理（NIF on_load / on_unload 用）。
// 必须在 on_load 中调用 acquire()，on_unload 中调用 release()。
// 确保 tbb::finalize() 在 .so 卸载前完成，避免 worker 线程持有已卸载的代码。
class TbbLifetime {
public:
    void acquire() {
        handle_.emplace(tbb::attach{});
    }

    void release() {
        if (handle_) {
            tbb::finalize(*handle_);
            handle_.reset();
        }
    }

    bool is_acquired() const { return handle_.has_value(); }

private:
    std::optional<tbb::task_scheduler_handle> handle_;
};

}  // namespace bitcask