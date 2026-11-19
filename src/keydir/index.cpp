// S42 live-only DocMap 实现。设计：docs/design/s42-docmap-live-store-design-zh.md。
// 内存形态：ext2ord_ + live_ + meta_ + delta_live_（见 index.hpp 顶注）；
// sidecar（"BCIS"）序列化格式逐字节不变（S18-2 平移）。

#include "bitcask/index.hpp"

#include "bitcask/codec.hpp"  // S18-2：sidecar CRC
#include "bitcask/vbyte.hpp"   // S21-2 A2：sidecar v2 行 gap+vbyte
#include "bitcask/detail/stream_cursor.hpp"  // 6.6.0：docmap 行段流式载入

#include <algorithm>
#include <cstring>

namespace bitcask::index {

// S21-1：布局契约——DocLoc 消 padding 后 16B；DocSlot 32B（tstamp u64）。
// serialize_docmap/deserialize_docmap 逐字段读写，不受内存布局影响。
static_assert(sizeof(DocLoc) == 16, "DocLoc 应为 16B（offset 前置消 padding）");
static_assert(sizeof(DocSlot) == 32, "DocSlot 应为 32B（tstamp u64）");

std::uint64_t Index::alloc_ord() {
    std::unique_lock lk(mutex_);
    return next_ord_++;
}

bool Index::put_doc(std::string_view ext_id, std::uint64_t ord,
                    const DocSlot& slot) {
    std::unique_lock lk(mutex_);

    next_ord_ = std::max(next_ord_, ord + 1);

    // S40 D2：key_view 指向 ext2ord_ 节点键（node-based，地址稳定）。
    // 覆盖写只更新节点 value，不移动 key——既有 LiveRow.ext 不悬垂。
    std::string_view key_view;
    if (auto it = ext2ord_.find(ext_id); it != ext2ord_.end()) {
        const std::uint64_t old_ord = it->second;
        // S45-2：LSN 守卫——应用序不再等于 LSN 序（索引管线去全局顺序化，
        // designs/index-pipeline-unordered-design-zh.md §4.1）。更旧版本到达
        // 直接丢弃；同 LSN = 重放/重复投递，幂等 no-op（保留既有行——含
        // BM25 回填的 doc_len 与 meta，重放的 doc_len=0 占位不得覆盖它）。
        if (old_ord >= ord) return false;
        // S42：旧行写路径同步淘汰（live row / meta / delta journal 三处）。
        if (live_.erase(old_ord) > 0) {
            ++retired_since_compact_;  // S12-2：仅旧版本实际 live 时计退休
        }
        meta_.erase(old_ord);
        delta_live_.erase(old_ord);
        it->second = ord;
        key_view = it->first;
    } else {
        auto [ins, ok] = ext2ord_.emplace(std::string(ext_id), ord);
        (void)ok;
        key_view = ins->first;
        ++live_docs_;
    }

    live_.insert_or_assign(ord, LiveRow{key_view, slot});
    // S42-2：窗口 journal 只记当前窗口写入；覆盖写淘汰旧 ord（上方）后
    // 仅留最新——热点覆盖写不会把 journal 撑到 O(writes)。
    if (!journal_suppressed_ && ord >= delta_window_wm_) {
        delta_live_.insert(ord);
    }
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
    return true;
}

bool Index::relocate(std::string_view ext_id, std::uint64_t ord,
                     const DocLoc& loc) {
    std::unique_lock lk(mutex_);
    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end() || it->second != ord) return false;
    auto lit = live_.find(ord);
    if (lit == live_.end()) return false;
    lit->second.slot.loc = loc;
    dirty_.store(true, std::memory_order_relaxed);
    return true;
}

bool Index::remove(std::string_view ext_id, std::uint64_t tomb_ord) {
    std::unique_lock lk(mutex_);

    next_ord_ = std::max(next_ord_, tomb_ord + 1);
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
    // S18-2：delta 窗口删除日志自记账（S14-4 门限：链覆盖区内的旧墓碑不入，
    // 防跨文件 stale removal 重放误杀复活文档）。**命中与否都入账**
    // ——与旧 recover_tomb 语义一致（keydir 半边的 remove_if_older 重放可能
    // 仍需要该条目，即使 docmap 已无此 key）。ckpt 载入重放产生的污染由
    // 载入方收尾 clear_removals()。
    const bool log_removal = tomb_ord >= delta_window_wm_;

    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) {
        if (log_removal) removals_.emplace_back(std::string(ext_id), tomb_ord);
        return false;
    }
    const std::uint64_t cur_ord = it->second;
    // S45-2：只删更旧的版本。当前行 LSN ≥ 墓碑 LSN = 墓碑之后的写已先行
    // 应用（乱序到达），本删除过期——保留当前行。
    if (cur_ord >= tomb_ord) {
        if (log_removal) removals_.emplace_back(std::string(ext_id), tomb_ord);
        return false;
    }
    // S42：先淘汰 live row（拿 retire 计数），再摘 ext 节点——LiveRow.ext
    // 指向节点键，extract 之后任何指向该节点的 view 都不得存活。
    if (live_.erase(cur_ord) > 0) {
        ++retired_since_compact_;  // S12-2：删除退休当前版本
    }
    meta_.erase(cur_ord);
    delta_live_.erase(cur_ord);
    // 命中路径:key 从节点 move 进删除日志,省一次拷贝(ext_id 此后不再使用,
    // 即使它恰是本节点键的 view 也无碍)。
    auto nh = ext2ord_.extract(it);
    if (log_removal) removals_.emplace_back(std::move(nh.key()), tomb_ord);
    --live_docs_;
    return true;
}

std::optional<DocHit> Index::get(std::string_view ext_id) const {
    std::shared_lock lk(mutex_);
    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) {
        return std::nullopt;
    }
    const std::uint64_t ord = it->second;
    // 不变量：ext2ord_ 命中 ⟹ live row 存在。防御性检查兜底不变量破坏。
    auto lit = live_.find(ord);
    if (lit == live_.end()) {
        return std::nullopt;
    }
    return DocHit{lit->second.slot, ord};
}

std::optional<std::uint64_t> Index::ord_of(std::string_view ext_id) const {
    std::shared_lock lk(mutex_);
    auto it = ext2ord_.find(ext_id);
    if (it == ext2ord_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> Index::ord_to_ext(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    // S40：契约「已删返回 nullopt」——live-only map 中死 ord 天然不存在。
    auto it = live_.find(ord);
    if (it == live_.end()) return std::nullopt;
    return std::string(it->second.ext);
}

bool Index::is_live(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    return live_.contains(ord);
}

std::uint32_t Index::doc_len(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    auto it = live_.find(ord);
    return it != live_.end() ? it->second.slot.doc_len : 0;
}

std::vector<std::byte> Index::meta_blob(std::uint64_t ord) const {
    std::shared_lock lk(mutex_);
    // 死 ord / 无 meta → 空返回（无 meta 的文档,filter 直接判 false）。
    if (!live_.contains(ord)) return {};
    auto it = meta_.find(ord);
    if (it == meta_.end()) return {};
    // 锁内拷贝返回:并发 set_meta 会重分配底层 buffer,返回内部指针/span
    // 会在锁外悬垂。
    return it->second;
}

// S13-P8：锁内求值（契约见头文件）。
bool Index::eval_meta(std::uint64_t ord, const meta::MetaFilter& filter) const {
    std::shared_lock lk(mutex_);
    if (!live_.contains(ord)) return false;  // 死 ord 恒不通过（S42 强化）
    auto it = meta_.find(ord);
    if (it == meta_.end() || it->second.empty()) {
        return false;  // 无 meta 恒不通过（与 materialize_hits 一致）
    }
    return filter.evaluate(std::span<const std::byte>(it->second));
}

void Index::fill_eval_meta(std::span<const std::uint64_t> ords,
                           const meta::MetaFilter& filter,
                           std::span<char> out) const {
    std::shared_lock lk(mutex_);
    for (std::size_t i = 0; i < ords.size(); ++i) {
        bool ok = false;
        if (live_.contains(ords[i])) {  // 死 ord 恒不通过
            const auto it = meta_.find(ords[i]);
            ok = it != meta_.end() && !it->second.empty() &&
                 filter.evaluate(std::span<const std::byte>(it->second));
        }
        out[i] = static_cast<char>(ok);
    }
}

void Index::set_doc_len(std::uint64_t ord, std::uint32_t len) {
    std::unique_lock lk(mutex_);
    auto it = live_.find(ord);
    if (it == live_.end()) return;  // 未登记（空 job 守卫路径）：no-op
    it->second.slot.doc_len = len;
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
}

void Index::set_meta(std::uint64_t ord, std::span<const std::byte> blob) {
    std::unique_lock lk(mutex_);
    // S42：meta 只挂 live 行——死 ord 不落条目（旧实现的平坦列对死 ord
    // 留值但序列化/求值均不可见，新实现 no-op 语义等价且更省）。
    if (!live_.contains(ord)) return;
    if (blob.empty()) {
        meta_.erase(ord);
    } else {
        meta_[ord].assign(blob.begin(), blob.end());
    }
    dirty_.store(true, std::memory_order_relaxed);  // S18-2：自记账
}

// S42：全局 DocMap 批量过滤放弃 SIMD gather（设计 §10）——一次 shared_lock
// 内逐 candidate hash lookup。BM25 主评分路径走 segment-local live checker，
// 不经本接口；本接口服务 HNSW/IVF callback 与结果物化等长尾路径。
void Index::fill_is_live(std::span<const std::uint64_t> ords,
                         std::span<char> out) const {
    std::shared_lock lk(mutex_);
    for (std::size_t i = 0; i < ords.size(); ++i) {
        out[i] = static_cast<char>(live_.contains(ords[i]));
    }
}

void Index::fill_doc_lens(std::span<const std::uint64_t> ords,
                          std::span<std::uint32_t> out) const {
    std::shared_lock lk(mutex_);
    for (std::size_t i = 0; i < ords.size(); ++i) {
        auto it = live_.find(ords[i]);
        out[i] = it != live_.end() ? it->second.slot.doc_len : 0u;
    }
}

IndexInfo Index::info() const {
    std::shared_lock lk(mutex_);
    return IndexInfo{
        .live_docs        = live_docs_,
        .total_ords       = next_ord_,
        .next_ord         = next_ord_,
        .chunks_allocated = chunks_alloc_,   // S42 起恒 0
        .chunks_freed     = chunks_freed_,   // S42 起恒 0
    };
}

std::uint64_t Index::compact_chunks() {
    // S42：live-only 结构无 chunk 可释放；保留 API 兼容（Cask merge / shim）。
    std::unique_lock lk(mutex_);
    return 0;
}

// ---- S18-2：docmap 持久化记账 ----

void Index::begin_delta_window(std::uint64_t wm) {
    std::unique_lock lk(mutex_);
    delta_window_wm_ = wm;
    // S42-2：窗口推进 = 旧窗口 journal 整体作废（base/delta 落成或载入
    // 成功后，下一窗口从空 journal 起记）。
    delta_live_.clear();
}

std::vector<std::pair<std::string, std::uint64_t>>
Index::removals_snapshot() const {
    std::shared_lock lk(mutex_);
    return removals_;
}

void Index::clear_removals() {
    std::unique_lock lk(mutex_);
    removals_.clear();
}

// ---- S18-2：docmap sidecar（"BCIS"）序列化（自 SearchLayer 平移）----

namespace {
constexpr std::uint32_t kSidecarMagic   = 0x42434953;  // "BCIS"
// S21-2 A2：行编码 gap+vbyte（ord 差分 + 标量 vbyte，tstamp 保持定宽）。
// v3：tstamp 定宽 8B（64 位时间戳 flag-day）。写端恒写 v3；读端仅收 v3——
// v1/v2 属 u32-tstamp 纪元，version 不符拒收 → 组件退 fold（可重建，降级安全）。
constexpr std::uint32_t kSidecarVersion   = 3;
void sc_put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void sc_put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
}
}  // namespace

bool Index::serialize_docmap(std::vector<std::uint8_t>& buf,
                             std::uint64_t covers_next_ord) const {
    buf.clear();
    // S4:预留容量——v2 变长典型 ≤15B/行 + 变长 ext_id（按 48B/行估值），
    // 常态零 realloc（偏小由几何增长兜底，reserve 仅设容量、绝不溢出）。
    const std::uint64_t live = info().live_docs;
    buf.reserve(28 + static_cast<std::size_t>(live) * (15 + 48));
    sc_put32(buf, kSidecarMagic);
    sc_put32(buf, kSidecarVersion);
    sc_put64(buf, covers_next_ord);
    // 行数占位,回填。
    const std::size_t cnt_pos = buf.size();
    sc_put64(buf, 0);
    std::uint64_t rows = 0;
    bool ok = true;
    std::uint64_t prev_ord = 0;
    // S42：for_each_live 锁内按 ord 升序回调——gap 编码压缩率与旧实现一致。
    for_each_live([&](std::uint64_t ord, std::string_view ext,
                      const DocSlot& slot) {
        if (ext.size() > 0xFFFF) { ok = false; return; }
        codec::vbyte_encode(ord - prev_ord, buf);
        prev_ord = ord;
        codec::vbyte_encode(ext.size(), buf);
        const auto* kd = reinterpret_cast<const std::uint8_t*>(ext.data());
        buf.insert(buf.end(), kd, kd + ext.size());
        codec::vbyte_encode(slot.loc.file_id, buf);
        codec::vbyte_encode(slot.loc.offset, buf);
        codec::vbyte_encode(slot.loc.total_sz, buf);
        sc_put64(buf, slot.tstamp);   // 定宽 8B：u64 unix 秒（v3）
        codec::vbyte_encode(slot.doc_len, buf);
        ++rows;
    });
    if (!ok) return false;
    std::memcpy(buf.data() + cnt_pos, &rows, 8);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 8));
    sc_put32(buf, crc);
    return true;
}

namespace {
// 6.6.0：行段解析对游标类型模板化——span 版（legacy_ckpt 等整段在手的调用
// 方）与流式版（docmap_ckpt 开库载入）共用这一份解析。Cur 须提供
// SnapCursor 同形接口：u64 / vb / bytes / fail / at_end。
struct SidecarSpanCursor {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool fail = false;
    [[nodiscard]] bool at_end() const noexcept { return p == end; }
    bool bytes(void* dst, std::size_t n) {
        if (fail || static_cast<std::size_t>(end - p) < n) {
            fail = true;
            return false;
        }
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    std::uint64_t u64() { std::uint64_t v = 0; bytes(&v, 8); return v; }
    std::uint64_t vb() {  // 边界安全 vbyte
        std::uint64_t v = 0, shift = 0;
        while (true) {
            std::uint8_t byte = 0;
            if (!bytes(&byte, 1)) return 0;
            v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if (byte & 0x80) return v;
            shift += 7;
            if (shift > 63) { fail = true; return 0; }
        }
    }
};

// 游标起点 = magic/version 之后（covers 字段），终点 = 尾部 CRC 之前。
template <class Cur>
std::optional<std::uint64_t> parse_sidecar_rows(Index& idx, Cur& c) {
    const std::uint64_t covers = c.u64();
    const std::uint64_t rows = c.u64();
    if (c.fail || rows > (1ull << 40)) return std::nullopt;
    std::uint64_t prev_ord = 0;
    std::string ext;
    for (std::uint64_t i = 0; i < rows; ++i) {
        const std::uint64_t ord = prev_ord + c.vb();  // gap（二补数回绕，正确性不依赖升序）
        prev_ord = ord;
        const std::uint64_t klen = c.vb();
        if (c.fail || klen > 0xFFFF) return std::nullopt;
        ext.resize(static_cast<std::size_t>(klen));
        if (!c.bytes(ext.data(), ext.size())) return std::nullopt;
        DocSlot slot;
        const std::uint64_t fid = c.vb(), off = c.vb(), tsz = c.vb();
        slot.tstamp = c.u64();
        const std::uint64_t dl = c.vb();
        if (c.fail || fid > 0xFFFFFFFFull || tsz > 0xFFFFFFFFull ||
            dl > 0xFFFFFFFFull) {
            return std::nullopt;
        }
        slot.loc.offset   = off;
        slot.loc.file_id  = static_cast<std::uint32_t>(fid);
        slot.loc.total_sz = static_cast<std::uint32_t>(tsz);
        slot.doc_len      = static_cast<std::uint32_t>(dl);
        idx.put_doc(ext, ord, slot);  // 重建 ext2ord/live/水位
    }
    if (!c.at_end()) return std::nullopt;
    return covers;
}
}  // namespace

std::optional<std::uint64_t>
Index::deserialize_docmap(std::span<const std::uint8_t> buf) {
    if (buf.size() < 28) return std::nullopt;
    auto rd32 = [&](std::size_t off) {
        std::uint32_t v; std::memcpy(&v, buf.data() + off, 4); return v;
    };
    if (rd32(0) != kSidecarMagic) return std::nullopt;
    const std::uint32_t ver = rd32(4);
    if (ver != kSidecarVersion) return std::nullopt;
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, buf.data() + buf.size() - 4, 4);
    const std::uint32_t crc = bitcask::codec::crc32(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf.data() + 8), buf.size() - 12));
    if (crc != stored_crc) return std::nullopt;

    SidecarSpanCursor c{buf.data() + 8, buf.data() + buf.size() - 4};
    return parse_sidecar_rows(*this, c);
}

std::optional<std::uint64_t>
Index::deserialize_docmap(io::FileHandle fd, std::uint64_t off,
                          std::uint64_t len) {
    // 与 span 版逐步对应：头 8B（magic/version）→ 尾 4B CRC → 分块校验
    // [8, len-4) → 流式解析。全程只有 StreamCursor 的一块缓冲在堆上。
    if (len < 28) return std::nullopt;
    std::uint32_t hdr[2] = {};
    if (!io::pread_all(fd, hdr, 8, off)) return std::nullopt;
    if (hdr[0] != kSidecarMagic || hdr[1] != kSidecarVersion) {
        return std::nullopt;
    }
    std::uint32_t stored_crc = 0;
    if (!io::pread_all(fd, &stored_crc, 4, off + len - 4)) return std::nullopt;
    const auto crc = bitcask::detail::crc32_file_range(fd, off + 8, len - 12);
    if (!crc || *crc != stored_crc) return std::nullopt;

    bitcask::detail::StreamCursor c(fd, off + 8, len - 12);
    return parse_sidecar_rows(*this, c);
}

}  // namespace bitcask::index
