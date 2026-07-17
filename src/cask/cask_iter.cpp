// cask_iter.cpp — CaskIter（fold 迭代器）实现。S21-3 B1：从 cask.cpp 纯物理
// 平移拆出（函数体不变），先例同 meta_file.cpp / legacy_ckpt.cpp。
#include "bitcask/cask.hpp"

#include <cstring>

#include "bitcask/codec.hpp"           // next()：解码 DocValue 取 text 段
#include "bitcask/format.hpp"          // RecordType（磁盘墓碑判定）
#include "bitcask/detail/scanner.hpp"  // pin_files()：扫描目录 pin 只读句柄

#include "cask_internal.hpp"  // err / io_fault / now_sec_default

namespace bitcask {

// =============================================================================
// CaskIter：fold 迭代器实现
//
// 把 keydir::IterHandle 包一层，加上「按 file_id 拿 DataFile 句柄、
// 按 (offset, total_sz) pread 出 value」的能力。see_tombstones=true 时
// 墓碑也作为带 is_tombstone 标志的 Entry 上交。
// =============================================================================

CaskIter::~CaskIter() noexcept { release(); }

std::expected<keydir::StartIterResult, CaskFault>
CaskIter::start(int maxage, int maxputs, std::uint64_t now_sec,
                bool see_tombstones, std::span<const std::byte> key_prefix) {
    // S11-W3：parent Cask 已 close → keydir_ 已释放,fail-fast 而非解引用空指针。
    if (parent_->is_closed()) {
        return std::unexpected(err(CaskError::kClosed, "cask is closed"));
    }
    if (iter_ && iter_->is_iterating()) {
        return std::unexpected(err(CaskError::kIo, "iter already started"));
    }
    // X1:先 pin KeyDir（shared_ptr 复制），再建 IterHandle——保证
    // close() 在迭代器存活期间 reset keydir_ 也不会让 iter_ 的裸指针悬空。
    keydir_pin_ = parent_->keydir_;
    iter_ = parent_->keydir_->make_iter();
    auto r = iter_->start(now_sec, maxage, maxputs);
    see_tombstones_ = see_tombstones;
    // S13-D4：前缀拷入自有存储（span 借 caller 缓冲，start 返回后可能失效）。
    key_prefix_.clear();
    if (!key_prefix.empty()) {
        key_prefix_.assign(reinterpret_cast<const char*>(key_prefix.data()),
                           key_prefix.size());
    }
    // S13：真正开始迭代后（kOk），pin 当前目录下所有 data file 的只读句柄快照，
    // 让并发 merge 在本次 fold 期间 unlink 旧文件不影响后续 next() 的 pread。
    // kOutOfDate 时 caller 会重试，不在此处 pin。
    if (r == keydir::StartIterResult::kOk) {
        pin_files();
    }
    return r;  // kOk or kOutOfDate (kAlreadyIterating handled above)
}

// 扫描目录、open 全部 data file 的只读句柄并 pin 住（S13）。best-effort：
// 扫描或单个 open 失败时跳过该文件，next() 对其退回 parent_->read_file（仍可
// 工作，只是少了「文件被 merge 删除」的保护）。active write file 不 pin——
// merge 从不合并 active 文件，且 parent_ 已为它持有 RW 句柄。
void CaskIter::pin_files() {
    pinned_files_.clear();
    auto scan = fileops::scan_dir(parent_->dirname_);
    if (!scan) return;
    // S13-F3：active_data_（shared_ptr 对象本体）与并发 roll/close 的
    // reset/赋值构成数据竞争——对同一 shared_ptr 对象的并发读写非线程安全，
    // 必须在 read_cache_mu_ 下拍快照（写点均持其独占锁）。快照略陈旧无害：
    // pin 是 best-effort，漏 pin 的文件由 next() 退回 parent_->read_file。
    bool has_active = false;
    std::uint32_t active_fid = 0;
    {
        std::shared_lock lk(parent_->read_cache_mu_);
        has_active = static_cast<bool>(parent_->active_data_);
        active_fid =
            parent_->active_file_id_.load(std::memory_order_relaxed);
    }
    for (const auto& e : *scan) {
        const auto fid = static_cast<std::uint32_t>(e.tstamp);
        if (has_active && fid == active_fid) continue;
        // P6:迭代器 pin 句柄经 read() 读(非 read_mmap),且只用于本次 fold——
        // 不 mmap(避免无谓映射;fd 开着,read() pread 正常)。
        auto df = fileops::DataFile::open(e.data_path,
                                          fileops::DataFile::Mode::kRead,
                                          /*sync*/ false, /*mmap_enabled*/ false);
        if (!df) continue;
        pinned_files_.emplace(
            fid, std::make_unique<fileops::DataFile>(std::move(*df)));
    }
}

std::expected<std::optional<CaskIter::Entry>, CaskFault> CaskIter::next() {
    if (!iter_ || !iter_->is_iterating()) return std::optional<Entry>{};

    const auto expiry = parent_->opts_.expiry_secs;
    const auto now = (expiry > 0) ? now_sec_default() : 0;

    // 跳过过期 entry；墓碑的处理由 see_tombstones_ 决定：
    //   false（默认）— 墓碑直接跳过（legacy fold/3 行为）
    //   true         — 墓碑也作为带 is_tombstone=true 的 Entry 上交。
    //                  sibling 墓碑没有真实的磁盘 record，我们合成一条
    //                  v0 marker 当 value 上交，避免给出空 value 让 caller
    //                  困惑。
    while (true) {
        auto proxy = iter_->next(/*include_tombstones=*/ see_tombstones_);
        if (!proxy) return std::optional<Entry>{};

        // 64 位求和防 u32 wrap（同 get 路径 / merge_policy cutoff）。
        if (expiry > 0 &&
            static_cast<std::uint64_t>(proxy->tstamp) + expiry <= now) {
            continue;  // expired; skip
        }

        // S13-D4：前缀过滤——proxy 层跳过（不 pread value、不跨界拷 key）。
        if (!key_prefix_.empty() &&
            (proxy->key.size() < key_prefix_.size() ||
             std::memcmp(proxy->key.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            continue;
        }

        // sibling 墓碑只活在 keydir 里（file_id 是 sentinel，磁盘上没
        // 对应 record）。跳过文件读，合成一条空 value 墓碑给 caller。
        if (proxy->is_tombstone) {
            Entry e;
            e.key.assign(reinterpret_cast<const std::byte*>(proxy->key.data()),
                          reinterpret_cast<const std::byte*>(proxy->key.data()) +
                          proxy->key.size());
            e.value.clear();
            e.value.shrink_to_fit();
            e.tstamp       = proxy->tstamp;
            e.file_id      = proxy->file_id;
            e.offset       = proxy->offset;
            e.total_sz     = proxy->total_sz;
            e.is_tombstone = true;
            e.ord          = proxy->ord;
            return std::optional<Entry>{std::move(e)};
        }

        // S13：优先用 fold 启动时 pin 的句柄（merge 可能已 unlink 该文件，但
        // 已 open 的 fd 仍可读）；未 pin 的（active 文件 / fold 后新建的文件）
        // 退回共享 read_file——这些文件不会在本次 fold 期间被 merge 删除。
        fileops::DataFile* df = nullptr;
        std::shared_ptr<fileops::DataFile> shared_df;  // pin 共享句柄到本次读结束
        if (auto pit = pinned_files_.find(proxy->file_id); pit != pinned_files_.end()) {
            df = pit->second.get();
        } else {
            shared_df = parent_->read_file(proxy->file_id);
            df = shared_df.get();
        }
        if (!df) {
            return std::unexpected(err(CaskError::kIo,
                "open read file_id=" + std::to_string(proxy->file_id)));
        }
        auto rec = df->read(proxy->offset, proxy->total_sz);
        if (!rec) {
            switch (rec.error().kind) {
                case fileops::DataFileError::kBadCrc:
                    return std::unexpected(err(CaskError::kBadCrc));
                case fileops::DataFileError::kIo:
                    return std::unexpected(io_fault(rec.error().errnum));
                default:
                    return std::unexpected(err(CaskError::kIo, "read"));
            }
        }
        // 即使 keydir 没把它标成墓碑，磁盘 record 自己也可能是墓碑——
        // keydir 指向的就是一条带墓碑类型的 value。这种「磁盘墓碑」要
        // 跟「sibling 墓碑」区分对待（前者有真实磁盘字节，后者纯内存）。
        const bool value_is_tomb = rec->type == format::RecordType::kTombstone;
        if (value_is_tomb && !see_tombstones_) continue;

        Entry e;
        e.key          = std::move(rec->key);
        // 磁盘上 kDoc value 是 DocValue 编码（text 段 = 原始 value），与
        // Cask::get 一致地解码取 text 段，避免把 doc 头/长度前缀漏给 caller。
        // 墓碑 record 不是 DocValue 编码，按原始字节上交（通常为空/marker）。
        if (value_is_tomb) {
            e.value = std::move(rec->value);
        } else {
            auto dv = codec::decode_doc_value(std::span<const std::byte>(rec->value));
            if (!dv) return std::unexpected(err(CaskError::kIo, "corrupt DocValue"));
            // S13-D5：per-key TTL——过期记录跳过（与整库 expiry_secs 同语义）。
            if (dv->expiry_at != 0 && dv->expiry_at <= now_sec_default()) {
                continue;
            }
            e.value.assign(dv->text.begin(), dv->text.end());
        }
        e.tstamp       = rec->tstamp;
        e.file_id      = proxy->file_id;
        e.offset       = proxy->offset;
        e.total_sz     = proxy->total_sz;
        e.is_tombstone = value_is_tomb;
        e.ord          = rec->ord;
        return std::optional<Entry>{std::move(e)};
    }
}

std::expected<std::vector<CaskIter::Entry>, CaskFault>
CaskIter::next_batch(std::size_t max_n) {
    std::vector<Entry> batch;
    batch.reserve(max_n);
    for (std::size_t i = 0; i < max_n; ++i) {
        auto r = next();
        if (!r) return std::unexpected(r.error());
        if (!r->has_value()) break;  // EOI
        batch.push_back(std::move(**r));
    }
    return batch;
}

// S11-W4：排干 live key（仅 key，不读 value）。走 keydir proxy（含 key + 定位，
// 无 pread），比逐条 next()（每条 pread+decode）廉价得多 → 供 parallel_scan 取
// 快照后并行读值。消费迭代器（推进 cursor 到尾）。
CaskIter::FlatKeys CaskIter::drain_live_keys() {
    FlatKeys out;
    if (!iter_ || !iter_->is_iterating()) return out;
    out.offs.push_back(0);
    // include_tombstones=false → keydir 层跳过墓碑，只给 live key。
    while (auto proxy = iter_->next(/*include_tombstones=*/false)) {
        // S13-D4：前缀过滤（与 next() 一致）。
        if (!key_prefix_.empty() &&
            (proxy->key.size() < key_prefix_.size() ||
             std::memcmp(proxy->key.data(), key_prefix_.data(),
                         key_prefix_.size()) != 0)) {
            continue;
        }
        const auto* p = reinterpret_cast<const std::byte*>(proxy->key.data());
        out.buf.insert(out.buf.end(), p, p + proxy->key.size());
        out.offs.push_back(out.buf.size());
    }
    return out;
}

void CaskIter::release() noexcept {
    if (iter_) {
        iter_->release();
        iter_.reset();
    }
    // S13：关掉 pin 的只读 fd；若文件已被 merge unlink，此刻 inode 才真正释放。
    pinned_files_.clear();
    // X1:最后释放 KeyDir pin——必在 iter_ 析构/release 之后，确保
    // BarrierGuard 锁的 KeyDir mutex 在锁期间始终存活。
    keydir_pin_.reset();
}

}  // namespace bitcask
