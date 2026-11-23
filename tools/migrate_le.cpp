// migrate_le — 把 v1 大端 bitcask 目录离线迁移成当前 data-only 纪元
//（meta v5）。保留的旧入口;统一入口（含 u32→u64 迁移与纪元检测）
// 见 tools/bitcask_migrate.cpp。
//
//   用法:  migrate_le <src_dir> <dst_dir>
//
// 非破坏性:只读 src、只写 dst。迁移 data（重编码）、meta、
// field.schema;ckpt/seg/wal 等可重建文件不迁移（新库首开自动重建）。
// 详见 bitcask/migrate.hpp 与 doc/format-zh.md。
//
// 路径编码：argv 经 argv_utf8.hpp 转成 UTF-8（库约定窄路径 = UTF-8）。

#include <cstdio>
#include <string>

#include "argv_utf8.hpp"
#include "bitcask/migrate.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <src_dir> <dst_dir>\n"
                     "  migrate a v1 big-endian bitcask dir to the current "
                     "era (meta v5, data-only;\n"
                     "  non-destructive: src read-only). for u32-era dirs "
                     "(meta v2/v3) use: bitcask_migrate tstamp64\n",
                     argv[0]);
        return 2;
    }
    const auto args = bitcask_tools::args_as_utf8(argc, argv);
    auto r = bitcask::migrate::migrate_be_to_le(args[1], args[2]);
    if (!r) {
        std::fprintf(stderr, "migrate failed: %s\n", r.error().c_str());
        return 1;
    }
    const auto& s = *r;
    std::printf("migrated %s -> %s\n", args[1].c_str(), args[2].c_str());
    std::printf("  data files     : %llu\n",
                static_cast<unsigned long long>(s.data_files));
    std::printf("  records        : %llu (tombstones %llu)\n",
                static_cast<unsigned long long>(s.records),
                static_cast<unsigned long long>(s.tombstones));
    std::printf("  skipped badcrc : %llu\n",
                static_cast<unsigned long long>(s.skipped_bad_crc));
    std::printf("  meta migrated  : %s\n", s.meta_migrated ? "yes" : "no");
    std::printf("  field.schema   : %s\n",
                s.field_schema_migrated ? "yes" : "(none)");
    if (s.skipped_bad_crc > 0) {
        std::fprintf(stderr,
                     "WARNING: %llu records failed CRC and were skipped "
                     "(source corruption?)\n",
                     static_cast<unsigned long long>(s.skipped_bad_crc));
    }
    return 0;
}
