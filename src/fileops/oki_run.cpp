#include "bitcask/oki_run.hpp"

#include <algorithm>
#include <cstring>

#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"   // crc32 / crc32_update
#include "bitcask/vbyte.hpp"   // vbyte_encode / vbyte_read_checked

namespace bitcask::oki {

namespace {

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
OkiRunWriter::create(std::string path, std::size_t block_target) {
    detail::AtomicFileWriter w(std::move(path), ".oki.tmp");
    if (!w) return std::unexpected(OkiError::kIo);
    return OkiRunWriter(std::move(w), std::max<std::size_t>(block_target, 64));
}

bool OkiRunWriter::flush_block() {
    if (blk_.empty()) return true;
    if (!header_written_) {
        // 惰性写 header（首块落盘前）：magic + version。
        std::byte hdr[kRunHeaderSize];
        le_store_u32(hdr, kRunMagic);
        le_store_u32(hdr + 4, kRunVersion);
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
    return true;
}

std::expected<void, OkiError>
OkiRunWriter::add(std::span<const std::byte> key, std::uint64_t ord,
                  bool tomb) {
    if (finished_) return std::unexpected(OkiError::kBadState);
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
    blk_.push_back(static_cast<std::byte>(tomb ? kFlagTomb : 0));

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
        le_store_u32(hdr + 4, kRunVersion);
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

    // trailer：[entry_count u64][index_off u64] 计入 CRC，随后 [crc][magic]。
    std::byte t1[16];
    le_store_u64(t1, entries_);
    le_store_u64(t1 + 8, index_off);
    if (!fwrite_crc(w_.get(), std::span<const std::byte>(t1, sizeof t1),
                    crc_)) {
        return std::unexpected(OkiError::kIo);
    }
    std::byte t2[8];
    le_store_u32(t2, crc_);
    le_store_u32(t2 + 4, kRunTrailerMagic);
    if (std::fwrite(t2, 1, sizeof t2, w_.get()) != sizeof t2) {
        return std::unexpected(OkiError::kIo);
    }
    const std::uint64_t total = file_off_ + 16 + 8;
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
    // 最小合法文件 = header(8) + 空索引(4) + trailer(24)。
    if (total < kRunHeaderSize + 4 + kRunTrailerSize) {
        return std::unexpected(OkiError::kCorrupt);
    }

    // header + trailer 定宽字段。
    std::byte hdr[kRunHeaderSize];
    {
        auto n = r.file_.pread_into(0, std::span(hdr, sizeof hdr));
        if (!n) return std::unexpected(OkiError::kIo);
        if (*n != sizeof hdr) return std::unexpected(OkiError::kCorrupt);
    }
    if (le_load_u32(hdr) != kRunMagic ||
        le_load_u32(hdr + 4) != kRunVersion) {
        return std::unexpected(OkiError::kCorrupt);
    }
    std::byte tr[kRunTrailerSize];
    {
        auto n = r.file_.pread_into(total - kRunTrailerSize,
                                    std::span(tr, sizeof tr));
        if (!n) return std::unexpected(OkiError::kIo);
        if (*n != sizeof tr) return std::unexpected(OkiError::kCorrupt);
    }
    if (le_load_u32(tr + 20) != kRunTrailerMagic) {
        return std::unexpected(OkiError::kCorrupt);
    }
    r.entry_count_ = le_load_u64(tr);
    const std::uint64_t index_off = le_load_u64(tr + 8);
    const std::uint32_t expected_crc = le_load_u32(tr + 16);
    if (index_off < kRunHeaderSize ||
        index_off > total - kRunTrailerSize - 4) {
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

    // 稀疏索引区 [index_off, total - trailer)。
    {
        const std::size_t idx_len =
            static_cast<std::size_t>(total - kRunTrailerSize - index_off);
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
    return r;
}

std::expected<bool, OkiError>
OkiRunReader::Cursor::load_block(std::size_t bi) {
    const auto& b = r_->blocks_[bi];
    const std::size_t len = static_cast<std::size_t>(b.end - b.off);
    blk_.resize(len);
    auto got = r_->file_.pread_into(b.off, std::span(blk_.data(), len));
    if (!got) return std::unexpected(OkiError::kIo);
    if (*got != len) return std::unexpected(OkiError::kCorrupt);
    pos_ = 0;
    prev_key_.clear();
    prev_ord_ = 0;
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
        if (!block_loaded_ || pos_ >= blk_.size()) {
            if (bi_ >= r_->blocks_.size()) return false;  // run 末尾
            auto l = load_block(bi_++);
            if (!l) return std::unexpected(l.error());
        }
        // 解一条（CRC 已过——结构错误仍防御性 fail-fast）。
        std::span<const std::byte> b(blk_);
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
        if ((flags & ~kKnownFlagsMask) != 0) {
            // Level B 扩展位：v1 读端 fail-fast，绝不静默跳。
            return std::unexpected(OkiError::kCorrupt);
        }
        prev_ord_ += od->first;  // 回绕还原
        pos_ = pos;

        out.key.assign(prev_key_);
        out.ord = prev_ord_;
        out.tomb = (flags & kFlagTomb) != 0;
        return true;
    }
}

std::expected<OkiRunReader::Cursor, OkiError>
OkiRunReader::seek(std::span<const std::byte> lo) const {
    Cursor c(this);
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

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

bool write_manifest(std::string_view dir, const OkiManifest& m) {
    std::vector<std::byte> buf;
    buf.reserve(24 + m.runs.size() * 16 + 16);
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
    put_u32(kManifestVersion);
    put_u32(static_cast<std::uint32_t>(m.runs.size()));
    for (const auto& r : m.runs) {
        put_u64(r.gen);
        put_u64(r.cover_ord);
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
        le_load_u32(b.data() + 4) != kManifestVersion ||
        le_load_u32(b.data() + b.size() - 4) != kManifestMagic) {
        return std::nullopt;
    }
    const std::uint32_t stored_crc = le_load_u32(b.data() + b.size() - 8);
    if (stored_crc !=
        codec::crc32(std::span<const std::byte>(b.data(), b.size() - 8))) {
        return std::nullopt;
    }
    const std::uint32_t count = le_load_u32(b.data() + 8);
    if (b.size() != 12 + static_cast<std::size_t>(count) * 16 + 8 + 8) {
        return std::nullopt;
    }
    OkiManifest m;
    m.runs.reserve(count);
    std::size_t pos = 12;
    for (std::uint32_t i = 0; i < count; ++i) {
        OkiManifestEntry e;
        e.gen = le_load_u64(b.data() + pos);
        e.cover_ord = le_load_u64(b.data() + pos + 8);
        pos += 16;
        m.runs.push_back(e);
    }
    m.wm = le_load_u64(b.data() + pos);
    return m;
}

}  // namespace bitcask::oki
