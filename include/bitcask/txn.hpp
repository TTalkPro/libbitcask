// txn.hpp — S34：应用层多键事务 helper（意图日志 + 前滚重放）。
//
// 模式原理：doc/multikey-txn-zh.md；实现设计：doc/multikey-txn-impl-design-zh.md。
// 建在 Cask 公共 API（put/put_batch/remove/sync/make_range_iter）之上，
// 零盘上格式改动。提供崩溃原子性（A）与持久性（D）；**不提供**隔离性（I）
// 与 CAS——事务中间态对并发读者可见（模式文档 §4）。
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

    // 启动恢复：按提交序前滚重放全部 pending 事务并清理意图，返回重放条数。
    // 契约：open 后、任何业务写之前调用；不得与 commit 并发。正常关闭的库
    // 扫描开销 O(0)。意图 blob 解码失败（record CRC 已过仍解不开 = 逻辑
    // 损坏）→ kBadCrc 停止，不静默跳过。
    [[nodiscard]] std::expected<std::size_t, CaskFault> recover();

    // 原子提交一批操作：① put 意图 → ②[sync] → ③ apply → ④ remove 意图。
    // 校验（违反 → kInvalidOption，零副作用）：ops 非空；key 非空且互不
    // 重复（PUT/REMOVE 拆分重放后重复 key 次序不可定义）；key 不以
    // "_txn:" 开头。③/④ 失败 → 意图保留，下次 recover() 前滚重试。
    // 并发：键集不相交的并发 commit 安全；键集重叠无隔离/定序保证，
    // 需应用层串行化。
    [[nodiscard]] std::expected<void, CaskFault> commit(std::span<const TxnOp> ops);

    // 运维巡检：枚举当前 pending 事务（不重放、不清理）。
    [[nodiscard]] std::expected<std::vector<PendingTxn>, CaskFault> pending_txns();

private:
    // ③ 与 recover 共用的重放路径：PUT 集一次 put_batch + REMOVE 逐条
    // remove（BatchItem 无墓碑形态）。两条路径必须同一实现（模式文档 §6）。
    [[nodiscard]] std::expected<void, CaskFault> apply(std::span<const TxnOp> ops);

    Cask* cask_;
    TxnSyncPolicy sync_;
};

}  // namespace bitcask
