// cask_range_iter.cpp — S33-5：OKI 有序 Range 迭代器。
// 语义与架构见 cask.hpp CaskRangeIter 类注释与
// doc/ordered-key-index-design-zh.md §4。

#include "bitcask/cask.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "cask_internal.hpp"  // err / io_fault / bytes_to_view

namespace bitcask {

namespace {

std::string_view sv(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

std::expected<std::unique_ptr<CaskRangeIter>, CaskFault>
Cask::make_range_iter(const RangeOptions& opts) {
    if (!authority_) return std::unexpected(err(CaskError::kClosed));
    auto view = authority_->state().make_read_view();
    if (!view && opts_.read_write && !opts_.merge_only) {
        // S43-5：Range 也进入写前/读前可用性闸。旧 Level A 首次 Range
        // 或运行期降级时触发同一条受控重建；并发读者短暂等待新视图，
        // 而不是把迁移/重建窗口折叠成“没有索引”。
        for (;;) {
            if (oki_rebuild_failed_) break;
            view = authority_->state().make_read_view();
            if (view) break;
            if (oki_rebuilding_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
                continue;
            }
            if (!ensure_oki_for_write()) break;
            view = authority_->state().make_read_view();
            if (view) break;
            // ensure 认为可用但视图尚未发布（理论竞态）；下一次重读。
            std::this_thread::yield();
        }
    }
    if (!view) {
        // 按成因拆码（见 cask.hpp 枚举注释）：试建而败 vs 本就不建。
        if (oki_rebuild_failed_) {
            return std::unexpected(err(
                CaskError::kIndexRebuildFailed,
                "oki rebuild failed at open (io/env problem, see log; "
                "fix and reopen to retry)"));
        }
        return std::unexpected(err(
            CaskError::kNoIndex,
            "oki unavailable on this handle (read-only or merge-only open "
            "of a dir without oki; reopen read-write to build)"));
    }

    // unique_ptr + 私有构造：make_unique 不可用，直接 new。
    std::unique_ptr<CaskRangeIter> it(new CaskRangeIter());
    it->cask_ = this;
    it->view_ = *std::move(view);
    it->hi_.assign(sv(opts.hi));
    it->has_hi_ = !opts.hi.empty();
    it->prefetch_ = opts.prefetch;
    it->prefetch_threads_ = opts.prefetch_threads;
    it->want_meta_ = opts.want_meta;
    it->filter_ = opts.filter;

    auto mc = oki::OkiMergeCursor::open(it->view_, opts.lo);
    if (!mc) {
        return std::unexpected(err(CaskError::kBadCrc,
                                   "oki run seek failed (corrupt run)"));
    }
    it->merged_.emplace(std::move(*mc));
    return it;
}

std::expected<std::optional<std::string>, CaskFault>
CaskRangeIter::next_merged_key() {
    if (done_ || !merged_) return std::optional<std::string>{};
    oki::OkiState::DeltaRow row;
    auto n = merged_->next(row);  // 规则见 OkiMergeCursor；墓碑已抵消
    if (!n) {
        return std::unexpected(err(CaskError::kBadCrc,
                                   "oki run read failed mid-scan"));
    }
    if (!*n || (has_hi_ && std::string_view(row.key) >= std::string_view(hi_))) {
        done_ = true;
        return std::optional<std::string>{};
    }
    return std::optional<std::string>(std::move(row.key));
}

// 惰性单条：归并出 key → 立刻回查 keydir 取权威值（key 可能已死 / 已被
// 更新——OKI 行陈旧无害）。kNotFound → 跳过；其余错误上抛。
std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::next() {
    if (prefetch_ > 1) {
        // 预取路径：缓冲空则再取一批（整批 key 全死时 buf_ 为空而迭代
        // 未结束——循环直到有货或到尾）。
        while (buf_pos_ >= buf_.size()) {
            if (done_) return std::optional<Entry>{};
            auto f = fill_prefetch();
            if (!f) return std::unexpected(f.error());
        }
        return std::optional<Entry>(std::move(buf_[buf_pos_++]));
    }

    while (true) {
        auto k = next_merged_key();
        if (!k) return std::unexpected(k.error());
        if (!*k) return std::optional<Entry>{};
        auto e = fetch_entry(**k);
        if (!e) return std::unexpected(e.error());
        if (!*e) continue;  // 死 key / filter 不通过
        return e;
    }
}

// 反馈 2026-10-06：走零拷贝 get 视图——filter 在视图的 meta 段上求值，
// 不通过的连 value 都不拷；value 与 meta 出自同一次读（同一版本）。
std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::fetch_entry(const std::string& key) const {
    const auto* kp = reinterpret_cast<const std::byte*>(key.data());
    auto g = cask_->get(std::span<const std::byte>(kp, key.size()));
    if (!g) {
        if (g.error().kind == CaskError::kNotFound) {
            return std::optional<Entry>{};
        }
        return std::unexpected(g.error());
    }
    if (filter_ && (g->meta.empty() || !filter_->evaluate(g->meta))) {
        return std::optional<Entry>{};  // 无 meta 不通过（同检索侧）
    }
    Entry out;
    out.key.assign(kp, kp + key.size());
    out.value.assign(g->value.begin(), g->value.end());
    if (want_meta_) out.meta.assign(g->meta.begin(), g->meta.end());
    out.tstamp = g->tstamp;
    out.ord = g->ord;
    return std::optional<Entry>(std::move(out));
}

// S33-6：预取一批。归并 ≤prefetch_ 个 key（串行、廉价，只碰 run 块与
// memdelta）→ 分段并发 get 取值（被并行化的是 value 的 pread+decode，
// 与 parallel_scan 同款 JoiningPool）→ 按 key 序压实进 buf_。
// 死 key（并发删除 / OKI 行陈旧）与 parallel_scan 同样静默跳过；filter 不
// 通过的同样不进 buf_。
std::expected<void, CaskFault> CaskRangeIter::fill_prefetch() {
    buf_.clear();
    buf_pos_ = 0;

    std::vector<std::string> keys;
    keys.reserve(prefetch_);
    while (keys.size() < prefetch_) {
        auto k = next_merged_key();
        if (!k) return std::unexpected(k.error());
        if (!*k) break;  // done_ 已由 next_merged_key 置位
        keys.push_back(*std::move(*k));
    }
    if (keys.empty()) return {};

    const std::size_t total = keys.size();
    std::size_t nthr = prefetch_threads_;
    if (nthr == 0) {
        nthr = std::min<std::size_t>(
            std::max<std::size_t>(std::thread::hardware_concurrency(), 1), 4);
    }
    nthr = std::min(nthr, total);
    // 线程是**每批**创建的（无常驻池）——单次创建 ~20µs 级，批太小时这笔
    // 成本会盖过并行取值的收益（实测：批 64 × 4 线程比惰性还慢 50%，批
    // 256 × 4 线程则快 1.6×）。故按「每线程至少 kMinKeysPerThread 个 key」
    // 收窄线程数：小批自动退化为串行（= 惰性路径的成本，不亏）。
    constexpr std::size_t kMinKeysPerThread = 64;
    nthr = std::min(nthr, (total + kMinKeysPerThread - 1) / kMinKeysPerThread);

    // 槽位与 keys 一一对应（保序）；死 key 留 nullopt，最后压实。
    std::vector<std::optional<Entry>> slots(total);
    std::atomic<bool> ok{true};
    std::mutex err_mu;
    CaskFault first_err{};
    auto worker = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            if (!ok.load(std::memory_order_relaxed)) return;
            auto e = fetch_entry(keys[i]);
            if (!e) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (ok.exchange(false)) first_err = e.error();
                return;
            }
            slots[i] = *std::move(e);  // nullopt = 死 key / filter 不通过
        }
    };

    if (nthr <= 1) {
        worker(0, total);
    } else {
        // 与 Cask::parallel_scan 同款 RAII join guard：emplace_back 抛异常
        // （资源耗尽）时已创建的 joinable 线程被析构自动 join。
        struct JoiningPool {
            std::vector<std::thread> threads;
            ~JoiningPool() {
                for (auto& t : threads) if (t.joinable()) t.join();
            }
        } ws;
        ws.threads.reserve(nthr);
        const std::size_t chunk = (total + nthr - 1) / nthr;
        for (std::size_t t = 0; t < nthr; ++t) {
            const std::size_t b = t * chunk;
            if (b >= total) break;
            ws.threads.emplace_back(worker, b, std::min(b + chunk, total));
        }
        for (auto& w : ws.threads) w.join();
    }
    if (!ok.load()) return std::unexpected(first_err);

    buf_.reserve(total);
    for (auto& s : slots) {
        if (s) buf_.push_back(*std::move(s));
    }
    return {};
}

}  // namespace bitcask
