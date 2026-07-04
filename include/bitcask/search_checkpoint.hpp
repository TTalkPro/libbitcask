// P14e:搜索索引分段 checkpoint 容器(`search.ckpt`)。
//
// 自描述、分段、**每段独立 CRC**;页脚最后写(tmp+rename 原子)——页脚存在且
// footerCrc 通过 = 文件结构完整。布局/语义见
// doc/recovery-unified-checkpoint-design-zh.md §3.2 与 doc/format-zh.md §十。
// 全多字节小端。本类只管**容器**(头部 watermark + 段载荷 + 页脚目录 + 逐段
// CRC),段 payload 是不透明字节(由各索引序列化器产出/消费),与序列化解耦。
//
//   头部 (16B):  "BCSC" | version u32=1 | watermark u64(覆盖 next_ord 上界)
//   段载荷区:    各段 payload 顺序拼接(位置/校验由页脚给出)
//   页脚:        directory{ sectionCount u32; 每段[type u16|flags u16|
//                  offset u64|len u64|crc32 u32] } | footerCrc u32 |
//                  dirLen u32 | trailer "BCSC"

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>  // S14-3: read_selected 的段选择谓词
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/codec.hpp"  // crc32

namespace bitcask::search {

// 段类型(与姊妹引擎 cellar 对齐)。
enum class CkptSectionType : std::uint16_t {
    kDocmap      = 1,  // 可选加速缓存:ord→key/loc/live/doc_len
    kBm25Default = 2,
    kBm25Fields  = 3,
    kHnsw        = 4,
    kMeta        = 5,  // 可选加速缓存
    kTerms       = 6,  // 可选加速缓存
    // S14-4：delta 链段型（只出现在 search.ckpt.d<seq> 文件里）。
    kBm25DefaultDelta = 7,   // InvertedIndex::serialize_delta 字节
    kBm25FieldsDelta  = 8,   // u32 count; 每字段 [u16 nameLen|name|u64 len|delta]
    kDeltaInfo        = 9,   // base_gen u64 | prev_wm u64 | seq u32（链校验）
    kDocmapDelta      = 10,  // 窗口 live 行 + 删除日志（按 ord 交错重放）
    kHnswDelta        = 11,  // 插入日志：count u64; 每条 ord u64 | f32[dim]
    kKeydirDelta      = 12,  // S14-7：keydir 元数据（"BKMD"：水位/标量/fstats）
};

// 写入用:caller 持有 payload 字节。
struct CkptSection {
    std::uint16_t type;
    std::uint16_t flags = 0;
    std::span<const std::byte> payload;
};

// 分段写入累加器（S20-1 R4）：delta/base 保存处「持有 owned payload 缓冲 +
// 并行登记引用它的 CkptSection」的公共骨架。SearchCheckpoint::write 消费 span，
// 故 payload 缓冲须活到写盘——本类接管所有权保证之。
//
// 不变量（依赖 std::vector 的移动语义）：外层 bufs_ 增长搬移内层 vector 时，
// 内层 vector 的堆缓冲指针不变，故已登记 CkptSection 的 span 不悬垂。
class SectionWriter {
public:
    // 追加一段：接管 payload 所有权，登记指向它的 CkptSection（flags=0）。
    void add(CkptSectionType type, std::vector<std::byte> payload) {
        bufs_.push_back(std::move(payload));
        secs_.push_back(CkptSection{
            static_cast<std::uint16_t>(type), 0,
            std::span<const std::byte>(bufs_.back().data(),
                                       bufs_.back().size())});
    }
    [[nodiscard]] const std::vector<CkptSection>& sections() const {
        return secs_;
    }
    [[nodiscard]] bool empty() const noexcept { return secs_.empty(); }

private:
    std::vector<std::vector<std::byte>> bufs_;  // owned payload，活到 write()
    std::vector<CkptSection>            secs_;  // span 引用 bufs_ 各元素
};

// 读取用:从文件载出的一段(payload owned;crc_ok 标记是否通过逐段校验)。
struct LoadedSection {
    std::uint16_t type;
    std::uint16_t flags;
    std::vector<std::byte> payload;
    bool crc_ok;
};

struct LoadedCheckpoint {
    std::uint64_t watermark = 0;
    std::vector<LoadedSection> sections;  // 结构内定位到的段(逐段带 crc_ok)
};

namespace detail {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};

constexpr char kCkptMagic[4] = {'B', 'C', 'S', 'C'};
constexpr std::uint32_t kCkptVersion = 1;
constexpr std::size_t kHeaderLen = 16;  // magic(4)+ver(4)+watermark(8)
constexpr std::size_t kTrailerLen = 12;  // footerCrc(4)+dirLen(4)+trailer(4)

inline void put_u16(std::vector<std::byte>& b, std::uint16_t v) {
    b.push_back(static_cast<std::byte>(v & 0xFF));
    b.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
inline void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
inline void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
inline std::uint16_t get_u16(const std::byte* p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8));
}
inline std::uint32_t get_u32(const std::byte* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t get_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace detail

class SearchCheckpoint {
public:
    // 写 header + sections + footer,tmp+rename 原子落盘。成功返回 true。
    [[nodiscard]] static bool write(std::string_view path,
                                    std::uint64_t watermark,
                                    std::span<const CkptSection> sections) {
        using namespace detail;
        std::vector<std::byte> buf;
        // 头部。
        buf.insert(buf.end(),
                   reinterpret_cast<const std::byte*>(kCkptMagic),
                   reinterpret_cast<const std::byte*>(kCkptMagic) + 4);
        put_u32(buf, kCkptVersion);
        put_u64(buf, watermark);
        // 段载荷区(记录每段 offset/len/crc 供页脚)。
        struct DirEnt { std::uint16_t type, flags; std::uint64_t off, len;
                        std::uint32_t crc; };
        std::vector<DirEnt> dir;
        dir.reserve(sections.size());
        for (const auto& s : sections) {
            const std::uint64_t off = buf.size();
            buf.insert(buf.end(), s.payload.begin(), s.payload.end());
            const std::uint32_t crc = bitcask::codec::crc32(s.payload);
            dir.push_back({s.type, s.flags, off,
                           static_cast<std::uint64_t>(s.payload.size()), crc});
        }
        // 页脚目录。
        std::vector<std::byte> d;
        put_u32(d, static_cast<std::uint32_t>(dir.size()));
        for (const auto& e : dir) {
            put_u16(d, e.type);
            put_u16(d, e.flags);
            put_u64(d, e.off);
            put_u64(d, e.len);
            put_u32(d, e.crc);
        }
        const std::uint32_t footer_crc = bitcask::codec::crc32(
            std::span<const std::byte>(d.data(), d.size()));
        buf.insert(buf.end(), d.begin(), d.end());
        put_u32(buf, footer_crc);
        put_u32(buf, static_cast<std::uint32_t>(d.size()));
        buf.insert(buf.end(),
                   reinterpret_cast<const std::byte*>(kCkptMagic),
                   reinterpret_cast<const std::byte*>(kCkptMagic) + 4);

        const std::string fp(path);
        const std::string tmp = fp + ".tmp";
        std::unique_ptr<std::FILE, FileCloser> f(std::fopen(tmp.c_str(), "wb"));
        if (!f) return false;
        const bool wrote =
            std::fwrite(buf.data(), 1, buf.size(), f.get()) == buf.size();
        f.reset();  // close before rename（须 flush OS buffer）
        if (!wrote || std::rename(tmp.c_str(), fp.c_str()) != 0) {
            std::remove(tmp.c_str());
            return false;
        }
        return true;
    }

    // S14-3:只载入 want(type) 选中的段——段级 dirty-bit 前移用（干净段原
    // 字节搬运进新 ckpt，免重序列化；脏段由调用方重建，不为其付读 I/O）。
    // 结构损坏（页脚缺失/footerCrc 失败/越界）→ nullopt；选中段逐段校验
    // CRC，失败的段不返回（调用方视作脏段重新序列化，安全收敛）。
    // 页脚解析逻辑与 read() 相同，但按目录 fseek 只读选中 payload。
    [[nodiscard]] static std::optional<std::vector<LoadedSection>>
    read_selected(std::string_view path,
                  const std::function<bool(std::uint16_t)>& want) {
        using namespace detail;
        std::unique_ptr<std::FILE, FileCloser> f(
            std::fopen(std::string(path).c_str(), "rb"));
        if (!f) return std::nullopt;
        std::fseek(f.get(), 0, SEEK_END);
        const long fsz = std::ftell(f.get());
        if (fsz < static_cast<long>(kHeaderLen + kTrailerLen)) {
            return std::nullopt;
        }
        const std::size_t n = static_cast<std::size_t>(fsz);

        std::byte head[kHeaderLen];
        std::fseek(f.get(), 0, SEEK_SET);
        if (std::fread(head, 1, kHeaderLen, f.get()) != kHeaderLen) {
            return std::nullopt;
        }
        if (std::memcmp(head, kCkptMagic, 4) != 0) return std::nullopt;
        if (get_u32(head + 4) != kCkptVersion) return std::nullopt;

        std::byte tail[kTrailerLen];
        std::fseek(f.get(), static_cast<long>(n - kTrailerLen), SEEK_SET);
        if (std::fread(tail, 1, kTrailerLen, f.get()) != kTrailerLen) {
            return std::nullopt;
        }
        if (std::memcmp(tail + 8, kCkptMagic, 4) != 0) return std::nullopt;
        const std::uint32_t dir_len = get_u32(tail + 4);
        const std::uint32_t footer_crc = get_u32(tail);
        if (static_cast<std::size_t>(dir_len) + kHeaderLen + kTrailerLen > n) {
            return std::nullopt;
        }
        const std::size_t dir_begin = n - kTrailerLen - dir_len;
        if (dir_begin < kHeaderLen) return std::nullopt;
        std::vector<std::byte> dir(dir_len);
        std::fseek(f.get(), static_cast<long>(dir_begin), SEEK_SET);
        if (std::fread(dir.data(), 1, dir_len, f.get()) != dir_len) {
            return std::nullopt;
        }
        if (bitcask::codec::crc32(
                std::span<const std::byte>(dir.data(), dir_len)) !=
            footer_crc) {
            return std::nullopt;
        }

        if (dir_len < 4) return std::nullopt;
        const std::uint32_t cnt = get_u32(dir.data());
        std::size_t p = 4;
        constexpr std::size_t kEntLen = 2 + 2 + 8 + 8 + 4;  // 24
        std::vector<LoadedSection> out;
        for (std::uint32_t i = 0; i < cnt; ++i) {
            if (p + kEntLen > dir_len) return std::nullopt;
            const std::byte* e = dir.data() + p;
            p += kEntLen;
            const std::uint16_t type = get_u16(e);
            if (!want(type)) continue;
            const std::uint16_t flags = get_u16(e + 2);
            const std::uint64_t off = get_u64(e + 4);
            const std::uint64_t len = get_u64(e + 12);
            const std::uint32_t crc = get_u32(e + 20);
            if (off < kHeaderLen || off > dir_begin ||
                len > dir_begin - off) {
                return std::nullopt;  // 目录越界 = 结构损坏。
            }
            LoadedSection ls;
            ls.type = type;
            ls.flags = flags;
            ls.payload.resize(static_cast<std::size_t>(len));
            std::fseek(f.get(), static_cast<long>(off), SEEK_SET);
            if (std::fread(ls.payload.data(), 1, ls.payload.size(), f.get()) !=
                ls.payload.size()) {
                return std::nullopt;
            }
            ls.crc_ok = bitcask::codec::crc32(std::span<const std::byte>(
                            ls.payload.data(), ls.payload.size())) == crc;
            if (!ls.crc_ok) continue;  // 坏段不搬——调用方重序列化
            out.push_back(std::move(ls));
        }
        return out;
    }

    // 读 + 校验。结构损坏(页脚缺失/footerCrc 失败/越界)→ nullopt(调用方退
    // .prev 或全量重建)。结构完整 → 返回 watermark + 各段(逐段带 crc_ok)。
    [[nodiscard]] static std::optional<LoadedCheckpoint>
    read(std::string_view path) {
        using namespace detail;
        std::unique_ptr<std::FILE, FileCloser> f(
            std::fopen(std::string(path).c_str(), "rb"));
        if (!f) return std::nullopt;
        std::fseek(f.get(), 0, SEEK_END);
        const long fsz = std::ftell(f.get());
        std::fseek(f.get(), 0, SEEK_SET);
        if (fsz < static_cast<long>(kHeaderLen + kTrailerLen)) {
            return std::nullopt;
        }
        std::vector<std::byte> buf(static_cast<std::size_t>(fsz));
        const bool rd =
            std::fread(buf.data(), 1, buf.size(), f.get()) == buf.size();
        if (!rd) return std::nullopt;

        const std::byte* base = buf.data();
        const std::size_t n = buf.size();
        // 头部 magic/version。
        if (std::memcmp(base, kCkptMagic, 4) != 0) return std::nullopt;
        if (get_u32(base + 4) != kCkptVersion) return std::nullopt;
        // 页脚(从尾倒走)。
        if (std::memcmp(base + n - 4, kCkptMagic, 4) != 0) return std::nullopt;
        const std::uint32_t dir_len = get_u32(base + n - 8);
        const std::uint32_t footer_crc = get_u32(base + n - 12);
        // directory 区间 [dir_begin, dir_begin+dir_len) 必须落在 header 与
        // trailer 之间。
        if (static_cast<std::size_t>(dir_len) + kHeaderLen + kTrailerLen > n) {
            return std::nullopt;
        }
        const std::size_t dir_begin = n - kTrailerLen - dir_len;
        if (dir_begin < kHeaderLen) return std::nullopt;
        const std::byte* d = base + dir_begin;
        if (bitcask::codec::crc32(std::span<const std::byte>(d, dir_len)) !=
            footer_crc) {
            return std::nullopt;  // 结构损坏。
        }

        LoadedCheckpoint out;
        out.watermark = get_u64(base + 8);
        if (dir_len < 4) return std::nullopt;
        const std::uint32_t cnt = get_u32(d);
        std::size_t p = 4;
        constexpr std::size_t kEntLen = 2 + 2 + 8 + 8 + 4;  // 24
        for (std::uint32_t i = 0; i < cnt; ++i) {
            if (p + kEntLen > dir_len) return std::nullopt;
            const std::byte* e = d + p;
            LoadedSection ls;
            ls.type = get_u16(e);
            ls.flags = get_u16(e + 2);
            const std::uint64_t off = get_u64(e + 4);
            const std::uint64_t len = get_u64(e + 12);
            const std::uint32_t crc = get_u32(e + 20);
            p += kEntLen;
            // payload 区间必须落在 header 与 directory 之间。
            if (off < kHeaderLen || off > dir_begin ||
                len > dir_begin - off) {
                return std::nullopt;  // 目录越界 = 结构损坏。
            }
            ls.payload.assign(base + off, base + off + len);
            ls.crc_ok = bitcask::codec::crc32(
                std::span<const std::byte>(base + off, len)) == crc;
            out.sections.push_back(std::move(ls));
        }
        return out;
    }
};

}  // namespace bitcask::search
