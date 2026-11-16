// OKI 组合视图的统一 k 路归并游标（runs + memdelta）。
//
// 胜出规则全库唯一一份（原先在 compact_all_locked / MergeRunner /
// CaskIter / CaskRangeIter 各写一遍）：
//   - key 字典序升序；
//   - 同 key 取 max ord；
//   - 等 ord 时**来源序更高者胜**：来源序 = runs（gen 升序）→ 内存段（冻结段
//     按冻结先后，最后是 active 快照）。merge 搬迁行与被搬迁行同 ord，新位置
//     总在更高来源。
// 墓碑默认在归并层抵消（不产出）；include_tombs=true 时原样产出胜出墓碑。
//
// 线程模型：单线程使用；输入 Reader 与 delta 快照均为不可变共享对象，
// 游标持有其所有权，可安全跨越 run 替换/删除。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/oki_run.hpp"
#include "bitcask/oki_state.hpp"

namespace bitcask::oki {

class OkiMergeCursor {
public:
    using Row = OkiState::DeltaRow;

    using MemSource = std::shared_ptr<const std::vector<Row>>;

    // runs 必须按 gen 升序；mem 为内存来源（由旧到新），每个都须按 key 升序
    // 且每 key 至多一行，可含空指针。lo 非空时从 ≥ lo 起。
    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open(std::vector<std::shared_ptr<OkiRunReader>> runs,
         std::vector<MemSource> mem, std::span<const std::byte> lo = {}) {
        return open_impl(std::move(runs), {}, nullptr, std::move(mem), lo);
    }

    // 单内存来源的便捷形式。
    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open(std::vector<std::shared_ptr<OkiRunReader>> runs, MemSource delta,
         std::span<const std::byte> lo = {}) {
        std::vector<MemSource> mem;
        mem.push_back(std::move(delta));
        return open(std::move(runs), std::move(mem), lo);
    }

    // 读视图：runs → 冻结段（旧→新）→ active 快照。lo 非空时各 run 的起点
    // 定位块经视图携带的块缓存取（seek_cached）。
    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open(const OkiState::ReadView& view, std::span<const std::byte> lo = {}) {
        std::vector<MemSource> mem(view.frozen.begin(), view.frozen.end());
        mem.push_back(view.delta);
        return open_impl(view.runs, view.gens, view.cache, std::move(mem), lo);
    }

    OkiMergeCursor(OkiMergeCursor&&) noexcept = default;
    OkiMergeCursor& operator=(OkiMergeCursor&&) noexcept = default;
    OkiMergeCursor(const OkiMergeCursor&) = delete;
    OkiMergeCursor& operator=(const OkiMergeCursor&) = delete;

    // 产出下一个胜出行；true = 有行，false = 结束。run IO/损坏 → 错误。
    [[nodiscard]] std::expected<bool, OkiError>
    next(Row& out, bool include_tombs = false);

private:
    OkiMergeCursor() = default;
    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open_impl(std::vector<std::shared_ptr<OkiRunReader>> runs,
              std::vector<std::uint64_t> gens, std::shared_ptr<OkiBlockCache> cache,
              std::vector<MemSource> mem, std::span<const std::byte> lo);

    std::vector<std::shared_ptr<OkiRunReader>> runs_;
    std::vector<MemSource> mem_;
    std::vector<OkiRunReader::Cursor> cursors_;
    std::vector<std::optional<OkiRunReader::Entry>> heads_;
    std::vector<std::size_t> mem_pos_;
    std::shared_ptr<OkiBlockCache> cache_;  // 持有：游标内的 run 游标可能引用其中的块
    std::string key_;                       // 当前 key 的复用缓冲（免每行分配）
    std::vector<std::size_t> heap_;         // 来源编号的小顶堆（按当前 key）
    std::vector<std::size_t> same_;         // 本轮同 key 的来源
    bool heap_built_ = false;
    bool done_ = false;

    [[nodiscard]] std::string_view head_key(std::size_t src) const;
    void sift_down(std::size_t i);
    [[nodiscard]] bool source_live(std::size_t src) const;
};

}  // namespace bitcask::oki
