// bitcask_migrate — bitcask 目录格式纪元的统一离线迁移入口。
//
// 三次 flag-day 造就四个纪元（见 doc/format-zh.md 与 bitcask/migrate.hpp）：
//   v1 纪元   : 大端（meta v1）
//   u32 纪元  : 小端 + 32 位时间戳（meta v2/v3）
//   u64 纪元  : 小端 + 64 位时间戳（meta v4,hint 无 ord）
//   当前纪元  : 同 u64 的 data 布局,hint BCH5 内嵌 ord（meta v5）
//
// 子命令:
//   detect   <dir>          读 bitcask.meta 报告纪元,并提示该用哪个迁移
//   be2le    <src> <dst>    v1 大端      → 当前纪元（meta v5）
//   tstamp64 <src> <dst>    u32 纪元 v2/v3 → 当前纪元（meta v5）
//   hintord  <src> <dst>    u64 纪元 v4  → 当前纪元（meta v5;data 硬链接
//                           零改动,仅重生成 hint + meta）
//
// 非破坏性:只读 src、只写 dst（dst 不存在则创建）。data 重编码（hintord
// 例外:硬链接零改动）+ hint 重生成;ckpt/seg/wal 等派生缓存不迁移,新库首开
// 自动 fold 重建。

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "bitcask/migrate.hpp"

namespace {

constexpr const char* kUsage =
    "usage: bitcask_migrate <command> ...\n"
    "\n"
    "commands:\n"
    "  detect   <dir>          detect the format era of a bitcask dir and\n"
    "                          suggest which migration (if any) to run\n"
    "  be2le    <src> <dst>    migrate a v1 big-endian dir (meta v1) to the\n"
    "                          current era (meta v5)\n"
    "  tstamp64 <src> <dst>    migrate a u32-timestamp little-endian dir\n"
    "                          (meta v2/v3) to the current era (meta v5:\n"
    "                          record header 23B->27B, tstamp/expiry\n"
    "                          u32->u64, DocValue v3->v4)\n"
    "  hintord  <src> <dst>    migrate an ord-less-hint dir (meta v4) to\n"
    "                          the current era (meta v5): data files are\n"
    "                          hard-linked UNCHANGED, only hints (BCH5,\n"
    "                          ord embedded) and meta are rewritten\n"
    "\n"
    "era cheat sheet (bitcask.meta version byte):\n"
    "  v1    big-endian legacy            -> run: be2le\n"
    "  v2/v3 little-endian, u32 tstamp    -> run: tstamp64\n"
    "  v4    u64 tstamp, ord-less hints   -> run: hintord\n"
    "  v5    current (BCH5 hints)         -> nothing to do\n"
    "  v6    v5 + atomic-batch records    -> nothing to do\n"
    "\n"
    "all migrations are non-destructive: src is opened read-only, output\n"
    "is written to dst (created if missing). derived caches (keydir/search\n"
    "checkpoints, segments) are NOT migrated -- the new library rebuilds\n"
    "them on first open via fold.\n";

void print_stats(const char* src, const char* dst,
                 const bitcask::migrate::MigrateStats& s) {
    std::printf("migrated %s -> %s\n", src, dst);
    std::printf("  data files      : %llu\n",
                static_cast<unsigned long long>(s.data_files));
    std::printf("  records         : %llu (tombstones %llu)\n",
                static_cast<unsigned long long>(s.records),
                static_cast<unsigned long long>(s.tombstones));
    std::printf("  skipped badcrc  : %llu\n",
                static_cast<unsigned long long>(s.skipped_bad_crc));
    std::printf("  skipped baddoc  : %llu\n",
                static_cast<unsigned long long>(s.skipped_bad_docvalue));
    std::printf("  meta migrated   : %s\n", s.meta_migrated ? "yes" : "no");
    std::printf("  field.schema    : %s\n",
                s.field_schema_migrated ? "yes" : "(none)");
    if (s.skipped_bad_crc > 0) {
        std::fprintf(stderr,
                     "WARNING: %llu records failed CRC and were skipped "
                     "(source corruption?)\n",
                     static_cast<unsigned long long>(s.skipped_bad_crc));
    }
    if (s.skipped_bad_docvalue > 0) {
        std::fprintf(stderr,
                     "WARNING: %llu kDoc records had an undecodable DocValue "
                     "and were skipped (the old reader could not decode them "
                     "either)\n",
                     static_cast<unsigned long long>(s.skipped_bad_docvalue));
    }
}

// detect：只读 bitcask.meta 的 magic + version 字节,不碰任何数据文件。
int cmd_detect(const char* dir) {
    const std::string path = std::string(dir) + "/bitcask.meta";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "detect: cannot open %s (not a bitcask dir?)\n",
                     path.c_str());
        return 1;
    }
    unsigned char hdr[5] = {0};
    const std::size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (got != sizeof(hdr) || std::memcmp(hdr, "BCME", 4) != 0) {
        std::fprintf(stderr, "detect: %s has no valid BCME magic\n",
                     path.c_str());
        return 1;
    }
    const unsigned ver = hdr[4];
    switch (ver) {
        case 1:
            std::printf("%s: meta v1 — big-endian legacy era\n", dir);
            std::printf("  next step: bitcask_migrate be2le %s <dst>\n", dir);
            return 0;
        case 2:
        case 3:
            std::printf("%s: meta v%u — little-endian u32-timestamp era\n",
                        dir, ver);
            std::printf("  next step: bitcask_migrate tstamp64 %s <dst>\n",
                        dir);
            return 0;
        case 4:
            std::printf("%s: meta v4 — u64-timestamp, ord-less-hint era\n",
                        dir);
            std::printf("  next step: bitcask_migrate hintord %s <dst>\n",
                        dir);
            return 0;
        case 5:
            std::printf("%s: meta v5 — current era (BCH5 hints), "
                        "nothing to migrate\n", dir);
            return 0;
        case 6:
            std::printf("%s: meta v6 — atomic-batch era (v5 + kBatchHeader "
                        "records), nothing to migrate\n", dir);
            return 0;
        default:
            std::fprintf(stderr,
                         "detect: unknown meta version %u (newer tool "
                         "required?)\n", ver);
            return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "-h") == 0 ||
                      std::strcmp(argv[1], "--help") == 0)) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const std::string_view cmd = argv[1];

    if (cmd == "detect") {
        if (argc != 3) {
            std::fprintf(stderr, "usage: %s detect <dir>\n", argv[0]);
            return 2;
        }
        return cmd_detect(argv[2]);
    }

    if (cmd == "be2le" || cmd == "tstamp64" || cmd == "hintord") {
        if (argc != 4) {
            std::fprintf(stderr, "usage: %s %s <src_dir> <dst_dir>\n",
                         argv[0], argv[1]);
            return 2;
        }
        auto r = (cmd == "be2le")
                     ? bitcask::migrate::migrate_be_to_le(argv[2], argv[3])
                 : (cmd == "tstamp64")
                     ? bitcask::migrate::migrate_u32_to_u64(argv[2], argv[3])
                     : bitcask::migrate::migrate_hint_ord(argv[2], argv[3]);
        if (!r) {
            std::fprintf(stderr, "%s failed: %s\n", argv[1],
                         r.error().c_str());
            return 1;
        }
        print_stats(argv[2], argv[3], *r);
        return 0;
    }

    std::fprintf(stderr, "unknown command '%s'\n\n", argv[1]);
    std::fputs(kUsage, stderr);
    return 2;
}
