// bitcask/index_dispatch.hpp — 宿主索引分发（S45-4，设计
// designs/index-pipeline-unordered-design-zh.md §4.1/§4.2/§4.7）。
//
// IndexPool 只管线程与队列；「一条索引任务怎样落到 docmap 与各插件」是宿主
// 语义，集中在本类（Cask 装配使用；契约测试直接复用，守护的是真实语义而非
// 一份拷贝）。
//
// shard_apply（分片线程，同 key 串行、不同 key 并发）：
//   Add    : 规则 R 裁决（过期丢弃）→ docmap.put_doc（LSN 守卫）+ set_meta
//            → 分片插件 prepare+on_put → 有串行插件接收则转串行道
//   Delete : docmap 只删更旧版本（捕 prior）→ 分片插件 on_delete
//            → 有串行插件接收则转串行道（携带 prior；不论是否删了行）
// serial_apply（串行道线程）：串行插件 prepare+on_put / on_delete。
//
// 线程安全：配置（set_plugins / docmap / is_current / on_error）只在注册车道
// 之前设置，之后只读。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "bitcask/index.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/thread_pool.hpp"

namespace bitcask {

class IndexDispatcher {
public:
    // 按注册序拆分分片/串行插件（各自保持注册序）。
    void set_plugins(std::span<plugin::CaskPlugin* const> all) {
        sharded_.clear();
        serial_.clear();
        for (auto* p : all) {
            if (p->concurrency() == plugin::CaskPlugin::Concurrency::kSharded) {
                sharded_.push_back(p);
            } else {
                serial_.push_back(p);
            }
        }
    }
    void clear() {
        sharded_.clear();
        serial_.clear();
    }
    [[nodiscard]] bool has_serial() const noexcept { return !serial_.empty(); }

    // 宿主 docmap（可空：无 docmap 的测试宿主）。
    index::Index* docmap = nullptr;
    // 规则 R：写任务 (key, ord) 是否仍为该 key 的最新版本（可空 = 恒真）。
    std::function<bool(std::string_view, std::uint64_t)> is_current;
    // prepare 抛异常时的错误上报（可空）。
    std::function<void()> on_error;

    [[nodiscard]] std::uint64_t stale_dropped() const noexcept {
        return stale_dropped_.load(std::memory_order_relaxed);
    }

    ApplyResult shard_apply(IndexTask& t, unsigned shard) {
        if (t.op == IndexOp::Add) {
            if (is_current && !is_current(t.key(), t.ord)) {
                stale_dropped_.fetch_add(1, std::memory_order_relaxed);
                return ApplyResult::kDone;
            }
            if (docmap) {
                // put_doc 的 LSN 守卫拒收（更旧 / 同版本已在）：在线写 = 过期或
                // 重复投递，不广播插件；**恢复重放**仍须送达——各组件 ckpt 覆盖
                // 水位可能不同（某组件未载入 / 损坏回退），docmap 已有不代表
                // 插件已有，插件按 ord 自行判重。
                const bool accepted = docmap->put_doc(
                    t.key(), t.ord,
                    index::DocSlot{index::DocLoc{.offset   = t.offset,
                                                 .file_id  = t.file_id,
                                                 .total_sz = t.total_sz},
                                   t.tstamp, /*doc_len=*/0});
                if (!accepted && !t.replay) {
                    stale_dropped_.fetch_add(1, std::memory_order_relaxed);
                    return ApplyResult::kDone;
                }
                if (accepted && !t.meta.empty()) docmap->set_meta(t.ord, t.meta);
            }
            const plugin::DocView doc = make_doc_view(t);
            plugin::PutEvent ev = make_put_event(t, &doc);
            ev.shard = shard;
            for (auto* p : sharded_) {
                if (!p->accepts_put(ev)) continue;
                p->on_put(ev, prepare(*p, ev));
            }
            for (auto* p : serial_) {
                if (p->accepts_put(ev)) return ApplyResult::kForwardSerial;
            }
            return ApplyResult::kDone;
        }
        if (t.op == IndexOp::Delete) {
            // 只删更旧版本（墓碑之后的写已先行应用 ⇒ 本删除过期，prior 保持
            // 哨兵，插件跳过）。get→remove 之间无同 key 交错（同 key 串行）。
            std::uint64_t prior = plugin::kNoPriorOrd;
            if (docmap) {
                if (auto slot = docmap->get(t.key());
                    slot && slot->ord < t.ord && docmap->remove(t.key(), t.ord)) {
                    prior = slot->ord;
                }
            } else {
                prior = t.prior_ord;
            }
            t.prior_ord = prior;
            plugin::DeleteEvent de{t.ord, t.key(), prior};
            de.shard = shard;
            for (auto* p : sharded_) {
                if (p->accepts_delete()) p->on_delete(de);
            }
            // 串行插件一律送达（不论 docmap 是否删了行）：key+LSN 裁决需要
            // 墓碑——「删除先于其更旧的写到达」时 docmap 无行可删，但插件仍须
            // 记下墓碑挡住随后的旧写（向量插件，S45）。
            for (auto* p : serial_) {
                if (p->accepts_delete()) return ApplyResult::kForwardSerial;
            }
            return ApplyResult::kDone;
        }
        return ApplyResult::kDone;
    }

    void serial_apply(IndexTask& t) {
        if (t.op == IndexOp::Add) {
            const plugin::DocView doc = make_doc_view(t);
            const plugin::PutEvent ev = make_put_event(t, &doc);
            for (auto* p : serial_) {
                if (!p->accepts_put(ev)) continue;
                p->on_put(ev, prepare(*p, ev));
            }
        } else if (t.op == IndexOp::Delete) {
            const plugin::DeleteEvent de{t.ord, t.key(), t.prior_ord};
            for (auto* p : serial_) {
                if (p->accepts_delete()) p->on_delete(de);
            }
        }
    }

private:
    // prepare 异常：上报 + 空产物（插件自行降级），事件照常送达。
    plugin::PreparedPtr prepare(plugin::CaskPlugin& p, const plugin::PutEvent& ev) {
        if (!p.wants_prepare()) return nullptr;
        try {
            return p.prepare(ev);
        } catch (...) {
            if (on_error) on_error();
            return nullptr;
        }
    }

    std::vector<plugin::CaskPlugin*> sharded_;
    std::vector<plugin::CaskPlugin*> serial_;
    std::atomic<std::uint64_t> stale_dropped_{0};
};

}  // namespace bitcask
