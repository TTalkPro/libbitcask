// OKI 组合视图的统一 k 路归并游标（runs + memdelta）。
//
// 胜出规则全库唯一一份（原先在 compact_all_locked / MergeRunner /
// CaskIter / CaskRangeIter 各写一遍）：
//   - key 字典序升序；
//   - 同 key 取 max ord；
//   - 等 ord 时**来源序更高者胜**：runs 按 gen 升序排列，memdelta 视作最高
//     来源（merge 搬迁行与被搬迁行同 ord，新位置总在更高来源）。
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
#include <vector>

#include "bitcask/oki_run.hpp"
#include "bitcask/oki_state.hpp"

namespace bitcask::oki {

class OkiMergeCursor {
public:
    using Row = OkiState::DeltaRow;

    // runs 必须按 gen 升序；delta 须按 key 升序且每 key 至多一行（即
    // make_read_view 产出的形态），可为空指针。lo 非空时从 ≥ lo 起。
    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open(std::vector<std::shared_ptr<OkiRunReader>> runs,
         std::shared_ptr<const std::vector<Row>> delta,
         std::span<const std::byte> lo = {});

    [[nodiscard]] static std::expected<OkiMergeCursor, OkiError>
    open(const OkiState::ReadView& view, std::span<const std::byte> lo = {}) {
        return open(view.runs, view.delta, lo);
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

    std::vector<std::shared_ptr<OkiRunReader>> runs_;
    std::shared_ptr<const std::vector<Row>> delta_;
    std::vector<OkiRunReader::Cursor> cursors_;
    std::vector<std::optional<OkiRunReader::Entry>> heads_;
    std::size_t delta_pos_ = 0;
    bool done_ = false;
};

}  // namespace bitcask::oki
