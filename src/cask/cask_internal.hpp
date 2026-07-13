// cask_internal.hpp — Cask 各 TU（cask.cpp / cask_iter.cpp / cask_search.cpp /
// cask_recovery.cpp）共用的文件级助手与 checkpoint 文件名常量。S21-3 B1：
// cask.cpp 物理拆分时从其匿名 namespace / 文件级 static 抽出，函数体逐字节
// 不变，仅链接性改为 inline（多 TU 共用，避免 ODR 冲突）。
// 内部头：不进 include/bitcask/，不属公共 API。
#pragma once

#include <time.h>   // ::clock_gettime / CLOCK_REALTIME_COARSE（now_sec_default）

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "bitcask/cask.hpp"            // CaskFault / CaskError
#include "bitcask/index_manifest.hpp"  // ComponentId（component_of_plugin）

namespace bitcask {

// P14a:恢复 checkpoint 文件名(目录级,与 bitcask.meta 同级)。命名契约
// {kv|search}.{组件}.{ckpt|seg|wal|manifest},见
// doc/recovery-unified-checkpoint-design-zh.md §3。后缀 .ckpt = 可 fold
// 重建的 checkpoint(纯优化);缺失/损坏时首次 open 走全量 fold 重建。
inline constexpr const char* kKeydirSnapName = "kv.keydir.ckpt";
// P14e:搜索索引统一分段 checkpoint（docmap/bm25/hnsw 单文件，逐段 CRC）。
inline constexpr const char* kSearchCkptName = "search.ckpt";
// S17-2:per-component 段文件名（docmap.ckpt / bm25.ckpt / vec.ckpt）。
// 取代旧的 kSearchCkptName。S17-5 迁移期间旧名仍被读（一次性迁移路径）。
inline constexpr const char* kDocmapCkptName = "docmap.ckpt";
inline constexpr const char* kBm25CkptName   = "bm25.ckpt";
inline constexpr const char* kVecCkptName    = "vec.ckpt";

inline CaskFault io_fault(int errnum, std::string detail = {}) {
    return CaskFault{CaskError::kIo, errnum, std::move(detail)};
}
inline CaskFault err(CaskError k, std::string detail = {}) {
    return CaskFault{k, 0, std::move(detail)};
}

inline std::uint32_t now_sec_default() {
#ifdef CLOCK_REALTIME_COARSE
    // 每次 get/put 都要取秒级时间戳:COARSE 时钟走 vDSO 无 syscall,
    // 粒度为内核 tick(1-4ms),对秒级语义无损。
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec);
#else
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
#endif
}

inline std::string_view bytes_to_view(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// S18-6：插件名 → manifest 组件槽的固定映射（P4 期宿主记账仍按 3 组件
// manifest；P5 泛化为按插件名的动态 manifest 再撤）。
inline std::optional<bitcask::ComponentId>
component_of_plugin(std::string_view name) {
    if (name == "bm25") return bitcask::ComponentId::kBm25;
    // kVec 槽 = 向量组件（引擎无关）：每库仅一个向量插件（建库时经
    // meta.vector_engine 选定），HNSW/IVF 共用同一 manifest 槽（S32-M3）。
    if (name == "hnsw" || name == "ivfrq") return bitcask::ComponentId::kVec;
    return std::nullopt;
}

}  // namespace bitcask
