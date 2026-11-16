#include "bitcask/detail/oki_compaction_policy.hpp"

#include <algorithm>

namespace bitcask::oki {

CompactionChoice choose_compaction(std::span<const std::uint64_t> sizes,
                                   std::size_t floor,
                                   const CompactionPolicyOptions& opts,
                                   bool force_full) noexcept {
    CompactionChoice none;
    const std::size_t n = sizes.size();
    if (floor >= n) return none;

    const auto full_from_zero = [&]() {
        CompactionChoice c;
        c.merge = true;
        c.begin = 0;
        c.full = true;
        return c;
    };

    if (floor == 0 && force_full && n >= 1) return full_from_zero();

    const bool small_base =
        floor == 0 && sizes[0] < opts.small_base_rows;
    if (opts.mode == CompactionMode::kFull || small_base) {
        // 旧策略 / 小库：run 数越阈值才全归并。年轻道（floor > 0）在此模式下
        // 不做事——主道的全归并会把一切收拢。
        if (floor == 0 && n > opts.full_limit) return full_from_zero();
        return none;
    }

    // 几何后缀：从最新往前累加。
    std::size_t j = n - 1;
    double acc = static_cast<double>(sizes[n - 1]);
    while (j > floor &&
           static_cast<double>(sizes[j - 1]) <= opts.k * acc) {
        --j;
        acc += static_cast<double>(sizes[j]);
    }
    if (floor == 0) {
        // 倒挂修复（只在主道）：几何规则保持「由老到新递减」，但年轻道的选段受
        // floor 截断而非大小条件，可能产出比其前面的 run 更大的「新而大」run。
        // 几何遍历从最新往前走会在它处停下，其前更小的老 run 从此并不进去（实测
        // 叠加硬上限后，每次 flush 都重写那个大 run）。发现明显倒挂（某老 run 的
        // 2 倍仍小于其后某个更新 run）时，从最早的倒挂点起做一次修复性归并。
        std::uint64_t newer_max = 0;
        std::size_t inv = n;
        for (std::size_t i = n; i-- > 0;) {
            if (i + 1 < n && sizes[i] * 2 < newer_max) inv = i;
            newer_max = std::max(newer_max, sizes[i]);
        }
        if (inv < j) {
            j = inv;
            acc = 0;
            for (std::size_t i = j; i < n; ++i) acc += static_cast<double>(sizes[i]);
        }
        // 硬上限：归并后 run 数 = j + 1，须 ≤ max_runs。**只作用于主道**：主道重写
        // 大 run 期间其输入仍在册，总 run 数必然越过上限；若年轻道也被上限驱动，
        // 每来一次 flush 它都会把自己积累的全部 run 重写一遍（实测写放大从 ~4 涨到
        // 28）。年轻道只走几何规则，期间由 flush 背压（stall_runs）兜底。
        if (opts.max_runs >= 1 && n > opts.max_runs && j > opts.max_runs - 1) {
            j = opts.max_runs - 1;
            acc = 0;
            for (std::size_t i = j; i < n; ++i) acc += static_cast<double>(sizes[i]);
            // 被上限扩大后继续按几何规则往前吸收，保持由老到新递减。
            while (j > 0 && static_cast<double>(sizes[j - 1]) <= opts.k * acc) {
                --j;
                acc += static_cast<double>(sizes[j]);
            }
        }
    }
    if (n - j < 2) return none;
    CompactionChoice c;
    c.merge = true;
    c.begin = j;
    c.full = (j == 0);
    return c;
}

}  // namespace bitcask::oki
