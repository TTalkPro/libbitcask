// 组件 checkpoint 共用数据类型（S20-1 R6）。
//
// text（bm25.ckpt）/ vector（vec.ckpt）/ docmap（docmap.ckpt）三组件的链状态
// 与载入结果结构完全同构——S15-S18 平移期各自独立定义，此处收敛为单一真源。
// 各插件类以嵌套 using 别名暴露（`TextPlugin::ChainState` 等既有名字不变），
// 差异化的 setter 语义（VectorPlugin::set_chain_state 联动 delta_window_wm_）
// 仍留在各类。

#pragma once

#include <cstdint>

namespace bitcask::ckpt {

// 组件链状态：base 世代 | 链覆盖水位 | 下一 delta 序号。与 manifest entry 对齐。
struct ChainState {
    std::uint64_t base_gen = 0;
    std::uint64_t chain_wm = 0;
    std::uint32_t next_seq = 1;
};

// delta 写结果：是否落盘 | 本次 .d 序号（manifest 用）。
struct DeltaSaveResult {
    bool          wrote = false;
    std::uint32_t new_seq = 0;
};

// 组件载入结果：结构完整 | base+链覆盖水位 | 是否 .prev 回退 | 全段健康。
struct LoadResult {
    bool          loaded = false;
    std::uint64_t watermark = 0;
    bool          from_prev = false;
    bool          all_segments_ok = false;
};

}  // namespace bitcask::ckpt
