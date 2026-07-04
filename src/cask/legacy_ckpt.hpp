// legacy_ckpt — pre-S17 统一 search.ckpt 的 load-only 读取器（S19-2）。
//
// 唯一生产用途：`Cask::migrate_legacy_search_ckpt`（S17-5 一次性迁移——
// 读旧单文件容器 + 单 delta 链，重建内存态后由 caller 改写为 per-component
// 文件族 + manifest）。写端（save_search_ckpt/save_delta_ckpt）不在生产
// 侧——随 SearchLayer shim 降级为测试夹具（生成旧格式文件喂本读取器的
// 迁移测试）。旧格式支持整体退役时（P6+）删本模块。
//
// 实现自 SearchLayer::load_search_ckpt / apply_delta_file 平移改造：
// 段分发到 Index / TextPlugin / VectorPlugin 原语，链重放语义逐字节一致。

#pragma once

#include "bitcask/docmap_ckpt.hpp"   // DocmapReplayHook
#include "bitcask/index.hpp"
#include "bitcask/text_plugin.hpp"
#include "bitcask/vector_plugin.hpp"

#include <cstdint>
#include <string_view>

namespace bitcask::legacy_ckpt {

struct LoadResult {
    bool loaded = false;           // 容器结构完整（base 或 .prev）
    std::uint64_t watermark = 0;   // base+链覆盖水位
    bool all_segments_ok = false;  // 全段健康 + 链完整
    bool from_prev = false;
};

// 读 path（结构损坏退 path.prev）→ 逐段分发反序列化 → .d 链连续重放。
// 收尾与原 SearchLayer 版一致：docmap 窗口/日志、插件链状态与脏位对齐
// 载入态。hook 透传 keydir 半边（migrate 路径传空——keydir 由 caller 的
// recovery 阶段接管）。
[[nodiscard]] LoadResult load(std::string_view path, index::Index& docmap,
                              text::TextPlugin& text, vec::VectorPlugin& vec,
                              const index::DocmapReplayHook& hook = {});

}  // namespace bitcask::legacy_ckpt
