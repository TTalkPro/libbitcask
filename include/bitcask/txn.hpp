// txn.hpp — S34/S35：多键事务 helper。
//
// S35 起提交路径 = 引擎原子批（Cask::put_batch_atomic，方案 C，设计
// doc/atomic-batch-design-zh.md）——跨崩溃 all-or-nothing 由引擎批头保证，
// 无意图日志写放大。recover() 保留方案 B 的意图重放，兼容旧目录遗留的
// "_txn:" pending（模式原理 doc/multikey-txn-zh.md）。
// 提供崩溃原子性（A）与持久性（D）；**不提供**隔离性（I）与 CAS——
// 事务中间态对并发读者可见（模式文档 §4）。
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/cask.hpp"

namespace bitcask {

// 事务操作。kRemove 忽略 value。
struct TxnOp {
    enum class Type : std::uint8_t { kPut = 0, kRemove = 1 };
    Type type = Type::kPut;
    std::span<const std::byte> key;
    std::span<const std::byte> value{};
};

// 提交点 fsync 策略（模式文档 §2.4）。
enum class TxnSyncPolicy : std::uint8_t {
    // 意图日志 put 之后显式 Cask::sync()——防掉电/内核 panic（默认）。
    kSyncOnCommit = 0,
    // 不显式 sync：依赖 CaskOptions::o_sync / sync_every_n，或只防进程崩溃
    //（意图仍在 page cache 即可被同机重启读到）。
    kNone = 1,
};

// 悬挂事务巡检条目（pending_txns）。
struct PendingTxn {
    std::string txn_key;           // "_txn:{seq:016x}-{rand:08x}"
    std::uint64_t created_at_us;   // 意图 blob 内的创建时刻（unix µs）
    std::size_t op_count;
};

// 多键事务门面。非拥有地包装 Cask*（生命周期须覆盖本对象）；自身无状态
//（txn 序号分配器为进程级静态），可随建随用。要求 cask 以 read_write 打开
//（否则写路径原样返回 kReadOnly）。
class TxnCask {
public:
    static constexpr std::string_view kTxnPrefix = "_txn:";

    explicit TxnCask(Cask* cask,
                     TxnSyncPolicy sync = TxnSyncPolicy::kSyncOnCommit)
        : cask_(cask), sync_(sync) {}

    // 启动恢复（legacy，方案 B 兼容）：按提交序前滚重放旧目录遗留的
    // "_txn:" 意图并清理，返回重放条数。S35 起 commit 不再产生意图——
    // 从未用过方案 B 版本的目录恒返回 0（扫描 O(0)）。
    // 契约：open 后、任何业务写之前调用；不得与 commit 并发。意图 blob
    // 解码失败（record CRC 已过仍解不开 = 逻辑损坏）→ kBadCrc 停止。
    [[nodiscard]] std::expected<std::size_t, CaskFault> recover();

    // 原子提交一批操作：一次 Cask::put_batch_atomic（S35 引擎原子批）——
    // 崩溃/掉电后整批要么全生效要么全不生效，无恢复重放依赖。
    // 校验（违反 → kInvalidOption，零副作用）：ops 非空；key 非空且互不
    // 重复；key 不以 "_txn:" 开头（保留给 legacy 意图命名空间）。
    // 注意：首次 commit 把目录 meta 懒升级为 v6（旧于 5.1.0 的读端
    // 拒开该目录，契约见 Cask::put_batch_atomic）。
    // 并发：键集不相交的并发 commit 安全；键集重叠无隔离/定序保证，
    // 需应用层串行化。
    [[nodiscard]] std::expected<void, CaskFault> commit(std::span<const TxnOp> ops);

    // 运维巡检：枚举 legacy pending 事务（不重放、不清理）。S35 后正常
    // 恒空——仅方案 B 时期目录的崩溃遗留会非空。
    [[nodiscard]] std::expected<std::vector<PendingTxn>, CaskFault> pending_txns();

private:
    // commit 与 recover（意图重放）共用：一次 put_batch_atomic。
    [[nodiscard]] std::expected<void, CaskFault> apply(std::span<const TxnOp> ops);

    Cask* cask_;
    TxnSyncPolicy sync_;
};

}  // namespace bitcask
