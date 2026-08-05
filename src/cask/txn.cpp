// txn.cpp — S34：TxnCask 多键事务 helper 实现。
// 设计：doc/multikey-txn-impl-design-zh.md。

#include "bitcask/txn.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <unordered_set>

#include "cask_internal.hpp"  // err / bytes_to_view

namespace bitcask {

namespace {

constexpr std::uint8_t kBlobVersion = 1;
// [u8 ver][u64 created_at_us][u32 count]
constexpr std::size_t kBlobHeaderSize = 1 + 8 + 4;
// [u8 type][u32 klen][u32 vlen]
constexpr std::size_t kOpHeaderSize = 1 + 4 + 4;

void put_u32le(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

void put_u64le(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

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

std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 进程级单调 seq：max(now_us, last+1)。进程内严格单调 ⇒ 定宽 hex 的
// 字典序 = 提交序（重放定序，设计 §4）；跨进程由 write.lock 排他 +
// 墙钟保证实用单调。
std::uint64_t alloc_seq() {
    static std::atomic<std::uint64_t> last{0};
    std::uint64_t prev = last.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t next = std::max(now_us(), prev + 1);
        if (last.compare_exchange_weak(prev, next, std::memory_order_relaxed))
            return next;
    }
}

// 进程随机 session id：防墙钟异常下跨进程 seq 碰撞。
std::uint32_t session_rand() {
    static const std::uint32_t r = [] {
        std::random_device rd;
        return static_cast<std::uint32_t>(rd());
    }();
    return r;
}

std::string make_txn_key() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*s%016llx-%08x",
                  static_cast<int>(TxnCask::kTxnPrefix.size()),
                  TxnCask::kTxnPrefix.data(),
                  static_cast<unsigned long long>(alloc_seq()),
                  session_rand());
    return buf;
}

std::span<const std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// 意图 blob v1 编码（设计 §3；布局为稳定格式，txn_test 手写编码器对拍）。
std::vector<std::byte> encode_ops(std::span<const TxnOp> ops,
                                  std::uint64_t created_at_us) {
    std::size_t total = kBlobHeaderSize;
    for (const auto& op : ops) {
        total += kOpHeaderSize + op.key.size();
        if (op.type == TxnOp::Type::kPut) total += op.value.size();
    }
    std::vector<std::byte> out;
    out.reserve(total);
    out.push_back(static_cast<std::byte>(kBlobVersion));
    put_u64le(out, created_at_us);
    put_u32le(out, static_cast<std::uint32_t>(ops.size()));
    for (const auto& op : ops) {
        out.push_back(static_cast<std::byte>(op.type));
        put_u32le(out, static_cast<std::uint32_t>(op.key.size()));
        const bool is_put = op.type == TxnOp::Type::kPut;
        put_u32le(out, is_put ? static_cast<std::uint32_t>(op.value.size()) : 0);
        out.insert(out.end(), op.key.begin(), op.key.end());
        if (is_put) out.insert(out.end(), op.value.begin(), op.value.end());
    }
    return out;
}

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
    // PUT 集一次 put_batch（本进程内 all-or-nothing 可见）+ REMOVE 逐条。
    // key 互不重复（commit 校验/decode 后仍由 commit 侧保证）⇒ 拆分次序无关。
    std::vector<Cask::BatchItem> puts;
    puts.reserve(ops.size());
    for (const auto& op : ops)
        if (op.type == TxnOp::Type::kPut)
            puts.push_back({.key = op.key, .value = op.value});
    if (!puts.empty())
        if (auto r = cask_->put_batch(puts); !r) return r;
    for (const auto& op : ops)
        if (op.type == TxnOp::Type::kRemove)
            if (auto r = cask_->remove(op.key); !r) return r;
    return {};
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

    const std::string txn_key = make_txn_key();
    const std::vector<std::byte> blob = encode_ops(ops, now_us());

    if (auto r = cask_->put(bytes(txn_key), blob); !r) return r;   // ① 意图
    if (sync_ == TxnSyncPolicy::kSyncOnCommit)                     // ② 落盘
        if (auto r = cask_->sync(); !r) return r;
    if (auto r = apply(ops); !r) return r;                         // ③ 数据
    return cask_->remove(bytes(txn_key));                          // ④ 完成
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
