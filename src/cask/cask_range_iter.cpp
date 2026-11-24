// cask_range_iter.cpp — S33-5：OKI 有序 Range 迭代器。
// 语义与架构见 cask.hpp CaskRangeIter 类注释与
// doc/ordered-key-index-design-zh.md §4。

#include "bitcask/cask.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
            "oki unavailable on this handle (merge-only open builds no read "
            "view; open read-only or read-write to iterate)"));
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

std::expected<std::optional<oki::OkiState::DeltaRow>, CaskFault>
CaskRangeIter::next_merged_key() {
    if (done_ || !merged_) return std::optional<oki::OkiState::DeltaRow>{};
    oki::OkiState::DeltaRow row;
    auto n = merged_->next(row);  // 规则见 OkiMergeCursor；墓碑已抵消
    if (!n) {
        return std::unexpected(err(CaskError::kBadCrc,
                                   "oki run read failed mid-scan"));
    }
    if (!*n || (has_hi_ && std::string_view(row.key) >= std::string_view(hi_))) {
        done_ = true;
        return std::optional<oki::OkiState::DeltaRow>{};
    }
    return std::optional<oki::OkiState::DeltaRow>(std::move(row));
}

// 惰性单条：归并出视图行 → 按行里的位置取值（S76）。跳过的行（TTL / filter /
// 已不可读）继续下一个；错误上抛。
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

// S76：按视图行的位置取值——与 CaskIter 全表扫描同一条读路径（Cask::read_located：
// 映射读、key/ord 自证、TTL / filter、merge 搬迁后重定位）。此前每个 key 回查一次
// public get，把点查定位整套重走一遍（重开后还要查 run）：20 万 key 重开后每行
// 776～936 ns，扫描只要 398～489 ns。filter 在 meta 段上求值，不通过的连 value 都不拷。
std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::fetch_entry(const oki::OkiState::DeltaRow& row) const {
    if (cask_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // 同 get
    }
    if (!row.has_loc) return fetch_by_get(row.key);  // v1 run 行：没有位置
    // 句柄级 expiry_secs（同 get / CaskIter：按写入时刻判）
    const auto expiry = cask_->opts_.expiry_secs;
    if (expiry > 0 &&
        static_cast<std::uint64_t>(row.loc.tstamp) + expiry <= now_sec_default()) {
        return std::optional<Entry>{};
    }
    Cask::LocatedRecord rec;
    auto st = cask_->read_located(row.key, row.ord, row.loc, /*pinned=*/nullptr,
                                  /*see_tombstones=*/false, filter_, want_meta_, rec);
    if (!st) return std::unexpected(st.error());
    if (*st == Cask::LocatedStatus::kSkip) return std::optional<Entry>{};
    if (*st == Cask::LocatedStatus::kSuperseded) {
        // 快照里的版本被 merge 合并掉了：交出该 key 的当前值（≤ 13.0.0 的行为），
        // 不让一个活 key 从结果里消失。
        return fetch_by_get(row.key);
    }
    Entry out;
    const auto* kp = reinterpret_cast<const std::byte*>(row.key.data());
    out.key.assign(kp, kp + row.key.size());
    out.value.assign(rec.value.begin(), rec.value.end());
    if (want_meta_) out.meta.assign(rec.meta.begin(), rec.meta.end());
    out.tstamp = rec.tstamp;
    out.ord = row.ord;   // 已与记录自证一致
    return std::optional<Entry>(std::move(out));
}

// 反馈 2026-10-06：走零拷贝 get 视图——filter 在视图的 meta 段上求值，
// 不通过的连 value 都不拷；value 与 meta 出自同一次读（同一版本）。
std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::fetch_by_get(const std::string& key) const {
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

// S33-6：预取一批。归并 ≤prefetch_ 个视图行（串行、廉价，只碰 run 块与
// memdelta）→ 分段并发按位置取值（被并行化的是 value 的读 + decode，
// 与 parallel_scan 同款 JoiningPool）→ 按 key 序压实进 buf_。
// 跳过的行（TTL / filter 不通过 / 已不可读）不进 buf_。
std::expected<void, CaskFault> CaskRangeIter::fill_prefetch() {
    buf_.clear();
    buf_pos_ = 0;

    std::vector<oki::OkiState::DeltaRow> keys;   // 视图行（key + 位置）
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
    const bool adaptive = nthr == 0;  // S71：显式给了线程数就照办，不做试探
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

    // S71：先串行取一小段并计时，按实测单条成本决定剩下的要不要并行。取值走
    // 映射读，页缓存热、值小时一条零点几微秒，一批 256 条串行不到 100 µs，
    // 还不够起几个线程再汇合（每线程几十微秒）——此前固定并行，热缓存下比不开
    // 预取还慢（feedbacks/2026-11-23-keydir-scan-…）。冷缓存 / 大值时单条要
    // 等 IO，估算出的串行时间远超起线程成本，照旧并行。只在 prefetch_threads
    // = 0（自动）时这样做；调用方显式给了线程数就按给的并行。
    constexpr std::size_t kProbeKeys = 32;
    constexpr double kThreadCostNs = 50'000.0;  // 起一个线程 + 汇合，偏保守
    std::size_t begin = 0;
    if (adaptive && nthr > 1) {
        const std::size_t probe = std::min(total, kProbeKeys);
        const auto t0 = std::chrono::steady_clock::now();
        worker(0, probe);
        const double per_key =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - t0).count()) /
            static_cast<double>(probe);
        begin = probe;
        const double rest_ns = per_key * static_cast<double>(total - begin);
        // 并行省下的时间 ≈ rest·(1 − 1/n)，要盖过 n 个线程的成本才划算
        const double n = static_cast<double>(nthr);
        if (total == begin || rest_ns * (1.0 - 1.0 / n) <= kThreadCostNs * n) nthr = 1;
    }
    if (nthr <= 1) {
        if (ok.load()) worker(begin, total);
    } else if (ok.load()) {
        const std::size_t rest = total - begin;
        // 与 Cask::parallel_scan 同款 RAII join guard：emplace_back 抛异常
        // （资源耗尽）时已创建的 joinable 线程被析构自动 join。
        struct JoiningPool {
            std::vector<std::thread> threads;
            ~JoiningPool() {
                for (auto& t : threads) if (t.joinable()) t.join();
            }
        } ws;
        ws.threads.reserve(nthr);
        const std::size_t chunk = (rest + nthr - 1) / nthr;
        for (std::size_t t = 0; t < nthr; ++t) {
            const std::size_t b = begin + t * chunk;
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
