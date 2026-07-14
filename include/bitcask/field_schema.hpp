// 字段名 ↔ id 注册表（#1，schema interning）。
//
// 背景：DocValue 的 fields 段此前每条 record 内联存字段名（"title"/"body"...），
// append-only 下同 schema 的百万文档把字段名重复百万次、merge 后再重复一遍。
// 改为存 field id（小整数 varint），字段名只在本注册表存一份。
//
// 持久化：append-only 文件 <dir>/field.schema。
//   文件头 8 字节 = [magic:u32 = "FSCH"][version:u32 = 1]（均小端）。
//   每条 entry = [NameLen:u16][name bytes][CRC32:u32]，CRC 覆盖 [NameLen|name]。
//   id == 出现顺序（0 基）。open 时顺序重放即还原 name↔id。
//
// 健壮性（S12-3）：magic/version 拒绝损坏或未知格式；每条 entry 的 CRC 检出位翻转/
// 中段损坏 → fail-fast（open 返回 false）。append-only 崩溃常态导致的「尾部半条」
// （torn tail）容忍跳过——与 WAL 语义一致（未持久化的 entry 等价于从未写入）。
//
// 兼容：旧库的 field.schema 是「无头 [len][name]」格式（flag-day 后小端）。open 时
// peek 前 4 字节：== magic 走新格式（校验 CRC）；否则按 legacy 无头照读，并在可写
// 目录下**原子升级**为新格式（temp + fsync + rename，权威数据零丢失窗口）。升级失败
// （如只读目录）则退回按 legacy 格式继续追加，保持该文件自洽。
//
// 并发：写路径持 unique_lock + fflush。

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <unistd.h>  // ::fsync / ::fileno（升级时 rename 前的持久化屏障）

#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"

namespace bitcask {

namespace detail {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};
}  // namespace detail

class FieldSchema {
public:
    FieldSchema() = default;
    FieldSchema(const FieldSchema&) = delete;
    FieldSchema& operator=(const FieldSchema&) = delete;

    // "FSCH"（大端可读）；盘上按小端存。
    static constexpr std::uint32_t kMagic      = 0x46534348;
    static constexpr std::uint32_t kVersion    = 1;
    static constexpr std::size_t   kHeaderSize = 8;

    // 打开/创建注册表并加载已有映射。
    // 返回 false = **硬失败**（magic 有但 version 未知，或某条 entry CRC 不符 = 真损坏）→
    // caller 应中止 open。返回 true = 可用（含 legacy 兼容读、只读目录无法升级等软路径）。
    // 只读路径只需 name_of（映射已加载），不会 intern。
    bool open(const std::string& path) {
        std::unique_lock lk(mu_);
        path_ = path;
        name_to_id_.clear();
        id_to_name_.clear();
        legacy_ = false;
        bool fresh = true;         // 无既有内容（新文件/空文件）→ 需写文件头
        bool need_upgrade = false; // 读到 legacy 无头内容 → 尝试升级为新格式

        if (std::FILE* rf = std::fopen(path.c_str(), "rb")) {
            std::byte magic_buf[4];
            const std::size_t got = std::fread(magic_buf, 1, 4, rf);
            if (got == 0) {
                // 空文件：当作新文件（下方补写文件头）。
            } else if (got == 4 && le_load_u32(magic_buf) == kMagic) {
                fresh = false;
                std::byte ver_buf[4];
                if (std::fread(ver_buf, 1, 4, rf) != 4 ||
                    le_load_u32(ver_buf) != kVersion) {
                    std::fclose(rf);
                    return false;  // 未知/更新版本 → fail-fast
                }
                if (!load_new_format_(rf)) {
                    std::fclose(rf);
                    return false;  // 某条 entry CRC 不符 → 真损坏 → fail-fast
                }
            } else {
                // 无 magic → legacy 无头文件。回卷按旧格式解析。
                fresh = false;
                std::rewind(rf);
                load_legacy_(rf);
                legacy_ = true;
                need_upgrade = true;
            }
            std::fclose(rf);
        }
        // 文件不存在 → fresh 保持 true，空注册表。

        if (need_upgrade && upgrade_legacy_to_new_()) {
            legacy_ = false;  // 升级成功：文件现已是带头新格式
        }

        if (fp_) fp_.reset();
        fp_.reset(std::fopen(path.c_str(), "ab"));  // best-effort 追加句柄

        // 全新文件：先写 8 字节文件头，后续 intern 的 entry 才是自洽的新格式。
        if (fresh && fp_) {
            write_header_(fp_.get());
            std::fflush(fp_.get());
        }
        return true;
    }

    // 返回字段名的 id；新名字分配下一个 id 并 append 持久化。线程安全。
    std::uint32_t intern(std::string_view name) {
        {
            std::shared_lock lk(mu_);
            if (auto it = name_to_id_.find(std::string(name)); it != name_to_id_.end()) {
                return it->second;
            }
        }
        std::unique_lock lk(mu_);
        // 双检：并发下可能已被别的线程加入。
        if (auto it = name_to_id_.find(std::string(name)); it != name_to_id_.end()) {
            return it->second;
        }
        const auto id = static_cast<std::uint32_t>(id_to_name_.size());
        if (fp_) {
            if (legacy_) {
                // 升级失败（只读目录等）→ 按旧无头格式追加，保持该文件自洽。
                std::byte lb[2];
                le_store_u16(lb, static_cast<std::uint16_t>(name.size()));
                std::fwrite(lb, 1, 2, fp_.get());
                if (!name.empty()) std::fwrite(name.data(), 1, name.size(), fp_.get());
            } else {
                const auto buf = encode_entry_(name);
                std::fwrite(buf.data(), 1, buf.size(), fp_.get());
            }
            std::fflush(fp_.get());
        }
        std::string key(name);
        name_to_id_.emplace(key, id);
        id_to_name_.push_back(std::move(key));
        return id;
    }

    // id → 字段名；越界返回 nullopt。
    [[nodiscard]] std::optional<std::string> name_of(std::uint32_t id) const {
        std::shared_lock lk(mu_);
        if (id >= id_to_name_.size()) return std::nullopt;
        return id_to_name_[id];
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(mu_);
        return id_to_name_.size();
    }

private:
    // [NameLen:u16][name][CRC32:u32]，CRC 覆盖 [NameLen|name]（单缓冲、单次 fwrite）。
    static std::vector<std::byte> encode_entry_(std::string_view name) {
        const auto nlen = static_cast<std::uint16_t>(name.size());
        std::vector<std::byte> buf(2 + name.size() + 4);
        le_store_u16(buf.data(), nlen);
        if (!name.empty()) std::memcpy(buf.data() + 2, name.data(), name.size());
        const std::uint32_t crc =
            codec::crc32(std::span<const std::byte>(buf.data(), 2 + name.size()));
        le_store_u32(buf.data() + 2 + name.size(), crc);
        return buf;
    }

    static bool write_header_(std::FILE* f) {
        std::byte hdr[kHeaderSize];
        le_store_u32(hdr, kMagic);
        le_store_u32(hdr + 4, kVersion);
        return std::fwrite(hdr, 1, kHeaderSize, f) == kHeaderSize;
    }

    // 新格式解析：rf 已越过 8 字节文件头。返回 false 仅当「完整 entry 但 CRC 不符」
    // （真损坏）；torn tail（尾部读不满一条）容忍，返回 true。
    bool load_new_format_(std::FILE* rf) {
        while (true) {
            std::byte lb[2];
            const std::size_t g = std::fread(lb, 1, 2, rf);
            if (g != 2) return true;  // clean EOF 或 torn tail（半条 len）
            const std::uint16_t nlen = le_load_u16(lb);
            std::vector<std::byte> namebuf(nlen);
            if (nlen > 0 &&
                std::fread(namebuf.data(), 1, nlen, rf) != nlen) {
                return true;          // torn tail（半条 name）
            }
            std::byte cb[4];
            if (std::fread(cb, 1, 4, rf) != 4) return true;  // torn tail（缺 CRC）
            std::uint32_t have = codec::crc32_update(0, std::span<const std::byte>(lb, 2));
            if (nlen > 0) {
                have = codec::crc32_update(
                    have, std::span<const std::byte>(namebuf.data(), nlen));
            }
            if (le_load_u32(cb) != have) return false;  // 完整 entry + 坏 CRC → fail-fast
            std::string name(reinterpret_cast<const char*>(namebuf.data()), nlen);
            const auto id = static_cast<std::uint32_t>(id_to_name_.size());
            name_to_id_.emplace(name, id);
            id_to_name_.push_back(std::move(name));
        }
    }

    // legacy 无头格式解析：[NameLen:u16][name] 循环（flag-day 后小端）。
    void load_legacy_(std::FILE* rf) {
        while (true) {
            std::byte lb[2];
            if (std::fread(lb, 1, 2, rf) != 2) break;
            const std::uint16_t nlen = le_load_u16(lb);
            std::string name(nlen, '\0');
            if (nlen > 0 && std::fread(name.data(), 1, nlen, rf) != nlen) break;
            const auto id = static_cast<std::uint32_t>(id_to_name_.size());
            name_to_id_.emplace(name, id);
            id_to_name_.push_back(std::move(name));
        }
    }

    // 把已加载的 legacy 映射原子重写为新格式：temp → fsync → rename 覆盖。
    // 崩溃安全：fsync 后再 rename，故要么旧文件完好（下次 open 重试升级），要么新文件完整。
    // 返回 false = 无法写（只读目录/IO 失败）→ caller 退回 legacy 追加。
    bool upgrade_legacy_to_new_() {
        const std::string tmp = path_ + ".upgrade.tmp";
        std::FILE* wf = std::fopen(tmp.c_str(), "wb");
        if (!wf) return false;
        bool ok = write_header_(wf);
        for (const auto& name : id_to_name_) {
            if (!ok) break;
            const auto buf = encode_entry_(name);
            ok = std::fwrite(buf.data(), 1, buf.size(), wf) == buf.size();
        }
        if (ok) {
            std::fflush(wf);
            ::fsync(::fileno(wf));  // 数据落盘后才允许 rename 覆盖
        }
        std::fclose(wf);
        if (!ok || std::rename(tmp.c_str(), path_.c_str()) != 0) {
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::uint32_t> name_to_id_;
    std::vector<std::string> id_to_name_;  // id == 下标
    std::string path_;
    std::unique_ptr<std::FILE, detail::FileCloser> fp_;  // append-only 写句柄
    bool legacy_ = false;  // true = 该文件为无头 legacy 且升级失败 → intern 按旧格式追加
};

}  // namespace bitcask
