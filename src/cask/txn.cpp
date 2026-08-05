// txn.cpp — S34：TxnCask 多键事务 helper 实现。
// 设计：doc/multikey-txn-impl-design-zh.md。

#include "bitcask/txn.hpp"

#include <cstring>
#include <unordered_set>

#include "cask_internal.hpp"  // err / bytes_to_view

namespace bitcask {

namespace {

constexpr std::uint8_t kBlobVersion = 1;
// [u8 ver][u64 created_at_us][u32 count]
constexpr std::size_t kBlobHeaderSize = 1 + 8 + 4;
// [u8 type][u32 klen][u32 vlen]
constexpr std::size_t kOpHeaderSize = 1 + 4 + 4;

std::uint32_t get_u32le(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | static_cast<std::uint32_t>(p[i]);
    return v;
}

std::uint64_t get_u64le(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    return v;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// 意图 blob v1（方案 B 遗留格式）：S35 起 commit 不再写意图——本解码仅
// 服务 recover() 对旧目录遗留 pending 的前滚重放（设计
// doc/atomic-batch-design-zh.md §4）。布局见 multikey-txn-impl-design-zh §3。
struct DecodedIntent {
    std::uint64_t created_at_us = 0;
    std::vector<TxnOp> ops;  // span 借 blob 存储——blob 必须比本结构活得久
};

std::expected<DecodedIntent, CaskFault>
decode_ops(std::span<const std::byte> blob) {
    const auto malformed = [](std::string_view what) {
        return std::unexpected(
            err(CaskError::kBadCrc,
                std::string("txn intent blob malformed: ") + std::string(what)));
    };
    if (blob.size() < kBlobHeaderSize) return malformed("short header");
    if (static_cast<std::uint8_t>(blob[0]) != kBlobVersion)
        return malformed("unknown version");
    DecodedIntent out;
    out.created_at_us = get_u64le(blob.data() + 1);
    const std::uint32_t count = get_u32le(blob.data() + 9);
    if (count == 0) return malformed("empty op list");
    std::size_t pos = kBlobHeaderSize;
    out.ops.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (blob.size() - pos < kOpHeaderSize) return malformed("short op header");
        const auto raw_type = static_cast<std::uint8_t>(blob[pos]);
        if (raw_type > static_cast<std::uint8_t>(TxnOp::Type::kRemove))
            return malformed("unknown op type");
        const std::uint32_t klen = get_u32le(blob.data() + pos + 1);
        const std::uint32_t vlen = get_u32le(blob.data() + pos + 5);
        pos += kOpHeaderSize;
        if (klen == 0) return malformed("empty key");
        if (blob.size() - pos < static_cast<std::size_t>(klen) + vlen)
            return malformed("short op payload");
        TxnOp op;
        op.type = static_cast<TxnOp::Type>(raw_type);
        op.key = blob.subspan(pos, klen);
        op.value = blob.subspan(pos + klen, vlen);
        if (op.type == TxnOp::Type::kRemove && vlen != 0)
            return malformed("remove carries value");
        pos += static_cast<std::size_t>(klen) + vlen;
        out.ops.push_back(op);
    }
    if (pos != blob.size()) return malformed("trailing bytes");
    return out;
}

}  // namespace

std::expected<void, CaskFault> TxnCask::apply(std::span<const TxnOp> ops) {
    // S35：一次引擎原子批（doc/atomic-batch-design-zh.md）——跨崩溃
    // all-or-nothing 由引擎批头保证，PUT/REMOVE 同批依序 apply。
    // commit 与 recover（意图重放）共用本路径。
    std::vector<Cask::BatchOp> batch;
    batch.reserve(ops.size());
    for (const auto& op : ops) {
        Cask::BatchOp b;
        b.type = op.type == TxnOp::Type::kPut ? Cask::BatchOp::Type::kPut
                                              : Cask::BatchOp::Type::kRemove;
        b.key = op.key;
        b.value = op.value;
        batch.push_back(b);
    }
    return cask_->put_batch_atomic(batch);
}

std::expected<void, CaskFault> TxnCask::commit(std::span<const TxnOp> ops) {
    if (ops.empty())
        return std::unexpected(err(CaskError::kInvalidOption, "txn: empty ops"));
    std::unordered_set<std::string_view> seen;
    seen.reserve(ops.size());
    for (const auto& op : ops) {
        if (op.key.empty())
            return std::unexpected(err(CaskError::kInvalidOption, "txn: empty key"));
        const std::string_view k = bytes_to_view(op.key);
        if (k.starts_with(kTxnPrefix))
            return std::unexpected(err(CaskError::kInvalidOption,
                                       "txn: key in reserved _txn: namespace"));
        if (!seen.insert(k).second)
            return std::unexpected(err(CaskError::kInvalidOption,
                                       "txn: duplicate key in ops"));
    }

    // S35：原子性下沉引擎——一次 put_batch_atomic 即崩溃后 all-or-nothing，
    // 意图日志(方案 B 的 ①②④)从热路径退役,写放大 2-3× → 1×。
    // TxnSyncPolicy 只管持久性(原子性与其正交)。
    if (auto r = apply(ops); !r) return r;
    if (sync_ == TxnSyncPolicy::kSyncOnCommit)
        if (auto r = cask_->sync(); !r) return r;
    return {};
}

std::expected<std::size_t, CaskFault> TxnCask::recover() {
    // 先全量收集再前滚——不在弱一致 range 迭代器活跃期间写库（设计 §5）。
    struct Pending {
        std::vector<std::byte> txn_key;
        std::vector<std::byte> blob;
    };
    std::vector<Pending> pending;
    {
        RangeOptions ro;
        ro.lo = bytes(kTxnPrefix);
        const std::string hi = "_txn;";  // ';' == ':' + 1，半开上界
        ro.hi = bytes(hi);
        auto it = cask_->make_range_iter(ro);
        if (!it) return std::unexpected(it.error());
        for (;;) {
            auto e = (*it)->next();
            if (!e) return std::unexpected(e.error());
            if (!e->has_value()) break;
            pending.push_back(
                {std::move((*e)->key), std::move((*e)->value)});
        }
    }
    // 迭代器输出 key 字典序 = seq 序 = 提交序（设计 §4）。
    for (const auto& p : pending) {
        auto intent = decode_ops(p.blob);
        if (!intent) return std::unexpected(intent.error());
        if (auto r = apply(intent->ops); !r) return std::unexpected(r.error());
        if (auto r = cask_->remove(p.txn_key); !r)
            return std::unexpected(r.error());
    }
    return pending.size();
}

std::expected<std::vector<PendingTxn>, CaskFault> TxnCask::pending_txns() {
    std::vector<PendingTxn> out;
    RangeOptions ro;
    ro.lo = bytes(kTxnPrefix);
    const std::string hi = "_txn;";
    ro.hi = bytes(hi);
    auto it = cask_->make_range_iter(ro);
    if (!it) return std::unexpected(it.error());
    for (;;) {
        auto e = (*it)->next();
        if (!e) return std::unexpected(e.error());
        if (!e->has_value()) break;
        auto intent = decode_ops((*e)->value);
        if (!intent) return std::unexpected(intent.error());
        out.push_back({.txn_key = std::string(bytes_to_view((*e)->key)),
                       .created_at_us = intent->created_at_us,
                       .op_count = intent->ops.size()});
    }
    return out;
}

}  // namespace bitcask
