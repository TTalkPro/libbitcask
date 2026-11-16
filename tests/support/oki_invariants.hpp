// S44-0：OKI 不变量守门助手（测试用）。
//
// I1（gen / 来源序）有两种消费方式：点查 `locate`（首命中即权威）与归并
// 游标 `OkiMergeCursor`（全来源裁决）。任何破坏 I1 的改动都会让两者在同一
// 视图上分叉。本助手对视图里每个 key 比对二者结果。
//
// 注意：调用期间若有并发写入，active 可能新增行，结果可能合法地不同；只在
// 无并发写入时调用。后台归并可以在跑——归并前后语义等价（全归并会把墓碑
// 变成缺席，二者都表示「不存在」，按等价处理）。
#pragma once

#include <string>

#include <gtest/gtest.h>

#include <bitcask/oki_merge_cursor.hpp>
#include <bitcask/oki_state.hpp>

namespace bitcask_test {

inline ::testing::AssertionResult locate_matches_cursor(
    const bitcask::oki::OkiState& st) {
    using bitcask::oki::OkiState;
    auto v = st.make_read_view();
    if (!v) return ::testing::AssertionFailure() << "no read view";
    auto mc = bitcask::oki::OkiMergeCursor::open(*v);
    if (!mc) return ::testing::AssertionFailure() << "cursor open failed";
    OkiState::DeltaRow row;
    std::size_t n = 0;
    while (true) {
        auto r = mc->next(row, /*include_tombs=*/true);
        if (!r) return ::testing::AssertionFailure() << "cursor read failed";
        if (!*r) break;
        ++n;
        const auto loc = st.locate(row.key);
        // 墓碑 ≡ 缺席：后台全归并可能在取视图与点查之间提交并回收墓碑行。
        if (row.tomb && loc.status == OkiState::LocateStatus::kMiss) continue;
        if (loc.status != OkiState::LocateStatus::kHit) {
            auto f = ::testing::AssertionFailure();
            f << "key " << row.key << ": cursor has row (ord " << row.ord
              << " tomb " << row.tomb << "), locate status "
              << static_cast<int>(loc.status) << "; sources:";
            for (std::size_t i = 0; i < v->runs.size(); ++i) {
                auto e = v->runs[i]->find(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(row.key.data()), row.key.size()));
                if (e && e->has_value()) f << " run" << i << "(ord " << (*e)->ord << " tomb " << (*e)->tomb << ")";
            }
            for (std::size_t i = 0; i < v->frozen.size(); ++i) {
                for (const auto& fr : *v->frozen[i]) {
                    if (fr.key == row.key) f << " frozen" << i << "(ord " << fr.ord << " tomb " << fr.tomb << " seq " << fr.seq << ")";
                }
            }
            for (const auto& dr : *v->delta) {
                if (dr.key == row.key) f << " delta(ord " << dr.ord << " tomb " << dr.tomb << " seq " << dr.seq << ")";
            }
            return f;
        }
        if (loc.ord != row.ord || loc.tomb != row.tomb) {
            return ::testing::AssertionFailure()
                   << "key " << row.key << ": cursor (ord " << row.ord
                   << ", tomb " << row.tomb << ") vs locate (ord " << loc.ord
                   << ", tomb " << loc.tomb << ")";
        }
        if (!row.tomb &&
            (loc.loc.file_id != row.loc.file_id || loc.loc.offset != row.loc.offset)) {
            return ::testing::AssertionFailure()
                   << "key " << row.key << ": loc mismatch cursor file "
                   << row.loc.file_id << " vs locate file " << loc.loc.file_id;
        }
    }
    return ::testing::AssertionSuccess() << n << " keys checked";
}

}  // namespace bitcask_test
