// 旧纪元目录 → 当前格式（meta v4,64 位时间戳）的离线迁移。
//
// 两次 flag-day 造就三个纪元（见 doc/format-zh.md）：
//   - v1 纪元：大端（meta v1）。
//   - u32 纪元：小端 + u32 时间戳（meta v2/v3;record header 23B、hint
//     BCH3、DocValue Ver=3、expiry 段 u32）。
//   - 当前纪元：小端 + u64 时间戳（meta v4;record header 27B、hint BCH4、
//     DocValue Ver=4、expiry 段 u64）。
//
// 对应两个迁移器（均非破坏性：只读 src、只写 dst）：
//   - migrate_be_to_le   ：v1 大端 → 当前纪元（逐 record 解大端头 → 用当前
//                          codec 重编码,CRC 重算;meta 1→4;field.schema
//                          NameLen 大端→小端 + 补 CRC）。
//   - migrate_u32_to_u64 ：u32 纪元（meta v2/v3）→ 当前纪元（逐 record 解
//                          23B 小端头 → 27B 重编码;kDoc 的 DocValue v3→v4
//                          转码,expiry 段 u32→u64;meta 2/3→4;field.schema
//                          格式未变,原样拷贝）。
// 两者都同步**重生成** <id>.bitcask.hint（从迁移后的 data 派生）。
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

// 把 src_dir（v1 大端纪元,meta v1）迁移到 dst_dir（当前纪元,meta v4）。
// dst_dir 不存在则创建。失败返回错误串（src 非 bitcask 目录 / meta 版本
// 不是 v1 / I/O 错误等）。
[[nodiscard]] std::expected<MigrateStats, std::string>
migrate_be_to_le(std::string_view src_dir, std::string_view dst_dir);

// 把 src_dir（u32 时间戳纪元,meta v2/v3）迁移到 dst_dir（当前纪元,
// meta v4:record header 23B→27B,tstamp/expiry u32→u64,DocValue v3→v4）。
// dst_dir 不存在则创建。失败返回错误串（src 非 bitcask 目录 / meta 版本
// 不是 v2/v3——v1 请先用 migrate_be_to_le,v4 无需迁移 / I/O 错误等）。
[[nodiscard]] std::expected<MigrateStats, std::string>
migrate_u32_to_u64(std::string_view src_dir, std::string_view dst_dir);

}  // namespace bitcask::migrate
