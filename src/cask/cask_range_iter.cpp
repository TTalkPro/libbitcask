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

std::expected<bool, CaskFault>
CaskRangeIter::next_merged_key(oki::OkiState::DeltaRow& row) {
    if (done_ || !merged_) return false;
    auto n = merged_->next(row);  // 规则见 OkiMergeCursor；墓碑已抵消；key 复用容量
    if (!n) {
        return std::unexpected(err(CaskError::kBadCrc,
                                   "oki run read failed mid-scan"));
    }
    if (!*n || (has_hi_ && std::string_view(row.key) >= std::string_view(hi_))) {
        done_ = true;
        return false;
    }
    return true;
}

// 下一行：预取路径从 pbuf_ 出货（缓冲空则再取一批；整批全被跳过时 pbuf_
// 为空而迭代未结束——循环直到有货或到尾）；惰性路径归并出视图行 → 按行里
// 的位置取值（S76），跳过的行（TTL / filter / 已不可读）继续下一个。
std::expected<bool, CaskFault>
CaskRangeIter::next_fetched(Fetched*& out) {
    // close 之后推进一律 kClosed（契约见类注释），预取缓冲里还有货也一样——此前
    // 预取路径会先把缓冲出完才撞上 fetch_row 的检查，惰性 / 预取行为不一。
    if (cask_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    if (prefetch_ > 1) {
        while (pbuf_pos_ >= pbuf_.size()) {
            if (done_) return false;
            auto f = fill_prefetch();
            if (!f) return std::unexpected(f.error());
        }
        out = &pbuf_[pbuf_pos_++];
        return true;
    }
    while (true) {
        auto k = next_merged_key(cur_row_);
        if (!k) return std::unexpected(k.error());
        if (!*k) return false;
        cur_.reset();  // 释放上一行的担保
        auto st = fetch_row(cur_row_, cur_);
        if (!st) return std::unexpected(st.error());
        if (*st) {
            out = &cur_;
            return true;
        }
    }
}

std::expected<std::optional<CaskRangeIter::Entry>, CaskFault>
CaskRangeIter::next() {
    // S75-3：推进调用使上一批视图失效（契约见 next_batch_view）
    if (!view_entries_.empty()) reset_view_batch();
    Fetched* fp = nullptr;
    auto r = next_fetched(fp);
    if (!r) return std::unexpected(r.error());
    if (!*r) return std::optional<Entry>{};
    const Fetched& f = *fp;
    Entry e;
    e.key.assign(f.key.begin(), f.key.end());
    e.value.assign(f.value.begin(), f.value.end());
    if (want_meta_) e.meta.assign(f.meta.begin(), f.meta.end());
    e.tstamp = f.tstamp;
    e.ord = f.ord;
    return std::optional<Entry>(std::move(e));
}

void CaskRangeIter::reset_view_batch() noexcept {
    view_entries_.clear();
    view_rows_.clear();
    view_scratch_keys_.clear();
    view_key_buf_.clear();  // 只 clear 不 shrink：容量跨批复用
}

std::expected<std::span<const CaskRangeIter::EntryView>, CaskFault>
CaskRangeIter::next_batch_view(std::size_t max_n) {
    reset_view_batch();
    std::size_t n = 0;
    while (n < max_n) {
        Fetched* fp = nullptr;
        auto r = next_fetched(fp);
        if (!r) {
            reset_view_batch();  // 错误即整批作废
            return std::unexpected(r.error());
        }
        if (!*r) break;
        Fetched& f = *fp;
        view_entries_.emplace_back();
        EntryView& ev = view_entries_.back();
        if (f.key_from_row) {
            // key 借的是视图行，下一次取行就会被覆盖：当下拷进扁平缓冲。
            view_scratch_keys_.push_back({n, view_key_buf_.size(), f.key.size()});
            view_key_buf_.append(reinterpret_cast<const char*>(f.key.data()),
                                 f.key.size());
        } else {
            ev.key = f.key;
        }
        ev.value = f.value;
        ev.meta = f.meta;
        ev.tstamp = f.tstamp;
        ev.ord = f.ord;
        view_rows_.push_back(std::move(f));  // span 指向堆缓冲 / 映射，搬动不失效
        ++n;
    }
    // 整批填完再回填借视图行的 key span（见 hpp 注释）。
    const auto* base = reinterpret_cast<const std::byte*>(view_key_buf_.data());
    for (const auto& s : view_scratch_keys_) {
        view_entries_[s.entry].key = std::span<const std::byte>(base + s.off, s.len);
    }
    return std::span<const EntryView>(view_entries_.data(), n);
}

// S76：按视图行的位置取值——与 CaskIter 全表扫描同一条读路径（Cask::read_located：
// 映射读、key/ord 自证、TTL / filter、merge 搬迁后重定位）。此前每个 key 回查一次
// public get，把点查定位整套重走一遍（重开后还要查 run）：20 万 key 重开后每行
// 776～936 ns，扫描只要 398～489 ns。filter 在 meta 段上求值，不通过的连 value 都不读出。
// S75-3：只交 span + 存活担保，拷不拷由调用方（next / next_batch_view）决定。
std::expected<bool, CaskFault>
CaskRangeIter::fetch_row(const oki::OkiState::DeltaRow& row, Fetched& out) const {
    if (cask_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));  // 同 get
    }
    if (!row.has_loc) return fetch_by_get(row.key, out);  // v1 run 行：没有位置
    // 句柄级 expiry_secs（同 get / CaskIter：按写入时刻判）
    const auto expiry = cask_->opts_.expiry_secs;
    if (expiry > 0 &&
        static_cast<std::uint64_t>(row.loc.tstamp) + expiry <= now_sec_default()) {
        return false;
    }
    Cask::LocatedRecord rec;
    auto st = cask_->read_located(row.key, row.ord, row.loc, /*pinned=*/nullptr,
                                  /*see_tombstones=*/false, filter_, want_meta_, rec);
    if (!st) return std::unexpected(st.error());
    if (*st == Cask::LocatedStatus::kSkip) return false;
    if (*st == Cask::LocatedStatus::kSuperseded) {
        // 快照里的版本被 merge 合并掉了：交出该 key 的当前值（≤ 13.0.0 的行为），
        // 不让一个活 key 从结果里消失。
        return fetch_by_get(row.key, out);
    }
    // key 借记录（已与视图行自证一致）；span 由 holder（映射）或 owned（pread）担保，
    // 搬进 out 后堆缓冲不动。
    out.key = rec.key;
    out.value = rec.value;
    out.meta = want_meta_ ? rec.meta : std::span<const std::byte>{};
    out.key_from_row = false;
    out.tstamp = rec.tstamp;
    out.ord = row.ord;
    out.file_holder = std::move(rec.holder);
    out.owned = std::move(rec.owned);
    out.got.reset();
    return true;
}

// 反馈 2026-10-06：走零拷贝 get 视图——filter 在视图的 meta 段上求值，
// 不通过的连 value 都不拷；value 与 meta 出自同一次读（同一版本）。
std::expected<bool, CaskFault>
CaskRangeIter::fetch_by_get(const std::string& key, Fetched& out) const {
    const auto* kp = reinterpret_cast<const std::byte*>(key.data());
    auto g = cask_->get(std::span<const std::byte>(kp, key.size()));
    if (!g) {
        if (g.error().kind == CaskError::kNotFound) return false;
        return std::unexpected(g.error());
    }
    if (filter_ && (g->meta.empty() || !filter_->evaluate(g->meta))) {
        return false;  // 无 meta 不通过（同检索侧）
    }
    // GetResultView 的 span 指向其 pread 缓冲或映射（map_holder_ 锚定），搬动不失效。
    out.got = std::make_unique<GetResultView>(std::move(g).value());
    out.key = std::span<const std::byte>(kp, key.size());
    out.key_from_row = true;
    out.value = out.got->value;
    out.meta = want_meta_ ? out.got->meta : std::span<const std::byte>{};
    out.tstamp = out.got->tstamp;
    out.ord = out.got->ord;
    out.file_holder.reset();
    out.owned.reset();
    return true;
}

// S33-6：预取一批。归并 ≤prefetch_ 个视图行（串行、廉价，只碰 run 块与
// memdelta）→ 分段并发按位置取值（被并行化的是 value 的读 + decode，
// 与 parallel_scan 同款 JoiningPool）→ 按 key 序压实进 pbuf_。
// 跳过的行（TTL / filter 不通过 / 已不可读）不进 pbuf_。
// S75-3：缓冲里放的是 Fetched（span + 担保）而不是拷好的 Entry，next() 与
// next_batch_view() 共用；三个 vector 都是成员，容量跨批复用。
std::expected<void, CaskFault> CaskRangeIter::fill_prefetch() {
    // 上一批的 pbuf_ 已出完货；其中 key_from_row 的 key 消费时已被拷走，
    // 所以这里可以覆盖 pkeys_。
    pbuf_.clear();
    pbuf_pos_ = 0;

    if (pkeys_.size() < prefetch_) pkeys_.resize(prefetch_);
    std::size_t total = 0;
    while (total < prefetch_) {
        auto k = next_merged_key(pkeys_[total]);
        if (!k) return std::unexpected(k.error());
        if (!*k) break;  // done_ 已由 next_merged_key 置位
        ++total;
    }
    if (total == 0) return {};

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

    // 槽位与 pkeys_ 一一对应（保序）；跳过的行留 nullopt，最后压实。
    pslots_.clear();
    pslots_.resize(total);
    std::atomic<bool> ok{true};
    std::mutex err_mu;
    CaskFault first_err{};
    auto worker = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            if (!ok.load(std::memory_order_relaxed)) return;
            pslots_[i].emplace();
            auto r = fetch_row(pkeys_[i], *pslots_[i]);
            if (!r) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (ok.exchange(false)) first_err = r.error();
                return;
            }
            if (!*r) pslots_[i].reset();  // 未填 = 跳过的行
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
    if (!ok.load()) {
        pslots_.clear();
        return std::unexpected(first_err);
    }

    pbuf_.reserve(total);
    for (auto& s : pslots_) {
        if (s) pbuf_.push_back(*std::move(s));
    }
    pslots_.clear();  // 释放担保（文件引用 / pread 缓冲），容量保留
    return {};
}

}  // namespace bitcask
