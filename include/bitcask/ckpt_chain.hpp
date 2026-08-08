// 组件 checkpoint 的 .d 链走读与坍缩（S20-2 R2/R8）。
//
// text/vector/docmap/legacy/测试 shim 五处 load 路径此前各自内联「逐 .d<seq>
// 存在性检查 → 读容器 → 定位 kDeltaInfo 校验三元组（base_gen/prev_wm/seq）
// → 应用 delta → 推进覆盖水位」的同构循环。此处收敛为单一模板函数，仅
// 「如何应用一个 delta 文件」与「有界/无界终止语义」由调用方定制。
//
// 仅供 .cpp 实现包含（拖入 <filesystem>）——不进任何被别的头包含的头，
// 避免加重公共头的编译成本。

#pragma once

#include "bitcask/search_checkpoint.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include "bitcask/detail/path_utf8.hpp"

namespace bitcask::search {

// 链走读结果：终覆盖水位 | 下一未用序号（成功应用 N 个 → N+1）| 链是否完整。
struct ChainWalk {
    std::uint64_t coverage = 0;
    std::uint32_t next_seq = 1;
    bool          ok = true;
};

// 走 base_path.d1, .d2, … 链，逐文件校验 kDeltaInfo 三元组后调 apply 应用。
//   base_gen      —— 链校验基准世代（= base 覆盖水位）
//   base_coverage —— 链起点覆盖水位（首个 delta 的 prev_wm 须等于它）
//   chain_seq     —— 有界模式的链长（走 1..chain_seq）；unbounded 时忽略。
//                    **注意 chain_seq==0 在有界模式表示「零已提交 delta」→ 零迭代**，
//                    绝不能与无界混淆——manifest 记 chain_seq==0 但盘上残留未提交的
//                    orphan .d1（crash 于「先写 delta 后提交 manifest」窗口）时，
//                    有界须忽略之（与 pre-S20 逐字节一致），无界才会扫盘重放。
//   unbounded     —— false=有界（缺文件=链断=ok:false，manifest 提示链长可信时用，
//                    text/vector/docmap load_component）；true=无界（走到文件缺失为
//                    正常链尾，ok 保持 true，legacy/shim 无 manifest 链长提示时用）
//   apply         —— (const LoadedCheckpoint&) -> bool：应用一个 delta 文件的
//                    段集（段级 CRC 预检由 apply 自理），失败返回 false 断链
//
// 任一环节（读失败/info 缺失或坏/三元组不匹配/apply 失败）→ ok=false 并停。
template <typename Apply>
ChainWalk walk_chain(const std::string& base_path, std::uint64_t base_gen,
                     std::uint64_t base_coverage, std::uint32_t chain_seq,
                     bool unbounded, Apply&& apply) {
    ChainWalk w{base_coverage, 1, true};
    for (std::uint32_t s = 1; unbounded || s <= chain_seq; ++s) {
        const std::string dpath = base_path + ".d" + std::to_string(s);
        std::error_code ec;
        if (!std::filesystem::exists(bitcask::detail::from_utf8(dpath), ec)) {
            if (!unbounded) w.ok = false;  // 有界：缺文件=链断；无界：正常链尾
            break;
        }
        auto dc = SearchCheckpoint::read(dpath);
        if (!dc) { w.ok = false; break; }
        const LoadedSection* info = nullptr;
        for (const auto& dls : dc->sections) {
            if (dls.type ==
                static_cast<std::uint16_t>(CkptSectionType::kDeltaInfo)) {
                info = &dls;
                break;
            }
        }
        if (!info || !info->crc_ok || info->payload.size() != 20) {
            w.ok = false;
            break;
        }
        const auto* q = info->payload.data();
        const std::uint64_t gen = detail::get_u64(q);
        const std::uint64_t prev_wm = detail::get_u64(q + 8);
        const std::uint32_t sq = detail::get_u32(q + 16);
        if (gen != base_gen || prev_wm != w.coverage || sq != s) {
            w.ok = false;
            break;
        }
        if (!apply(*dc)) { w.ok = false; break; }
        w.coverage = dc->watermark;
        w.next_seq = s + 1;
    }
    return w;
}

// base 落成后链坍缩：删 base_path.d1, .d2, …，连续 miss 8 个序号即停
// （链恒连续 1..N，8 空洞 orphan 扫尾足够）。
inline void remove_chain_files(const std::string& base_path) {
    std::uint32_t misses = 0;
    for (std::uint32_t i = 1; misses < 8; ++i) {
        std::error_code ec;
        if (std::filesystem::remove(bitcask::detail::from_utf8(base_path + ".d" + std::to_string(i)), ec)) {
            misses = 0;
        } else {
            ++misses;
        }
    }
}

}  // namespace bitcask::search
