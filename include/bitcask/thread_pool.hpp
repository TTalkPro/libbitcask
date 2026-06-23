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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

// 索引任务：put/delete 路径提交到 Index Pool 的异步任务。
// key / text 必须拥有独立存储（非 string_view），因为原始数据在 put()
// 返回后可能被释放。两者合并进单个 buf（= key ⧺ text，key_len 划界），
// 一次分配替代原先 key/text 两个 string。
struct IndexTask {
    IndexOp              op;
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

// per-Cask 的索引线程管理器。
// S6-P2: 拆成 dispatcher（pop 任务 + 分发到 TBB 或 reorder）+ reducer（按 ord
// 序 apply）双线程架构。tbb::task_group 跑 map_fn_；reorder buffer 用 mutex
// + cv 协调两端。
// T6 阶段会增加 Search Pool（基于 TBB task_arena）。
class IndexPool {
public:
    explicit IndexPool(int concurrency = 1, std::size_t queue_capacity = 10240)
        : stopped_(false)
        , pending_(0)
        , next_apply_ord_(0)
        , queue_(queue_capacity)
    {
        (void)concurrency;
    }

    ~IndexPool() { stop(); }

    IndexPool(const IndexPool&) = delete;
    IndexPool& operator=(const IndexPool&) = delete;

    // S6-P2: 注入 reorder buffer 起始 ord —— cask::open 时从 keydir 读出
    // 「已分配的最大 ord + 1」，跳过 [0, init_ord) 区间（视为已 applied，
    // 由 disk fold 恢复时统一建索引）。不在 start() 之前调，等于 0，
    // 与 P0/P1 单 worker 行为一致。
    void set_initial_ord(std::uint64_t init_ord) {
        next_apply_ord_ = init_ord;
    }

    void submit(IndexTask task) {
        if (stopped_.load(std::memory_order_acquire)) return;
        // S6-P2: RebuildHnsw 现在携带 ord（merge 路径 alloc_ord），所以纳入
        // submitted_ord_hwm 跟踪；Sentinel 不携带 ord，仍跳过。
        if (task.op != IndexOp::Sentinel) {
            auto prev = submitted_ord_hwm_.load(std::memory_order_relaxed);
            while (task.ord > prev &&
                   !submitted_ord_hwm_.compare_exchange_weak(
                       prev, task.ord,
                       std::memory_order_seq_cst,
                       std::memory_order_relaxed)) {
                // prev updated by CAS failure; retry
            }
        }
        pending_.fetch_add(1, std::memory_order_relaxed);
        queue_.push(std::move(task));
    }

    // S6-P2: 新 start API — map/reduce/error 三段回调。
    // reducer 线程先启动，确保 reducer 在 dispatcher 开始推 entry 之前就
    // 已在 reorder_cv_ 上等待（无丢失唤醒）。
    void start(MapFn map_fn, ReduceFn reduce_fn, ErrorFn error_fn) {
        map_fn_    = std::move(map_fn);
        reduce_fn_ = std::move(reduce_fn);
        error_fn_  = std::move(error_fn);
        reducer_    = std::thread([this] { reducer_loop(); });
        dispatcher_ = std::thread([this] { dispatcher_loop(); });
    }

    void stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
            return;
        }
        queue_.push(IndexTask{IndexOp::Sentinel});
        // joinable() guard 兼容「start() 从未调用」场景：默认构造的
        // std::thread 不 joinable，跳过 join——Cask::open 失败回滚时
        // IndexPool 可能在 start() 前被析构，此 guard 防止 UB。
        if (dispatcher_.joinable()) dispatcher_.join();
        // S6-P2: dispatcher 退出时已串行排干所有 in-flight map（dispatcher
        // 单线程 + parallel_for 同步）。无需额外 wait。
        if (reducer_.joinable())    reducer_.join();
    }

    // 等待所有已提交任务被 reducer 消费完毕。
    // W3:cv 替代自旋 yield。改为阻塞等待，reducer 把 pending_ 减到
    // 0 时 notify。谓词在锁下复查规避丢失唤醒（reducer 的 notify 也持同
    // 一锁）。
    //
    // S6-P1: flush 等待 (1) 所有已提交任务消费完毕（pending_==0，含
    // RebuildHnsw 等非 ord 任务）+ (2) applied_ord 追上 submitted_ord_hwm
    // （ord 任务全部 apply）。P2 并行 map 后 (2) 成为独立必要条件——
    // 严格 ord 序 apply 由 reducer_loop 的 next_apply_ord_ 顺序保证。
    void flush() {
        std::unique_lock<std::mutex> lk(flush_mu_);
        flush_cv_.wait(lk, [this] {
            return pending_.load(std::memory_order_acquire) == 0
                && applied_ord_.load(std::memory_order_acquire) >=
                   submitted_ord_hwm_.load(std::memory_order_acquire);
        });
    }

    bool is_stopped() const {
        return stopped_.load(std::memory_order_acquire);
    }

    IndexTaskQueue&       queue()       { return queue_; }
    const IndexTaskQueue& queue() const { return queue_; }

    // S6-P1: 测试用——返回当前 applied_ord（reducer 已处理到的最大 ord）。
    std::uint64_t applied_ord() const {
        return applied_ord_.load(std::memory_order_acquire);
    }
    std::uint64_t submitted_ord_hwm() const {
        return submitted_ord_hwm_.load(std::memory_order_acquire);
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
            MapFn     map_fn   = map_fn_;
            ErrorFn   error_fn = error_fn_;
            std::mutex*                       mu = &reorder_mu_;
            std::condition_variable*          cv = &reorder_cv_;
            std::map<std::uint64_t, ReorderEntry>* pending = &reorder_pending_;
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
            push_reorder(task.ord, ReorderEntry{RebuildEntry{}});
        } else if (task.op == IndexOp::Delete) {
            DeleteEntry de;
            de.key = std::string(task.key());
            de.ord = task.ord;
            push_reorder(task.ord, ReorderEntry{std::move(de)});
        } else if (task.op == IndexOp::Skip) {
            push_reorder(task.ord, ReorderEntry{SkipEntry{}});
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
            push_reorder(task.ord, ReorderEntry{std::move(owe)});
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

    void push_reorder(std::uint64_t ord, ReorderEntry entry) {
        {
            std::lock_guard<std::mutex> lk(reorder_mu_);
            reorder_pending_.emplace(ord, std::move(entry));
        }
        reorder_cv_.notify_one();
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

    // reducer：严格按 next_apply_ord_ 升序取出 entry 并 apply（I2）。
    // 单一写者（I3）：HNSW / 索引的全部变更都跑在此线程，无并发写。
    // 异常路径：try/catch 包裹 reduce_fn_，失败调 error_fn_，但仍推进 ord
    // ——否则 reorder buffer 在该 ord 处永久 stall（apply 失败也必须推进）。
    void reducer_loop() {
        while (true) {
            std::unique_lock<std::mutex> lk(reorder_mu_);
            reorder_cv_.wait(lk, [this] {
                return reorder_pending_.count(next_apply_ord_) > 0
                    || (reorder_pending_.empty() && got_sentinel_);
            });

            // 退出：sentinel 收到 + buffer 已空
            if (got_sentinel_ && reorder_pending_.empty()) break;

            // 按 ord 连续 apply
            while (reorder_pending_.count(next_apply_ord_) > 0) {
                auto node = reorder_pending_.extract(next_apply_ord_);
                auto& entry = node.mapped();
                lk.unlock();

                try {
                    reduce_fn_(entry);
                } catch (...) {
                    if (error_fn_) error_fn_();
                }

                applied_ord_.store(next_apply_ord_, std::memory_order_release);
                ++next_apply_ord_;
                dec_pending();  // 必然执行：异常也推进 ord
                lk.lock();
            }
        }
    }

    // pending_ 减 1；归 0 时持锁 notify 唤醒 flush()。仅在真正归零时取锁，
    // 重负载下队列非空，notify 罕见，无额外争用。
    void dec_pending() {
        if (pending_.fetch_sub(1, std::memory_order_release) == 1) {
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

    // 回调（start 注入；reducer 线程与 TBB 线程都会读——const 引用读安全）
    MapFn    map_fn_;
    ReduceFn reduce_fn_;
    ErrorFn  error_fn_;

    // Reorder buffer：dispatcher / TBB map 推，reducer 按 ord 升序取
    std::mutex                       reorder_mu_;
    std::condition_variable          reorder_cv_;
    std::uint64_t                    next_apply_ord_ = 0;  // 仅 reducer 访问
    std::map<std::uint64_t, ReorderEntry> reorder_pending_;
    bool                             got_sentinel_ = false;  // 退出信号

    // 控制 / 跟踪（保留）
    std::atomic<bool>                stopped_{false};
    std::atomic<std::size_t>         pending_{0};
    // S6-P1: ord 水位跟踪（flush 语义升级基础设施）。
    // submitted_ord_hwm_：已 submit 的最大 ord（Add/Delete/Skip/RebuildHnsw
    //   携带 ord；Sentinel 不携带，不更新 hwm）。
    // applied_ord_：reducer 已处理到的最大 ord（reduce_fn_ 返回后更新）。
    // P2 并行 map 下 pending_ 不再蕴含 applied，此跟踪成为 flush 的主依据。
    std::atomic<std::uint64_t>       submitted_ord_hwm_{0};
    std::atomic<std::uint64_t>       applied_ord_{0};
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