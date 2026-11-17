// 索引线程池：registry 级共享的 Index Pool（写/索引侧）。
//
// === S45-4：分片执行器（去 reducer）===
// 设计：designs/index-pipeline-unordered-design-zh.md §4.2/§4.4/§5。
//
//   写线程 submit(lane, task)
//      └→ shard = hash(key) mod N → 该分片的有界队列
//   分片线程 s（N 个，跨库共享）：
//      lane.shard_apply(task, s)          —— 宿主裁决 + docmap + 分片并发插件
//        ├ kDone          → 记 LSN 完成
//        └ kForwardSerial → 转串行道
//   串行道线程（1 个）：lane.serial_apply(task) → 记 LSN 完成（HNSW 等单写者插件）
//   维护线程（1 个）：run_exclusive(lane, fn) —— 按库暂停 → fn → 恢复
//
// **没有任何线程等待某个特定 LSN**：写线程只会被「目标队列满」挡住，分片/
// 串行线程只在自己队列空时等待，维护线程只等该库「正在执行的应用」结束——
// 三者不成环（旧 reorder ring + 单 reducer 的乱序死锁在结构上不存在）。
//
// 正确性不再依赖应用顺序：同 key 任务恒落同一分片（串行），乱序到达的旧
// 版本由宿主的 key+LSN 裁决丢弃（规则 R，§4.1）。「已应用到哪」由每库的
// LsnTracker 给出（LSN 连续完成前缀，§4.3）。
//
// === 锁与死锁约定 ===
//   - lanes_mu_：护 lanes_/graveyard_/next_lib_id_；持有期间不取其它锁、不阻塞。
//   - lane.mu：护该库暂停/排空的 cv 等待；持有期间不取其它锁。
//   - start_mu_：护 started_ + 建线程/停线程。
//   - maint_mu_：护维护作业队列。
//   任一线程任一时刻至多持其中一把（队列 push/pop 的内部锁不与它们嵌套；
//   stop() 持 start_mu_ 时只做 push 哨兵 + join，被 join 的线程不取 start_mu_）。
//   独占闭包（run_exclusive 的 fn）内**不得**提交本库索引任务、不得等本库
//   读屏障（wait_lsn）——它们要等应用推进，而应用正被本闭包暂停。
//
// === 生命周期 ===
//   1. registry 懒创建 IndexPool（IndexRegistry::index_pool）
//   2. Cask::open（search 模式）→ register_lib 注册本库 lane（首次时启动线程）
//   3. put/delete → submit(lane, task)；失败/不进索引的 LSN → complete(lane, ord)
//   4. Cask::close → unregister_lib（有界排空；超时则 lane 摘钩入墓地，不析构）
//   5. registry 析构 → ~IndexPool → stop()：哨兵排干后 join 全部线程

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <oneapi/tbb/concurrent_queue.h>  // concurrent_bounded_queue（oneTBB 已并入此头）
#include <oneapi/tbb/global_control.h>

#include "bitcask/lsn_tracker.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/string_hash.hpp"
#include "bitcask/detail/cpu_features.hpp"  // S37-4：BITCASK_TSAN_ENABLED

#if BITCASK_TSAN_ENABLED   // S37-4：见 detail/cpu_features.hpp
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#endif

namespace bitcask {

// 索引操作类型。S45-4：Skip（序号空洞）由 IndexPool::complete 取代；RunFn
// （reducer 内执行回调）由 run_exclusive 取代——二者都不再占 LSN、不再排队。
enum class IndexOp : std::uint8_t {
    Add,         // 记录写入（宿主裁决后广播 on_put）
    Delete,      // 记录删除（宿主裁决后广播 on_delete）
    Sentinel,    // 停止信号（每个分片/串行线程各消费一个后退出）
};

struct IndexLane;

// 索引任务：put/delete 路径提交到 Index Pool 的异步任务。
// key / text 必须拥有独立存储（原始数据在 put() 返回后可能被释放），合并进
// 单个 buf（= key ⧺ text，key_len 划界）。
struct IndexTask {
    IndexOp              op;
    // 路由车道（submit 时填入）。生命周期：lane 在其在途任务全部完成前不
    // 析构（unregister 超时则摘钩入墓地，见 IndexPool::unregister_lib）。
    IndexLane*           lane = nullptr;
    std::string          buf;          // key ⧺ text 合并存储
    std::uint64_t        ord       = 0;
    std::uint32_t        key_len   = 0;
    std::uint32_t        file_id   = 0;
    std::uint64_t        offset    = 0;
    std::uint32_t        total_sz  = 0;
    std::uint64_t        tstamp    = 0;
    std::uint32_t doc_len   = 0; // token 总数（BM25 统计用）
    // S10-A5:多字段名+值打包进 fields_store(一次分配),fields 持 string_view 借自其中。
    std::vector<char> fields_store;
    std::vector<std::pair<std::string_view, std::string_view>> fields;
    // V3.3:Add 任务的文档向量(已归一化;空 = 无)。
    std::vector<float> vec;
    // V5:文档结构化 meta blob(可为空)。
    std::vector<std::byte> meta;
    // S45-4：Delete 转串行道时携带分片阶段捕获的被删版本 ord（kNoPriorOrd =
    // 未删除任何版本，串行插件应跳过）。
    std::uint64_t prior_ord = plugin::kNoPriorOrd;
    // S45-5：恢复重放任务（透传 PutEvent::replay，插件据此调整执行策略）。
    bool replay = false;

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
                          std::uint32_t total_sz_, std::uint64_t tstamp_,
                          std::uint32_t doc_len_) {
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
        return t;
    }

    static IndexTask sentinel() {
        IndexTask t;
        t.op = IndexOp::Sentinel;
        return t;
    }
};

// ---- IndexTask → 插件事件视图（宿主分发与契约测试共用）----
// 返回值借用 task 的缓冲，仅在 task 存活期间有效。
inline plugin::DocView make_doc_view(const IndexTask& t) {
    return plugin::DocView{t.text(), t.fields, t.vec, t.meta};
}
inline plugin::PutEvent make_put_event(const IndexTask& t,
                                       const plugin::DocView* doc) {
    plugin::PutEvent e;
    e.ord    = t.ord;
    e.key    = t.key();
    e.value  = t.text();
    e.doc    = doc;
    e.loc    = {t.file_id, t.offset, t.total_sz};
    e.tstamp = t.tstamp;
    e.replay = t.replay;
    return e;
}

// 有界任务队列（多生产者 → 单消费者：每分片一条 + 串行道一条）。
class IndexTaskQueue {
public:
    explicit IndexTaskQueue(std::size_t capacity = 2048) {
        queue_.set_capacity(static_cast<std::ptrdiff_t>(capacity));
    }

    // C1:tbb 队列内部以 fence 同步,TSan 不建模独立 fence——用标注 API 把
    // 移交的 happens-before 显式告知 runtime;非 TSan 构建为空操作。
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
    std::size_t size() const {
        const auto n = queue_.size();
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

private:
#if BITCASK_TSAN_ENABLED
    void annotate_release() { __tsan_release(&queue_); }
    void annotate_acquire() { __tsan_acquire(&queue_); }
#else
    void annotate_release() {}
    void annotate_acquire() {}
#endif
    tbb::concurrent_bounded_queue<IndexTask> queue_;
};

// ---- 车道回调 ----
enum class ApplyResult : std::uint8_t {
    kDone,           // 本任务已全部应用（或被裁决丢弃）
    kForwardSerial,  // 还需串行道（单写者插件）应用
};
// 分片线程调用：同 key 串行、不同 key 并发；shard ∈ [0, shard_count)。
using ShardApplyFn  = std::function<ApplyResult(IndexTask&, unsigned shard)>;
// 串行道线程调用：全池单线程。
using SerialApplyFn = std::function<void(IndexTask&)>;
using ErrorFn       = std::function<void()>;

struct LaneCallbacks {
    ShardApplyFn  shard_apply;
    SerialApplyFn serial_apply;   // 可空（无单写者插件）
    ErrorFn       on_error;       // 可空
};

using LibId = std::uint64_t;

// 每库车道：回调 + LSN 完成跟踪 + 暂停/排空状态。
struct IndexLane {
    LibId         id = 0;
    LaneCallbacks cb;
    LsnTracker    tracker;
    // 在途 = 已提交未完成的任务 + 已排队未结束的维护作业（unregister 排空）。
    std::atomic<std::size_t> in_flight{0};
    // 正在执行的应用/维护闭包数（暂停与摘钩等它归零）。
    std::atomic<std::size_t> active{0};
    std::atomic<bool> paused{false};
    // 摘钩：unregister 超时后置位——此后到达的任务/作业不再回调宿主（宿主
    // 资源即将释放），只记完成。
    std::atomic<bool> detached{false};
    std::mutex              mu;
    std::condition_variable cv;
};

// S45-6 校准（docs/design/s45-6-bench-zh.md）：512 与 2048 吞吐持平（≈7 万
// 文档/s），但在途积压小 4 倍——持续写入下读屏障 p50 220→46 ms、checkpoint
// 220→94 ms（积压决定「等调用时刻之前的 LSN 全部应用」要排多久）。
inline constexpr std::size_t kDefaultShardQueueCapacity  = 512;
inline constexpr std::size_t kDefaultSerialQueueCapacity = 4096;

class IndexPool {
public:
    // shards = 分片线程数（即 index_workers）。
    explicit IndexPool(int shards = 1,
                       std::size_t shard_queue_capacity = kDefaultShardQueueCapacity,
                       std::size_t serial_queue_capacity = kDefaultSerialQueueCapacity,
                       std::chrono::milliseconds unregister_flush_timeout =
                           kUnregisterFlushTimeout)
        : shard_count_(shards > 0 ? static_cast<unsigned>(shards) : 1u),
          unregister_flush_timeout_(unregister_flush_timeout),
          serial_q_(serial_queue_capacity) {
        shard_q_.reserve(shard_count_);
        for (unsigned i = 0; i < shard_count_; ++i) {
            shard_q_.push_back(std::make_unique<IndexTaskQueue>(shard_queue_capacity));
        }
    }

    ~IndexPool() { stop(); }

    IndexPool(const IndexPool&) = delete;
    IndexPool& operator=(const IndexPool&) = delete;

    // 拆卸路径排空上界（与 Cask::close / checkpoint 的 30s 同量级）。
    static constexpr std::chrono::seconds kUnregisterFlushTimeout{30};

    [[nodiscard]] unsigned shard_count() const noexcept { return shard_count_; }
    [[nodiscard]] unsigned shard_of(std::string_view key) const noexcept {
        return static_cast<unsigned>(StringHash{}(key) % shard_count_);
    }

    // 注册一条库车道。init_ord = 该库「下一个待分配 LSN」（之前的 LSN 视为
    // 已完成——恢复期已建索引）。首次注册时惰性启动线程。
    IndexLane* register_lib(LaneCallbacks cb, std::uint64_t init_ord = 0) {
        IndexLane* raw = nullptr;
        {
            std::lock_guard<std::mutex> lk(lanes_mu_);
            auto lane = std::make_shared<IndexLane>();
            lane->id = next_lib_id_++;
            lane->cb = std::move(cb);
            lane->tracker.reset(init_ord);
            raw = lane.get();
            lanes_.emplace(lane->id, std::move(lane));
        }
        ensure_started();
        return raw;
    }

    // 注销车道：有界排空（在途任务 + 排队的维护作业）。排空 → 析构 lane；
    // 超时 → 摘钩（此后不再回调宿主），等正在执行的回调结束（同一上界），
    // lane 移入墓地不析构——在途任务持其裸指针，析构即 UAF。返回是否排空。
    // final_coverage 非空时回填注销前一刻的 LSN 连续完成前缀。
    [[nodiscard]] bool unregister_lib(IndexLane* lane,
                                      std::uint64_t* final_coverage = nullptr) {
        if (!lane) return true;
        const bool drained = flush(lane, unregister_flush_timeout_);
        if (final_coverage) *final_coverage = lane->tracker.contiguous();
        if (!drained) {
            lane->detached.store(true, std::memory_order_seq_cst);
            std::unique_lock<std::mutex> lk(lane->mu);
            (void)lane->cv.wait_for(lk, unregister_flush_timeout_, [lane] {
                return lane->active.load(std::memory_order_seq_cst) == 0;
            });
        }
        std::lock_guard<std::mutex> lk(lanes_mu_);
        auto it = lanes_.find(lane->id);
        if (it != lanes_.end()) {
            if (!drained) graveyard_.push_back(it->second);
            lanes_.erase(it);
        }
        return drained;
    }

    // 提交任务到车道。背压 = 目标分片队列有界（满则阻塞调用者）。
    void submit(IndexLane* lane, IndexTask task) {
        if (!lane) return;
        const std::uint64_t ord = task.ord;
        if (stopped_.load(std::memory_order_acquire) ||
            lane->detached.load(std::memory_order_acquire)) {
            // 池已停 / 车道已摘钩：任务丢弃，但 LSN 必须记完成——否则 tracker
            // 前缀永久停住，读屏障挂死。
            lane->tracker.done(ord);
            return;
        }
        lane->in_flight.fetch_add(1, std::memory_order_relaxed);
        task.lane = lane;
        const unsigned s = shard_of(task.key());
        try {
            shard_q_[s]->push(std::move(task));
        } catch (...) {
            // 队列分配失败（bad_alloc）：本条索引丢失，但 LSN 记完成、在途
            // 归还——不得让读屏障/close 挂死（P6-MEM-1 同款补偿）。
            lane->tracker.done(ord);
            dec_in_flight(*lane);
            throw;
        }
    }

    // 失败/不进索引的 LSN 记完成（取代旧 Skip 任务；不排队、不阻塞）。
    void complete(IndexLane* lane, std::uint64_t ord) noexcept {
        if (lane) lane->tracker.done(ord);
    }

    // 读屏障：等 LSN 连续完成前缀 ≥ target。timeout 为空 = 无界（搜索读己之写）。
    bool wait_lsn(IndexLane* lane, std::uint64_t target,
                  std::optional<std::chrono::milliseconds> timeout =
                      std::nullopt) {
        if (!lane) return true;
        return lane->tracker.wait_until(target, timeout);
    }
    [[nodiscard]] std::uint64_t coverage(IndexLane* lane) const {
        return lane ? lane->tracker.contiguous() : 0;
    }

    // 等该车道在途归零（已提交任务全部完成 + 排队维护作业全部结束）。
    bool flush(IndexLane* lane,
               std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
        if (!lane) return true;
        std::unique_lock<std::mutex> lk(lane->mu);
        auto drained = [lane] {
            return lane->in_flight.load(std::memory_order_acquire) == 0;
        };
        if (!timeout) {
            lane->cv.wait(lk, drained);
            return true;
        }
        return lane->cv.wait_for(lk, *timeout, drained);
    }

    // 独占执行：维护线程上按 FIFO 执行 fn——执行期间该库暂停（不再开始新的
    // 应用，已在执行的先结束）。fire-and-forget。池已停/车道未注册时调用线程
    // 直跑（此时无并发应用）。
    void run_exclusive(IndexLane* lane, std::function<void()> fn) {
        if (!lane || !fn) return;
        std::shared_ptr<IndexLane> sp;
        {
            std::lock_guard<std::mutex> lk(lanes_mu_);
            auto it = lanes_.find(lane->id);
            if (it != lanes_.end()) sp = it->second;
        }
        if (!sp || stopped_.load(std::memory_order_acquire)) {
            if (!lane->detached.load(std::memory_order_acquire)) {
                try { fn(); } catch (...) {
                    if (lane->cb.on_error) lane->cb.on_error();
                }
            }
            return;
        }
        lane->in_flight.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(maint_mu_);
            maint_q_.push_back(MaintJob{std::move(sp), std::move(fn), false});
        }
        maint_cv_.notify_one();
    }

    // 停整池（registry 析构 / 测试收尾）。哨兵排在全部真任务之后：分片先排干
    // （可能转串行道），再停串行道，最后停维护线程。幂等。
    void stop() {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
            return;
        }
        std::lock_guard<std::mutex> slk(start_mu_);
        if (!started_) return;
        for (auto& q : shard_q_) q->push(IndexTask::sentinel());
        for (auto& t : shard_threads_) if (t.joinable()) t.join();
        serial_q_.push(IndexTask::sentinel());
        if (serial_thread_.joinable()) serial_thread_.join();
        {
            std::lock_guard<std::mutex> lk(maint_mu_);
            maint_q_.push_back(MaintJob{nullptr, nullptr, true});
        }
        maint_cv_.notify_one();
        if (maint_thread_.joinable()) maint_thread_.join();
    }

    [[nodiscard]] bool is_stopped() const {
        return stopped_.load(std::memory_order_acquire);
    }

    // 内省：各队列当前深度之和（StatusInfo / 测试）。
    [[nodiscard]] std::size_t queued_tasks() const {
        std::size_t n = serial_q_.size();
        for (const auto& q : shard_q_) n += q->size();
        return n;
    }
    [[nodiscard]] std::size_t lane_count() {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        return lanes_.size();
    }
    [[nodiscard]] std::size_t graveyard_size() {
        std::lock_guard<std::mutex> lk(lanes_mu_);
        return graveyard_.size();
    }

private:
    struct MaintJob {
        std::shared_ptr<IndexLane> lane;
        std::function<void()>      fn;
        bool                       sentinel = false;
    };

    void ensure_started() {
        std::lock_guard<std::mutex> lk(start_mu_);
        if (started_ || stopped_.load(std::memory_order_acquire)) return;
        started_ = true;
        shard_threads_.reserve(shard_count_);
        for (unsigned s = 0; s < shard_count_; ++s) {
            shard_threads_.emplace_back([this, s] { shard_loop(s); });
        }
        serial_thread_ = std::thread([this] { serial_loop(); });
        maint_thread_  = std::thread([this] { maint_loop(); });
    }

    // ---- 暂停协议（Dekker 式：applier 先登记 active 再查 paused，维护者先置
    // paused 再等 active 归零；seq_cst 保证二者至少一方看见对方）----
    static void enter(IndexLane& l) {
        for (;;) {
            l.active.fetch_add(1, std::memory_order_seq_cst);
            if (!l.paused.load(std::memory_order_seq_cst)) return;
            leave(l);  // 退让：让维护者看到 active 归零
            std::unique_lock<std::mutex> lk(l.mu);
            l.cv.wait(lk, [&l] { return !l.paused.load(std::memory_order_seq_cst); });
        }
    }
    static void leave(IndexLane& l) {
        if (l.active.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
            (l.paused.load(std::memory_order_seq_cst) ||
             l.detached.load(std::memory_order_seq_cst))) {
            std::lock_guard<std::mutex> lk(l.mu);
            l.cv.notify_all();
        }
    }
    static void pause(IndexLane& l) {
        l.paused.store(true, std::memory_order_seq_cst);
        std::unique_lock<std::mutex> lk(l.mu);
        l.cv.wait(lk, [&l] { return l.active.load(std::memory_order_seq_cst) == 0; });
    }
    static void resume(IndexLane& l) {
        {
            std::lock_guard<std::mutex> lk(l.mu);
            l.paused.store(false, std::memory_order_seq_cst);
        }
        l.cv.notify_all();
    }

    // 归零的那一次递减必须在 lane.mu 内完成：否则「递减到 0 → 取锁 notify」
    // 之间，flush/unregister 可能已看到 0 并析构车道，本线程随即锁一把已
    // 释放的 mutex（TSan 实测 heap-use-after-free）。非归零递减走无锁 CAS。
    // 归零递减返回后本线程不再触碰车道。
    static void dec_in_flight(IndexLane& l) {
        std::size_t prev = l.in_flight.load(std::memory_order_acquire);
        for (;;) {
            if (prev <= 1) {
                std::lock_guard<std::mutex> lk(l.mu);
                if (l.in_flight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    l.cv.notify_all();
                }
                return;
            }
            if (l.in_flight.compare_exchange_weak(prev, prev - 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return;
            }
        }
    }
    static void finish(IndexLane& l, std::uint64_t ord) {
        l.tracker.done(ord);
        dec_in_flight(l);
    }

    void shard_loop(unsigned s) {
        IndexTaskQueue& q = *shard_q_[s];
        for (;;) {
            IndexTask t = q.pop();
            if (t.op == IndexOp::Sentinel) break;
            IndexLane& lane = *t.lane;
            ApplyResult r = ApplyResult::kDone;
            enter(lane);
            if (!lane.detached.load(std::memory_order_acquire)) {
                try {
                    r = lane.cb.shard_apply(t, s);
                } catch (...) {
                    if (lane.cb.on_error) lane.cb.on_error();
                    r = ApplyResult::kDone;
                }
            }
            leave(lane);
            if (r == ApplyResult::kForwardSerial && lane.cb.serial_apply) {
                const std::uint64_t ord = t.ord;
                try {
                    serial_q_.push(std::move(t));
                    continue;
                } catch (...) {
                    if (lane.cb.on_error) lane.cb.on_error();
                    finish(lane, ord);
                    continue;
                }
            }
            finish(lane, t.ord);
        }
    }

    void serial_loop() {
        for (;;) {
            IndexTask t = serial_q_.pop();
            if (t.op == IndexOp::Sentinel) break;
            IndexLane& lane = *t.lane;
            enter(lane);
            if (!lane.detached.load(std::memory_order_acquire)) {
                try {
                    lane.cb.serial_apply(t);
                } catch (...) {
                    if (lane.cb.on_error) lane.cb.on_error();
                }
            }
            leave(lane);
            finish(lane, t.ord);
        }
    }

    void maint_loop() {
        for (;;) {
            MaintJob job;
            {
                std::unique_lock<std::mutex> lk(maint_mu_);
                maint_cv_.wait(lk, [this] { return !maint_q_.empty(); });
                job = std::move(maint_q_.front());
                maint_q_.pop_front();
            }
            if (job.sentinel) break;
            IndexLane& lane = *job.lane;
            if (!lane.detached.load(std::memory_order_acquire)) {
                pause(lane);
                // 维护闭包自身也计入 active：摘钩路径等它结束再释放宿主资源。
                lane.active.fetch_add(1, std::memory_order_seq_cst);
                try {
                    job.fn();
                } catch (...) {
                    if (lane.cb.on_error) lane.cb.on_error();
                }
                leave(lane);
                resume(lane);
            }
            dec_in_flight(lane);
        }
    }

    const unsigned                   shard_count_;
    const std::chrono::milliseconds  unregister_flush_timeout_;
    std::vector<std::unique_ptr<IndexTaskQueue>> shard_q_;
    IndexTaskQueue                   serial_q_;
    std::vector<std::thread>         shard_threads_;
    std::thread                      serial_thread_;
    std::thread                      maint_thread_;

    std::mutex                       start_mu_;
    bool                             started_ = false;

    std::mutex                       lanes_mu_;
    std::unordered_map<LibId, std::shared_ptr<IndexLane>> lanes_;
    std::vector<std::shared_ptr<IndexLane>> graveyard_;  // 超时摘钩的车道
    LibId                            next_lib_id_ = 0;

    std::mutex                       maint_mu_;
    std::condition_variable          maint_cv_;
    std::deque<MaintJob>             maint_q_;

    std::atomic<bool>                stopped_{false};
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
