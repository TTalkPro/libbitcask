// docmap 组件 checkpoint —— 宿主侧持久化（S18-2，设计 §4/§5）。
//
// DocMap（index::Index）是宿主服务：其组件文件族 docmap.ckpt（base + .prev
// + .d<seq> 链，链内联 kKeydirDelta 成对段，S14-7 原子性保留）由宿主（Cask）
// 直接驱动，不再经 SearchLayer。本头提供三个入口：
//   save_base   —— rename→.prev + 写新 base（kDocmap 段）+ 清链文件
//   save_delta  —— 写 .d<seq>（kDeltaInfo + kDocmapDelta 窗口行/删除日志
//                  + 可选 kKeydirDelta）
//   load        —— base（wm 校验，失败退 .prev）→ 链重放（行/删除交错按
//                  ord 应用到 Index，keydir 半边经 hook 透传给宿主）
//
// 记账协同（Index 自记账，见 index.hpp S18-2 段）：save/load 成功后本模块
// 负责 clear_dirty / clear_removals / begin_delta_window 收尾——调用方只管
// 何时保存与 manifest commit。
//
// 文件格式与 SearchLayer 时代逐字节相同（S18-2 只移交发起权）。

#pragma once

#include "bitcask/index.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::index {

// 链重放时透传给宿主的 keydir 半边（原 SearchLayer::DeltaDocRow/DeltaRemoval
// 的宿主版）。rows/removals → keydir LWW put / remove_if_older；keydir_meta
// 为 kKeydirDelta 段原始字节（可为空 → 字节水位不推进，方向安全）。
struct DocmapDeltaRow {
    std::uint64_t ord = 0;
    std::string   ext;
    DocSlot       slot;
};
struct DocmapDeltaRemoval {
    std::uint64_t tomb = 0;
    std::string   key;
};
using DocmapReplayHook = std::function<void(
    const std::vector<DocmapDeltaRow>&,
    const std::vector<DocmapDeltaRemoval>&,
    std::span<const std::byte> keydir_meta)>;

// base 写：dir/docmap.ckpt。rename 旧文件 → .prev，写新 base，删 .d 链文件，
// 成功后 begin_delta_window(watermark) + clear_removals + clear_dirty。
[[nodiscard]] bool save_docmap_base(Index& docmap, std::string_view dir,
                                    std::uint64_t watermark);

// delta 写：dir/docmap.ckpt.d<seq>。窗口 = [from, watermark)。
// base_gen/from/seq 由调用方从 manifest 提供（kDeltaInfo 链校验三元组）。
// 成功后 begin_delta_window(watermark) + clear_removals + clear_dirty。
[[nodiscard]] bool save_docmap_delta(Index& docmap, std::string_view dir,
                                     std::uint64_t watermark,
                                     std::uint64_t base_gen,
                                     std::uint64_t from, std::uint32_t seq,
                                     std::span<const std::byte> keydir_delta);

// 组件载入结果（形态与 SearchLayer::ComponentLoadResult 一致）。
struct DocmapLoadResult {
    bool loaded = false;           // base 结构完整 + wm 匹配 + 链完整
    std::uint64_t watermark = 0;   // base+链覆盖水位
    bool from_prev = false;
    bool all_segments_ok = false;
};

// 载入：base（header.watermark == expected_base_wm，失败退 .prev）→ 链
// .d1..d{chain_seq} 连续重放。成功后 begin_delta_window(覆盖水位) +
// clear_removals（重放污染）+ clear_dirty。失败时窗口归零（fold 全量重建）。
[[nodiscard]] DocmapLoadResult
load_docmap(Index& docmap, std::string_view dir,
            std::uint64_t expected_base_wm, std::uint32_t chain_seq,
            const DocmapReplayHook& hook);

}  // namespace bitcask::index
