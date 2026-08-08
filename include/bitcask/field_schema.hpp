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
// 并发：写路径持 unique_lock。P3 后写是 pwrite 定位写（无用户态缓冲，
// 故不再需要 fflush）；偏移 woff_ 由该锁保护。

#pragma once

#include <array>
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


#include "bitcask/byte_order.hpp"
#include "bitcask/codec.hpp"
#include "bitcask/detail/file_util.hpp"  // detail::FileCloser / FilePtr（RED-2 归并）
#include "bitcask/io.hpp"                // S37-6：open_handle 恒带 share-delete

namespace bitcask {

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
        // 必须重置：文件不存在/为空时下面两个 load_ 都不会跑，若留着上一次
        // open 的值，woff_ 会被定到一个凭空的偏移上（同一对象重复 open 才会
        // 暴露，测试里就有这种用法）。
        valid_end_ = 0;
        bool fresh = true;         // 无既有内容（新文件/空文件）→ 需写文件头
        bool need_upgrade = false; // 读到 legacy 无头内容 → 尝试升级为新格式

        // MEM-LOW-1 修复：用 FilePtr RAII 包裹读句柄——load_new_format_ /
        // load_legacy_ 内的 vector/string/map 分配可能抛 bad_alloc，
        // 裸 FILE* 跳过 fclose → fd 泄漏。detail::FilePtr 见 file_util.hpp。
        if (detail::FilePtr rf{detail::fopen_utf8(path, "rb")}) {
            std::FILE* raw = rf.get();
            std::byte magic_buf[4];
            const std::size_t got = std::fread(magic_buf, 1, 4, raw);
            if (got == 0) {
                // 空文件：当作新文件（下方补写文件头）。
            } else if (got == 4 && le_load_u32(magic_buf) == kMagic) {
                fresh = false;
                std::byte ver_buf[4];
                if (std::fread(ver_buf, 1, 4, raw) != 4 ||
                    le_load_u32(ver_buf) != kVersion) {
                    return false;  // 未知/更新版本 → fail-fast
                }
                if (!load_new_format_(raw)) {
                    return false;  // 某条 entry CRC 不符 → 真损坏 → fail-fast
                }
            } else {
                // 无 magic → legacy 无头文件。回卷按旧格式解析。
                fresh = false;
                std::rewind(raw);
                load_legacy_(raw);
                legacy_ = true;
                need_upgrade = true;
            }
            // rf 析构自动 fclose，抛出路径也覆盖
        }
        // 文件不存在 → fresh 保持 true，空注册表。

        if (need_upgrade && upgrade_legacy_to_new_()) {
            legacy_ = false;  // 升级成功：文件现已是带头新格式
            // 文件被整体重写了，load_legacy_ 记下的 valid_end_ 是**旧格式**的
            // 偏移，对新文件无意义（照它截断会把刚升级好的文件切掉一截）。
            // 升级产物 = 文件头 + 全部 entry 的新格式编码，按此重算。
            valid_end_ = kHeaderSize;
            for (const auto& n : id_to_name_) valid_end_ += 2u + n.size() + 4u;
        }

        wf_.close_quiet();
        woff_ = 0;
        persist_failed_ = false;
        // 这里曾是**全库唯一长期持有的 std::FILE***。P3 把它退成内核句柄，
        // 于是本类不再碰 CRT 的流层，`detail::adopt_stream` 也随之退役。
        //
        // 句柄仍必须由 io::open_handle 打开，理由不变（S37-6）：MSVC 的 CRT 用
        // `_SH_DENYNO` 开文件，共享位不含 FILE_SHARE_DELETE 且无开关——只要
        // FieldSchema 活着，field.schema 就删不掉（ERROR_SHARING_VIOLATION），
        // 连带整个库目录都删不掉。open_handle 恒带 SHARE_DELETE。
        //
        // **kNoAppend + 自己记偏移**，而不是 kNone 的 O_APPEND：
        //   - POSIX 下 O_APPEND 会让 pwrite **忽略 offset** 改为追加
        //     （io.hpp 的 kNoAppend 注释记的那条暗礁）；
        //   - Windows 没有 O_APPEND 的等价物，seam 的定位写走 OVERLAPPED 偏移。
        // 两边要行为一致，就只能开时定一次起点、之后自己推进。
        // kUmaskDefault 仍是为了逐字复刻 fopen 的 0666&~umask 建档权限。
        if (auto h = io::open_handle(path, io::OpenFlag::kNoAppend,
                                     io::FileMode::kUmaskDefault)) {
            io::File wf{*h};
            if (const auto sz = io::handle_size(wf.fd())) {
                // **起点是「最后一条完整 entry 的末尾」，不是文件大小。**
                //
                // 上一次进程若崩在半条 entry 上，文件尾部会留着那半条：
                // load_* 对它是「短读即停止解析」（容忍，见 load_new_format_），
                // 于是它**不在**内存序列里。若仍按文件大小续写，下一条就落到
                // 半条**之后**，文件变成 [完整条…][半条][新条]——再下次打开时
                // 解析会在半条处停住或把后面的字节读成名字，实测直接 open()
                // 返回 false（= 真损坏，caller 须中止 open，整个库打不开）。
                //
                // 这个隐患在 P3 之前（fopen "ab" + O_APPEND）就存在，形态一模
                // 一样——O_APPEND 同样从文件末尾（含半条）开始写。P3 的前缀
                // 不变式只覆盖「写失败后停止持久化」，覆盖不到这个既有状态。
                //
                // 修法：起点取 valid_end_，并把尾部多余字节截掉。截掉的一定是
                // **解析不出来的残条**（valid_end_ 由刚才那趟解析产出），不是
                // 有效数据；不截则残条会夹在新条之前，更糟。目录只读时
                // open_handle 就失败了，走不到这里，行为与既往一致。
                woff_ = valid_end_;
                if (*sz > woff_ && !io::truncate_handle(wf.fd(), woff_)) {
                    // 截不掉：宁可不持有句柄（= 本类既有的「只在内存生效」降级），
                    // 也不要在残条之后追加。
                    wf.close_quiet();
                } else {
                    wf_ = std::move(wf);  // best-effort：量不到大小就不持有
                }
            }
        }

        // 全新文件：先写 8 字节文件头，后续 intern 的 entry 才是自洽的新格式。
        if (fresh && wf_.is_open()) {
            const auto hdr = header_bytes_();
            if (!append_(std::span<const std::byte>(hdr.data(), hdr.size()))) {
                stop_persisting_();
            }
        }
        return true;
    }

    // 追加写是否已因 I/O 错误停摆（见 intern 里的说明）。停摆后本类仍可用，
    // 但只在内存中生效——盘上是内存序列的一个前缀。
    [[nodiscard]] bool persist_failed() const {
        std::shared_lock lk(mu_);
        return persist_failed_;
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
        if (wf_.is_open()) {
            // legacy（升级失败，如只读目录）→ 按旧无头格式 [len][name] 追加，
            // 保持该文件自洽；新格式则是 encode_entry_ 的 [len][name][crc]。
            // 两者都先组好整条再一次写出——原先 legacy 分支是两次 fwrite 靠
            // CRT 缓冲合并，现在直接就是一次定位写。
            const std::vector<std::byte> buf =
                legacy_ ? encode_legacy_entry_(name) : encode_entry_(name);
            if (!append_(std::span<const std::byte>(buf.data(), buf.size()))) {
                // P3：**写失败必须停止后续持久化，不能接着写下一条。**
                //
                // id 是位置性的——写侧和读侧（load_new_format_）都用
                // `id_to_name_.size()` 定序，entry 里不存 id。所以「丢掉中间
                // 一条、后面继续写」会让重启后所有后续 id 前移一位：老数据里
                // 记的 field id N 会解析成另一个字段名。这是静默的数据错位，
                // 比丢一条字段名严重得多。
                //
                // 关掉句柄后，盘上序列始终是内存序列的**前缀**，重启后已持久
                // 的那些 id 全部保持正确。这正是本类原本就接受的降级形态
                // （句柄压根开不出来时即如此），只是现在也覆盖「开着但写挂了」。
                stop_persisting_();
            }
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

    static std::array<std::byte, kHeaderSize> header_bytes_() {
        std::array<std::byte, kHeaderSize> hdr{};
        le_store_u32(hdr.data(), kMagic);
        le_store_u32(hdr.data() + 4, kVersion);
        return hdr;
    }

    // upgrade_legacy_to_new_ 仍走 AtomicFileWriter（短命 FILE*，原子写路径
    // 不在 P3 范围内），故保留这个 FILE* 版。
    static bool write_header_(std::FILE* f) {
        const auto hdr = header_bytes_();
        return std::fwrite(hdr.data(), 1, kHeaderSize, f) == kHeaderSize;
    }

    // legacy 无头格式的一条 entry：[NameLen:u16][name]，无 CRC。
    static std::vector<std::byte> encode_legacy_entry_(std::string_view name) {
        std::vector<std::byte> buf(2 + name.size());
        le_store_u16(buf.data(), static_cast<std::uint16_t>(name.size()));
        if (!name.empty()) std::memcpy(buf.data() + 2, name.data(), name.size());
        return buf;
    }

    // 定位追加。**返回值必须检查**——调用方见 intern 里的说明。
    // 调用者须持 unique_lock（woff_ 非原子）。
    [[nodiscard]] bool append_(std::span<const std::byte> b) {
        if (!wf_.is_open()) return false;
        if (b.empty()) return true;
        if (!io::pwrite_all(wf_.fd(), b.data(), b.size(), woff_)) return false;
        woff_ += b.size();
        return true;
    }

    // 写挂了：停止持久化，盘上序列就此定格为内存序列的前缀。
    void stop_persisting_() {
        wf_.close_quiet();
        persist_failed_ = true;
    }

    // 新格式解析：rf 已越过 8 字节文件头。返回 false 仅当「完整 entry 但 CRC 不符」
    // （真损坏）；torn tail（尾部读不满一条）容忍，返回 true。
    bool load_new_format_(std::FILE* rf) {
        valid_end_ = kHeaderSize;  // rf 已越过文件头
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
            valid_end_ += 2u + nlen + 4u;  // [len][name][crc]，见 encode_entry_
        }
    }

    // legacy 无头格式解析：[NameLen:u16][name] 循环（flag-day 后小端）。
    void load_legacy_(std::FILE* rf) {
        valid_end_ = 0;  // legacy 无文件头
        while (true) {
            std::byte lb[2];
            if (std::fread(lb, 1, 2, rf) != 2) break;
            const std::uint16_t nlen = le_load_u16(lb);
            std::string name(nlen, '\0');
            if (nlen > 0 && std::fread(name.data(), 1, nlen, rf) != nlen) break;
            const auto id = static_cast<std::uint32_t>(id_to_name_.size());
            name_to_id_.emplace(name, id);
            id_to_name_.push_back(std::move(name));
            valid_end_ += 2u + nlen;  // legacy 一条是 [len][name]，无 CRC
        }
    }

    // 把已加载的 legacy 映射原子重写为新格式：temp → fsync → rename 覆盖。
    // 崩溃安全：fsync 后再 rename，故要么旧文件完好（下次 open 重试升级），要么新文件完整。
    // 返回 false = 无法写（只读目录/IO 失败）→ caller 退回 legacy 追加。
    bool upgrade_legacy_to_new_() {
        // T21：原子写归 detail::AtomicFileWriter（tmp 后缀保留 .upgrade.tmp 的
        // 诊断价值）。MEM-LOW-1：encode_entry_ 内的 string 构造可抛——writer
        // 析构负责 fclose + 清 tmp，异常路径不留垃圾。
        // 原用 ::fsync，归并后统一 fdatasync：新文件的尺寸元数据属于「取回
        // 数据所必需」，fdatasync 同样保证，差别只在 mtime（无人依赖）。
        detail::AtomicFileWriter w(path_, ".upgrade.tmp");
        if (!w) return false;
        std::FILE* raw = w.get();
        if (!write_header_(raw)) return false;
        for (const auto& name : id_to_name_) {
            const auto buf = encode_entry_(name);
            if (std::fwrite(buf.data(), 1, buf.size(), raw) != buf.size()) {
                return false;
            }
        }
        return w.commit();
    }

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::uint32_t> name_to_id_;
    std::vector<std::string> id_to_name_;  // id == 下标
    std::string path_;
    // P3：内核句柄 + 自记偏移，取代原先长期持有的 std::FILE*。
    io::File      wf_;                  // append-only 写句柄（best-effort）
    std::uint64_t woff_ = 0;            // 下一次追加的偏移（持 unique_lock 推进）
    // 解析出的「最后一条完整 entry 的末尾」。与文件大小的差 = 上次崩溃留下的
    // 残条；open 据此定 woff_ 并截掉尾巴（理由见 open 里的长注释）。
    std::uint64_t valid_end_ = 0;
    bool persist_failed_ = false;       // 写挂过 → 已停止持久化，见 intern
    bool legacy_ = false;  // true = 该文件为无头 legacy 且升级失败 → intern 按旧格式追加
};

}  // namespace bitcask
