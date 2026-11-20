// DocValue meta 段的「结构化 KV」二进制格式编解码（V5 metadata filter, §1）。
//
// DocValue v3 的 meta 段此前是一个不透明二进制 blob，引擎无法在不解码的情况下
// 解析或过滤。V5 起约定一个紧凑的「key-sorted KV 列表」结构，让 HNSW 过滤
// 搜索等热路径可以只读单字段（meta_lookup 二分查找）而无需全量 decode。
//
// 格式：
//   [Ver:u8=1][NumEntries:varint]
//     对每个 entry（key 升序）：
//       [KeyLen:varint][KeyBytes: UTF-8]
//       [ValueType:u8]   (0=null, 1=bool, 2=int64, 3=float64, 4=string,
//                         5=string array)
//       [ValueData]:
//         null: 0 字节
//         bool: 1 字节（0=false, 1=true）
//         int64: 8 字节小端
//         float64: 8 字节小端（IEEE 754）
//         string: [Len:varint][Bytes]
//         string array（S62）: [Count:varint] { [Len:varint][Bytes] } × Count
//           元素保持写入顺序、不去重；允许空数组。
//
// === S62 兼容纪元 ===
// 旧读端（≤ 11.2）遇到 tag 5：decode_meta 报错，**meta_lookup 把游标推到末尾——
// 排在数组 key 之后的 key 全被当成「不存在」，过滤静默出错**。所以含数组值的目录
// 必须让旧二进制打不开：写入第一条含数组值的记录之前把 bitcask.meta 懒升 v7
// （见 meta_file.hpp / designs/meta-multivalue-design-zh.md §3.2）。格式版本 Ver 不变。
//
// === 不变式 / 性能契约 ===
// - keys 必须按字典序升序排列，由 encode_meta 校验（assert），被 meta_lookup
//   用作二分查找的 key——这是 hot path 上「O(log n) 跳读」的前提。
// - 没有重复 key，由 caller 保证（encode_meta 也 assert）。
// - Type tag 同时是 fixed-size 类型（null/bool/int64/float64）的长度指示；
//   只有 string 需要额外的长度前缀。
//
// === 线程模型 ===
// 所有函数均为纯函数：只读 / append 到 caller 提供的 buffer，不分配除 caller
// 容器之外的内存。线程安全、可重入、无锁。
// - meta_lookup 是热路径（HNSW 每个被访问节点调用一次），必须尽量紧凑。

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bitcask/vbyte.hpp"

namespace bitcask::meta {

// meta 二进制格式的 value type tag——直接落盘 u8，禁止重排/重用。
enum class MetaType : std::uint8_t {
    Null        = 0,
    Bool        = 1,
    Int64       = 2,
    Float64     = 3,
    String      = 4,
    StringArray = 5,  // S62：字符串数组（含它的目录 meta ≥ v7）
};

// S62：字符串数组值。元素是 UTF-8 字符串，保持写入顺序、不去重。
using StringArray = std::vector<std::string>;

// MetaValue 用 std::variant 表达；std::monostate 对应 Null（默认构造即 Null）。
// String 持 owned 数据——decode_meta 会分配、caller 拿到后长期持有 OK。
// S62：追加 StringArray（变体下标 5，与 MetaType 数值一致）。⚠️ 对 C++ 下游是 ABI
// 变化：自己 std::visit / index() 分支的代码须补一个分支。
using MetaValue = std::variant<
    std::monostate,
    bool,
    std::int64_t,
    double,
    std::string,
    StringArray>;

// 单条 KV。encode_meta 要求 entries 已按 key 升序、无重复——调用方责任。
struct MetaEntry {
    std::string key;
    MetaValue   value;
};

// 格式版本号。改格式必须 bump 这个字节。
inline constexpr std::uint8_t kMetaFormatVersion = 1;

namespace detail {

// 把 VByte 写入 [p, p+n)，返回写入字节数。供 encode_meta 直接顺序写，
// 不再触发 vector 的 push_back realloc。
inline std::size_t vbyte_store(std::byte* p, std::uint64_t val) noexcept {
    std::size_t off = 0;
    while (val >= 128) {
        p[off++] = static_cast<std::byte>(val & 0x7Fu);
        val >>= 7;
    }
    p[off++] = static_cast<std::byte>(val | 0x80u);
    return off;
}

// 一条 entry 在 encode 时的 worst-case 字节数：含 KeyLen:varint(<=9) +
// KeyBytes + ValueType:u8 + ValueData(fixed-size 或 String 的 varint+bytes)。
inline std::size_t entry_payload_size(const MetaEntry& e) noexcept {
    std::size_t key_part = 9 + e.key.size();
    switch (e.value.index()) {
        case 0: return key_part + 1;
        case 1: return key_part + 1 + 1;
        case 2: return key_part + 1 + 8;
        case 3: return key_part + 1 + 8;
        case 4: {
            const auto& s = std::get<std::string>(e.value);
            return key_part + 1 + 9 + s.size();
        }
        case 5: {
            std::size_t n = key_part + 1 + 9;
            for (const auto& it : std::get<StringArray>(e.value)) n += 9 + it.size();
            return n;
        }
        default: return key_part + 1;
    }
}

}  // namespace detail

// 把一组 entries append 到 out 末尾。返回写入的字节数。
// 契约：entries 必须按 key 升序、无重复（assert 兜底，违反是 caller bug）。
// 线程安全：是（纯函数）；不需任何锁。
inline std::size_t encode_meta(std::vector<std::byte>& out,
                                std::span<const MetaEntry> entries) {
#ifndef NDEBUG
    for (std::size_t i = 1; i < entries.size(); ++i) {
        assert(entries[i - 1].key < entries[i].key &&
               "encode_meta: entries 必须按 key 升序排列");
    }
    for (std::size_t i = 1; i < entries.size(); ++i) {
        assert(entries[i - 1].key != entries[i].key &&
               "encode_meta: 不允许重复 key");
    }
#endif

    // 一次性 reserve worst-case 空间，避免 push_back 反复 realloc。
    std::size_t reserve = 1 + 9;  // Ver + NumEntries(varint 最坏 9)
    for (const auto& e : entries) reserve += detail::entry_payload_size(e);

    const std::size_t base = out.size();
    out.resize(base + reserve);
    std::byte* p = out.data() + base;
    std::size_t off = 0;

    p[off++] = static_cast<std::byte>(kMetaFormatVersion);
    off += detail::vbyte_store(p + off, entries.size());

    for (const auto& e : entries) {
        off += detail::vbyte_store(p + off, e.key.size());
        if (!e.key.empty()) {
            std::memcpy(p + off, e.key.data(), e.key.size());
            off += e.key.size();
        }
        switch (e.value.index()) {
            case 0: {
                p[off++] = static_cast<std::byte>(MetaType::Null);
                break;
            }
            case 1: {
                p[off++] = static_cast<std::byte>(MetaType::Bool);
                p[off++] = static_cast<std::byte>(
                    std::get<bool>(e.value) ? 1u : 0u);
                break;
            }
            case 2: {
                const std::int64_t v = std::get<std::int64_t>(e.value);
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                p[off++] = static_cast<std::byte>(MetaType::Int64);
                for (std::size_t i = 0; i < 8; ++i) {
                    p[off++] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFu);
                }
                break;
            }
            case 3: {
                const double v = std::get<double>(e.value);
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                p[off++] = static_cast<std::byte>(MetaType::Float64);
                for (std::size_t i = 0; i < 8; ++i) {
                    p[off++] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFu);
                }
                break;
            }
            case 4: {
                const auto& s = std::get<std::string>(e.value);
                p[off++] = static_cast<std::byte>(MetaType::String);
                off += detail::vbyte_store(p + off, s.size());
                if (!s.empty()) {
                    std::memcpy(p + off, s.data(), s.size());
                    off += s.size();
                }
                break;
            }
            case 5: {
                const auto& arr = std::get<StringArray>(e.value);
                p[off++] = static_cast<std::byte>(MetaType::StringArray);
                off += detail::vbyte_store(p + off, arr.size());
                for (const auto& it : arr) {
                    off += detail::vbyte_store(p + off, it.size());
                    if (!it.empty()) {
                        std::memcpy(p + off, it.data(), it.size());
                        off += it.size();
                    }
                }
                break;
            }
            default:
                assert(false && "encode_meta: 未知 MetaType，invariant 违反");
        }
    }

    out.resize(base + off);
    return off;
}

// 解码整个 blob 到 entries。失败时返回错误字符串（截断 / 非法 varint /
// 未支持 Ver / 非法 Type tag 等）。成功时 entries 按 key 升序（格式不变式）。
// 线程安全：是（纯函数，只读 buf）；不需任何锁。
inline std::expected<std::vector<MetaEntry>, std::string>
decode_meta(std::span<const std::byte> buf) {
    if (buf.size() < 2) {
        return std::unexpected("meta: buffer too short for header");
    }
    if (static_cast<std::uint8_t>(buf[0]) != kMetaFormatVersion) {
        return std::unexpected("meta: unsupported version");
    }
    auto n_vr = codec::vbyte_read_checked(buf, 1);
    if (!n_vr) return std::unexpected("meta: truncated header");
    auto pos = n_vr->second;
    const auto n = n_vr->first;

    std::vector<MetaEntry> out;
    out.reserve(static_cast<std::size_t>(n));

    for (std::uint64_t i = 0; i < n; ++i) {
        if (pos >= buf.size()) {
            return std::unexpected("meta: truncated at entry keylen");
        }
        auto klen_vr = codec::vbyte_read_checked(buf, pos);
        if (!klen_vr) return std::unexpected("meta: truncated keylen");
        const auto klen = klen_vr->first;
        pos = klen_vr->second;
        if (klen > buf.size() - pos) {
            return std::unexpected("meta: truncated key bytes");
        }
        MetaEntry e;
        e.key.assign(reinterpret_cast<const char*>(buf.data() + pos),
                     static_cast<std::size_t>(klen));
        pos += static_cast<std::size_t>(klen);

        if (pos >= buf.size()) {
            return std::unexpected("meta: truncated at value type tag");
        }
        const auto tag = static_cast<MetaType>(
            static_cast<std::uint8_t>(buf[pos++]));

        switch (tag) {
            case MetaType::Null:
                e.value = std::monostate{};
                break;
            case MetaType::Bool:
                if (pos >= buf.size()) {
                    return std::unexpected("meta: truncated bool byte");
                }
                e.value = (static_cast<std::uint8_t>(buf[pos++]) != 0);
                break;
            case MetaType::Int64: {
                if (buf.size() - pos < 8) {
                    return std::unexpected("meta: truncated int64 bytes");
                }
                std::uint64_t bits = 0;
                for (std::size_t j = 0; j < 8; ++j) {
                    bits |= static_cast<std::uint64_t>(
                                static_cast<std::uint8_t>(buf[pos + j]))
                            << (8 * j);
                }
                pos += 8;
                std::int64_t v;
                std::memcpy(&v, &bits, sizeof(v));
                e.value = v;
                break;
            }
            case MetaType::Float64: {
                if (buf.size() - pos < 8) {
                    return std::unexpected("meta: truncated float64 bytes");
                }
                std::uint64_t bits = 0;
                for (std::size_t j = 0; j < 8; ++j) {
                    bits |= static_cast<std::uint64_t>(
                                static_cast<std::uint8_t>(buf[pos + j]))
                            << (8 * j);
                }
                pos += 8;
                double v;
                std::memcpy(&v, &bits, sizeof(v));
                e.value = v;
                break;
            }
            case MetaType::String: {
                if (pos >= buf.size()) {
                    return std::unexpected("meta: truncated string len");
                }
                auto slen_vr = codec::vbyte_read_checked(buf, pos);
                if (!slen_vr) return std::unexpected("meta: truncated string len");
                const auto slen = slen_vr->first;
                pos = slen_vr->second;
                if (slen > buf.size() - pos) {
                    return std::unexpected("meta: truncated string bytes");
                }
                e.value.emplace<std::string>(
                    reinterpret_cast<const char*>(buf.data() + pos),
                    static_cast<std::size_t>(slen));
                pos += static_cast<std::size_t>(slen);
                break;
            }
            case MetaType::StringArray: {
                auto cnt_vr = codec::vbyte_read_checked(buf, pos);
                if (!cnt_vr) return std::unexpected("meta: truncated array count");
                pos = cnt_vr->second;
                // 每个元素至少 1 字节长度前缀：count 超过剩余字节必是损坏（防巨量 reserve）。
                if (cnt_vr->first > buf.size() - pos) {
                    return std::unexpected("meta: array count exceeds buffer");
                }
                StringArray arr;
                arr.reserve(static_cast<std::size_t>(cnt_vr->first));
                for (std::uint64_t j = 0; j < cnt_vr->first; ++j) {
                    auto il_vr = codec::vbyte_read_checked(buf, pos);
                    if (!il_vr || il_vr->first > buf.size() - il_vr->second) {
                        return std::unexpected("meta: truncated array item");
                    }
                    arr.emplace_back(reinterpret_cast<const char*>(buf.data() + il_vr->second),
                                     static_cast<std::size_t>(il_vr->first));
                    pos = il_vr->second + static_cast<std::size_t>(il_vr->first);
                }
                e.value = std::move(arr);
                break;
            }
            default:
                return std::unexpected("meta: unknown value type tag");
        }
        out.push_back(std::move(e));
    }
    return out;
}

// S62：一条 entry 的值在 blob 里的原始区间（零分配视图）。tag 之后、到值末尾为止；
// 数组时 bytes 从 Count 开始。found == false = key 不存在（或 blob 非法 / 截断）。
struct MetaRawValue {
    bool found = false;
    MetaType tag = MetaType::Null;
    std::span<const std::byte> bytes{};
};

namespace detail {

// 从 cur（tag 之后）跳过一个值，返回值末尾；损坏 / 未知 tag 返回 npos。
inline std::size_t skip_value(std::span<const std::byte> blob, std::size_t cur,
                              MetaType tag) noexcept {
    constexpr std::size_t npos = static_cast<std::size_t>(-1);
    switch (tag) {
        case MetaType::Null:
            return cur;
        case MetaType::Bool:
            return cur < blob.size() ? cur + 1 : npos;
        case MetaType::Int64:
        case MetaType::Float64:
            return blob.size() - cur >= 8 ? cur + 8 : npos;
        case MetaType::String: {
            auto sl = codec::vbyte_read_checked(blob, cur);
            if (!sl || sl->first > blob.size() - sl->second) return npos;
            return sl->second + static_cast<std::size_t>(sl->first);
        }
        case MetaType::StringArray: {
            auto cn = codec::vbyte_read_checked(blob, cur);
            if (!cn) return npos;
            std::size_t p = cn->second;
            for (std::uint64_t j = 0; j < cn->first; ++j) {
                auto il = codec::vbyte_read_checked(blob, p);
                if (!il || il->first > blob.size() - il->second) return npos;
                p = il->second + static_cast<std::size_t>(il->first);
            }
            return p;
        }
    }
    return npos;  // 未知 tag：无从跳过
}

}  // namespace detail

// 在 meta blob 中定位单个 key 的值区间，不解码、零分配。这是 hot path——HNSW 每访问
// 一个节点过滤时调用一次。
//
// 实现策略（S29-2）：单趟线性扫描，边解析边比较 key。变长编码下任何查找都必须从头
// 解析（无随机访问）；key 升序（格式不变式，encode_meta assert 兜底）→ 遇 entry key
// > 目标即可提前退出，平均只扫半程。
// 线程安全：是（纯函数，只读 blob）；不需任何锁。
inline MetaRawValue meta_find_raw(std::span<const std::byte> blob,
                                  std::string_view key) noexcept {
    MetaRawValue out;
    if (blob.size() < 2) return out;
    if (static_cast<std::uint8_t>(blob[0]) != kMetaFormatVersion) return out;
    auto n_vr = codec::vbyte_read_checked(blob, 1);
    if (!n_vr) return out;
    const auto n = n_vr->first;
    std::size_t cur = n_vr->second;
    for (std::uint64_t i = 0; i < n; ++i) {
        if (cur >= blob.size()) break;
        auto kl_vr = codec::vbyte_read_checked(blob, cur);
        if (!kl_vr) break;
        const auto kl = kl_vr->first;
        const std::size_t p = kl_vr->second;
        if (kl > blob.size() - p) break;
        const std::string_view ek(reinterpret_cast<const char*>(blob.data() + p),
                                  static_cast<std::size_t>(kl));
        const int cmp = ek.compare(key);
        if (cmp > 0) break;  // key 升序：后续只会更大，提前退出。
        cur = p + static_cast<std::size_t>(kl);
        if (cur >= blob.size()) break;
        const auto tag = static_cast<MetaType>(static_cast<std::uint8_t>(blob[cur]));
        ++cur;
        // 标量的跳读内联（热路径：S62 前的 meta_lookup 就是逐类型内联跳读，抽成函数
        // 调用实测慢 ~17%）；只有字符串数组走通用 skip_value。
        std::size_t end;
        switch (tag) {
            case MetaType::Null:
                end = cur;
                break;
            case MetaType::Bool:
                if (cur >= blob.size()) return out;
                end = cur + 1;
                break;
            case MetaType::Int64:
            case MetaType::Float64:
                if (blob.size() - cur < 8) return out;
                end = cur + 8;
                break;
            case MetaType::String: {
                auto sl = codec::vbyte_read_checked(blob, cur);
                if (!sl || sl->first > blob.size() - sl->second) return out;
                end = sl->second + static_cast<std::size_t>(sl->first);
                break;
            }
            case MetaType::StringArray:
                end = detail::skip_value(blob, cur, tag);
                if (end == static_cast<std::size_t>(-1)) return out;
                break;
            default:
                return out;  // 未知 tag：无从跳过
        }
        if (cmp == 0) {
            out.found = true;
            out.tag = tag;
            out.bytes = blob.subspan(cur, end - cur);
            return out;
        }
        cur = end;
    }
    return out;
}

// S62：逐个访问数组区间（meta_find_raw 返回的 StringArray bytes）里的元素，零分配。
// fn(std::string_view) 返回 true 即停止并返回 true；遍历完 / 区间损坏返回 false。
template <class Fn>
inline bool for_each_array_item(std::span<const std::byte> bytes, Fn&& fn) {
    auto cn = codec::vbyte_read_checked(bytes, 0);
    if (!cn) return false;
    std::size_t p = cn->second;
    for (std::uint64_t j = 0; j < cn->first; ++j) {
        auto il = codec::vbyte_read_checked(bytes, p);
        if (!il || il->first > bytes.size() - il->second) return false;
        const std::string_view item(reinterpret_cast<const char*>(bytes.data() + il->second),
                                    static_cast<std::size_t>(il->first));
        if (fn(item)) return true;
        p = il->second + static_cast<std::size_t>(il->first);
    }
    return false;
}

// S62：blob 里是否有字符串数组值（写入路径的 v7 纪元判定用）。只扫 tag、跳过值，
// O(entry 数)。非法 / 截断 blob 返回 false（写入侧对非法 blob 的处理不在此）。
inline bool meta_has_string_array(std::span<const std::byte> blob) noexcept {
    if (blob.size() < 2) return false;
    if (static_cast<std::uint8_t>(blob[0]) != kMetaFormatVersion) return false;
    auto n_vr = codec::vbyte_read_checked(blob, 1);
    if (!n_vr) return false;
    std::size_t cur = n_vr->second;
    for (std::uint64_t i = 0; i < n_vr->first; ++i) {
        auto kl_vr = codec::vbyte_read_checked(blob, cur);
        if (!kl_vr || kl_vr->first >= blob.size() - kl_vr->second) return false;
        cur = kl_vr->second + static_cast<std::size_t>(kl_vr->first);
        const auto tag = static_cast<MetaType>(static_cast<std::uint8_t>(blob[cur]));
        if (tag == MetaType::StringArray) return true;
        cur = detail::skip_value(blob, cur + 1, tag);
        if (cur == static_cast<std::size_t>(-1)) return false;
    }
    return false;
}

// S62：把 meta_find_raw 的结果物化成 owning 值（数组时分配）。未找到 ⇒ monostate。
inline MetaValue value_from_raw(const MetaRawValue& raw) {
    if (!raw.found) return std::monostate{};
    const auto b = raw.bytes;
    auto le64 = [&b]() {
        std::uint64_t bits = 0;
        for (std::size_t j = 0; j < 8; ++j) {
            bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b[j])) << (8 * j);
        }
        return bits;
    };
    switch (raw.tag) {
        case MetaType::Null:
            return std::monostate{};
        case MetaType::Bool:
            return static_cast<std::uint8_t>(b[0]) != 0;
        case MetaType::Int64: {
            const std::uint64_t bits = le64();
            std::int64_t v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case MetaType::Float64: {
            const std::uint64_t bits = le64();
            double v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        case MetaType::String: {
            auto sl = codec::vbyte_read_checked(b, 0);
            return std::string(reinterpret_cast<const char*>(b.data() + sl->second),
                               static_cast<std::size_t>(sl->first));
        }
        case MetaType::StringArray: {
            StringArray arr;
            for_each_array_item(b, [&arr](std::string_view it) {
                arr.emplace_back(it);
                return false;
            });
            return arr;
        }
    }
    return std::monostate{};
}

// 在 meta blob 中查找单个 key，返回 owning 值（命中数组时分配）。过滤热路径走
// meta_find_raw（MetaCondition::evaluate），不经此处。未找到返回 std::monostate。
// 线程安全：是（纯函数，只读 blob）；不需任何锁。
inline MetaValue meta_lookup(std::span<const std::byte> blob,
                             std::string_view key) {
    return value_from_raw(meta_find_raw(blob, key));
}

}  // namespace bitcask::meta
