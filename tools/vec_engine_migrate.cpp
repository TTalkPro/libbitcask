// vec_engine_migrate — 向量引擎离线切换工具（S32-M4；设计
// doc/vector-dual-engine-selection-zh.md §6.4）。
//
//   用法: vec_engine_migrate --dir <db> --to hnsw|ivfrq [--purge-old] [--dry-run]
//
// 原理（核心属性由 CaskDocValueTest.S32M4EngineSwitchViaMetaRebuild 守护）：
// data file 是向量权威——本工具只改写 meta.vector_engine；目标引擎组件
// 在**首次 open 时由全量 fold 重建**（时长 ∝ 库大小）。旧引擎组件文件
// 默认原地保留（回滚 = 再跑一次 --to 旧引擎；新旧水位差由恢复机制自愈），
// --purge-old 释放磁盘（HNSW 族: vec.ckpt*/vec.vec/vec.qc8；IVF 族:
// ivf.ckpt*/ivf.biv）。
//
// 安全性：经 write.lock 排他（拒绝与在线进程并跑）；meta 改写前校验
// 目录模式/维度/度量与目标引擎的兼容性（ivfrq 拒 kL2）。

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/file_lock.hpp"
#include "bitcask/meta_file.hpp"

namespace fs = std::filesystem;
using bitcask::meta::VectorEngine;

namespace {

const char* engine_name(VectorEngine e) {
    switch (e) {
        case VectorEngine::kHnsw:    return "hnsw";
        case VectorEngine::kIvfRq:   return "ivfrq";
        case VectorEngine::kDiskann: return "diskann";
    }
    return "?";
}

// 各引擎的组件文件族（清理/统计用；.d 链按前缀匹配）。
std::vector<fs::path> engine_files(const fs::path& dir, VectorEngine e) {
    std::vector<fs::path> out;
    const char* base = e == VectorEngine::kHnsw ? "vec.ckpt" : "ivf.ckpt";
    out.push_back(dir / base);
    out.push_back(dir / (std::string(base) + ".prev"));
    if (e == VectorEngine::kHnsw) {
        out.push_back(dir / "vec.vec");
        out.push_back(dir / "vec.qc8");
    } else {
        out.push_back(dir / "ivf.biv");
    }
    // .d 链。
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        const std::string name = ent.path().filename().string();
        if (name.rfind(std::string(base) + ".d", 0) == 0) {
            out.push_back(ent.path());
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir;
    std::string to;
    bool purge_old = false;
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--dir" && i + 1 < argc) {
            dir = argv[++i];
        } else if (a == "--to" && i + 1 < argc) {
            to = argv[++i];
        } else if (a == "--purge-old") {
            purge_old = true;
        } else if (a == "--dry-run") {
            dry_run = true;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            dir.clear();
            break;
        }
    }
    if (dir.empty() || (to != "hnsw" && to != "ivfrq")) {
        std::fprintf(stderr,
            "usage: %s --dir <db> --to hnsw|ivfrq [--purge-old] [--dry-run]\n"
            "  switch the vector engine of a bitcask dir (offline).\n"
            "  the target engine's index is rebuilt by full fold on the\n"
            "  next open (duration ~ store size). old engine's component\n"
            "  files are kept unless --purge-old (rollback = run again\n"
            "  with --to <old engine>).\n",
            argv[0]);
        return 2;
    }
    const VectorEngine target =
        to == "hnsw" ? VectorEngine::kHnsw : VectorEngine::kIvfRq;

    // 排他：write.lock（与在线写进程同一把锁）。
    const std::string lock_path = (fs::path(dir) / "write.lock").string();
    auto lock = bitcask::lock::FileLock::acquire(lock_path, true);
    if (!lock) {
        std::fprintf(stderr,
                     "cannot acquire %s (store in use by another process?)\n",
                     lock_path.c_str());
        return 1;
    }

    auto mc = bitcask::meta::read_meta(dir);
    if (!mc) {
        std::fprintf(stderr, "read meta failed: %s\n",
                     mc.error().message.c_str());
        return 1;
    }
    if (mc->mode != bitcask::meta::Mode::kIndex || mc->vector_dim == 0) {
        std::fprintf(stderr,
                     "store has no vector config (mode/dim) — nothing to "
                     "switch\n");
        return 1;
    }
    if (mc->vector_engine == target) {
        std::printf("already on engine %s — no-op\n", engine_name(target));
        return 0;
    }
    if (target == VectorEngine::kIvfRq &&
        mc->vector_metric == bitcask::meta::VectorMetric::kL2) {
        std::fprintf(stderr,
                     "ivfrq requires kDot/cosine metric (store is kL2)\n");
        return 1;
    }
    const VectorEngine old_engine = mc->vector_engine;

    std::printf("switch plan: %s -> %s (dim=%u)\n", engine_name(old_engine),
                engine_name(target), mc->vector_dim);
    std::printf("  next open will FULL-FOLD rebuild the %s index\n",
                engine_name(target));
    const auto old_files = engine_files(dir, old_engine);
    std::uintmax_t old_bytes = 0;
    for (const auto& f : old_files) {
        std::error_code ec;
        if (fs::exists(f, ec)) old_bytes += fs::file_size(f, ec);
    }
    std::printf("  old engine files: %llu bytes (%s)\n",
                static_cast<unsigned long long>(old_bytes),
                purge_old ? "will purge" : "kept for rollback");
    if (dry_run) {
        std::printf("dry-run: no changes made\n");
        return 0;
    }

    mc->vector_engine = target;
    if (auto wr = bitcask::meta::write_meta(dir, *mc); !wr) {
        std::fprintf(stderr, "write meta failed: %s\n",
                     wr.error().message.c_str());
        return 1;
    }
    if (purge_old) {
        for (const auto& f : old_files) {
            std::error_code ec;
            fs::remove(f, ec);
        }
    }
    std::printf("done. reopen with vector_engine=%s to rebuild.\n",
                engine_name(target));
    return 0;
}
