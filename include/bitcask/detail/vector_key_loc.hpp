// bitcask/detail/vector_key_loc.hpp — 向量插件的 key + LSN 定位表（S45）。
//
// 与 TextPlugin 的 key_to_location_ 同一裁决模型（设计
// designs/index-pipeline-unordered-design-zh.md §4.1、§10.2）：
//
//   on_put(key, L)    : 表中该 key 已有 LSN ≥ L（更新的写 / 墓碑 / 同版本重放）
//                       ⇒ 跳过；否则插入并登记 key → L。
//   on_delete(key, T) : 表中已有 LSN ≥ T ⇒ 过期跳过；否则登记墓碑 key → T
//                       （墓碑保留：防止更旧的写乱序后到「复活」）。
//
// 乱序插入与恢复重放的幂等都由此承担——HNSW 图本身不再有任何水位门，
// 节点 ord 也不要求随节点 id 递增。
//
// 持久化（S45-A）：定位表随向量检查点落盘——base 写快照（kVecKeyLoc，不含
// 墓碑），delta 写窗口内新登记的条目（kVecKeyLocDelta，即 journal）。重开时
// 直接由 base + 链重建，**不依赖 docmap**（与 BM25 段自带 key/LSN 对称）。
//
// 回退重建（旧检查点无 key 段 / 链中某个 delta 缺 key 段）：图 / 段里只存
// ord——经宿主 docmap 的 ord_to_ext 反查当前存活版本。
//
// 墓碑修剪：覆盖水位 wm 之下的墓碑可删——ord < wm 的写都已完成，不可能再有
// 比墓碑更旧的写到达（prune_tombs_below，flush 时调用）。
//
// 线程模型：单写者（串行道线程 on_put/on_delete；open 时的 rebuild 与 flush
// 时的修剪在本库应用暂停期执行）。查询路径不读本表。

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <utility>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bitcask/doc_table.hpp"
#include "bitcask/string_hash.hpp"

namespace bitcask::vec::detail {

class VectorKeyLocations {
public:
    // 写：返回是否应插入（是则已登记 key → ord，并按窗口记入 journal）。
    bool admit_put(std::string_view key, std::uint64_t ord) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            map_.emplace(std::string(key), Loc{ord, false});
        } else {
            if (it->second.ord >= ord) return false;
            it->second = Loc{ord, false};
        }
        if (ord >= journal_window_) journal_.emplace_back(ord, std::string(key));
        return true;
    }

    // ---- 持久化（S45-A）----
    // journal 窗口与插件的 delta 插入日志同门（ord ≥ 窗口才记）。
    void set_journal_window(std::uint64_t wm) noexcept { journal_window_ = wm; }
    void clear_journal() { journal_.clear(); }
    [[nodiscard]] bool journal_empty() const noexcept { return journal_.empty(); }
    // kVecKeyLoc：当前非墓碑条目快照。
    void encode_snapshot(std::vector<std::byte>& out) const {
        std::uint64_t n = 0;
        for (const auto& [k, loc] : map_) n += loc.tomb ? 0 : 1;
        put_u64(out, n);
        for (const auto& [k, loc] : map_) {
            if (!loc.tomb) put_entry(out, loc.ord, k);
        }
    }
    // kVecKeyLocDelta：窗口内新登记的条目。
    void encode_journal(std::vector<std::byte>& out) const {
        put_u64(out, journal_.size());
        for (const auto& [ord, k] : journal_) put_entry(out, ord, k);
    }
    // 两段同一布局。per_entry(ord, key)；结构损坏返回 false。
    template <typename Fn>
    [[nodiscard]] static bool parse(std::span<const std::byte> payload, Fn&& per_entry) {
        const std::byte* p = payload.data();
        const std::byte* end = p + payload.size();
        auto rd = [&](void* dst, std::size_t n) {
            if (static_cast<std::size_t>(end - p) < n) return false;
            std::memcpy(dst, p, n);
            p += n;
            return true;
        };
        std::uint64_t cnt = 0;
        if (!rd(&cnt, 8)) return false;
        for (std::uint64_t i = 0; i < cnt; ++i) {
            std::uint64_t ord = 0;
            std::uint32_t klen = 0;
            if (!rd(&ord, 8) || !rd(&klen, 4)) return false;
            if (static_cast<std::size_t>(end - p) < klen) return false;
            per_entry(ord, std::string_view(reinterpret_cast<const char*>(p), klen));
            p += klen;
        }
        return p == end;
    }
    // 载入一个 kVecKeyLoc / kVecKeyLocDelta 段（合并进当前表）。
    [[nodiscard]] bool load_section(std::span<const std::byte> payload) {
        return parse(payload, [this](std::uint64_t ord, std::string_view k) {
            load_entry(k, ord);
        });
    }
    // 载入一条（base / delta 段）：取 max(ord)，不记 journal。
    void load_entry(std::string_view key, std::uint64_t ord) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            map_.emplace(std::string(key), Loc{ord, false});
        } else if (it->second.ord < ord) {
            it->second = Loc{ord, false};
        }
    }

    // 删：登记墓碑（已有更新版本则过期跳过）。
    void on_delete(std::string_view key, std::uint64_t ord) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            map_.emplace(std::string(key), Loc{ord, true});
            return;
        }
        if (it->second.ord >= ord) return;
        it->second = Loc{ord, true};
    }

    // 由节点 ord 枚举 + docmap 反查重建（只登记当前存活版本）。
    void rebuild(const std::function<void(const std::function<void(std::uint64_t)>&)>&
                     for_each_ord,
                 const bm25::DocTable& docs) {
        map_.clear();
        for_each_ord([&](std::uint64_t ord) {
            auto key = docs.ord_to_ext(ord);
            if (!key) return;
            auto it = map_.find(*key);
            if (it == map_.end()) {
                map_.emplace(std::move(*key), Loc{ord, false});
            } else if (it->second.ord < ord) {
                it->second = Loc{ord, false};
            }
        });
    }

    void prune_tombs_below(std::uint64_t wm) {
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->second.tomb && it->second.ord < wm) {
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        map_.clear();
        journal_.clear();
    }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    // 测试 / 诊断：key 当前登记的 ord（墓碑亦返回其 ord）；无登记 = -1。
    [[nodiscard]] std::uint64_t ord_of(std::string_view key) const {
        auto it = map_.find(key);
        return it == map_.end() ? ~std::uint64_t{0} : it->second.ord;
    }

private:
    struct Loc {
        std::uint64_t ord = 0;
        bool tomb = false;
    };
    static void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
        const auto* b = reinterpret_cast<const std::byte*>(&v);
        out.insert(out.end(), b, b + 8);
    }
    static void put_entry(std::vector<std::byte>& out, std::uint64_t ord,
                          std::string_view key) {
        put_u64(out, ord);
        const auto klen = static_cast<std::uint32_t>(key.size());
        const auto* b = reinterpret_cast<const std::byte*>(&klen);
        out.insert(out.end(), b, b + 4);
        const auto* kb = reinterpret_cast<const std::byte*>(key.data());
        out.insert(out.end(), kb, kb + key.size());
    }
    std::unordered_map<std::string, Loc, StringHash, std::equal_to<>> map_;
    std::vector<std::pair<std::uint64_t, std::string>> journal_;
    std::uint64_t journal_window_ = 0;
};

}  // namespace bitcask::vec::detail
