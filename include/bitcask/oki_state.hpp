// OKI 运行态（S33-4）：memdelta + run 集合 + manifest 水位。
// 设计：doc/ordered-key-index-design-zh.md §2/§5。
//
// 归属：挂 KeyDir（进程内随 KeyDirRegistry 共享，生命周期随 KeyDir——
// CaskIter 的 keydir_pin_ 语义自动顺延）；目录路径由 Cask 注入（KeyDir
// 不知道自己的目录，与 kv.keydir.ckpt 同模式）。
//
// === 挂钩与水位模型 ===
// 写挂钩收敛在 KeyDir::put / KeyDir::remove 的单一咽喉点（本类的 append
// 由 keydir.cpp 调用，Cask 各写路径零改动——设计文档 §8 难点 1 的对策）。
// append 以 `ord > wm` 为门：
//   - 运行期写（ord 恒新）→ 全收；
//   - 恢复 tail 重放（fold/hint 皆携真实 ord）→ 自动只收 wm 之后的行；
//   - merge 搬迁 put（old_file_id≠0，keydir.cpp 侧过滤）与链重放 remove
//     （ord=0）→ 不收。
// wm = **排他上界**：尚未被 runs 覆盖的最小 ord（首个合法 LSN 是 0——
// alloc_ord 从 0 起——含上界语义会漏掉 ord 0，故全链路统一排他）。正确性
// 依赖「append 按 ord 升序到达」——运行期由 Cask write_mu_ 串行保证；
// 恢复期并行 fold 乱序 append 无碍（恢复期间不 flush，flush 前全量到齐）。
//
// 快照崩溃窗口：keydir 快照使 fold 跳过字节水位前的行——这些行若未进
// runs 即是洞。约定 flush 恒在 write_keydir_snapshot **之后**（同站点搭
// 车），正常路径 wm ≥ 快照覆盖；崩溃丢 flush 时由 open 端检查
// `wm < 快照 next_ord - 1` → 整体重建兜底（cask_recovery）。
//
// === 线程模型 ===
// append/delta 统计：内部 mu_（运行期单写者无争用；恢复期并行 fold 短锁）。
// flush/load/rebuild：flush_mu_ 串行（close/checkpoint/merge 收尾间互斥）；
// flush 期间 memdelta 换出，append 不被 IO 阻塞。

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/oki_run.hpp"

namespace bitcask::oki {

class OkiState {
public:
    struct DeltaRow {
        std::string key;
        std::uint64_t ord = 0;
        bool tomb = false;
    };

    // 写挂钩（keydir.cpp 的 put/remove 咽喉点调用）。ord ≤ wm 的行直接
    // 丢弃（见文件头水位模型）。
    void append(std::string_view key, std::uint64_t ord, bool tomb);

    // 写路径探询：memdelta 达阈值（行数/字节）？无锁近似读。
    [[nodiscard]] bool should_flush() const noexcept {
        return flush_hint_.load(std::memory_order_relaxed);
    }

    // open 早期载入 manifest。缺失/损坏 → 未加载态（caller 依恢复形态决定
    // 重建）。幂等；与 flush/rebuild 互斥。
    void load(std::string_view dir);
    [[nodiscard]] bool loaded() const noexcept {
        return loaded_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t wm() const noexcept {
        return wm_.load(std::memory_order_acquire);
    }

    // flush：memdelta 换出 → 按 key 排序 + 同 key 取 max-ord 去重（墓碑
    // 保留为 tomb 行）→ 写新 run → manifest 提交（唯一 commit point）→
    // 推进 wm。空 delta 且已加载 → no-op true。IO 失败 → 行放回、状态不
    // 变、返回 false（下次重试）。未加载态拒绝（先 load/rebuild）。
    [[nodiscard]] bool flush(std::string_view dir);

    // 全量重建：rows 由 caller 从 keydir 收集（活 key + ord；墓碑不需要
    // ——单一全量 run 的缺席即语义）。排序写单 run、manifest 只含它、
    // wm=cover_ord，删除全部旧 run 文件，清空 memdelta（其内容已被快照
    // 覆盖）。与 flush 互斥。
    // S33-6：rows 为空（空库 / 全删）时**不落空 run**——manifest 记 0 个
    // run + wm=cover_ord，语义等价且不留空文件与常驻 fd。
    [[nodiscard]] bool rebuild(std::string_view dir,
                               std::vector<DeltaRow>&& rows,
                               std::uint64_t cover_ord);

    // S33-5：range 读者视图——runs 的共享 Reader（不可变，多线程各持
    // Cursor 并发读）+ memdelta 快照（已按 key 排序、同 key max-ord 去重，
    // 墓碑保留）。**弱一致**：视图是创建时刻的近似（per-key 语义，非 fold
    // 快照）。OKI 未加载 → nullopt（RO 打开无 OKI 的目录等）。
    // shared_ptr 使在途视图安全跨越 rebuild 的旧 run 删除（POSIX unlink
    // 后已开 fd 仍可读到 close）。
    struct ReadView {
        std::vector<std::shared_ptr<OkiRunReader>> runs;
        std::vector<DeltaRow> delta;
    };
    [[nodiscard]] std::optional<ReadView> make_read_view() const;

    // 诊断。
    [[nodiscard]] std::size_t delta_rows() const;
    [[nodiscard]] std::size_t delta_bytes() const;
    [[nodiscard]] std::size_t run_count() const;

    // flush 阈值（写路径 should_flush 探询；超限由 Cask 在写路径同步
    // flush——罕见且有界）。
    static constexpr std::size_t kFlushRowLimit  = 1u << 20;       // 1M 行
    static constexpr std::size_t kFlushByteLimit = 64u << 20;      // 64 MiB

    // S33-6：run 归并阈值（设计 §5.2「极简两层」）。flush 提交后 run 数超此
    // 值即把**全部** run 归并成一个。不做 leveled compaction——OKI 条目不含
    // value，全归并 1 亿 key 也就 ~1-2GB 顺序 IO。
    // 不归并的后果（实测）：run 数 = flush 次数（close/merge 收尾/checkpoint
    // 各一次）线性增长 ⟹ 每 run 一个常驻 fd + open 期全文件 CRC + range 多
    // 一路归并，且墓碑行永远回收不掉。
    static constexpr std::size_t kCompactRunLimit = 8;

private:
    // S33-6：把**全部** run 归并成单个新 run（持 flush_mu_ 调用）。
    // run 数 ≤1 时 no-op。best-effort：失败原状不变（下次 flush 再试）。
    //
    // **墓碑真正丢弃**——仅在「全归并」下成立：同 key 的 put 行与 tomb 行必
    // 定同在本次归并里，max-ord 胜出者若是 tomb 则两行一起丢，绝不会留下被
    // 抵消掉的陈旧 put 行。若将来改成部分归并，这条**必须**收回（否则旧 run
    // 里的 put 行会因抵消它的 tomb 消失而"复活"）。
    // 完整性不变量（OKI key 集 ⊇ keydir 活 key 集）不受影响：被丢弃的 key
    // 若之后重新 put，新行 ord > wm 走 memdelta（读视图含之），崩溃时也由
    // tail 重放补回——归并不动 wm。
    [[nodiscard]] bool compact_all_locked(std::string_view dir);

    void update_flush_hint_locked() noexcept {
        flush_hint_.store(
            delta_.size() >= kFlushRowLimit || delta_bytes_ >= kFlushByteLimit,
            std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t next_gen_locked() const noexcept;

    mutable std::mutex mu_;        // delta_ / delta_bytes_
    std::vector<DeltaRow> delta_;
    std::size_t delta_bytes_ = 0;

    mutable std::mutex flush_mu_;  // flush/load/rebuild 串行；manifest_/readers_ 归其保护
    OkiManifest manifest_;
    // manifest_.runs 一一对应的常驻 Reader（load 全量 CRC 校验后开；
    // flush/rebuild 产出新 run 时随 manifest 提交同步维护）。
    std::vector<std::pair<std::uint64_t, std::shared_ptr<OkiRunReader>>>
        readers_;

    std::atomic<bool> loaded_{false};
    std::atomic<std::uint64_t> wm_{0};
    std::atomic<bool> flush_hint_{false};
};

}  // namespace bitcask::oki
