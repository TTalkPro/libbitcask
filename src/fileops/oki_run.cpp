#include "bitcask/oki_run.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"   // crc32 / crc32_update
#include "bitcask/vbyte.hpp"   // vbyte_encode / vbyte_read_checked
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::oki {

namespace {

// ---- S36-1 bloom 稳定哈希（持久化格式的一部分，跨平台/版本不变）----
// h1 = FNV-1a 64；h2 = splitmix64(h1)|1；bit_i = (h1 + i*h2) % n_bits。
std::uint64_t fnv1a64(std::string_view k) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : k) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x00000100000001b3ull;
    }
    return h;
}
std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// key span → string_view（比较用；OKI 的 key 序 = 无符号字节字典序）。
std::string_view sv(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// fwrite 整段并把字节算进 running CRC。
[[nodiscard]] bool fwrite_crc(std::FILE* f, std::span<const std::byte> b,
                              std::uint32_t& crc) {
    if (b.empty()) return true;
    if (std::fwrite(b.data(), 1, b.size(), f) != b.size()) return false;
    crc = codec::crc32_update(crc, b);
    return true;
}

}  // namespace

std::string mk_run_filename(std::string_view dir, std::uint64_t gen) {
    return std::string(dir) + "/kv.oki.seg-" + std::to_string(gen);
}

std::string mk_manifest_filename(std::string_view dir) {
    return std::string(dir) + "/" + kManifestName;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

std::expected<OkiRunWriter, OkiError>
OkiRunWriter::create(std::string path, std::size_t block_target,
                     std::uint32_t version, std::uint64_t expected_entries) {
    if (version != kRunVersion && version != kRunVersion2) {
        return std::unexpected(OkiError::kBadState);
    }
    detail::AtomicFileWriter w(std::move(path), ".oki.tmp");
    if (!w) return std::unexpected(OkiError::kIo);
    return OkiRunWriter(std::move(w), std::max<std::size_t>(block_target, 64),
                        version, expected_entries);
}

void OkiRunWriter::init_bloom(std::uint64_t expected_entries) {
    // n_bits 向上取 64 的倍数；expected=0 → 最小 64 bits（超容只推高 FP）。
    const std::uint64_t want =
        std::max<std::uint64_t>(64, expected_entries * kBloomBitsPerEntry);
    bloom_nbits_ = (want + 63) / 64 * 64;
    bloom_bits_.assign(static_cast<std::size_t>(bloom_nbits_ / 64), 0);
}

void OkiRunWriter::bloom_insert(std::string_view key) {
    const std::uint64_t h1 = fnv1a64(key);
    const std::uint64_t h2 = splitmix64(h1) | 1;
    for (std::uint8_t i = 0; i < kBloomHashes; ++i) {
        const std::uint64_t bit = (h1 + i * h2) % bloom_nbits_;
        bloom_bits_[static_cast<std::size_t>(bit / 64)] |= 1ull << (bit % 64);
    }
}

bool OkiRunWriter::flush_block() {
    if (blk_.empty()) return true;
    if (!header_written_) {
        // 惰性写 header（首块落盘前）：magic + version。
        std::byte hdr[kRunHeaderSize];
        le_store_u32(hdr, kRunMagic);
        le_store_u32(hdr + 4, version_);
        if (!fwrite_crc(w_.get(), std::span<const std::byte>(hdr, sizeof hdr),
                        crc_)) {
            return false;
        }
        header_written_ = true;
    }
    index_.push_back({blk_first_key_, file_off_});
    if (!fwrite_crc(w_.get(), blk_, crc_)) return false;
    file_off_ += blk_.size();
    blk_.clear();
    blk_first_key_.clear();
    prev_key_.clear();
    prev_ord_ = 0;
    prev_tstamp_ = 0;
    return true;
}

std::expected<void, OkiError>
OkiRunWriter::add(std::span<const std::byte> key, std::uint64_t ord,
                  bool tomb, const RowLoc* loc) {
    if (finished_) return std::unexpected(OkiError::kBadState);
    if (loc != nullptr && version_ != kRunVersion2) {
        return std::unexpected(OkiError::kBadState);  // v1 无位置字段
    }
    const std::string_view k = sv(key);
    if (has_last_ && !(k > std::string_view(last_key_))) {
        return std::unexpected(OkiError::kOutOfOrder);
    }

    if (blk_.empty()) blk_first_key_.assign(k);

    // 块内前缀差分（块首条 prev_key_ 为空 → shared=0 全量 key）。
    const std::size_t max_shared = std::min(prev_key_.size(), k.size());
    std::size_t shared = 0;
    while (shared < max_shared && prev_key_[shared] == k[shared]) ++shared;

    codec::vbyte_encode(shared, blk_);
    codec::vbyte_encode(k.size() - shared, blk_);
    const auto* suffix = key.data() + shared;
    blk_.insert(blk_.end(), suffix, suffix + (key.size() - shared));
    codec::vbyte_encode(ord - prev_ord_, blk_);  // 回绕差分（同 hint v5）
    std::uint8_t flags = tomb ? kFlagTomb : 0;
    if (loc != nullptr) flags |= kFlagHasLoc;
    blk_.push_back(static_cast<std::byte>(flags));
    if (loc != nullptr) {
        codec::vbyte_encode(loc->file_id, blk_);
        codec::vbyte_encode(loc->total_sz, blk_);
        codec::vbyte_encode(loc->offset, blk_);
        codec::vbyte_encode(loc->tstamp - prev_tstamp_, blk_);  // 回绕差分
        prev_tstamp_ = loc->tstamp;
    }
    if (version_ == kRunVersion2) bloom_insert(k);

    prev_key_.assign(k);
    prev_ord_ = ord;
    last_key_.assign(k);
    has_last_ = true;
    ++entries_;

    if (blk_.size() >= block_target_) {
        if (!flush_block()) return std::unexpected(OkiError::kIo);
    }
    return {};
}

std::expected<OkiRunWriter::Stats, OkiError>
OkiRunWriter::finish(bool fsync_dir) {
    if (finished_) return std::unexpected(OkiError::kBadState);
    finished_ = true;
    if (!flush_block()) return std::unexpected(OkiError::kIo);
    if (!header_written_) {
        // 空 run：仍写 header（合法——0 条目、0 块）。
        std::byte hdr[kRunHeaderSize];
        le_store_u32(hdr, kRunMagic);
        le_store_u32(hdr + 4, version_);
        if (!fwrite_crc(w_.get(), std::span<const std::byte>(hdr, sizeof hdr),
                        crc_)) {
            return std::unexpected(OkiError::kIo);
        }
        header_written_ = true;
    }

    // 稀疏索引区。
    const std::uint64_t index_off = file_off_;
    std::vector<std::byte> idx;
    {
        std::byte cnt[4];
        le_store_u32(cnt, static_cast<std::uint32_t>(index_.size()));
        idx.insert(idx.end(), cnt, cnt + 4);
    }
    for (const auto& e : index_) {
        codec::vbyte_encode(e.first_key.size(), idx);
        const auto* p = reinterpret_cast<const std::byte*>(e.first_key.data());
        idx.insert(idx.end(), p, p + e.first_key.size());
        std::byte off[8];
        le_store_u64(off, e.off);
        idx.insert(idx.end(), off, off + 8);
    }
    if (!fwrite_crc(w_.get(), idx, crc_)) return std::unexpected(OkiError::kIo);
    file_off_ += idx.size();

    // v2：bloom 区 [n_bits u64][k u8][位数组]（计入 CRC）。
    std::uint64_t bloom_off = 0;
    if (version_ == kRunVersion2) {
        bloom_off = file_off_;
        std::vector<std::byte> bl;
        bl.reserve(9 + bloom_bits_.size() * 8);
        std::byte nb[8];
        le_store_u64(nb, bloom_nbits_);
        bl.insert(bl.end(), nb, nb + 8);
        bl.push_back(static_cast<std::byte>(kBloomHashes));
        for (const std::uint64_t w64 : bloom_bits_) {
            std::byte b8[8];
            le_store_u64(b8, w64);
            bl.insert(bl.end(), b8, b8 + 8);
        }
        if (!fwrite_crc(w_.get(), bl, crc_)) {
            return std::unexpected(OkiError::kIo);
        }
        file_off_ += bl.size();
    }

    // trailer：定宽字段计入 CRC，随后 [crc][magic]。
    // v1: [entry_count][index_off]；v2: [entry_count][index_off][bloom_off]。
    const std::size_t t1_len = version_ == kRunVersion2 ? 24 : 16;
    std::byte t1[24];
    le_store_u64(t1, entries_);
    le_store_u64(t1 + 8, index_off);
    if (version_ == kRunVersion2) le_store_u64(t1 + 16, bloom_off);
    if (!fwrite_crc(w_.get(), std::span<const std::byte>(t1, t1_len), crc_)) {
        return std::unexpected(OkiError::kIo);
    }
    std::byte t2[8];
    le_store_u32(t2, crc_);
    le_store_u32(t2 + 4, kRunTrailerMagic);
    if (std::fwrite(t2, 1, sizeof t2, w_.get()) != sizeof t2) {
        return std::unexpected(OkiError::kIo);
    }
    const std::uint64_t total = file_off_ + t1_len + 8;
    if (!w_.commit(fsync_dir)) return std::unexpected(OkiError::kIo);
    return Stats{entries_, index_.size(), total};
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

std::expected<OkiRunReader, OkiError> OkiRunReader::open(std::string path) {
    auto f = io::PosixFile::open(path, io::OpenFlag::kReadOnly);
    if (!f) return std::unexpected(OkiError::kIo);
    OkiRunReader r;
    r.file_ = std::move(*f);

    auto end = r.file_.seek(0, SEEK_END);
    if (!end) return std::unexpected(OkiError::kIo);
    const std::uint64_t total = *end;
    if (total < kRunHeaderSize + 4 + kRunTrailerSize) {
        return std::unexpected(OkiError::kCorrupt);
    }

    // header：magic + version（1/2 都收——S36-1 起读端双版本）。
    std::byte hdr[kRunHeaderSize];
    {
        auto n = r.file_.pread_into(0, std::span(hdr, sizeof hdr));
        if (!n) return std::unexpected(OkiError::kIo);
        if (*n != sizeof hdr) return std::unexpected(OkiError::kCorrupt);
    }
    if (le_load_u32(hdr) != kRunMagic) {
        return std::unexpected(OkiError::kCorrupt);
    }
    r.version_ = le_load_u32(hdr + 4);
    if (r.version_ != kRunVersion && r.version_ != kRunVersion2) {
        return std::unexpected(OkiError::kCorrupt);  // 未来版本：拒收重建
    }
    const bool v2 = r.version_ == kRunVersion2;
    const std::size_t trailer_size = v2 ? kRunTrailerSizeV2 : kRunTrailerSize;
    // 最小合法文件 = header + 空索引(4) + (v2: 最小 bloom 17B) + trailer。
    if (total < kRunHeaderSize + 4 + (v2 ? 17 : 0) + trailer_size) {
        return std::unexpected(OkiError::kCorrupt);
    }
    std::byte tr[kRunTrailerSizeV2];
    {
        auto n = r.file_.pread_into(total - trailer_size,
                                    std::span(tr, trailer_size));
        if (!n) return std::unexpected(OkiError::kIo);
        if (*n != trailer_size) return std::unexpected(OkiError::kCorrupt);
    }
    if (le_load_u32(tr + trailer_size - 4) != kRunTrailerMagic) {
        return std::unexpected(OkiError::kCorrupt);
    }
    r.entry_count_ = le_load_u64(tr);
    const std::uint64_t index_off = le_load_u64(tr + 8);
    const std::uint64_t bloom_off = v2 ? le_load_u64(tr + 16) : 0;
    const std::uint32_t expected_crc = le_load_u32(tr + trailer_size - 8);
    // 区域边界：数据块 [hdr, index_off) → 索引 [index_off, 区末) →
    // (v2) bloom [bloom_off, total - trailer)。
    const std::uint64_t index_end = v2 ? bloom_off : total - trailer_size;
    if (index_off < kRunHeaderSize || index_off + 4 > index_end ||
        index_end > total - trailer_size) {
        return std::unexpected(OkiError::kCorrupt);
    }
    if (v2 && (bloom_off < index_off + 4 ||
               bloom_off + 17 > total - trailer_size)) {
        return std::unexpected(OkiError::kCorrupt);
    }

    // 全文件 CRC（覆盖 [0, size-8)）。run 是派生缓存：eager 校验换绝对安全。
    {
        std::uint64_t remaining = total - 8;
        std::uint64_t off = 0;
        std::uint32_t crc = 0;
        constexpr std::size_t kChunk = 65536;
        std::vector<std::byte> buf;
        while (remaining > 0) {
            const std::size_t n = static_cast<std::size_t>(
                std::min<std::uint64_t>(kChunk, remaining));
            if (buf.size() < n) buf.resize(n);
            auto got = r.file_.pread_into(off, std::span(buf.data(), n));
            if (!got) return std::unexpected(OkiError::kIo);
            if (*got != n) return std::unexpected(OkiError::kCorrupt);
            crc = codec::crc32_update(
                crc, std::span<const std::byte>(buf.data(), n));
            remaining -= n;
            off += n;
        }
        if (crc != expected_crc) return std::unexpected(OkiError::kCorrupt);
    }

    // 稀疏索引区 [index_off, index_end)。
    {
        const std::size_t idx_len =
            static_cast<std::size_t>(index_end - index_off);
        std::vector<std::byte> idx(idx_len);
        auto got = r.file_.pread_into(index_off,
                                      std::span(idx.data(), idx.size()));
        if (!got || *got != idx_len) {
            return std::unexpected(OkiError::kCorrupt);
        }
        if (idx_len < 4) return std::unexpected(OkiError::kCorrupt);
        const std::uint32_t count = le_load_u32(idx.data());
        std::span<const std::byte> ib(idx);
        std::size_t pos = 4;
        std::uint64_t prev_off = 0;
        r.blocks_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto klen = codec::vbyte_read_checked(ib, pos);
            if (!klen) return std::unexpected(OkiError::kCorrupt);
            pos = klen->second;
            const std::uint64_t kl = klen->first;
            if (pos + kl + 8 > ib.size()) {
                return std::unexpected(OkiError::kCorrupt);
            }
            BlockRef b;
            b.first_key.assign(reinterpret_cast<const char*>(ib.data() + pos),
                               static_cast<std::size_t>(kl));
            pos += static_cast<std::size_t>(kl);
            b.off = le_load_u64(ib.data() + pos);
            pos += 8;
            // 块偏移必须严格递增且落在数据块区内。
            if (b.off < kRunHeaderSize || b.off >= index_off ||
                (i > 0 && b.off <= prev_off) ||
                (i == 0 && b.off != kRunHeaderSize)) {
                return std::unexpected(OkiError::kCorrupt);
            }
            if (i > 0) r.blocks_[i - 1].end = b.off;
            prev_off = b.off;
            r.blocks_.push_back(std::move(b));
        }
        if (!r.blocks_.empty()) r.blocks_.back().end = index_off;
        if (pos != ib.size()) return std::unexpected(OkiError::kCorrupt);
        // 块首 key 必须升序（seek 二分依赖）。
        for (std::size_t i = 1; i < r.blocks_.size(); ++i) {
            if (!(r.blocks_[i].first_key > r.blocks_[i - 1].first_key)) {
                return std::unexpected(OkiError::kCorrupt);
            }
        }
    }

    // v2：bloom 区 [bloom_off, total - trailer) 整载内存。
    if (v2) {
        const std::size_t bl_len =
            static_cast<std::size_t>(total - trailer_size - bloom_off);
        std::vector<std::byte> bl(bl_len);
        auto got = r.file_.pread_into(bloom_off,
                                      std::span(bl.data(), bl.size()));
        if (!got || *got != bl_len) return std::unexpected(OkiError::kCorrupt);
        const std::uint64_t nbits = le_load_u64(bl.data());
        const auto k = static_cast<std::uint8_t>(bl[8]);
        if (nbits == 0 || nbits % 64 != 0 || k == 0 ||
            bl_len != 9 + static_cast<std::size_t>(nbits / 8)) {
            return std::unexpected(OkiError::kCorrupt);
        }
        r.bloom_nbits_ = nbits;
        r.bloom_k_ = k;
        r.bloom_bits_.resize(static_cast<std::size_t>(nbits / 64));
        for (std::size_t i = 0; i < r.bloom_bits_.size(); ++i) {
            r.bloom_bits_[i] = le_load_u64(bl.data() + 9 + i * 8);
        }
    }
    return r;
}

bool OkiRunReader::may_contain(std::span<const std::byte> key) const noexcept {
    if (version_ != kRunVersion2 || bloom_nbits_ == 0) return true;
    const std::string_view k = sv(key);
    const std::uint64_t h1 = fnv1a64(k);
    const std::uint64_t h2 = splitmix64(h1) | 1;
    for (std::uint8_t i = 0; i < bloom_k_; ++i) {
        const std::uint64_t bit = (h1 + i * h2) % bloom_nbits_;
        if ((bloom_bits_[static_cast<std::size_t>(bit / 64)] &
             (1ull << (bit % 64))) == 0) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// S36-3：块 LRU 缓存
// ---------------------------------------------------------------------------

std::optional<OkiBlockCache::Block> OkiBlockCache::get_or_load(
    std::uint64_t gen, std::uint64_t block_idx,
    const std::function<std::optional<std::vector<std::byte>>()>& loader) {
    const Key k{gen, block_idx};
    Shard& sh = shard_for(k);
    {
        std::lock_guard<std::mutex> lk(sh.mu);
        auto it = sh.map.find(k);
        if (it != sh.map.end()) {
            sh.lru.splice(sh.lru.begin(), sh.lru, it->second);  // 升温
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second->block;
        }
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    auto bytes = loader();  // IO 在锁外
    if (!bytes) return std::nullopt;
    auto block = std::make_shared<const std::vector<std::byte>>(
        *std::move(bytes));
    if (shard_cap_.load(std::memory_order_relaxed) == 0) return block;  // 缓存关
    {
        std::lock_guard<std::mutex> lk(sh.mu);
        auto it = sh.map.find(k);
        if (it != sh.map.end()) return it->second->block;  // 并发 double-load
        sh.lru.push_front(Node{k, block});
        sh.map.emplace(k, sh.lru.begin());
        sh.bytes += block->size();
        evict_over_cap_locked(sh);
    }
    return block;
}

void OkiBlockCache::evict_over_cap_locked(Shard& sh) noexcept {
    const std::size_t cap = shard_cap_.load(std::memory_order_relaxed);
    while (sh.bytes > cap && !sh.lru.empty()) {
        const Node& victim = sh.lru.back();
        sh.bytes -= victim.block->size();
        sh.map.erase(victim.key);
        sh.lru.pop_back();
    }
}

void OkiBlockCache::purge_except(std::span<const std::uint64_t> keep_gens) {
    for (auto& sh : shards_) {
        std::lock_guard<std::mutex> lk(sh.mu);
        for (auto it = sh.lru.begin(); it != sh.lru.end();) {
            const bool keep = std::find(keep_gens.begin(), keep_gens.end(),
                                        it->key.gen) != keep_gens.end();
            if (keep) {
                ++it;
                continue;
            }
            sh.bytes -= it->block->size();
            sh.map.erase(it->key);
            it = sh.lru.erase(it);
        }
    }
}

void OkiBlockCache::reset_capacity(std::size_t capacity_bytes) {
    shard_cap_.store(capacity_bytes / kShards, std::memory_order_relaxed);
    for (auto& sh : shards_) {
        std::lock_guard<std::mutex> lk(sh.mu);
        sh.lru.clear();
        sh.map.clear();
        sh.bytes = 0;
    }
}

OkiBlockCache::Stats OkiBlockCache::stats() const {
    Stats s;
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    for (auto& sh : shards_) {
        std::lock_guard<std::mutex> lk(sh.mu);
        s.bytes += sh.bytes;
        s.blocks += sh.lru.size();
    }
    return s;
}

std::expected<bool, OkiError>
OkiRunReader::Cursor::load_block(std::size_t bi) {
    const auto& b = r_->blocks_[bi];
    const std::size_t len = static_cast<std::size_t>(b.end - b.off);
    if (cache_ != nullptr) {
        // S36-3：块缓存路径——命中零 IO，miss 时 loader pread（锁外）。
        auto blk = cache_->get_or_load(
            cache_gen_, bi,
            [&]() -> std::optional<std::vector<std::byte>> {
                std::vector<std::byte> buf(len);
                auto got = r_->file_.pread_into(b.off,
                                                std::span(buf.data(), len));
                if (!got || *got != len) return std::nullopt;
                return buf;
            });
        if (!blk) return std::unexpected(OkiError::kIo);
        if ((*blk)->size() != len) {
            return std::unexpected(OkiError::kCorrupt);  // 防御（gen 不重用）
        }
        blk_hold_ = *std::move(blk);
    } else {
        blk_hold_.reset();
        blk_.resize(len);
        auto got = r_->file_.pread_into(b.off, std::span(blk_.data(), len));
        if (!got) return std::unexpected(OkiError::kIo);
        if (*got != len) return std::unexpected(OkiError::kCorrupt);
    }
    pos_ = 0;
    prev_key_.clear();
    prev_ord_ = 0;
    prev_tstamp_ = 0;
    block_loaded_ = true;
    return true;
}

std::expected<bool, OkiError> OkiRunReader::Cursor::next(Entry& out) {
    if (pending_) {
        out = *std::move(pending_);
        pending_.reset();
        return true;
    }
    while (true) {
        if (!block_loaded_ || pos_ >= block_span().size()) {
            if (bi_ >= r_->blocks_.size()) return false;  // run 末尾
            auto l = load_block(bi_++);
            if (!l) return std::unexpected(l.error());
        }
        // 解一条（CRC 已过——结构错误仍防御性 fail-fast）。
        const std::span<const std::byte> b = block_span();
        auto shared = codec::vbyte_read_checked(b, pos_);
        if (!shared) return std::unexpected(OkiError::kCorrupt);
        auto sfx = codec::vbyte_read_checked(b, shared->second);
        if (!sfx) return std::unexpected(OkiError::kCorrupt);
        std::size_t pos = sfx->second;
        const std::uint64_t shared_len = shared->first;
        const std::uint64_t suffix_len = sfx->first;
        if (shared_len > prev_key_.size() ||
            pos + suffix_len + 1 > b.size()) {
            return std::unexpected(OkiError::kCorrupt);
        }
        // 重建 key：prev 的公共前缀 + suffix（in-place，跨 next 复用容量）。
        prev_key_.resize(static_cast<std::size_t>(shared_len));
        prev_key_.append(reinterpret_cast<const char*>(b.data() + pos),
                         static_cast<std::size_t>(suffix_len));
        pos += static_cast<std::size_t>(suffix_len);
        auto od = codec::vbyte_read_checked(b, pos);
        if (!od) return std::unexpected(OkiError::kCorrupt);
        pos = od->second;
        if (pos + 1 > b.size()) return std::unexpected(OkiError::kCorrupt);
        const auto flags = static_cast<std::uint8_t>(b[pos]);
        pos += 1;
        const bool v2 = r_->version_ == kRunVersion2;
        const std::uint8_t known = v2 ? kKnownFlagsMaskV2 : kKnownFlagsMask;
        if ((flags & ~known) != 0) {
            // 未知扩展位 fail-fast，绝不静默跳（行无长度前缀，跳不动）。
            return std::unexpected(OkiError::kCorrupt);
        }
        prev_ord_ += od->first;  // 回绕还原

        out.has_loc = false;
        out.loc = RowLoc{};
        if ((flags & kFlagHasLoc) != 0) {
            auto fid = codec::vbyte_read_checked(b, pos);
            if (!fid) return std::unexpected(OkiError::kCorrupt);
            auto tsz = codec::vbyte_read_checked(b, fid->second);
            if (!tsz) return std::unexpected(OkiError::kCorrupt);
            auto off = codec::vbyte_read_checked(b, tsz->second);
            if (!off) return std::unexpected(OkiError::kCorrupt);
            auto tsd = codec::vbyte_read_checked(b, off->second);
            if (!tsd) return std::unexpected(OkiError::kCorrupt);
            pos = tsd->second;
            if (fid->first > 0xFFFF'FFFFull || tsz->first > 0xFFFF'FFFFull) {
                return std::unexpected(OkiError::kCorrupt);
            }
            prev_tstamp_ += tsd->first;  // 回绕还原
            out.has_loc = true;
            out.loc.file_id = static_cast<std::uint32_t>(fid->first);
            out.loc.total_sz = static_cast<std::uint32_t>(tsz->first);
            out.loc.offset = off->first;
            out.loc.tstamp = prev_tstamp_;
        }
        pos_ = pos;

        out.key.assign(prev_key_);
        out.ord = prev_ord_;
        out.tomb = (flags & kFlagTomb) != 0;
        return true;
    }
}

std::expected<OkiRunReader::Cursor, OkiError>
OkiRunReader::seek(std::span<const std::byte> lo) const {
    return seek_impl(lo, /*cache=*/nullptr, /*cache_gen=*/0);
}

std::expected<OkiRunReader::Cursor, OkiError>
OkiRunReader::seek_impl(std::span<const std::byte> lo, OkiBlockCache* cache,
                        std::uint64_t cache_gen) const {
    Cursor c(this);
    c.cache_ = cache;
    c.cache_gen_ = cache_gen;
    if (lo.empty() || blocks_.empty()) return c;
    const std::string_view lov = sv(lo);
    // 最后一个 first_key ≤ lo 的块（全部 > lo 则从块 0 起）。
    auto it = std::upper_bound(
        blocks_.begin(), blocks_.end(), lov,
        [](std::string_view v, const BlockRef& b) {
            return v < std::string_view(b.first_key);
        });
    c.bi_ = (it == blocks_.begin())
                ? 0
                : static_cast<std::size_t>(it - blocks_.begin()) - 1;
    // 块内线性推进到首个 key ≥ lo，越位条目暂存 pending。
    Entry e;
    while (true) {
        auto n = c.next(e);
        if (!n) return std::unexpected(n.error());
        if (!*n) return c;  // 全部 < lo → 空游标
        if (std::string_view(e.key) >= lov) {
            c.pending_ = std::move(e);
            return c;
        }
    }
}

std::expected<std::optional<OkiRunReader::Entry>, OkiError>
OkiRunReader::find(std::span<const std::byte> key, OkiBlockCache* cache,
                   std::uint64_t cache_gen) const {
    // seek 定位（首个 ≥ key）→ 首行等值比对。run 内同 key 至多一行
    //（flush/compact/外排都做同 key 去重），单行不跨块——命中必在首行。
    auto c = seek_impl(key, cache, cache_gen);
    if (!c) return std::unexpected(c.error());
    Entry e;
    auto n = c->next(e);
    if (!n) return std::unexpected(n.error());
    if (*n && std::string_view(e.key) == sv(key)) {
        return std::optional<Entry>(std::move(e));
    }
    return std::optional<Entry>{};  // 缺席（或 bloom 假阳性）
}

// ---------------------------------------------------------------------------
// S36-1 SpillingRunBuilder（外排）
// ---------------------------------------------------------------------------

std::expected<SpillingRunBuilder, OkiError>
SpillingRunBuilder::create(std::string dir, std::uint64_t gen,
                           std::uint32_t version, std::size_t spill_bytes,
                           bool drop_tombstones, std::size_t block_target) {
    if (version != kRunVersion && version != kRunVersion2) {
        return std::unexpected(OkiError::kBadState);
    }
    SpillingRunBuilder b;
    b.dir_ = std::move(dir);
    b.gen_ = gen;
    b.version_ = version;
    b.spill_bytes_ = std::max<std::size_t>(spill_bytes, 4096);
    b.drop_tombstones_ = drop_tombstones;
    b.block_target_ = block_target;
    return b;
}

SpillingRunBuilder::~SpillingRunBuilder() {
    // 未 finish（错误路径/析构弃用）：best-effort 清 spill 残件。
    for (const auto& p : spill_paths_) {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(p), ec);
    }
}

std::string SpillingRunBuilder::spill_path(std::size_t n) const {
    return dir_ + "/kv.oki.spill-" + std::to_string(gen_) + "-" +
           std::to_string(n);
}

// (key asc, ord asc, seq asc) 排序后同 key 只留末行 = max (ord, seq)。
void SpillingRunBuilder::sort_dedup(std::vector<Row>& rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.ord != b.ord) return a.ord < b.ord;
        return a.seq < b.seq;
    });
    std::size_t w = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i + 1 < rows.size() && rows[i + 1].key == rows[i].key) continue;
        if (w != i) rows[w] = std::move(rows[i]);
        ++w;
    }
    rows.resize(w);
}

std::expected<void, OkiError> SpillingRunBuilder::spill() {
    if (buf_.empty()) return {};
    sort_dedup(buf_);
    auto w = OkiRunWriter::create(spill_path(spill_paths_.size()),
                                  block_target_, version_, buf_.size());
    if (!w) return std::unexpected(w.error());
    for (const auto& r : buf_) {
        auto a = w->add(std::span<const std::byte>(
                            reinterpret_cast<const std::byte*>(r.key.data()),
                            r.key.size()),
                        r.ord, r.tomb, r.has_loc ? &r.loc : nullptr);
        if (!a) return std::unexpected(a.error());
    }
    // spill 是临时文件：不 fsync 目录（崩溃丢 spill = 整次构建重来，
    // 派生缓存语义;最终 run 的 finish 才带 fsync_dir）。
    if (auto f = w->finish(/*fsync_dir=*/false); !f) {
        return std::unexpected(f.error());
    }
    spill_paths_.push_back(spill_path(spill_paths_.size()));
    buf_.clear();
    buf_bytes_ = 0;
    return {};
}

std::expected<void, OkiError>
SpillingRunBuilder::add(std::span<const std::byte> key, std::uint64_t ord,
                        bool tomb, const RowLoc* loc) {
    if (finished_) return std::unexpected(OkiError::kBadState);
    if (loc != nullptr && version_ != kRunVersion2) {
        return std::unexpected(OkiError::kBadState);
    }
    Row r;
    r.key.assign(reinterpret_cast<const char*>(key.data()), key.size());
    r.ord = ord;
    r.tomb = tomb;
    if (loc != nullptr) {
        r.has_loc = true;
        r.loc = *loc;
    }
    r.seq = seq_++;
    buf_bytes_ += r.key.size() + sizeof(Row);
    buf_.push_back(std::move(r));
    ++total_rows_;
    if (buf_bytes_ >= spill_bytes_) {
        if (auto sp = spill(); !sp) return sp;
    }
    return {};
}

std::expected<OkiRunWriter::Stats, OkiError>
SpillingRunBuilder::finish(bool fsync_dir) {
    if (finished_) return std::unexpected(OkiError::kBadState);
    finished_ = true;

    auto w = OkiRunWriter::create(mk_run_filename(dir_, gen_), block_target_,
                                  version_, total_rows_);
    if (!w) return std::unexpected(w.error());

    // 归并源：各 spill run（rank = spill 序号）+ 内存尾批（rank 最高）。
    // 同 key 胜出 = max (ord, rank)；rank 内已由 sort_dedup 保证唯一。
    sort_dedup(buf_);
    struct Source {
        std::optional<OkiRunReader> reader;      // 内存尾批则为空
        std::optional<OkiRunReader::Cursor> cur;
        const std::vector<Row>* mem = nullptr;
        std::size_t mem_pos = 0;
        OkiRunReader::Entry head;
        bool has_head = false;
        std::size_t rank = 0;
    };
    std::vector<Source> srcs;
    srcs.reserve(spill_paths_.size() + 1);  // 容量锁死：Cursor 持 Reader 裸指针
    for (std::size_t i = 0; i < spill_paths_.size(); ++i) {
        auto rd = OkiRunReader::open(spill_paths_[i]);
        if (!rd) return std::unexpected(rd.error());
        Source src;
        src.reader.emplace(*std::move(rd));
        src.rank = i;
        srcs.push_back(std::move(src));
        // Cursor 必须在 Source 落位之后再建——它持 reader 的裸指针，
        // 先建再 move 进 vector 会悬垂。
        srcs.back().cur.emplace(srcs.back().reader->begin());
    }
    {
        Source src;
        src.mem = &buf_;
        src.rank = spill_paths_.size();
        srcs.push_back(std::move(src));
    }
    auto advance = [&](Source& src) -> std::expected<void, OkiError> {
        if (src.mem != nullptr) {
            if (src.mem_pos < src.mem->size()) {
                const Row& r = (*src.mem)[src.mem_pos++];
                src.head.key = r.key;
                src.head.ord = r.ord;
                src.head.tomb = r.tomb;
                src.head.has_loc = r.has_loc;
                src.head.loc = r.loc;
                src.has_head = true;
            } else {
                src.has_head = false;
            }
            return {};
        }
        auto n = src.cur->next(src.head);
        if (!n) return std::unexpected(n.error());
        src.has_head = *n;
        return {};
    };
    for (auto& src : srcs) {
        if (auto a = advance(src); !a) return std::unexpected(a.error());
    }

    OkiRunWriter::Stats stats{};
    while (true) {
        // 取最小 key；同 key 各源比 (ord, rank)。
        const std::string* min_key = nullptr;
        for (const auto& src : srcs) {
            if (!src.has_head) continue;
            if (min_key == nullptr || src.head.key < *min_key) {
                min_key = &src.head.key;
            }
        }
        if (min_key == nullptr) break;
        const std::string key = *min_key;  // 拷贝：advance 会改写 head
        const OkiRunReader::Entry* win = nullptr;
        std::size_t win_rank = 0;
        for (const auto& src : srcs) {
            if (!src.has_head || src.head.key != key) continue;
            if (win == nullptr || src.head.ord > win->ord ||
                (src.head.ord == win->ord && src.rank > win_rank)) {
                win = &src.head;
                win_rank = src.rank;
            }
        }
        const bool emit = !(drop_tombstones_ && win->tomb);
        if (emit) {
            auto a = w->add(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(key.data()),
                                key.size()),
                            win->ord, win->tomb,
                            win->has_loc ? &win->loc : nullptr);
            if (!a) return std::unexpected(a.error());
        }
        for (auto& src : srcs) {
            if (src.has_head && src.head.key == key) {
                if (auto a = advance(src); !a) return std::unexpected(a.error());
            }
        }
    }
    auto f = w->finish(fsync_dir);
    if (!f) return std::unexpected(f.error());
    stats = *f;

    for (const auto& p : spill_paths_) {
        std::error_code ec;
        std::filesystem::remove(bitcask::detail::from_utf8(p), ec);
    }
    spill_paths_.clear();
    buf_.clear();
    buf_bytes_ = 0;
    return stats;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

bool write_manifest(std::string_view dir, const OkiManifest& m) {
    // S36-1 惰性版本：全 v1 run → BCOM v1（字节不变，老读端可读）；
    // 含 v2 run → BCOM v2（条目多 1 字节 format_ver；老读端拒收 → 重建自愈）。
    // S36-4：level_b（Level B 写者维护中——run loc 可信）→ BCOM v3
    //（v2 布局 + 头部 1 字节 flags；老读端拒收自愈）。
    bool any_v2 = m.level_b;  // v3 恒带 format_ver 字节
    for (const auto& r : m.runs) {
        if (r.format_ver != 1) any_v2 = true;
    }
    std::vector<std::byte> buf;
    buf.reserve(25 + m.runs.size() * 17 + 16);
    auto put_u32 = [&](std::uint32_t v) {
        std::byte b[4];
        le_store_u32(b, v);
        buf.insert(buf.end(), b, b + 4);
    };
    auto put_u64 = [&](std::uint64_t v) {
        std::byte b[8];
        le_store_u64(b, v);
        buf.insert(buf.end(), b, b + 8);
    };
    put_u32(kManifestMagic);
    put_u32(m.level_b ? kManifestVersion3
                      : (any_v2 ? kManifestVersion2 : kManifestVersion));
    if (m.level_b) buf.push_back(std::byte{0x01});  // flags: bit0 = level_b
    put_u32(static_cast<std::uint32_t>(m.runs.size()));
    for (const auto& r : m.runs) {
        put_u64(r.gen);
        put_u64(r.cover_ord);
        if (any_v2) buf.push_back(static_cast<std::byte>(r.format_ver));
    }
    put_u64(m.wm);
    const std::uint32_t crc = codec::crc32(buf);
    put_u32(crc);
    put_u32(kManifestMagic);
    return detail::atomic_write_bytes(mk_manifest_filename(dir), buf,
                                      /*fsync_dir=*/true);
}

std::optional<OkiManifest> read_manifest(std::string_view dir) {
    auto bytes = detail::read_file_bytes<>(mk_manifest_filename(dir));
    if (!bytes) return std::nullopt;
    const auto& b = *bytes;
    // 定长部分：magic+ver+count(12) + wm(8) + crc+magic(8)。
    if (b.size() < 28) return std::nullopt;
    if (le_load_u32(b.data()) != kManifestMagic ||
        le_load_u32(b.data() + b.size() - 4) != kManifestMagic) {
        return std::nullopt;
    }
    const std::uint32_t ver = le_load_u32(b.data() + 4);
    if (ver != kManifestVersion && ver != kManifestVersion2 &&
        ver != kManifestVersion3) {
        return std::nullopt;
    }
    const bool has_entry_ver = ver >= kManifestVersion2;
    const std::size_t entry_len = has_entry_ver ? 17 : 16;
    const std::uint32_t stored_crc = le_load_u32(b.data() + b.size() - 8);
    if (stored_crc !=
        codec::crc32(std::span<const std::byte>(b.data(), b.size() - 8))) {
        return std::nullopt;
    }
    OkiManifest m;
    std::size_t pos = 8;
    if (ver == kManifestVersion3) {  // S36-4：flags 字节
        if (b.size() < 29) return std::nullopt;
        const auto flags = static_cast<std::uint8_t>(b[pos]);
        if ((flags & ~0x01u) != 0) return std::nullopt;  // 未知位 fail-fast
        m.level_b = (flags & 0x01u) != 0;
        pos += 1;
    }
    const std::uint32_t count = le_load_u32(b.data() + pos);
    pos += 4;
    if (b.size() != pos + static_cast<std::size_t>(count) * entry_len + 8 + 8) {
        return std::nullopt;
    }
    m.runs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        OkiManifestEntry e;
        e.gen = le_load_u64(b.data() + pos);
        e.cover_ord = le_load_u64(b.data() + pos + 8);
        pos += 16;
        if (has_entry_ver) {
            e.format_ver = static_cast<std::uint8_t>(b[pos]);
            pos += 1;
            if (e.format_ver != 1 && e.format_ver != 2) return std::nullopt;
        }
        m.runs.push_back(e);
    }
    m.wm = le_load_u64(b.data() + pos);
    return m;
}

}  // namespace bitcask::oki
