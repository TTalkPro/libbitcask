#include "bitcask/oki_state.hpp"

#include <chrono>
#include <thread>

#include "bitcask/oki_merge_cursor.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <filesystem>
#include <limits>
#include <type_traits>
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::oki {

namespace {

// `kv.oki.seg-<gen>` → gen；非该形态 / 数字非法 → nullopt。
std::optional<std::uint64_t> parse_run_gen(std::string_view filename) {
    constexpr std::string_view kPrefix = "kv.oki.seg-";
    if (!filename.starts_with(kPrefix)) return std::nullopt;
    const auto digits = filename.substr(kPrefix.size());
    if (digits.empty()) return std::nullopt;
    std::uint64_t g = 0;
    const auto* end = digits.data() + digits.size();
    auto [p, ec] = std::from_chars(digits.data(), end, g);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return g;
}

// 目录内现存 run 文件的最大 gen（含孤儿/滞留件）；无 → 0。
std::uint64_t max_gen_in_dir(std::string_view dir) {
    std::uint64_t g = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(
             bitcask::detail::from_utf8(dir), ec)) {
        if (auto v = parse_run_gen(bitcask::detail::to_utf8(e.path().filename()))) {
            g = std::max(g, *v);
        }
    }
    return g;
}

// 删除目录内 gen 不在 keep 集里的全部 run 文件。
// **不能只删旧 manifest 列出的那些**：触发重建的典型场景恰恰是 manifest
// 缺失/损坏（此时内存 manifest_ 为空），那批 run 文件就成了无人回收的
// 孤儿，每重建一次多一批。在途 ReadView 经 shared_ptr 持旧 Reader——
// POSIX 下 unlink 后 fd 仍可读到 close；B4：删除失败（非 POSIX 语义下
// 文件仍被打开等）的路径回填 backlog，下次 sweep 重试（延迟删除队列的
// OKI 侧形态——正确性不依赖 unlink-while-open）。
// inflight：已预留、正在锁外构建的 gen（重建 / 后台归并）。它们的 run 与
// spill 文件不属于任何 manifest，但绝不能被并发提交的 sweep 删掉。
void sweep_runs(std::string_view dir, std::span<const std::uint64_t> keep,
                std::vector<std::string>& backlog,
                std::span<const std::uint64_t> inflight = {}) {
    const auto is_inflight = [&](std::uint64_t g) {
        return std::find(inflight.begin(), inflight.end(), g) != inflight.end();
    };
    // 先重试上一轮滞留（可能已无读者）。
    std::vector<std::string> still;
    for (const auto& p : backlog) {
        std::error_code rm_ec;
        const auto stale_path = bitcask::detail::from_utf8(p);
        std::filesystem::remove(stale_path, rm_ec);
        if (rm_ec && std::filesystem::exists(stale_path)) still.push_back(p);
    }
    backlog = std::move(still);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(bitcask::detail::from_utf8(dir), ec)) {
        const auto name = bitcask::detail::to_utf8(e.path().filename());
        // S36-1：外排 spill 残件（kv.oki.spill-*，仅崩溃遗留）一律清理。
        if (name.rfind("kv.oki.spill-", 0) == 0) {
            // kv.oki.spill-<gen>-<n>：在途构建的 spill 保留。
            const auto rest = std::string_view(name).substr(13);
            std::uint64_t sg = 0;
            auto [p, sec] = std::from_chars(rest.data(), rest.data() + rest.size(), sg);
            if (sec == std::errc{} && p != rest.data() && is_inflight(sg)) continue;
            std::error_code rm_ec;
            std::filesystem::remove(e.path(), rm_ec);
            continue;
        }
        const auto g = parse_run_gen(name);
        if (!g) continue;
        if (std::find(keep.begin(), keep.end(), *g) != keep.end()) continue;
        if (is_inflight(*g)) continue;
        std::error_code rm_ec;
        std::filesystem::remove(e.path(), rm_ec);
        if (rm_ec && std::filesystem::exists(e.path())) {
            backlog.push_back(bitcask::detail::to_utf8(e.path()));  // B4：滞留重试
        }
    }
}

// S43-2：caller 快照进入 manifest 前的最后一道结构/水位门。这比 CRC
// 更严格：reader 拒收能保护 open，但提交端必须在发布前拦住本地协议错误。
[[nodiscard]] bool valid_commit_snapshot(
    const OkiManifest& next,
    const OkiManifestMetadata& g,
    const std::vector<OkiManifestFStats>& fs) {
    const auto max_cover = [&]() {
        std::uint64_t v = 0;
        for (const auto& r : next.runs) v = std::max(v, r.cover_ord);
        return v;
    };
    return g.reserved == 0 &&
           g.next_file_id == g.biggest_file_id + 1 &&
           g.next_ord >= next.wm &&
           g.next_ord >= max_cover() &&
           (g.active_keys == 0) == (g.live_bytes == 0) &&
           !(g.oldest_tstamp == 0 && g.newest_tstamp != 0) &&
           !(g.active_keys == 0 &&
             (g.oldest_tstamp != 0 || g.newest_tstamp != 0)) &&
           !(g.active_keys != 0 &&
             (g.oldest_tstamp == 0 || g.newest_tstamp == 0)) &&
           fs.size() <= (1u << 24);
}

}  // namespace

namespace {

bool row_less(const OkiState::DeltaRow& a, const OkiState::DeltaRow& b) {
    if (a.key != b.key) return a.key < b.key;
    return a.ord < b.ord;
}

// 已按 (key, ord) 稳定有序的行：每 key 取末行（(ord, 到达序) 胜出者）。
void dedup_sorted(std::vector<OkiState::DeltaRow>& rows) {
    // out == i 的自移动赋值对 std::string 是 valid-but-unspecified，必须跳过。
    std::size_t out = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
        if (out != i) rows[out] = std::move(rows[i]);
        ++out;
    }
    rows.resize(out);
}

// 按 (key, ord) 稳定排序后每 key 取末行（(ord, 到达序) 胜出者）。
std::vector<OkiState::DeltaRow> sort_dedup(std::vector<OkiState::DeltaRow> rows) {
    std::stable_sort(rows.begin(), rows.end(), row_less);
    dedup_sorted(rows);
    return rows;
}

// 两段各自已 sort_dedup 的行归并去重；older 的行到达在前（等 ord 时 newer 胜）。
// 线性归并，不重排。
std::vector<OkiState::DeltaRow> merge_dedup(const std::vector<OkiState::DeltaRow>& older,
                                            std::vector<OkiState::DeltaRow> newer) {
    std::vector<OkiState::DeltaRow> out;
    out.reserve(older.size() + newer.size());
    out.insert(out.end(), older.begin(), older.end());
    out.insert(out.end(), std::make_move_iterator(newer.begin()),
               std::make_move_iterator(newer.end()));
    std::inplace_merge(out.begin(),
                       out.begin() + static_cast<std::ptrdiff_t>(older.size()),
                       out.end(), row_less);
    dedup_sorted(out);
    return out;
}

// ---- S64：active 的增量有序视图（make_read_view 的缓存）----
//
// active（delta_）在一个「纪元」内只追加（append_update / apply_batch 只
// push_back；行内容不再变）。缓存把它的一段前缀整理成若干不可变有序段 + 一小
// 段有序尾巴：
//   segs  覆盖 delta_[0, covered)，旧→新；每段已 sort_dedup，行数大致逐级翻倍
//   tail  覆盖 delta_[covered, count)，已 sort_dedup
// 写后首查只拷贝并整理新增的行 delta_[count, size)：并进尾巴；尾巴攒够
// kTailPromoteRows 行就升成一段，相邻两段行数相近时归并（二进制计数器式，段数
// O(log 行数)、每行被归并 O(log 行数) 次）。此前是每次写之后整份拷贝 + 全量排序
// （feedbacks/2026-11-23-range-iter-after-write-resorts-memdelta.md）。
// 纪元在 active 被重排时 +1（drop_active_through），旧缓存整体作废。
//
// 对象发布后不可变。OkiState::sorted_view_ 存的是指向 tail 的别名指针（类布局
// 不能动——IndexAuthority 内嵌 OkiState，其后的成员有内联访问器）；tail 必须是
// 首成员，由它转回整个对象（标准布局，首成员指针可互转）。
struct ActiveSorted {
    std::vector<OkiState::DeltaRow> tail;
    std::vector<OkiState::SegmentPtr> segs;
    std::vector<std::uint64_t> seg_min_seq;  // 与 segs 一一对应（保守下界）
    std::uint64_t tail_min_seq = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t epoch = 0;
    std::size_t covered = 0;
    std::size_t count = 0;
};
// 守的其实是「tail 在偏移 0」（别名指针转回整个对象）。standard-layout 蕴含它，但 MSVC
// Debug（/MDd，_ITERATOR_DEBUG_LEVEL=2）下 std::vector 带调试迭代器代理、不是 standard-layout，
// 布局却与 Release 一致 ⇒ 只断 standard-layout 会让 msvc-debug / msvc-asan 编不过
// （12.0.2 起，S74）。标准布局时照旧断言；否则退到 offsetof。
static_assert(std::is_standard_layout_v<ActiveSorted> || offsetof(ActiveSorted, tail) == 0);

// ---- S65：memdelta 点查索引（key → delta_ 内最新行下标）----
//
// 开放寻址、线性探测的平表。槽 = 一个 u64：高 24 位是 key 哈希的高位（tag，先筛
// 掉绝大多数不相干的槽），低 40 位是「行下标 + 1」（0 = 空槽）。key 本身不另存，
// 比较时直接比 delta_[下标].key——一次点查是「槽 → 行」两处内存，原来的节点式
// 哈希表是「桶 → 节点 → key 字符串 → 行」，且每个 key 的字节存两份。
// 只增不删（active 重排时整表重建）；装填率不超过 1/2。
struct FlatIdx {
    static constexpr std::uint64_t kIdxMask = (std::uint64_t{1} << 40) - 1;
    // S70：扩容不再一次性按行重算哈希重排（52 万行时约 10 ms，持 mu_，挡全部
    // 写者——feedbacks/2026-11-23-write-path-stalls-on-memdelta-flush-and-growth.md）。
    // 装填率到 1/2 时开一张两倍大的新表（calloc：大块走零页，不 memset），旧表
    // 留着；之后每次插入顺带把旧表的 kMigrateStep 个槽搬进新表，搬完释放旧表。
    // 搬迁期间查找先查新表、再查旧表（一个 key 只在其中一张里）；新插入只进新表。
    // 新表容量 = 旧表 2 倍、旧表在册项 ≈ 容量 / 2，按每次插入搬 16 个槽，旧表在
    // 新表涨到 1/2 之前早已搬空。
    static constexpr std::size_t kMigrateStep = 16;

    struct Table {
        std::uint64_t* slots = nullptr;  // calloc 分配；容量为 2 的幂（或 0）
        std::size_t cap = 0;
        Table() = default;
        Table(const Table&) = delete;
        Table& operator=(const Table&) = delete;
        Table(Table&& o) noexcept : slots(o.slots), cap(o.cap) { o.slots = nullptr; o.cap = 0; }
        Table& operator=(Table&& o) noexcept {
            if (this != &o) {
                std::free(slots);
                slots = o.slots; cap = o.cap;
                o.slots = nullptr; o.cap = 0;
            }
            return *this;
        }
        ~Table() { std::free(slots); }
        void alloc(std::size_t n) {
            std::free(slots);
            slots = static_cast<std::uint64_t*>(std::calloc(n, sizeof(std::uint64_t)));
            if (slots == nullptr) throw std::bad_alloc();
            cap = n;
        }
    };
    Table cur;            // 新插入进这里
    Table old;            // 搬迁中的旧表（cap == 0 = 不在搬迁）
    std::size_t old_pos = 0;  // 旧表下一个待搬的槽
    std::size_t used = 0;     // 两张表的在册项合计

    static std::uint64_t tag_of(std::size_t h) noexcept {
        return (static_cast<std::uint64_t>(h) >> 40) << 40;
    }

    template <typename Rows>
    static std::uint64_t* find_in(Table& t, const Rows& rows, std::string_view key,
                                  std::size_t h) noexcept {
        if (t.cap == 0) return nullptr;
        const std::size_t mask = t.cap - 1;
        const std::uint64_t tag = tag_of(h);
        for (std::size_t pos = h & mask;; pos = (pos + 1) & mask) {
            const std::uint64_t s = t.slots[pos];
            if (s == 0) return nullptr;
            if ((s & ~kIdxMask) == tag && rows[(s & kIdxMask) - 1].key == key) {
                return &t.slots[pos];
            }
        }
    }

    // 命中 → 槽指针；未命中 → nullptr。
    template <typename Rows>
    std::uint64_t* find(const Rows& rows, std::string_view key, std::size_t h) noexcept {
        if (std::uint64_t* s = find_in(cur, rows, key, h)) return s;
        return find_in(old, rows, key, h);
    }

    static void put_slot(Table& t, std::size_t h, std::uint64_t slot) noexcept {
        const std::size_t mask = t.cap - 1;
        std::size_t pos = h & mask;
        while (t.slots[pos] != 0) pos = (pos + 1) & mask;
        t.slots[pos] = slot;
    }

    // 前置：key 不在表里；已 reserve_one。
    void insert_new(std::size_t h, std::size_t idx) noexcept {
        put_slot(cur, h, tag_of(h) | (static_cast<std::uint64_t>(idx) + 1));
        ++used;
    }

    // 保证再放一项后新表装填率 ≤ 1/2；推进一段搬迁。
    template <typename Rows>
    void reserve_one(const Rows& rows) {
        if (old.cap != 0) migrate_some(rows);
        if (cur.cap != 0 && (used + 1) * 2 <= cur.cap) return;
        if (old.cap != 0) migrate_all(rows);   // 不该发生（搬迁远快于填充）：兜底
        old = std::move(cur);
        old_pos = 0;
        cur.alloc(old.cap == 0 ? 64 : old.cap * 2);
        if (old.cap == 0) return;
        migrate_some(rows);
    }

    template <typename Rows>
    void migrate_some(const Rows& rows) {
        std::size_t moved = 0;
        while (old_pos < old.cap && moved < kMigrateStep) {
            const std::uint64_t s = old.slots[old_pos++];
            if (s == 0) continue;
            const std::size_t idx = static_cast<std::size_t>((s & kIdxMask) - 1);
            put_slot(cur, StringHash{}(std::string_view(rows[idx].key)), s);
            ++moved;
        }
        if (old_pos >= old.cap) {
            old = Table{};
            old_pos = 0;
        }
    }

    template <typename Rows>
    void migrate_all(const Rows& rows) {
        while (old.cap != 0) migrate_some(rows);
    }

    void reset(std::size_t expect_rows) {
        std::size_t cap = 64;
        while (cap < expect_rows * 2) cap *= 2;
        old = Table{};
        old_pos = 0;
        cur.alloc(cap);
        used = 0;
    }
};

// S70：后台分块持锁之间的停顿（见 flush 拷贝处的注释）。
void lk_pause() {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
}

// 把 rows[idx] 记进索引（规则同 OkiState::index_row_locked：同 key 时 ord 不更小的
// 后到行顶替）。S70：drop_active_through 在锁外给新数组建索引用。
void flat_index_row(FlatIdx& fi, const std::vector<OkiState::DeltaRow>& rows, std::size_t idx) {
    const auto& row = rows[idx];
    const std::size_t h = StringHash{}(std::string_view(row.key));
    if (std::uint64_t* s = fi.find(rows, row.key, h)) {
        const std::size_t cur = static_cast<std::size_t>((*s & FlatIdx::kIdxMask) - 1);
        if (rows[cur].ord <= row.ord) {
            *s = (*s & ~FlatIdx::kIdxMask) | (static_cast<std::uint64_t>(idx) + 1);
        }
        return;
    }
    fi.reserve_one(rows);
    fi.insert_new(h, idx);
}

FlatIdx* flat_idx(std::uint64_t slot) noexcept {
    return reinterpret_cast<FlatIdx*>(static_cast<std::uintptr_t>(slot));
}

constexpr std::size_t kTailPromoteRows = 128;
// 一次持 mu_ 最多拷这么多行：首建 / 久未读之后要拷的行可能很多，分批拷，别让
// 开迭代器把点查和写挡住一整份拷贝的时间。
constexpr std::size_t kViewCopyChunkRows = 4096;

std::atomic<std::uint64_t> g_read_view_rows_processed{0};

const OkiState::DeltaRow* segment_find(const OkiState::Segment& seg,
                                       std::string_view key) {
    auto it = std::lower_bound(
        seg.begin(), seg.end(), key,
        [](const OkiState::DeltaRow& r, std::string_view k) { return r.key < k; });
    if (it == seg.end() || it->key != key) return nullptr;
    return &*it;
}

constexpr std::size_t kCancelCheckRows = 4096;

}  // namespace

// S70：行数组扩容。翻倍搬迁持 mu_、随行数线性变贵（52 万行时搬一次约 5～10 ms）；
// 小库照常翻倍，涨到 kRowsJumpAt 行时直接预留到 kRowsJumpTo（落段阈值的 1.25 倍，
// 留出后台落段期间继续写入的余量），之后不再搬迁。大块分配只占虚拟地址，行写进来
// 才占内存。落段后的新 active 沿用这块容量（drop_active_through）。
constexpr std::size_t kRowsJumpAt = std::size_t{1} << 16;
constexpr std::size_t kRowsJumpTo = OkiState::kFlushRowLimit + OkiState::kFlushRowLimit / 4;

static void reserve_rows_locked(std::vector<OkiState::DeltaRow>& rows, std::size_t more) {
    const std::size_t need = rows.size() + more;
    if (need <= rows.capacity()) return;
    if (need > kRowsJumpAt && rows.capacity() < kRowsJumpTo) {
        rows.reserve(std::max(need, kRowsJumpTo));
    }
    // 其余情况交给 push_back 的翻倍（小库；或后台落段跟不上、超过 1.25 倍阈值）。
}

// ---------------------------------------------------------------------------
// active memdelta（只追加；mu_）
// ---------------------------------------------------------------------------

void OkiState::append(std::string_view key, std::uint64_t ord, bool tomb,
                      const RowLoc* loc) {
    // 水位门（wm = 排他上界 = 尚未覆盖的最小 ord；首个合法 LSN 是 0，
    // 故不能有 ord==0 特判）：tail 重放只收 wm 起的行。
    if (ord < wm_.load(std::memory_order_acquire)) return;
    append_update(key, ord, tomb, loc);
}

void OkiState::append_update(std::string_view key, std::uint64_t ord,
                             bool tomb, const RowLoc* loc,
                             std::uint32_t dur_file, std::uint64_t dur_end) {
    std::lock_guard<std::mutex> lk(mu_);
    reserve_rows_locked(delta_, 1);
    delta_bytes_ += key.size() + sizeof(DeltaRow);
    delta_.push_back(DeltaRow{std::string(key), ord, tomb,
                              /*has_loc=*/loc != nullptr,
                              loc != nullptr ? *loc : RowLoc{}});
    delta_.back().seq = ++append_seq_;
    delta_.back().dur_file = dur_file;
    delta_.back().dur_end = dur_end;
    ++delta_version_;  // 排序视图缓存失效
    index_row_locked(delta_.size() - 1);
    update_flush_hint_locked();
}

// S63：批量发布。锁外只构造行（含 key 拷贝），临界区内只做 move 追加、
// 到达序号分配与点查哈希登记。delta_version_ 只 bump 一次——排序视图
// 缓存对整批只失效一次；flush_hint 同样在批末更新。
void OkiState::apply_batch(std::span<const BatchRow> rows) {
    if (rows.empty()) return;
    std::vector<DeltaRow> prepared;
    prepared.reserve(rows.size());
    std::size_t bytes = 0;
    for (const auto& row : rows) {
        bytes += row.key.size() + sizeof(DeltaRow);
        DeltaRow out{std::string(row.key), row.ord, row.tomb,
                     /*has_loc=*/!row.tomb,
                     row.tomb ? RowLoc{} : row.loc};
        out.dur_file = row.tomb ? row.dur_file : 0;
        out.dur_end = row.tomb ? row.dur_end : 0;
        prepared.push_back(std::move(out));
    }

    std::lock_guard<std::mutex> lk(mu_);
    reserve_rows_locked(delta_, prepared.size());
    const std::size_t first = delta_.size();
    delta_.insert(delta_.end(), std::make_move_iterator(prepared.begin()),
                  std::make_move_iterator(prepared.end()));
    delta_bytes_ += bytes;
    for (std::size_t i = first; i < delta_.size(); ++i) {
        delta_[i].seq = ++append_seq_;
        index_row_locked(i);
    }
    ++delta_version_;  // 排序视图缓存失效
    update_flush_hint_locked();
}

void OkiState::index_row_locked(std::size_t idx) {
    if (!point_query_.load(std::memory_order_relaxed)) return;
    const DeltaRow& row = delta_[idx];
    FlatIdx* fi = flat_idx(delta_flat_idx_);
    if (fi == nullptr) {
        fi = new FlatIdx();
        delta_flat_idx_ = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fi));
    }
    const std::size_t h = StringHash{}(std::string_view(row.key));
    if (std::uint64_t* s = fi->find(delta_, row.key, h)) {
        // (ord, 到达序) 胜出：等 ord 后到者顶替（搬迁行），更小 ord 不顶替。
        const std::size_t cur = static_cast<std::size_t>((*s & FlatIdx::kIdxMask) - 1);
        if (delta_[cur].ord <= row.ord) {
            *s = (*s & ~FlatIdx::kIdxMask) | (static_cast<std::uint64_t>(idx) + 1);
        }
        return;
    }
    fi->reserve_one(delta_);
    fi->insert_new(h, idx);
}

void OkiState::rebuild_index_locked() {
    FlatIdx* fi = flat_idx(delta_flat_idx_);
    if (!point_query_.load(std::memory_order_relaxed)) {
        if (fi != nullptr) fi->reset(0);
        return;
    }
    if (fi == nullptr) {
        fi = new FlatIdx();
        delta_flat_idx_ = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fi));
    }
    fi->reset(delta_.size());
    for (std::size_t i = 0; i < delta_.size(); ++i) index_row_locked(i);
}

void OkiState::enable_point_query() {
    std::lock_guard<std::mutex> lk(mu_);
    if (point_query_.load(std::memory_order_relaxed)) return;
    point_query_.store(true, std::memory_order_release);
    rebuild_index_locked();
}

void OkiState::drop_active_through(std::uint64_t through) {
    // S70：持 mu_ 的时间与行数无关。delta_ 按到达序追加、seq 单调不减，seq ≤ through
    // 的行恰是前缀，要留下的是后缀——它在后台落段排序期间还在长（实测 7 万行）。
    // 先在锁外把后缀分块拷进新数组、给新数组建好索引（每块短暂持锁取行，块间让出
    // 一次，免得写者抢不到锁）；剩下不到一块时才在锁内搬完零头、整体交换。旧数组
    // （连同前缀里上百万个 key 字符串）与旧索引在锁外析构。此前在锁内搬后缀、
    // 重建索引，挡写者约 4 ms；更早（S70 之前）逐行判断整份数组，几十毫秒。
    // 前置：持 flush_mu_（删除 / 重排只有这里与 rebuild / clear，都要 flush_mu_），
    // 所以期间 delta_ 只会在尾部追加，下标稳定。
    std::size_t copied = 0;
    std::size_t cap = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copied = static_cast<std::size_t>(
            std::partition_point(delta_.begin(), delta_.end(),
                                 [&](const DeltaRow& r) { return r.seq <= through; }) -
            delta_.begin());
        cap = std::max(delta_.capacity(), delta_.size() - copied);
    }
    const bool indexed = point_query_.load(std::memory_order_acquire);
    std::vector<DeltaRow> kept;
    kept.reserve(cap);   // 沿用原容量：落段后不必再从小翻倍（大块只占虚拟地址）
    auto fresh = std::make_unique<FlatIdx>();
    fresh->reset(0);
    std::size_t bytes = 0;
    const auto add = [&](std::size_t from) {
        for (std::size_t i = from; i < kept.size(); ++i) {
            bytes += kept[i].key.size() + sizeof(DeltaRow);
            if (indexed) flat_index_row(*fresh, kept, i);
        }
    };
    std::vector<DeltaRow> old;
    std::unique_ptr<FlatIdx> old_idx;
    while (true) {
        std::unique_lock<std::mutex> lk(mu_);
        const std::size_t n = delta_.size();
        const std::size_t base = kept.size();
        if (n - copied > kViewCopyChunkRows) {
            // 拷（不搬）：这些行此刻仍是 active，读者照常可见
            for (std::size_t i = copied; i < copied + kViewCopyChunkRows; ++i) {
                kept.push_back(delta_[i]);
            }
            copied += kViewCopyChunkRows;
            lk.unlock();
            add(base);
            lk_pause();
            continue;
        }
        for (std::size_t i = copied; i < n; ++i) kept.push_back(std::move(delta_[i]));
        add(base);
        old = std::move(delta_);
        delta_ = std::move(kept);
        delta_bytes_ = bytes;
        ++delta_version_;
        ++delta_epoch_;  // active 被重排：增量有序视图（ActiveSorted）整体作废
        if (point_query_.load(std::memory_order_relaxed) == indexed && indexed) {
            old_idx.reset(flat_idx(delta_flat_idx_));
            delta_flat_idx_ = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(fresh.release()));
        } else {
            rebuild_index_locked();   // 期间点查开关被切换（罕见）：老办法重建
        }
        update_flush_hint_locked();
        break;
    }
    // old / old_idx 在这里（锁外）析构
}

// S43-5：写前探测是 locate 的显式语义别名。
OkiState::WriteProbe OkiState::probe_for_write(std::string_view key) const {
    const auto r = locate(key);
    return WriteProbe{r.status, r.ord, r.tomb, r.has_loc, r.loc};
}

bool OkiState::write_probe_ready() const noexcept {
    if (!point_query_.load(std::memory_order_acquire) ||
        !loaded_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto view = view_.load();
    if (!view) return false;
    return std::none_of(
        view->runs.begin(), view->runs.end(),
        [](const auto& run) { return run.second->version() == kRunVersion; });
}

// 运行期应用一律绕过 wm 门：调用方（IndexAuthority）已在写序闸门内做过
// probe + CAS 裁决，行必然是该 key 的最新态。wm 门只服务恢复期的 tail
// 重放；用在运行期会误丢在途写（运行期重建把 wm 设为 next_ord，含已分配、
// 尚未应用的 ord）。
void OkiState::apply_put(std::string_view key, std::uint64_t ord,
                         const RowLoc& loc) {
    append_update(key, ord, /*tomb=*/false, &loc);
}

void OkiState::apply_remove(std::string_view key, std::uint64_t ord,
                            std::uint32_t dur_file, std::uint64_t dur_end) {
    append_update(key, ord, /*tomb=*/true, nullptr, dur_file, dur_end);
}

void OkiState::apply_conditional_remove(std::string_view key,
                                        std::uint64_t victim_ord) {
    append_update(key, victim_ord, /*tomb=*/true, nullptr);
}

void OkiState::apply_relocation(std::string_view key, std::uint64_t ord,
                                const RowLoc& loc) {
    append_update(key, ord, /*tomb=*/false, &loc);
}

// S43-1：坏块运行期降级。先关点查，再置 unloaded。
void OkiState::degrade_for_rebuild() noexcept {
    std::lock_guard<std::mutex> flk(flush_mu_);
    point_query_.store(false, std::memory_order_release);
    loaded_.store(false, std::memory_order_release);
}

void OkiState::publish_view_locked() {
    auto v = std::make_shared<OkiView>();
    v->runs = readers_;
    v->frozen = frozen_;
    v->frozen_through = frozen_through_;
    for (const auto& r : manifest_.runs) gen_floor_ = std::max(gen_floor_, r.gen);
    view_.publish(std::move(v));
}

// ---------------------------------------------------------------------------
// load（S44-5：run 缺失时重读 manifest 重试）
// ---------------------------------------------------------------------------

void OkiState::load(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    // 另一个进程（可写句柄）的归并 / flush 可能恰在我们读完 manifest、打开
    // run 之前把旧 run 提交掉并 sweep。run 文件「不存在」时重读 manifest 重
    // 试（有界次数）；CRC / 结构损坏仍整体弃用（未加载态 → caller 重建）。
    constexpr int kMaxAttempts = 8;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto m = read_manifest(dir);
        if (!m) return;  // 缺失/损坏 → 未加载态（caller 决定重建）
        RunsVec rds;
        rds.reserve(m->runs.size());
        bool missing = false;
        bool broken = false;
        for (const auto& r : m->runs) {
            const auto path = mk_run_filename(dir, r.gen);
            auto rd = OkiRunReader::open(path);
            if (!rd) {
                std::error_code ec;
                if (!std::filesystem::exists(bitcask::detail::from_utf8(path), ec)) {
                    missing = true;
                } else {
                    broken = true;
                }
                break;
            }
            rds.emplace_back(r.gen,
                             std::make_shared<OkiRunReader>(*std::move(rd)));
        }
        if (broken) return;
        if (missing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1 << attempt));
            continue;
        }
        manifest_ = *std::move(m);
        readers_ = std::move(rds);
        publish_view_locked();
        manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
        wm_.store(manifest_.wm, std::memory_order_release);
        loaded_.store(true, std::memory_order_release);
        return;
    }
}

// S36-4：降级戳（level_b 关而盘上戳开 → 改写 manifest 清戳）。
void OkiState::stamp_mode(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return;
    const bool want = level_b_.load(std::memory_order_acquire);
    if (manifest_.level_b == want) return;
    if (want) return;  // 升级只经 rebuild
    OkiManifest next = manifest_;
    next.level_b = false;
    if (!write_manifest(dir, next)) return;
    manifest_ = std::move(next);
    manifest_level_b_.store(false, std::memory_order_release);
}

std::uint64_t OkiState::next_gen_locked(std::string_view dir) {
    std::uint64_t g = gen_floor_;
    for (const auto& r : manifest_.runs) g = std::max(g, r.gen);
    for (const auto x : inflight_gens_) g = std::max(g, x);
    g = std::max(g, max_gen_in_dir(dir));
    return g + 1;
}

void OkiState::release_gen_locked(std::uint64_t gen) {
    std::erase(inflight_gens_, gen);
}

// ---------------------------------------------------------------------------
// flush：冻结 → 落盘 → 单次发布
// ---------------------------------------------------------------------------

bool OkiState::flush(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    bool need_compact = false;
    const bool ok = flush_locked_impl(dir, durable_wms,
                                      std::move(commit_snapshot), need_compact);
    if (ok && need_compact) {
        schedule_compaction(dir);
        wait_backpressure();
    }
    return ok;
}

bool OkiState::flush_locked_impl(
    std::string_view dir,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> durable_wms,
    std::optional<OkiCommitSnapshot> commit_snapshot, bool& need_compact) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (!loaded_.load(std::memory_order_relaxed)) return false;

    // S43-2：空 memdelta 也是合法提交点（v3→v4 迁移目录可能没有待固化行，
    // 但仍要把刚捕获的标量/fstats 随 manifest 原子发布）。
    const auto commit_metadata_only = [&]() -> bool {
        if (!commit_snapshot.has_value()) return true;
        const auto& g = commit_snapshot->metadata;
        if (!valid_commit_snapshot(manifest_, g, commit_snapshot->fstats)) {
            return false;
        }
        OkiManifest next = manifest_;
        if (!durable_wms.empty()) {
            next.data_wms.assign(durable_wms.begin(), durable_wms.end());
        }
        next.metadata = g;
        next.fstats = commit_snapshot->fstats;
        next.level_b = level_b_.load(std::memory_order_acquire);
        if (next.metadata == manifest_.metadata &&
            next.fstats == manifest_.fstats &&
            next.level_b == manifest_level_b_.load(std::memory_order_acquire) &&
            next.data_wms == manifest_.data_wms) {
            return true;
        }
        if (!write_manifest(dir, next)) return false;
        manifest_ = std::move(next);
        manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
        return true;
    };

    // ---- 1) 切面之内的 active 行 → 胜出行 ----
    // 带快照：切面 = cut_seq（seq ≤ cut 的行恰是账本已计入的行）；无快照：
    // 当前全部行。
    const std::uint64_t cut_seq =
        commit_snapshot.has_value() ? commit_snapshot->cut_seq
                                    : std::numeric_limits<std::uint64_t>::max();
    std::vector<DeltaRow> eligible;
    std::uint64_t max_eligible_seq = 0;
    std::uint64_t min_remaining_ord = std::numeric_limits<std::uint64_t>::max();
    {
        // S70：分块拷贝，每块单独持 mu_。持 flush_mu_ 期间 delta_ 只会在尾部追加
        // （删除 / 重排都要 flush_mu_），所以下标稳定：先记下此刻的行数，再一块一块
        // 拷。此前一次持锁拷完百万行，写者被挡几十毫秒。拷到的行集合与原来相同
        // （此刻之后追加的行不在内，与原来「持锁那一刻之后的行不在内」同义）。
        std::size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            n = delta_.size();
        }
        eligible.reserve(n);
        for (std::size_t i = 0; i < n; i += kViewCopyChunkRows) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::size_t end = std::min(n, i + kViewCopyChunkRows);
                for (std::size_t j = i; j < end; ++j) {
                    const auto& r = delta_[j];
                    if (r.seq <= cut_seq) {
                        eligible.push_back(r);
                        max_eligible_seq = std::max(max_eligible_seq, r.seq);
                    } else {
                        min_remaining_ord = std::min(min_remaining_ord, r.ord);
                    }
                }
            }
            // 放锁后歇一下再拿下一块：std::mutex 不保证公平，等锁的写者在别的核上被
            // 唤醒要几微秒，这边几纳秒就又抢到锁，写者整段拷贝期间都拿不到（实测一次
            // 挡了约 11 ms）。每块歇约 50 µs（定时器精度），百万行多花几毫秒。
            lk_pause();
        }
    }
    auto winners = sort_dedup(std::move(eligible));

    // 持久性：胜出行引用了未持久字节 → 放弃本轮（状态不变）。带快照时这些行
    // 已计入账本，run 不含而 data 水位又在其前，崩溃重放会重复记账；无快照
    // 时同样不得发布引用可能随掉电蒸发的字节的 run。
    if (!durable_wms.empty()) {
        std::uint32_t max_snap_fid = 0;
        for (const auto& [fid, wm] : durable_wms) {
            max_snap_fid = std::max(max_snap_fid, fid);
        }
        const auto beyond = [&](std::uint32_t fid, std::uint64_t end) {
            if (fid == 0) return false;
            for (const auto& [f, wm] : durable_wms) {
                if (f == fid) return end > wm;
            }
            // 快照之后新建的文件（id 更大）尚无持久保证；更小 id 的缺席文件
            // 是已退役的旧文件，其行必然已被覆盖。
            return fid > max_snap_fid;
        };
        for (const auto& r : winners) {
            const bool b = r.has_loc ? beyond(r.loc.file_id,
                                              r.loc.offset + r.loc.total_sz)
                                     : (r.tomb && beyond(r.dur_file, r.dur_end));
            if (b) return false;
        }
    }

    if (winners.empty() && frozen_.empty()) return commit_metadata_only();

    // ---- 2) 冻结：发布后从 active 物理删除 ----
    if (!winners.empty()) {
        frozen_.push_back(std::make_shared<const Segment>(std::move(winners)));
        frozen_through_ = std::max(frozen_through_, max_eligible_seq);
        publish_view_locked();
        drop_active_through(frozen_through_);
    }

    // ---- 3) 落盘：全部待落盘段归并成一个 run（墓碑保留）----
    std::vector<OkiMergeCursor::MemSource> segs(frozen_.begin(), frozen_.end());
    std::uint64_t est = 0;
    for (const auto& sp : frozen_) est += sp->size();
    const std::uint64_t gen = next_gen_locked(dir);
    const auto run_path = mk_run_filename(dir, gen);
    const auto drop_run = [&] {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(run_path), ec);
    };
    auto w = OkiRunWriter::create(run_path, kDefaultBlockBytes, kRunVersion3,
                                  /*expected_entries=*/est);
    if (!w) return false;
    std::uint64_t cover = 0;
    std::uint64_t written = 0;
    {
        auto mc = OkiMergeCursor::open({}, std::move(segs));
        if (!mc) return false;
        DeltaRow r;
        while (true) {
            auto n = mc->next(r, /*include_tombs=*/true);
            if (!n) return false;
            if (!*n) break;
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(r.key.data()),
                                r.key.size()),
                            r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
            if (!a) {
                drop_run();
                return false;
            }
            cover = std::max(cover, r.ord + 1);
            ++written;
        }
    }
    if (!w->finish(/*fsync_dir=*/true)) {
        drop_run();
        return false;
    }
    auto rd = OkiRunReader::open(run_path);
    if (!rd) {
        drop_run();
        return false;
    }

    // ---- 4) manifest 提交（唯一 commit point）----
    OkiManifest next = manifest_;
    if (!durable_wms.empty()) {
        next.data_wms.assign(durable_wms.begin(), durable_wms.end());
    }
    next.runs.push_back({gen, cover, /*format_ver=*/3});
    next.wm = std::max(next.wm, std::min(cover, min_remaining_ord));
    next.level_b = level_b_.load(std::memory_order_acquire);  // S36-4 模式戳
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = std::move(commit_snapshot->fstats);
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) {
            drop_run();
            return false;
        }
    }
    if (!write_manifest(dir, next)) {
        drop_run();
        return false;
    }

    // ---- 5) 单次发布：run 入视图、段出视图 ----
    manifest_ = std::move(next);
    readers_.emplace_back(gen, std::make_shared<OkiRunReader>(*std::move(rd)));
    frozen_.clear();
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    {
        std::lock_guard<std::mutex> sl(stats_mu_);
        ++stats_.flushes;
        stats_.flushed_rows += written;
    }
    need_compact = needs_compaction_locked();
    return true;
}

// ---------------------------------------------------------------------------
// 归并（S44-3 / S44-4）
// ---------------------------------------------------------------------------

void OkiState::set_compaction_policy(const CompactionPolicyOptions& opts) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    policy_ = opts;
}

CompactionPolicyOptions OkiState::compaction_policy() const {
    std::lock_guard<std::mutex> flk(flush_mu_);
    return policy_;
}

void OkiState::set_compaction_row_hook_for_test(CompactionRowHook hook) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    row_hook_ = std::move(hook);
}

OkiState::CompactionStats OkiState::compaction_stats() const {
    std::lock_guard<std::mutex> sl(stats_mu_);
    return stats_;
}

bool OkiState::needs_compaction_locked() const {
    std::vector<std::uint64_t> sizes;
    sizes.reserve(readers_.size());
    for (const auto& [g, rd] : readers_) sizes.push_back(rd->entry_count());
    return choose_compaction(sizes, 0, policy_,
                             force_full_next_.load(std::memory_order_relaxed))
        .merge;
}

// 一次归并（三段式；设计 §4.5）：
//   1) 锁内按策略选后缀 Rj..Rn，快照输入，预留输出 gen（在途登记）；
//   2) 锁外 OkiMergeCursor 归并：不含最老 run 时**保留墓碑**（更老的 run
//      里可能还有该 key 的 put，丢墓碑会让它复活）；含最老 run 时丢弃；
//   3) 锁内校验：输入仍在册且连续，其后的 run gen 全部大于输出 gen；替换并
//      单次发布视图（冻结段与 frozen_through 原样沿用——归并只换 runs，新旧
//      内容等价）。
OkiState::CompactResult OkiState::compact_once(std::string_view dir,
                                               Lane lane) {
    if (cancel_.load(std::memory_order_acquire)) return CompactResult::kCancelled;
    const auto t0 = std::chrono::steady_clock::now();

    RunsVec inputs;
    std::uint64_t gen = 0;
    std::uint64_t cover = 0;
    std::uint32_t out_ver = kRunVersion3;
    std::uint64_t est = 0;
    bool full = false;
    CompactionRowHook hook;
    {
        std::lock_guard<std::mutex> flk(flush_mu_);
        if (!loaded_.load(std::memory_order_acquire)) return CompactResult::kNone;
        hook = row_hook_;
        std::size_t floor = 0;
        if (lane == Lane::kYoung) {
            if (!main_out_gen_) return CompactResult::kNone;
            floor = readers_.size();
            for (std::size_t i = 0; i < readers_.size(); ++i) {
                if (readers_[i].first > *main_out_gen_) {
                    floor = i;
                    break;
                }
            }
        }
        std::vector<std::uint64_t> sizes;
        sizes.reserve(readers_.size());
        for (const auto& [g, rd] : readers_) sizes.push_back(rd->entry_count());
        const bool want_full =
            lane == Lane::kMain && force_full_next_.load(std::memory_order_relaxed);
        const auto choice = choose_compaction(sizes, floor, policy_, want_full);
        if (!choice.merge) return CompactResult::kNone;
        full = choice.full;
        inputs.assign(readers_.begin() + static_cast<std::ptrdiff_t>(choice.begin),
                      readers_.end());
        for (std::size_t i = choice.begin; i < manifest_.runs.size(); ++i) {
            cover = std::max(cover, manifest_.runs[i].cover_ord);
        }
        // S43-1：全 v2/v3 输入才出 v3；含 v1 则整体降 v1（待 rebuild 淘汰后自愈）。
        for (const auto& [g, rd] : inputs) {
            if (rd->version() == kRunVersion) out_ver = kRunVersion;
            est += rd->entry_count();
        }
        gen = next_gen_locked(dir);
        inflight_gens_.push_back(gen);  // 期间的 flush 必取更大 gen
        if (lane == Lane::kMain) main_out_gen_ = gen;
    }

    const auto out_path = mk_run_filename(dir, gen);
    // 任何退出路径都撤销在途登记 / 主道标记，并删除未提交的输出。
    bool committed = false;
    const auto finish = [&](CompactResult r) {
        {
            std::lock_guard<std::mutex> flk(flush_mu_);
            release_gen_locked(gen);
            if (lane == Lane::kMain) main_out_gen_.reset();
        }
        if (!committed) {
            std::error_code ec;
            std::filesystem::remove(bitcask::detail::from_utf8(out_path), ec);
        }
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        std::lock_guard<std::mutex> sl(stats_mu_);
        stats_.compaction_ns += ns;
        if (r == CompactResult::kFailed) ++stats_.aborted;
        if (r == CompactResult::kCancelled) ++stats_.cancelled;
        return r;
    };

    std::uint64_t rows = 0;
    std::uint64_t tombs = 0;
    {
        auto w = OkiRunWriter::create(out_path, kDefaultBlockBytes, out_ver, est);
        if (!w) return finish(CompactResult::kFailed);
        std::vector<std::shared_ptr<OkiRunReader>> rs;
        rs.reserve(inputs.size());
        for (const auto& [g, rd] : inputs) rs.push_back(rd);
        auto mc = OkiMergeCursor::open(std::move(rs),
                                       std::vector<OkiMergeCursor::MemSource>{});
        if (!mc) return finish(CompactResult::kFailed);
        DeltaRow row;
        while (true) {
            if (hook && (rows % kHookEveryRows) == 0) {
                hook(lane == Lane::kYoung, rows);
            }
            if ((rows % kCancelCheckRows) == 0 &&
                cancel_.load(std::memory_order_acquire)) {
                return finish(CompactResult::kCancelled);
            }
            auto n = mc->next(row, /*include_tombs=*/!full);
            if (!n) return finish(CompactResult::kFailed);
            if (!*n) break;
            const bool loc = out_ver != kRunVersion && row.has_loc && !row.tomb;
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(row.key.data()),
                                row.key.size()),
                            row.ord, row.tomb, loc ? &row.loc : nullptr);
            if (!a) return finish(CompactResult::kFailed);
            ++rows;
            if (row.tomb) ++tombs;
        }
        if (!w->finish(/*fsync_dir=*/true)) return finish(CompactResult::kFailed);
    }
    auto rd = OkiRunReader::open(out_path);
    if (!rd) return finish(CompactResult::kFailed);

    {
        std::lock_guard<std::mutex> flk(flush_mu_);
        if (!loaded_.load(std::memory_order_acquire)) {
            // 期间被 rebuild / 降级：放弃（finish 会再取 flush_mu_，先放锁）。
        } else {
            // 输入必须仍在册且连续；其后每个 run 的 gen 必须大于输出 gen。
            std::size_t pos = readers_.size();
            for (std::size_t i = 0; i < readers_.size(); ++i) {
                if (readers_[i].first == inputs.front().first) {
                    pos = i;
                    break;
                }
            }
            bool ok = pos + inputs.size() <= readers_.size() &&
                      manifest_.runs.size() == readers_.size();
            for (std::size_t k = 0; ok && k < inputs.size(); ++k) {
                ok = readers_[pos + k].first == inputs[k].first &&
                     manifest_.runs[pos + k].gen == inputs[k].first;
            }
            for (std::size_t i = pos + inputs.size(); ok && i < readers_.size();
                 ++i) {
                ok = readers_[i].first > gen;
            }
            // 全归并必须仍从最老 run 开始（否则丢墓碑不安全）。
            if (ok && full) ok = pos == 0;
            if (ok) {
                OkiManifest next = manifest_;
                next.runs.clear();
                next.runs.insert(next.runs.end(), manifest_.runs.begin(),
                                 manifest_.runs.begin() +
                                     static_cast<std::ptrdiff_t>(pos));
                next.runs.push_back(
                    {gen, cover, static_cast<std::uint8_t>(out_ver)});
                next.runs.insert(next.runs.end(),
                                 manifest_.runs.begin() +
                                     static_cast<std::ptrdiff_t>(pos + inputs.size()),
                                 manifest_.runs.end());
                next.level_b = level_b_.load(std::memory_order_acquire);
                if (write_manifest(dir, next)) {
                    RunsVec nr;
                    nr.reserve(next.runs.size());
                    nr.insert(nr.end(), readers_.begin(),
                              readers_.begin() + static_cast<std::ptrdiff_t>(pos));
                    nr.emplace_back(gen, std::make_shared<OkiRunReader>(
                                             *std::move(rd)));
                    nr.insert(nr.end(),
                              readers_.begin() +
                                  static_cast<std::ptrdiff_t>(pos + inputs.size()),
                              readers_.end());
                    manifest_ = std::move(next);
                    readers_ = std::move(nr);
                    publish_view_locked();
                    manifest_level_b_.store(manifest_.level_b,
                                            std::memory_order_release);
                    committed = true;
                    std::vector<std::uint64_t> keep;
                    for (const auto& r : manifest_.runs) keep.push_back(r.gen);
                    sweep_runs(dir, keep, sweep_backlog_, inflight_gens_);
                    std::vector<std::uint64_t> cache_keep = keep;
                    cache_keep.insert(cache_keep.end(), inflight_gens_.begin(),
                                      inflight_gens_.end());
                    block_cache_->purge_except(cache_keep);
                    // 墓碑只在全归并时回收；非全归并输出里墓碑占比过高 →
                    // 下一次主道归并强制全归并。
                    if (full) {
                        force_full_next_.store(false, std::memory_order_relaxed);
                    } else if (rows > 0 && tombs * 4 > rows) {
                        force_full_next_.store(true, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    if (!committed) return finish(CompactResult::kFailed);
    {
        std::lock_guard<std::mutex> sl(stats_mu_);
        ++stats_.compactions;
        if (full) ++stats_.full_compactions;
        if (lane == Lane::kYoung) ++stats_.young_compactions;
        stats_.rows_written += rows;
        stats_.tomb_rows_written += tombs;
    }
    return finish(CompactResult::kDone);
}

void OkiState::schedule_compaction(std::string_view dir) {
    std::lock_guard<std::mutex> lk(lane_mu_);
    if (cancel_.load(std::memory_order_acquire)) return;
    compact_dir_ = std::string(dir);
    if (!main_running_) {
        if (main_thread_.joinable()) main_thread_.join();  // 已退出，立即返回
        main_running_ = true;
        pending_ = false;
        main_thread_ = std::thread([this] { main_lane_loop(); });
        return;
    }
    pending_ = true;
    // 主道正在一次（可能很大的）归并里：起年轻道收拾期间新 flush 的小 run。
    if (main_busy_ && !young_running_) {
        if (young_thread_.joinable()) young_thread_.join();
        young_running_ = true;
        young_thread_ = std::thread([this] { young_lane_loop(); });
    }
}

void OkiState::main_lane_loop() {
    for (;;) {
        std::string dir;
        {
            std::unique_lock<std::mutex> lk(lane_mu_);
            // 年轻道运行期间主道不开始新一轮（两道输入不得交叠）。
            lane_cv_.wait(lk, [&] {
                return !young_running_ || cancel_.load(std::memory_order_acquire);
            });
            if (cancel_.load(std::memory_order_acquire)) {
                main_running_ = false;
                lane_cv_.notify_all();
                return;
            }
            main_busy_ = true;
            pending_ = false;
            dir = compact_dir_;
        }
        const auto r = compact_once(dir, Lane::kMain);
        std::lock_guard<std::mutex> lk(lane_mu_);
        main_busy_ = false;
        lane_cv_.notify_all();
        if (r == CompactResult::kDone) continue;
        if (r == CompactResult::kNone && pending_) continue;
        main_running_ = false;
        pending_ = false;
        lane_cv_.notify_all();
        return;
    }
}

void OkiState::young_lane_loop() {
    for (;;) {
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(lane_mu_);
            dir = compact_dir_;
        }
        const auto r = compact_once(dir, Lane::kYoung);
        if (r == CompactResult::kDone) continue;
        std::lock_guard<std::mutex> lk(lane_mu_);
        young_running_ = false;
        lane_cv_.notify_all();
        return;
    }
}

// 背压：run 数超过硬上限时等待归并追上；归并已全部停下（无法再推进）即返回。
void OkiState::wait_backpressure() {
    const auto pol = compaction_policy();
    const std::size_t cap = pol.stall_runs != 0 ? pol.stall_runs : 2 * pol.max_runs;
    if (run_count() <= cap) return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lk(lane_mu_);
        while (!cancel_.load(std::memory_order_acquire) &&
               (main_running_ || young_running_)) {
            lane_cv_.wait_for(lk, std::chrono::milliseconds(20));
            lk.unlock();
            const bool done = run_count() <= cap;
            lk.lock();
            if (done) break;
        }
    }
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    std::lock_guard<std::mutex> sl(stats_mu_);
    ++stats_.backpressure_waits;
    stats_.backpressure_ns += ns;
}

void OkiState::wait_compaction() {
    std::unique_lock<std::mutex> lk(lane_mu_);
    lane_cv_.wait(lk, [&] { return !main_running_ && !young_running_; });
    // 线程最后一步是在 lane_mu_ 下清标志后返回，此时 join 立即完成。
    if (main_thread_.joinable()) main_thread_.join();
    if (young_thread_.joinable()) young_thread_.join();
}

OkiState::~OkiState() {
    // close 的最后一步。小归并（常见：close 那次 flush 刚调度的）给一个短
    // 宽限期跑完——否则只在 close 时 flush 的短会话永远完不成归并；大归并
    // 超过宽限即取消（每 kCancelCheckRows 行检查一次，半成品输出删除、输入
    // 未动），close 耗时有上界（S44-4）。
    {
        std::unique_lock<std::mutex> lk(lane_mu_);
        lane_cv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
            return !main_running_ && !young_running_;
        });
    }
    cancel_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(lane_mu_);
        lane_cv_.notify_all();
    }
    wait_compaction();
    delete flat_idx(delta_flat_idx_);
    delta_flat_idx_ = 0;
}

// ---------------------------------------------------------------------------
// 重建 / 只读内存视图
// ---------------------------------------------------------------------------

std::uint64_t OkiState::reserve_gen(std::string_view dir) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    // 只登记在途、不抬 gen_floor_：放弃的 gen 从未发布 Reader，可以复用。
    const auto g = next_gen_locked(dir);
    inflight_gens_.push_back(g);
    return g;
}

void OkiState::abandon_gen(std::string_view dir, std::uint64_t gen) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    release_gen_locked(gen);
    std::error_code ec;
    std::filesystem::remove(bitcask::detail::from_utf8(mk_run_filename(dir, gen)), ec);
}

bool OkiState::rebuild(std::string_view dir, std::vector<DeltaRow>&& rows,
                       std::uint64_t cover_ord,
                       std::optional<OkiCommitSnapshot> commit_snapshot) {
    // S33-6：零活 key（空库 / 全删）不落空 run。
    if (rows.empty()) {
        return install_built_run(dir, 0, cover_ord, std::move(commit_snapshot));
    }
    // S36-1：外排构建（64MiB 分段 spill + k 路归并）。S43-1：出 v3。
    const std::uint64_t gen = reserve_gen(dir);
    const auto fail = [&] {
        abandon_gen(dir, gen);
        return false;
    };
    auto b = SpillingRunBuilder::create(std::string(dir), gen, kRunVersion3);
    if (!b) return fail();
    for (const auto& r : rows) {
        auto a = b->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
        if (!a) return fail();
    }
    if (!b->finish(/*fsync_dir=*/true)) return fail();
    return install_built_run(dir, gen, cover_ord, std::move(commit_snapshot));
}

bool OkiState::install_built_run(
    std::string_view dir, std::uint64_t gen, std::uint64_t cover_ord,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    if (gen != 0) release_gen_locked(gen);
    const auto run_path = gen != 0 ? mk_run_filename(dir, gen) : std::string();
    auto drop_run = [&] {
        if (gen == 0) return;
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(run_path), ec);
    };
    std::shared_ptr<OkiRunReader> new_reader;
    if (gen != 0) {
        auto rd = OkiRunReader::open(run_path);
        if (!rd) {
            drop_run();
            return false;
        }
        new_reader = std::make_shared<OkiRunReader>(*std::move(rd));
    }

    OkiManifest next;
    if (new_reader) next.runs.push_back({gen, cover_ord, /*format_ver=*/3});
    next.wm = cover_ord;
    next.level_b = level_b_.load(std::memory_order_acquire);
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = std::move(commit_snapshot->fstats);
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) {
            drop_run();
            return false;
        }
    }
    if (!write_manifest(dir, next)) {
        drop_run();
        return false;
    }
    {
        std::vector<std::uint64_t> keep;
        if (new_reader) keep.push_back(gen);
        sweep_runs(dir, keep, sweep_backlog_, inflight_gens_);
        std::vector<std::uint64_t> cache_keep = keep;
        cache_keep.insert(cache_keep.end(), inflight_gens_.begin(),
                          inflight_gens_.end());
        block_cache_->purge_except(cache_keep);
    }
    // 新视图覆盖到此刻的全部 active 行：frozen_through 抬到当前最大到达序号，
    // 旧 active 行随即被水位整体作废（读者按 seq 过滤），再物理删除。
    std::uint64_t through = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        through = append_seq_;
    }
    manifest_ = std::move(next);
    readers_.clear();
    if (new_reader) readers_.emplace_back(gen, std::move(new_reader));
    frozen_.clear();
    frozen_through_ = std::max(frozen_through_, through);
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    drop_active_through(frozen_through_);
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }
    return true;
}

void OkiState::install_memory_view(
    std::vector<DeltaRow>&& rows,
    std::uint64_t cover_ord,
    std::optional<OkiCommitSnapshot> commit_snapshot) {
    std::lock_guard<std::mutex> flk(flush_mu_);
    OkiManifest next;
    next.wm = cover_ord;
    next.level_b = level_b_.load(std::memory_order_acquire);
    if (commit_snapshot.has_value()) {
        next.metadata = commit_snapshot->metadata;
        next.fstats = commit_snapshot->fstats;
        if (!valid_commit_snapshot(next, *next.metadata, next.fstats)) return;
    }
    // 只读视图：空 run 集 + 一个冻结段（内存行覆盖全序）。不修改盘面。
    for (auto& r : rows) r.seq = 0;
    auto seg = std::make_shared<const Segment>(sort_dedup(std::move(rows)));
    std::uint64_t through = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        through = append_seq_;
    }
    manifest_ = std::move(next);
    readers_.clear();
    frozen_.clear();
    if (!seg->empty()) frozen_.push_back(std::move(seg));
    frozen_through_ = std::max(frozen_through_, through);
    publish_view_locked();
    manifest_level_b_.store(manifest_.level_b, std::memory_order_release);
    wm_.store(manifest_.wm, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
    drop_active_through(frozen_through_);
    {
        std::lock_guard<std::mutex> vlk(view_mu_);
        sorted_view_.reset();
    }
}

// ---------------------------------------------------------------------------
// 读：点查与读视图（S44-1：先 active、后视图、按 seq 过滤）
// ---------------------------------------------------------------------------

// S36-2：组合视图点查。查询序：active（seq > frozen_through 才算数）→ 冻结
// 段（新→旧）→ run（gen 降序）。首命中即权威——依赖不变量「行只随时间进入
// 更新的来源」（flush 按时序冻结、run 按 gen、归并输出取最大 gen）。
OkiState::LocateResult OkiState::locate(std::string_view key) const {
    LocateResult out;
    if (!point_query_.load(std::memory_order_acquire)) return out;
    if (!loaded_.load(std::memory_order_acquire)) return out;

    const auto hit = [&](const auto& r) {
        out.status = LocateStatus::kHit;
        out.ord = r.ord;
        out.tomb = r.tomb;
        out.has_loc = r.has_loc;
        out.loc = r.loc;
        return out;
    };

    // 1) active：必须先于视图读取（反过来会漏掉恰在两步之间被冻结的行）。
    bool have_active = false;
    DeltaRow active;
    {
        std::lock_guard<std::mutex> lk(mu_);
        FlatIdx* fi = flat_idx(delta_flat_idx_);
        const std::uint64_t* slot =
            fi != nullptr ? fi->find(delta_, key, StringHash{}(key)) : nullptr;
        if (slot != nullptr) {
            const DeltaRow& r = delta_[static_cast<std::size_t>((*slot & FlatIdx::kIdxMask) - 1)];
            active.ord = r.ord;
            active.tomb = r.tomb;
            active.has_loc = r.has_loc;
            active.loc = r.loc;
            active.seq = r.seq;
            have_active = true;
        }
    }
    const auto view = view_.load();
    if (!view) return out;  // kUnavailable（loaded 但视图未发布——不可达兜底）
    if (have_active && active.seq > view->frozen_through) return hit(active);

    // 2) 冻结段（新→旧）。
    for (auto it = view->frozen.rbegin(); it != view->frozen.rend(); ++it) {
        if (const DeltaRow* r = segment_find(**it, key)) return hit(*r);
    }

    // 3) run（gen 降序）：bloom 试探 → seek 二分 → 首行比对。哈希只算一次。
    const std::span<const std::byte> kspan(
        reinterpret_cast<const std::byte*>(key.data()), key.size());
    const auto bh = OkiRunReader::bloom_hash(kspan);
    for (auto it = view->runs.rbegin(); it != view->runs.rend(); ++it) {
        const OkiRunReader& rd = *it->second;
        if (rd.version() == kRunVersion) {
            // v1 run：无位置字段也无 bloom——组合视图对该 key 无点查能力。
            return LocateResult{};
        }
        if (!rd.may_contain_hashed(bh)) continue;
        auto e = rd.find(kspan, block_cache_.get(), it->first);
        if (!e) return LocateResult{};  // IO/损坏 → kUnavailable
        if (e->has_value()) return hit(**e);
    }
    out.status = LocateStatus::kMiss;
    return out;
}

std::uint64_t OkiState::read_view_rows_processed_for_test() noexcept {
    return g_read_view_rows_processed.load(std::memory_order_relaxed);
}

std::optional<OkiState::ReadView> OkiState::make_read_view() const {
    // 不持 flush_mu_（flush/归并/重建持该锁横跨长 IO）。成对一致由构造保证：
    // runs 与冻结段来自同一个已发布视图；active 先取、视图后取，丢弃
    // seq ≤ frozen_through 的 active 行（已归入视图）。
    if (!loaded_.load(std::memory_order_acquire)) return std::nullopt;

    // 1) active 的有序视图（S64，见 ActiveSorted）：缓存仍覆盖全部行 → 零拷贝；
    //    否则只拷缓存之后新增的行。切面 = 首次持锁时的 delta_.size()——
    //    apply_batch 整批在一个 mu_ 临界区内追加，切面不会落在批中间。
    using Base = std::shared_ptr<const ActiveSorted>;
    Base base;                       // 可复用的缓存（纪元相符）；否则空
    std::vector<DeltaRow> fresh;     // delta_[from0, target)，未排序
    std::uint64_t epoch = 0;
    std::size_t target = 0;
    for (int attempt = 0;; ++attempt) {
        base.reset();
        fresh.clear();
        bool restart = false;
        std::size_t from = 0;
        for (bool first = true;; first = false) {
            std::lock_guard<std::mutex> lk(mu_);
            if (first) {
                epoch = delta_epoch_;
                target = delta_.size();
                std::lock_guard<std::mutex> vlk(view_mu_);
                if (sorted_view_) {
                    // 别名指针 → 整个对象（tail 是首成员）。
                    const auto* as = reinterpret_cast<const ActiveSorted*>(sorted_view_.get());
                    if (as->epoch == epoch && as->count <= target) {
                        base = Base(sorted_view_, as);
                        from = as->count;
                    }
                }
                fresh.reserve(target - from);
            } else if (delta_epoch_ != epoch) {
                restart = true;  // 分批拷贝途中 active 被重排：重来
                break;
            }
            // 最后一次尝试不再分批（不会被饿死）。
            const std::size_t chunk = attempt >= 2 ? target - from : kViewCopyChunkRows;
            const std::size_t upto = std::min(target, from + chunk);
            fresh.insert(fresh.end(),
                         delta_.begin() + static_cast<std::ptrdiff_t>(from),
                         delta_.begin() + static_cast<std::ptrdiff_t>(upto));
            from = upto;
            if (from == target) break;
        }
        if (!restart) break;
    }
    // 2) 视图（必须在 active 切面之后取）。
    const auto view = view_.load();
    if (!view) return std::nullopt;

    Base cur = base;
    if (!cur || !fresh.empty()) {
        // 锁外整理：新增行排序去重后并进尾巴；尾巴够大就升段并逐级归并。
        auto ns = std::make_shared<ActiveSorted>();
        ns->epoch = epoch;
        ns->count = target;
        std::uint64_t fresh_min = std::numeric_limits<std::uint64_t>::max();
        for (const auto& r : fresh) fresh_min = std::min(fresh_min, r.seq);
        std::uint64_t processed = fresh.size();
        auto sorted_fresh = sort_dedup(std::move(fresh));
        if (base) {
            ns->segs = base->segs;
            ns->seg_min_seq = base->seg_min_seq;
            ns->covered = base->covered;
            ns->tail_min_seq = std::min(base->tail_min_seq, fresh_min);
            if (base->tail.empty()) {
                ns->tail = std::move(sorted_fresh);
            } else {
                processed += base->tail.size();
                ns->tail = merge_dedup(base->tail, std::move(sorted_fresh));
            }
        } else {
            ns->tail_min_seq = fresh_min;
            ns->tail = std::move(sorted_fresh);
        }
        if (target - ns->covered >= kTailPromoteRows) {
            ns->segs.push_back(std::make_shared<const Segment>(std::move(ns->tail)));
            ns->seg_min_seq.push_back(ns->tail_min_seq);
            ns->tail = {};
            ns->tail_min_seq = std::numeric_limits<std::uint64_t>::max();
            ns->covered = target;
            while (ns->segs.size() >= 2) {
                const std::size_t n = ns->segs.size();
                if (ns->segs[n - 2]->size() >= 2 * ns->segs[n - 1]->size()) break;
                processed += ns->segs[n - 2]->size() + ns->segs[n - 1]->size();
                auto merged = std::make_shared<const Segment>(
                    merge_dedup(*ns->segs[n - 2], std::vector<DeltaRow>(*ns->segs[n - 1])));
                const auto mn = std::min(ns->seg_min_seq[n - 2], ns->seg_min_seq[n - 1]);
                ns->segs.pop_back();
                ns->seg_min_seq.pop_back();
                ns->segs.back() = std::move(merged);
                ns->seg_min_seq.back() = mn;
            }
        }
        g_read_view_rows_processed.fetch_add(processed, std::memory_order_relaxed);
        cur = ns;
        {
            // 回填：纪元没变，且没有别的读者已经放进覆盖更多行的缓存。
            std::lock_guard<std::mutex> lk(mu_);
            std::lock_guard<std::mutex> vlk(view_mu_);
            if (delta_epoch_ == epoch) {
                const auto* have = sorted_view_
                    ? reinterpret_cast<const ActiveSorted*>(sorted_view_.get()) : nullptr;
                if (!have || have->epoch != epoch || have->count <= ns->count) {
                    sorted_view_ = std::shared_ptr<const std::vector<DeltaRow>>(cur, &cur->tail);
                }
            }
        }
    }

    ReadView v;
    v.runs.reserve(view->runs.size());
    v.gens.reserve(view->runs.size());
    for (const auto& [gen, rd] : view->runs) {
        v.runs.push_back(rd);
        v.gens.push_back(gen);
    }
    v.cache = block_cache_;
    v.frozen = view->frozen;
    // 3) seq 过滤。胜出行 seq ≤ frozen_through ⟹ 该 key 的权威在视图里
    //    （冻结段恰持有它），整行剔除。常见情形全部行都新于水位，零拷贝。
    //    active 的有序段排在冻结段之后、尾巴之前（来源序：旧 → 新）。
    const auto ft = view->frozen_through;
    const auto filtered = [ft](const std::vector<DeltaRow>& rows) {
        auto out = std::make_shared<std::vector<DeltaRow>>();
        out->reserve(rows.size());
        for (const auto& r : rows) {
            if (r.seq > ft) out->push_back(r);
        }
        return out;
    };
    v.frozen.reserve(v.frozen.size() + cur->segs.size());
    for (std::size_t i = 0; i < cur->segs.size(); ++i) {
        if (cur->seg_min_seq[i] > ft) {
            v.frozen.push_back(cur->segs[i]);
        } else if (auto f = filtered(*cur->segs[i]); !f->empty()) {
            v.frozen.push_back(std::move(f));
        }
    }
    if (cur->tail.empty() || cur->tail_min_seq > ft) {
        v.delta = std::shared_ptr<const std::vector<DeltaRow>>(cur, &cur->tail);
    } else {
        v.delta = filtered(cur->tail);
    }
    return v;
}

std::size_t OkiState::delta_rows() const {
    // 尚未落盘的行：active + 待落盘冻结段。
    std::size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        n = delta_.size();
    }
    if (const auto view = view_.load()) {
        for (const auto& seg : view->frozen) n += seg->size();
    }
    return n;
}

std::size_t OkiState::delta_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_bytes_;
}

std::size_t OkiState::run_count() const {
    std::lock_guard<std::mutex> lk(flush_mu_);
    return manifest_.runs.size();
}

}  // namespace bitcask::oki
