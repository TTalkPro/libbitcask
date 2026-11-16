// OKI run 归并选段策略（S44-2；设计 designs/oki-size-tiered-compaction-design-zh.md §4.2/§4.3）。
//
// 纯函数：输入当前 run 序列的大小（按 gen 升序，sizes[0] 为最老的底座），
// 输出「归并哪一段后缀」。不碰 IO、不持锁。
//
// 只能归并**连续后缀** Rj..Rn（设计 §4.1）：同 ord 的搬迁行靠「gen 越大越新」
// 裁决，归并不连续或中间一段会让旧位置压过新位置。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bitcask::oki {

enum class CompactionMode {
    kTiered,  // 几何后缀（默认）
    kFull,    // 旧策略：run 数 > full_limit 即全归并（A/B 与回退）
};

struct CompactionPolicyOptions {
    CompactionMode mode = CompactionMode::kTiered;
    // 扇入系数 K：从最新 run 往前累加，前一个 run 的大小 ≤ K × 已累加量就并入。
    // K = 1 时为二进制计数器式合并，每行至多被重写约 log2(N/F) 次；K → ∞ 退化为全归并。
    double k = 1.0;
    // 常驻 run 数目标上限：超过时强制把后缀扩到满足上限。读路径（点查的 bloom
    // 探测、区间扫描的逐 run 定位与归并）开销与 run 数成正比；S44-6 实测
    // 10⁸ 行 / 64K 每 run 时，24 会稳态留下约 9～10 个 run（点查 p99 1.6×、
    // 区间扫描数倍），6 只让写放大从约 5.0 升到约 6.8（选段模拟）。
    std::size_t max_runs = 6;
    // flush 背压阈值：run 数超过它时 flush 等待归并追上。0 = 自动取 2 × max_runs。
    // 不能直接用 max_runs：主道重写大 run 期间其输入仍在册，run 数短暂越过
    // max_runs 很正常，若此时就让 flush 等待，写入会被卡到大归并结束。
    std::size_t stall_runs = 0;
    // kFull 模式（以及小库）的触发阈值：run 数 > full_limit 即全归并。
    std::size_t full_limit = 8;
    // 小库：底座行数低于此值时按 kFull 规则处理——全量重写很便宜，分级反而多写。
    std::uint64_t small_base_rows = std::uint64_t{1} << 20;
};

struct CompactionChoice {
    bool merge = false;     // false = 不需要归并
    std::size_t begin = 0;  // 归并 sizes[begin..n)
    bool full = false;      // begin == 0：含最老 run，可丢墓碑
};

// floor：只能取下标 ≥ floor 的 run（年轻道用；主道传 0）。
// force_full：墓碑占比过高等原因要求全归并（仅 floor == 0 时生效）。
[[nodiscard]] CompactionChoice choose_compaction(
    std::span<const std::uint64_t> sizes, std::size_t floor,
    const CompactionPolicyOptions& opts, bool force_full = false) noexcept;

}  // namespace bitcask::oki
