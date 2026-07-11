// bitcask/term_snapshot_cache.hpp — 查询线程私有的 term→FlatPostings 快照缓存
// (S29-6B,设计见 docs/design/s29-6b-inverted-term-cache.md)。
//
// 目标:BOW 查询命中路径**零共享写**——免每词一次 TBB 桶锁 RMW + shared_ptr
// 引用计数 RMW(热词并发查询下这 4 次/词的共享 cacheline 弹跳是 BOW 扩展性
// 瓶颈,BM_Inverted_QueryThroughputBOW 1→8 线程聚合 QPS 零扩展的根因)。
//
// === 协议 ===
// 失效:per-shard generation(InvertedIndex::Shard::gen_,写者变更 posting 后
//   fetch_add release)。命中判据 = (index_id, term) 相等 且 entry.gen ==
//   当前 shard gen(caller 先 acquire load 后传入)。
// 钉住:begin_query() 划定查询边界;probe/upsert 标记 use_seq,淘汰跳过
//   use_seq == 当前查询的槽——同一查询内后插入的词不得覆写先前命中、仍被
//   评分视图引用的条目。窗口全钉住 ⇒ upsert 返回 nullptr,caller 回退私有槽。
// 生命周期:index_id 进程级单调、永不复用 ⇒ 指向已析构索引的残留条目永不
//   假命中,无需析构清扫。
//
// 线程安全:实例经 tls_instance() 取,线程私有,无同步;唯一共享触点是
// caller 读的 shard gen 原子。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/inverted.hpp"

namespace bitcask::bm25 {

class TermSnapshotCache {
public:
    // 256 槽 × 全量条目 ≤12KB(BOW 标量路径按定义 <1024 行)≈ 3MB/线程最坏,
    // 典型远小;vector 容量随覆写复用,稳态零分配。
    static constexpr std::size_t kSlots = 256;  // pow2
    static constexpr std::size_t kProbe = 8;

    struct Entry {
        std::uint64_t index_id = 0;
        std::uint64_t gen = 0;
        std::uint64_t df = 0;
        std::uint64_t use_seq = 0;
        bool occupied = false;
        // true = fp 权威(含「缺席」负缓存:fp 空 + df==0);
        // false = df-only(doc_freq 对大 term 产出,不搬行)。
        bool has_rows = false;
        std::string term;
        FlatPostings fp;
    };

    // 查询边界:此后 probe/upsert 触碰的槽在本查询内不被淘汰。
    void begin_query() noexcept { ++query_seq_; }

    // 命中(id+term+gen 全等)返回条目并钉住;否则 nullptr。
    [[nodiscard]] Entry* probe(std::uint64_t index_id, std::string_view term,
                               std::uint64_t gen) noexcept {
        const std::size_t h = slot_hash(index_id, term);
        for (std::size_t p = 0; p < kProbe; ++p) {
            Entry& e = slots_[(h + p) & (kSlots - 1)];
            if (e.occupied && e.index_id == index_id && e.term == term) {
                if (e.gen != gen) return nullptr;  // 同 key 陈旧:等 upsert 刷新
                e.use_seq = query_seq_;
                return &e;
            }
        }
        return nullptr;
    }

    // 取一个可写槽(优先同 key 刷新 → 空槽 → 窗口内未钉住的最老槽)。
    // caller 负责随后填 gen/df/fp/has_rows。窗口全钉住返回 nullptr。
    [[nodiscard]] Entry* upsert(std::uint64_t index_id, std::string_view term,
                                std::uint64_t gen) {
        const std::size_t h = slot_hash(index_id, term);
        Entry* victim = nullptr;
        for (std::size_t p = 0; p < kProbe; ++p) {
            Entry& e = slots_[(h + p) & (kSlots - 1)];
            if (e.occupied && e.index_id == index_id && e.term == term) {
                victim = &e;  // 同 key 刷新(陈旧条目原地覆写)
                break;
            }
            if (!e.occupied) {
                if (victim == nullptr) victim = &e;
                continue;
            }
            if (e.use_seq == query_seq_) continue;  // 本查询钉住,不可覆写
            if (victim == nullptr || (victim->occupied &&
                                      e.use_seq < victim->use_seq)) {
                victim = &e;
            }
        }
        if (victim == nullptr) return nullptr;
        victim->occupied = true;
        victim->index_id = index_id;
        victim->gen = gen;
        victim->df = 0;
        victim->has_rows = false;
        victim->use_seq = query_seq_;
        victim->term.assign(term);  // 复用容量
        return victim;
    }

    // 线程私有单例。
    [[nodiscard]] static TermSnapshotCache& tls_instance() {
        static thread_local TermSnapshotCache c;
        return c;
    }

private:
    TermSnapshotCache() : slots_(kSlots) {}

    [[nodiscard]] static std::size_t slot_hash(std::uint64_t index_id,
                                               std::string_view term) noexcept {
        // id 混入 term hash(简单 xor-fold 足够:槽表小、探测窗口兜碰撞)。
        std::size_t h = std::hash<std::string_view>{}(term);
        h ^= (index_id * 0x9E3779B97F4A7C15ull) >> 3;
        return h;
    }

    std::uint64_t query_seq_ = 0;
    std::vector<Entry> slots_;
};

}  // namespace bitcask::bm25
