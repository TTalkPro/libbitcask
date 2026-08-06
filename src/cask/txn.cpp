// txn.cpp — S34/S35：TxnCask 多键事务 helper 实现。
// 设计：doc/multikey-txn-impl-design-zh.md + doc/atomic-batch-design-zh.md。
// B2（2026-08-06）：方案 B 的意图重放整体删除——意图日志只存在于
// dc81bbc..S35 之间的**未发布**构建（TxnCask 本身即未发布版本的新增），
// 没有任何已发布版本写过意图 blob，无兼容对象。recover()/pending_txns()
// 保留签名（C API 稳定面）恒返空；罕见的开发期残留（"_txn:" 前缀 key）
// 可经普通 KV API 手工清理。

#include "bitcask/txn.hpp"

#include <unordered_set>

#include "cask_internal.hpp"  // err / bytes_to_view

namespace bitcask {

std::expected<void, CaskFault> TxnCask::apply(std::span<const TxnOp> ops) {
    // S35：一次引擎原子批（doc/atomic-batch-design-zh.md）——跨崩溃
    // all-or-nothing 由引擎批头保证，PUT/REMOVE 同批依序 apply。
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
    // 意图日志(方案 B 的 ①②④)退役,写放大 2-3× → 1×。
    // TxnSyncPolicy 只管持久性(原子性与其正交)。
    if (auto r = apply(ops); !r) return r;
    if (sync_ == TxnSyncPolicy::kSyncOnCommit)
        if (auto r = cask_->sync(); !r) return r;
    return {};
}

std::expected<std::size_t, CaskFault> TxnCask::recover() {
    // B2：意图重放已删除（无已发布版本写过意图，见文件头）——恒 0。
    return 0;
}

std::expected<std::vector<PendingTxn>, CaskFault> TxnCask::pending_txns() {
    // B2：同上——恒空。
    return std::vector<PendingTxn>{};
}

}  // namespace bitcask
