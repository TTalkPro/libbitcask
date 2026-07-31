// 旧纪元目录 → 当前格式（meta v5,hint 内嵌 ord）的离线迁移。
//
// 三次 flag-day 造就四个纪元（见 doc/format-zh.md）：
//   - v1 纪元：大端（meta v1）。
//   - u32 纪元：小端 + u32 时间戳（meta v2/v3;record header 23B、hint
//     BCH3、DocValue Ver=3、expiry 段 u32）。
//   - u64 纪元：小端 + u64 时间戳（meta v4;record header 27B、hint BCH4、
//     DocValue Ver=4、expiry 段 u64）。
//   - 当前纪元：同 u64 纪元的 data/DocValue 布局,hint BCH5（记录内嵌
//     ord,meta v5）——S33 flag-day,见 doc/ordered-key-index-design-zh.md §3.4。
//
// 对应三个迁移器（均非破坏性：只读 src、只写 dst）：
//   - migrate_be_to_le   ：v1 大端 → 当前纪元（逐 record 解大端头 → 用当前
//                          codec 重编码,CRC 重算;meta 1→5;field.schema
//                          NameLen 大端→小端 + 补 CRC）。
//   - migrate_u32_to_u64 ：u32 纪元（meta v2/v3）→ 当前纪元（逐 record 解
//                          23B 小端头 → 27B 重编码;kDoc 的 DocValue v3→v4
//                          转码,expiry 段 u32→u64;meta 2/3→5;field.schema
//                          格式未变,原样拷贝）。
//   - migrate_hint_ord   ：u64 纪元（meta v4）→ 当前纪元。**data 文件一字节
//                          不动**（硬链接,跨设备退化为拷贝）,仅从 data 重扫
//                          生成 BCH5 hint（ord 在 record header 内现成）+
//                          meta 4→5。meta 最后写,是 dst 的 commit point
//                          （中途失败的 dst 无 meta,不会被误开）。
// 三者都同步**重生成** <id>.bitcask.hint（从 data 派生）。
// 跳过（新库首开由 fold 自动重建，无需迁移）：kv.keydir.ckpt、search.*（ckpt/
//   seg/wal/manifest）、旧 hint（已重生成）、锁文件。

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace bitcask::migrate {

struct MigrateStats {
    std::uint64_t data_files = 0;       // 迁移的 data 文件数
    std::uint64_t records = 0;          // 迁移的 record 总数（含墓碑）
    std::uint64_t tombstones = 0;       // 其中墓碑数
    std::uint64_t skipped_bad_crc = 0;  // CRC 校验失败被跳过的 record 数
    // 仅 migrate_u32_to_u64：kDoc 的 value 段不是合法 DocValue v3（Ver 字节
    // 不符 / 短于头部）被跳过的 record 数——这类 record 旧读端同样解不动。
    std::uint64_t skipped_bad_docvalue = 0;
    bool meta_migrated = false;
    bool field_schema_migrated = false;
};

// 把 src_dir（v1 大端纪元,meta v1）迁移到 dst_dir（当前纪元,meta v5）。
// dst_dir 不存在则创建。失败返回错误串（src 非 bitcask 目录 / meta 版本
// 不是 v1 / I/O 错误等）。
[[nodiscard]] std::expected<MigrateStats, std::string>
migrate_be_to_le(std::string_view src_dir, std::string_view dst_dir);

// 把 src_dir（u32 时间戳纪元,meta v2/v3）迁移到 dst_dir（当前纪元,
// meta v5:record header 23B→27B,tstamp/expiry u32→u64,DocValue v3→v4）。
// dst_dir 不存在则创建。失败返回错误串（src 非 bitcask 目录 / meta 版本
// 不是 v2/v3——v1 请先用 migrate_be_to_le,v4 用 migrate_hint_ord,
// v5 无需迁移 / I/O 错误等）。
[[nodiscard]] std::expected<MigrateStats, std::string>
migrate_u32_to_u64(std::string_view src_dir, std::string_view dst_dir);

// 把 src_dir（u64 纪元,meta v4）迁移到 dst_dir（当前纪元,meta v5）。
// data 文件硬链接进 dst（跨设备退化为拷贝,内容零改动）,hint 从 data 全量
// 重扫生成（BCH5,含 ord）,field.schema 原样拷贝,meta 最后原子写（commit
// point）。幂等：失败后删 dst 重跑即可。失败返回错误串（src 非 bitcask
// 目录 / meta 版本不是 v4——更旧的先跑对应迁移,v5 无需迁移 / I/O 错误等）。
[[nodiscard]] std::expected<MigrateStats, std::string>
migrate_hint_ord(std::string_view src_dir, std::string_view dst_dir);

}  // namespace bitcask::migrate
