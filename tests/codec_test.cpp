// codec golden test: locks the on-disk byte layout of bitcask typed-record
// data records, kDoc value packing, and hint records so subsequent changes
// cannot silently drift. See doc/vector-db-design-zh.md §2.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "bitcask/codec.hpp"
#include "bitcask/format.hpp"

// V5 metadata filter §1：把 meta_codec + meta_filter 两个 header-only 模块
// 拉进来编译，强制走一遍 encode_meta → meta_lookup → MetaFilter::evaluate。
// 没有 ASSERT——只是为了确保这两个 header 的所有模板实例化都通过编译。
#include "bitcask/meta_codec.hpp"
#include "bitcask/meta_filter.hpp"

using namespace bitcask;
using namespace bitcask::format;

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::vector<std::byte> hex_to_bytes(std::string_view hex) {
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::byte>(
            (nyb(hex[i]) << 4) | nyb(hex[i + 1])));
    }
    return out;
}

std::string bytes_to_hex(std::span<const std::byte> b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(b.size() * 2);
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto v = static_cast<std::uint8_t>(b[i]);
        out[2 * i + 0] = kHex[v >> 4];
        out[2 * i + 1] = kHex[v & 0xF];
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC32: must match erlang:crc32/1 (zlib / IEEE 802.3).
// ---------------------------------------------------------------------------
TEST(Crc32, KnownVectors) {
    EXPECT_EQ(codec::crc32({}), 0u);
    EXPECT_EQ(codec::crc32(as_bytes("123456789")), 0xCBF43926u);
    EXPECT_EQ(codec::crc32(as_bytes("hello")), 0x3610A686u);
}

// ---------------------------------------------------------------------------
// Data record golden: byte-by-byte layout for known input.
//   [CRC(4)] [Type(1)] [Tstamp(8)] [Ord(8)] [KeySz(2)] [ValueSz(4)] [Key][Value]
//   CRC covers Type..Value.
// ---------------------------------------------------------------------------
TEST(DataRecord, GoldenLayout) {
    std::vector<std::byte> out;
    codec::encode_data_record(out, RecordType::kDoc, /*tstamp*/ 0x12345678,
                              /*ord*/ 1, as_bytes("k"), as_bytes("vv"));
    ASSERT_EQ(out.size(), kHeaderSize + 1 + 2);

    // Covered region (everything after the 4-byte CRC). P:小端盘格式——
    // 多字节字段低位在前。
    //   Type:    00
    //   Tstamp:  78 56 34 12 00 00 00 00   (0x12345678 LE，u64)
    //   Ord:     01 00 00 00 00 00 00 00
    //   KeySz:   01 00
    //   ValueSz: 02 00 00 00
    //   Key:     6b
    //   Value:   76 76
    auto covered = hex_to_bytes("00" "7856341200000000" "0100000000000000"
                                "0100" "02000000" "6b" "7676");
    const std::uint32_t expected_crc = codec::crc32(covered);

    EXPECT_EQ(out[0], static_cast<std::byte>(expected_crc & 0xFF));
    EXPECT_EQ(out[1], static_cast<std::byte>((expected_crc >> 8) & 0xFF));
    EXPECT_EQ(out[2], static_cast<std::byte>((expected_crc >> 16) & 0xFF));
    EXPECT_EQ(out[3], static_cast<std::byte>((expected_crc >> 24) & 0xFF));
    for (std::size_t i = 0; i < covered.size(); ++i) {
        EXPECT_EQ(out[4 + i], covered[i]) << "mismatch at byte " << (4 + i);
    }
}

// Full pinned layout (incl. CRC) — any silent format drift fails here.
// P:小端盘格式——从 covered(全小端字段)+ 计算 CRC(小端前置)独立重建期望,
// 钉死「[crc:u32 LE][type][tstamp:u64 LE][ord:u64 LE][keysz:u16 LE][valsz:u32 LE][key][val]」。
TEST(DataRecord, GoldenHex) {
    auto le_u32 = [](std::vector<std::byte>& b, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    };
    auto build = [&](std::span<const std::byte> covered) {
        std::vector<std::byte> e;
        le_u32(e, codec::crc32(covered));
        e.insert(e.end(), covered.begin(), covered.end());
        return e;
    };

    auto doc_covered = hex_to_bytes("00" "7856341200000000" "0100000000000000"
                                    "0100" "02000000" "6b" "7676");
    std::vector<std::byte> doc;
    codec::encode_data_record(doc, RecordType::kDoc, 0x12345678, 1,
                              as_bytes("k"), as_bytes("vv"));
    EXPECT_EQ(doc, build(doc_covered));

    auto tomb_covered = hex_to_bytes("01" "0700000000000000" "0900000000000000"
                                     "0100" "00000000" "6b");
    std::vector<std::byte> tomb;
    codec::encode_data_record(tomb, RecordType::kTombstone, 7, 9,
                              as_bytes("k"), {});
    EXPECT_EQ(tomb, build(tomb_covered));
}

TEST(DataRecord, RoundTrip) {
    std::vector<std::byte> out;
    const std::string key = "the-key";
    const std::string val(257, 'x');  // crosses the > 255 byte boundary
    codec::encode_data_record(out, RecordType::kDoc, 42, 0xABCDEF0123,
                              as_bytes(key), as_bytes(val));

    auto rec = codec::decode_data_record(out);
    ASSERT_TRUE(rec.has_value()) << "decode failed";
    EXPECT_EQ(rec->type, RecordType::kDoc);
    EXPECT_EQ(rec->tstamp, 42u);
    EXPECT_EQ(rec->ord, 0xABCDEF0123ull);
    EXPECT_EQ(rec->total_size, out.size());
    EXPECT_EQ(rec->key.size(), key.size());
    EXPECT_EQ(rec->value.size(), val.size());
    EXPECT_EQ(0, std::memcmp(rec->key.data(), key.data(), key.size()));
    EXPECT_EQ(0, std::memcmp(rec->value.data(), val.data(), val.size()));
}

TEST(DataRecord, TombstoneRoundTrip) {
    std::vector<std::byte> out;
    codec::encode_data_record(out, RecordType::kTombstone, 7, 9,
                              as_bytes("k"), {});
    auto rec = codec::decode_data_record(out);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->type, RecordType::kTombstone);
    EXPECT_EQ(rec->ord, 9u);
    EXPECT_TRUE(rec->value.empty());
}

TEST(DataRecord, EmptyKeyAndValue) {
    std::vector<std::byte> out;
    codec::encode_data_record(out, RecordType::kDoc, 7, 0, {}, {});
    EXPECT_EQ(out.size(), kHeaderSize);

    auto rec = codec::decode_data_record(out);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->tstamp, 7u);
    EXPECT_TRUE(rec->key.empty());
    EXPECT_TRUE(rec->value.empty());
}

TEST(DataRecord, DetectsBadCrc) {
    std::vector<std::byte> out;
    codec::encode_data_record(out, RecordType::kDoc, 1, 1,
                              as_bytes("k"), as_bytes("v"));
    out.back() = static_cast<std::byte>(static_cast<std::uint8_t>(out.back()) ^ 0x01);
    auto rec = codec::decode_data_record(out);
    ASSERT_FALSE(rec.has_value());
    EXPECT_EQ(rec.error(), codec::DecodeError::kBadCrc);
}

TEST(DataRecord, ShortBufferBeforeHeader) {
    std::vector<std::byte> tiny(5);
    auto rec = codec::decode_data_record(tiny);
    ASSERT_FALSE(rec.has_value());
    EXPECT_EQ(rec.error(), codec::DecodeError::kBufferTooShort);
}

TEST(DataRecord, ShortBufferAfterHeader) {
    std::vector<std::byte> out;
    codec::encode_data_record(out, RecordType::kDoc, 1, 1,
                              as_bytes("kkkk"), as_bytes("vvvvvv"));
    out.resize(out.size() - 3);  // truncate value
    auto rec = codec::decode_data_record(out);
    ASSERT_FALSE(rec.has_value());
    EXPECT_EQ(rec.error(), codec::DecodeError::kBufferTooShort);
}

// ---------------------------------------------------------------------------
// kDoc value packing golden (§2.4).
//   [Ver(1)] [Flags(1)] [vector: Dim(4) f32×Dim(LE)] [text: Len(4) bytes] [meta...]
// ---------------------------------------------------------------------------
TEST(DocValue, GoldenHex) {
    // vector [1.0f, 2.0f], text "hi", no meta.
    float vec[2] = {1.0f, 2.0f};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec, 2);
    parts.text   = as_bytes("hi");

    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    // 04           ver = 4（v3 + ExpiryAt u64）
    // 03           flags = has_vector|has_text
    // 82           dim = 2（varint：末字节高位=终止标记，2|0x80=0x82）
    // 0000803f     1.0f little-endian
    // 00000040     2.0f little-endian
    // 82           text len = 2（varint）
    // 6869         "hi"
    EXPECT_EQ(bytes_to_hex(out), "0403820000803f00000040826869");
}

TEST(DocValue, RoundTripAllSections) {
    float vec[3] = {0.5f, -1.5f, 3.0f};
    const std::string text = "美联储宣布降息";
    const std::array<std::byte, 3> meta = {std::byte{1}, std::byte{2}, std::byte{3}};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec, 3);
    parts.text   = as_bytes(text);
    parts.meta   = std::span<const std::byte>(meta);

    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->ver, kDocValueVersion);
    ASSERT_TRUE(v->has_vector);
    EXPECT_EQ(v->dim, 3u);
    EXPECT_EQ(v->vector_raw.size(), 3u * sizeof(float));
    float back[3];
    std::memcpy(back, v->vector_raw.data(), sizeof(back));
    EXPECT_FLOAT_EQ(back[0], 0.5f);
    EXPECT_FLOAT_EQ(back[1], -1.5f);
    EXPECT_FLOAT_EQ(back[2], 3.0f);
    ASSERT_TRUE(v->has_text);
    EXPECT_EQ(0, std::memcmp(v->text.data(), text.data(), text.size()));
    ASSERT_TRUE(v->has_meta);
    EXPECT_EQ(v->meta.size(), 3u);
}

TEST(DocValue, VectorOnly) {
    float vec[1] = {42.0f};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec, 1);
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->has_vector);
    EXPECT_FALSE(v->has_text);
    EXPECT_FALSE(v->has_meta);
    EXPECT_EQ(v->dim, 1u);
}

// P3a：量化向量段往返。encode(f32, vec_quantized) → decode → dequant，
// 误差在一个量化步长内；体积远小于 f32。
TEST(DocValue, QuantizedVectorRoundTrip) {
    std::vector<float> vec = {0.5f, -1.0f, 0.25f, 0.0f, 0.75f, -0.5f, 1.0f, -0.125f};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec.data(), vec.size());
    parts.vec_quantized = true;
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    // 量化段 ≈ varint(dim)+1+4+dim，远小于 f32 的 dim*4。
    EXPECT_LT(out.size(), 2 + 5 + vec.size() * sizeof(float));

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->vec_quantized);
    EXPECT_FALSE(v->has_vector);                    // 量化标志，非裸 f32 标志
    EXPECT_EQ(v->dim, vec.size());
    EXPECT_EQ(v->vector_raw.size(), vec.size());    // int8 codes，每元素 1 字节
    EXPECT_GT(v->vec_scale, 0.0f);

    auto recon = codec::doc_vector_f32(*v);
    ASSERT_EQ(recon.size(), vec.size());
    const float step = v->vec_scale / 127.0f;       // 对称 int8 一个量化步长
    for (std::size_t i = 0; i < vec.size(); ++i) {
        EXPECT_LE(std::abs(recon[i] - vec[i]), step + 1e-6f) << "i=" << i;
    }
}

// P3a：量化向量与 text/meta 段共存，分段定序解出正确。
TEST(DocValue, QuantizedVectorWithTextMeta) {
    std::vector<float> vec = {1.0f, 2.0f, 3.0f, 4.0f};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec.data(), vec.size());
    parts.vec_quantized = true;
    parts.text = as_bytes("hi");
    parts.meta = as_bytes("m");
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->vec_quantized);
    EXPECT_EQ(v->dim, 4u);
    EXPECT_TRUE(v->has_text);
    EXPECT_EQ(v->text.size(), 2u);
    EXPECT_TRUE(v->has_meta);
    EXPECT_EQ(v->meta.size(), 1u);
    EXPECT_EQ(codec::doc_vector_f32(*v).size(), 4u);
}

// P3a：doc_vector_f32 对未量化向量精确还原。
TEST(DocValue, DocVectorF32Unquantized) {
    std::vector<float> vec = {0.6f, 0.8f, 0.0f};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec.data(), vec.size());
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    auto recon = codec::doc_vector_f32(*v);
    ASSERT_EQ(recon.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(recon[i], vec[i]);
}

TEST(DocValue, TextOnly) {
    codec::DocValueParts parts;
    parts.text = as_bytes("plain doc");
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(v->has_vector);
    EXPECT_TRUE(v->has_text);
    EXPECT_EQ(v->text.size(), 9u);
}

TEST(DocValue, RejectsUnsupportedVersion) {
    std::vector<std::byte> out;
    out.push_back(std::byte{0xFE});  // bogus ver
    out.push_back(std::byte{0x00});  // flags
    auto v = codec::decode_doc_value(out);
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error(), codec::DecodeError::kUnsupportedVersion);
}

TEST(DocValue, DetectsTruncation) {
    float vec[4] = {1, 2, 3, 4};
    codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec, 4);
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    out.resize(out.size() - 5);  // chop into the f32 payload
    auto v = codec::decode_doc_value(out);
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error(), codec::DecodeError::kBufferTooShort);
}

// --- S8.6: DocValue v2 多字段段 ---

// 空 fields → 不设 has_fields 标记（v3 不再有 v1/v2 区分）。
TEST(DocValue, EmptyFieldsNoFlag) {
    codec::DocValueParts parts;
    parts.text = as_bytes("hello");
    // fields 默认空
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    EXPECT_EQ(static_cast<std::uint8_t>(out[0]), kDocValueVersion);  // Ver=3
    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(v->has_fields);
}

// text + 多字段 round-trip（#1）：fields 存 id，值正确、顺序保持。
TEST(DocValue, MultiFieldRoundTrip) {
    const std::string text = "default text";
    const std::string v1s = "BM25 ranking";
    const std::string v2s = "正文内容";
    codec::DocValueParts parts;
    parts.text = as_bytes(text);
    parts.fields.push_back({7, as_bytes(v1s)});   // id=7
    parts.fields.push_back({42, as_bytes(v2s)});  // id=42

    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);

    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->ver, kDocValueVersion);  // Ver=3
    ASSERT_TRUE(v->has_text);
    ASSERT_TRUE(v->has_fields);
    ASSERT_EQ(v->fields.size(), 2u);
    auto span_eq = [](std::span<const std::byte> s, const std::string& str) {
        return s.size() == str.size() && std::memcmp(s.data(), str.data(), str.size()) == 0;
    };
    EXPECT_EQ(v->fields[0].id, 7u);
    EXPECT_TRUE(span_eq(v->fields[0].value, v1s));
    EXPECT_EQ(v->fields[1].id, 42u);
    EXPECT_TRUE(span_eq(v->fields[1].value, v2s));
}

// 仅 fields、无 text。
TEST(DocValue, FieldsOnly) {
    const std::string val = "hi";
    codec::DocValueParts parts;
    parts.fields.push_back({3, as_bytes(val)});
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(v->has_text);
    ASSERT_EQ(v->fields.size(), 1u);
    EXPECT_EQ(v->fields[0].id, 3u);
}

// 大 field id 走多字节 varint，round-trip 正确。
TEST(DocValue, FieldIdMultibyteVarint) {
    const std::string val = "x";
    codec::DocValueParts parts;
    parts.fields.push_back({300, as_bytes(val)});     // 300 → 2 字节 varint
    parts.fields.push_back({1000000, as_bytes(val)}); // 大 id
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    auto v = codec::decode_doc_value(out);
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(v->fields.size(), 2u);
    EXPECT_EQ(v->fields[0].id, 300u);
    EXPECT_EQ(v->fields[1].id, 1000000u);
}

// fields 段被截断 → kBufferTooShort，不崩。
TEST(DocValue, DetectsFieldsTruncation) {
    const std::string val = "some value here";
    codec::DocValueParts parts;
    parts.fields.push_back({1, as_bytes(val)});
    std::vector<std::byte> out;
    codec::encode_doc_value(out, parts);
    out.resize(out.size() - 3);  // 砍进字段 value
    auto v = codec::decode_doc_value(out);
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error(), codec::DecodeError::kBufferTooShort);
}

// V6.4.1: vec_quantized stub 写入后读端拒绝——需 V7+ codeword 支持
// P3a：量化向量自 V7 起被读端接受（取代 V6.4.1 的 stub-reject 行为）。
// 未知 SchemeVer 仍按 kUnsupportedVersion 拒绝——构造一条 ver≠1 的码字验证。
TEST(DocValue, QuantizedAcceptedAndUnknownSchemeRejected) {
    codec::DocValueParts parts;
    parts.text = as_bytes("hello");
    parts.vec_quantized = true;
    float dummy[] = {1.0f, 0.0f, 0.0f, 0.0f};
    parts.vector = std::span<const float>(dummy, 4);

    std::vector<std::byte> buf;
    codec::encode_doc_value(buf, parts);

    auto result = codec::decode_doc_value(buf);
    ASSERT_TRUE(result.has_value());            // P3a：不再拒绝
    EXPECT_TRUE(result->vec_quantized);
    EXPECT_EQ(result->dim, 4u);
    EXPECT_TRUE(result->has_text);

    // 篡改 SchemeVer 字节（紧跟在 Dim varint 之后；此处 dim=4 → varint 单字节，
    // header 2 字节，故 SchemeVer 在下标 3）→ 未知版本应拒绝。
    auto bad = buf;
    bad[3] = static_cast<std::byte>(0xFE);
    auto bad_res = codec::decode_doc_value(bad);
    ASSERT_FALSE(bad_res.has_value());
    EXPECT_EQ(bad_res.error(), codec::DecodeError::kUnsupportedVersion);
}

// ---------------------------------------------------------------------------
// Layout constants are part of the on-disk contract.
// （v2 定宽 hint 常量已随 S33 flag-day 删除——hint 仅 v5，见下方 HintRecordV5。）
// ---------------------------------------------------------------------------
TEST(Layout, ConstantsLocked) {
    EXPECT_EQ(format::kHeaderSize, 27u);  // 4+1+8+8+2+4
    EXPECT_EQ(format::kCrcOffset, 0u);
    EXPECT_EQ(format::kTypeOffset, 4u);
    EXPECT_EQ(format::kTstampOffset, 5u);
    EXPECT_EQ(format::kOrdOffset, 13u);
    EXPECT_EQ(format::kKeySzOffset, 21u);
    EXPECT_EQ(format::kValueSzOffset, 23u);
    EXPECT_EQ(format::kHintMagicV5, 0x35484342u);      // "BCH5" LE
    EXPECT_EQ(format::kHintTrailerMagic, 0x45484342u); // "BCHE" LE
    EXPECT_EQ(format::kHintHeader, 4u);
    EXPECT_EQ(format::kHintTrailer, 8u);
    EXPECT_EQ(format::kMaxKeySize, 0xFFFFu);
    EXPECT_EQ(format::kMaxValueSize, 0xFFFFFFFFu);
    EXPECT_EQ(static_cast<std::uint8_t>(format::RecordType::kDoc), 0u);
    EXPECT_EQ(static_cast<std::uint8_t>(format::RecordType::kTombstone), 1u);
    // S35：原子批批头。
    EXPECT_EQ(static_cast<std::uint8_t>(format::RecordType::kBatchHeader), 2u);
    EXPECT_EQ(format::kBatchHeaderVersion, 1u);
    EXPECT_EQ(format::kBatchHeaderValueSize, 13u);  // 1+4+8
}

// S35：批头 value 黄金字节（锁定磁盘布局）。
// [Ver=1][Count u32 LE][SpanBytes u64 LE]
TEST(BatchHeader, GoldenBytesAndRoundtrip) {
    std::vector<std::byte> out;
    codec::encode_batch_header_value(out, {.count = 3, .span_bytes = 0x0102030405ull});
    ASSERT_EQ(out.size(), format::kBatchHeaderValueSize);
    const std::uint8_t golden[13] = {
        0x01,                          // ver
        0x03, 0x00, 0x00, 0x00,        // count = 3 LE
        0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00,  // span LE
    };
    for (std::size_t i = 0; i < sizeof(golden); ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(out[i]), golden[i]) << "byte " << i;
    }
    auto back = codec::decode_batch_header_value(out);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->count, 3u);
    EXPECT_EQ(back->span_bytes, 0x0102030405ull);

    // 拒收：错长度 / 错版本 / count==0 / span==0。
    EXPECT_FALSE(codec::decode_batch_header_value(
        std::span<const std::byte>(out.data(), 12)));
    std::vector<std::byte> bad = out;
    bad[0] = std::byte{9};
    EXPECT_FALSE(codec::decode_batch_header_value(bad));
    std::vector<std::byte> zero_count;
    codec::encode_batch_header_value(zero_count, {.count = 0, .span_bytes = 5});
    EXPECT_FALSE(codec::decode_batch_header_value(zero_count));
    std::vector<std::byte> zero_span;
    codec::encode_batch_header_value(zero_span, {.count = 1, .span_bytes = 0});
    EXPECT_FALSE(codec::decode_batch_header_value(zero_span));
}

// V3.1:vector 段黄金字节(锁定磁盘布局)。
// [Ver=4][Flags=0x01 hasVector][Dim varint=2(VByte 终止位:0x82)]
// [1.0f LE: 00 00 80 3F][2.0f LE: 00 00 00 40]
TEST(DocValue, VectorSegmentGoldenHex) {
    const float vec[2] = {1.0f, 2.0f};
    bitcask::codec::DocValueParts parts;
    parts.vector = std::span<const float>(vec, 2);
    std::vector<std::byte> out;
    bitcask::codec::encode_doc_value(out, parts);

    const std::uint8_t expect[] = {0x04, 0x01, 0x82,
                                   0x00, 0x00, 0x80, 0x3F,
                                   0x00, 0x00, 0x00, 0x40};
    ASSERT_EQ(out.size(), sizeof(expect));
    for (std::size_t i = 0; i < sizeof(expect); ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(out[i]), expect[i]) << i;
    }

    auto dv = bitcask::codec::decode_doc_value(out);
    ASSERT_TRUE(dv);
    EXPECT_TRUE(dv->has_vector);
    EXPECT_EQ(dv->dim, 2u);
    float back[2];
    std::memcpy(back, dv->vector_raw.data(), sizeof(back));
    EXPECT_FLOAT_EQ(back[0], 1.0f);
    EXPECT_FLOAT_EQ(back[1], 2.0f);
}

// ---------------------------------------------------------------------------
// V5 metadata filter §1：把 meta_codec + meta_filter 两个 header-only 模块
// 拉进编译并实际走一遍 encode/decode/lookup/evaluate。目的不是测正确性
// （后续会有专门的 meta_filter_test），而是确保这两个 header 在任何 TU 中
// 被 include 时所有模板 / std::visit 分支都通过编译。
// ---------------------------------------------------------------------------
TEST(MetaFilterCompileCheck, HeaderOnlyRoundtrip) {
    using namespace bitcask::meta;
    std::vector<MetaEntry> entries{
        {"city",    MetaValue(std::string{"sf"})},
        {"nothing", MetaValue(std::monostate{})},
        {"price",   MetaValue(std::int64_t{99})},
        {"score",   MetaValue(3.14)},
        {"vip",     MetaValue(true)},
    };
    std::vector<std::byte> buf;
    const auto wrote = encode_meta(buf, entries);
    EXPECT_EQ(wrote, buf.size());

    auto decoded = decode_meta(buf);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ((*decoded)[i].key, entries[i].key);
    }

    const std::span<const std::byte> sp(buf.data(), buf.size());
    auto price = meta_lookup(sp, "price");
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(price));
    EXPECT_EQ(std::get<std::int64_t>(price), 99);

    auto missing = meta_lookup(sp, "absent");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(missing));

    MetaFilter f;
    f.logic = MetaFilter::Logic::And;
    f.conditions.push_back(
        {"price", MetaOp::Gt, MetaValue(std::int64_t{10}), {}});
    f.conditions.push_back(
        {"vip", MetaOp::Eq, MetaValue(true), {}});
    EXPECT_TRUE(f.evaluate(sp));

    MetaFilter f2;
    f2.logic = MetaFilter::Logic::Or;
    f2.conditions.push_back(
        {"price", MetaOp::Lt, MetaValue(std::int64_t{10}), {}});
    EXPECT_FALSE(f2.evaluate(sp));

    // 嵌套子 filter：And { price > 10, child(Or { city=="sf", vip==true }) }
    auto child = std::make_unique<MetaFilter>();
    child->logic = MetaFilter::Logic::Or;
    child->conditions.push_back(
        {"city", MetaOp::Eq, MetaValue(std::string{"sf"}), {}});
    child->conditions.push_back(
        {"vip", MetaOp::Eq, MetaValue(true), {}});
    MetaFilter root;
    root.logic = MetaFilter::Logic::And;
    root.conditions.push_back(
        {"price", MetaOp::Gt, MetaValue(std::int64_t{10}), {}});
    root.children.push_back(std::move(child));
    EXPECT_TRUE(root.evaluate(sp));

    // 类型不匹配的 Eq：stored=int64, value=string → false（visit 类型分支）
    MetaFilter f3;
    f3.conditions.push_back(
        {"price", MetaOp::Eq, MetaValue(std::string{"99"}), {}});
    EXPECT_FALSE(f3.evaluate(sp));

    // In 操作：types match 时命中、type 不匹配时按 Eq 规则返回 false。
    MetaFilter f4;
    MetaCondition in_cond;
    in_cond.key = "price";
    in_cond.op  = MetaOp::In;
    in_cond.values = {MetaValue(std::int64_t{1}),
                      MetaValue(std::int64_t{99}),
                      MetaValue(std::int64_t{100})};
    f4.conditions.push_back(in_cond);
    EXPECT_TRUE(f4.evaluate(sp));

    MetaFilter f5;
    MetaCondition in_cond2;
    in_cond2.key = "price";
    in_cond2.op  = MetaOp::In;
    in_cond2.values = {MetaValue(std::string{"x"}),
                       MetaValue(std::string{"y"})};
    f5.conditions.push_back(in_cond2);
    EXPECT_FALSE(f5.evaluate(sp));

    // Exists：忽略 value，看 key 是否出现。
    MetaFilter f6;
    MetaCondition ex;
    ex.key = "city";
    ex.op  = MetaOp::Exists;
    f6.conditions.push_back(ex);
    EXPECT_TRUE(f6.evaluate(sp));

    MetaFilter f7;
    MetaCondition ex2;
    ex2.key = "missing";
    ex2.op  = MetaOp::Exists;
    f7.conditions.push_back(ex2);
    EXPECT_FALSE(f7.evaluate(sp));

    // 空 filter：恒 true。
    MetaFilter empty;
    EXPECT_TRUE(empty.evaluate(sp));
}

// ---------------------------------------------------------------------------
// Hint record v5（S33 flag-day：vbyte + gap/ord 双差分；文件级布局见
// format.hpp）。
// ---------------------------------------------------------------------------
namespace {
std::string hint_key_str(std::span<const std::byte> k) {
    return {reinterpret_cast<const char*>(k.data()), k.size()};
}
}  // namespace

TEST(HintRecordV5, GoldenLayoutContiguous) {
    std::vector<std::byte> out;
    // 连续追加（offset == prev_end=0，ord=5 相对 prev_ord=0）：gap=0(1B) +
    // total_sz=0x10(1B) + keysz<<1|0=4(1B) + ord_delta=5(1B) + tstamp 8B +
    // "ab" = 14B。
    std::uint64_t prev_end = 0, prev_ord = 0;
    codec::encode_hint_record_v5(out, 0xDEADBEEF, 0x10, 0, false,
                                 as_bytes("ab"), /*ord=*/5,
                                 prev_end, prev_ord);
    EXPECT_EQ(prev_end, 0x10u);
    EXPECT_EQ(prev_ord, 5u);
    auto expected = hex_to_bytes("80" "90" "84" "85"
                                 "efbeadde00000000" "6162");
    ASSERT_EQ(out.size(), expected.size());
    EXPECT_EQ(bytes_to_hex(out), bytes_to_hex(expected));

    std::uint64_t d_end = 0, d_ord = 0;
    auto rec = codec::decode_hint_record_v5(out, d_end, d_ord);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->tstamp, 0xDEADBEEFu);
    EXPECT_EQ(rec->total_sz, 0x10u);
    EXPECT_EQ(rec->offset, 0u);
    EXPECT_EQ(rec->ord, 5u);
    EXPECT_FALSE(rec->tombstone);
    EXPECT_EQ(hint_key_str(rec->key), "ab");
    EXPECT_EQ(rec->consumed, out.size());
    EXPECT_EQ(d_end, 0x10u);
    EXPECT_EQ(d_ord, 5u);
}

TEST(HintRecordV5, TombstoneBitAndGap) {
    std::vector<std::byte> out;
    // 非零 gap（offset=100, prev_end=64 → gap=36）+ tomb 位 + ord 差分。
    std::uint64_t prev_end = 64, prev_ord = 41;
    codec::encode_hint_record_v5(out, 7, 22, 100, true, as_bytes("k"),
                                 /*ord=*/42, prev_end, prev_ord);
    EXPECT_EQ(prev_end, 122u);
    EXPECT_EQ(prev_ord, 42u);
    std::uint64_t d_end = 64, d_ord = 41;
    auto rec = codec::decode_hint_record_v5(out, d_end, d_ord);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->offset, 100u);
    EXPECT_EQ(rec->ord, 42u);
    EXPECT_TRUE(rec->tombstone);
    EXPECT_EQ(d_end, 122u);
}

TEST(HintRecordV5, WraparoundOffsetAndOrdRegression) {
    // offset < prev_end / ord < prev_ord（merge 输出等场景不应出现，但语义
    // 须无损）：u64 二补数回绕还原，正确性不依赖单调性。
    std::vector<std::byte> out;
    std::uint64_t prev_end = 100, prev_ord = 50;
    codec::encode_hint_record_v5(out, 1, 8, /*offset=*/16, false,
                                 as_bytes("x"), /*ord=*/3,
                                 prev_end, prev_ord);
    std::uint64_t d_end = 100, d_ord = 50;
    auto rec = codec::decode_hint_record_v5(out, d_end, d_ord);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->offset, 16u);
    EXPECT_EQ(rec->ord, 3u);
    EXPECT_EQ(d_end, 24u);
    EXPECT_EQ(d_ord, 3u);
}

TEST(HintRecordV5, StreamRoundTripChainsDeltas) {
    // 三条流式串联：offset 连续、ord 递增——差分状态跨条传递。
    std::vector<std::byte> out;
    std::uint64_t prev_end = 0, prev_ord = 0;
    codec::encode_hint_record_v5(out, 1, 100, 0,   false, as_bytes("a"),
                                 10, prev_end, prev_ord);
    codec::encode_hint_record_v5(out, 2, 200, 100, true,  as_bytes("bb"),
                                 11, prev_end, prev_ord);
    codec::encode_hint_record_v5(out, 3, 300, 300, false, as_bytes("ccc"),
                                 12, prev_end, prev_ord);

    std::span<const std::byte> rest = out;
    std::uint64_t d_end = 0, d_ord = 0;
    auto r1 = codec::decode_hint_record_v5(rest, d_end, d_ord);
    ASSERT_TRUE(r1);
    rest = rest.subspan(r1->consumed);
    auto r2 = codec::decode_hint_record_v5(rest, d_end, d_ord);
    ASSERT_TRUE(r2);
    rest = rest.subspan(r2->consumed);
    auto r3 = codec::decode_hint_record_v5(rest, d_end, d_ord);
    ASSERT_TRUE(r3);
    rest = rest.subspan(r3->consumed);
    EXPECT_TRUE(rest.empty());

    EXPECT_EQ(r1->ord, 10u); EXPECT_EQ(r1->offset, 0u);
    EXPECT_EQ(r2->ord, 11u); EXPECT_EQ(r2->offset, 100u);
    EXPECT_TRUE(r2->tombstone);
    EXPECT_EQ(r3->ord, 12u); EXPECT_EQ(r3->offset, 300u);
}

TEST(HintRecordV5, ShortBufferReturnsTooShort) {
    std::vector<std::byte> out;
    std::uint64_t prev_end = 0, prev_ord = 0;
    codec::encode_hint_record_v5(out, 1, 30, 0, false, as_bytes("abc"),
                                 7, prev_end, prev_ord);
    for (std::size_t cut = 0; cut < out.size(); ++cut) {
        std::uint64_t d_end = 0, d_ord = 0;
        auto rec = codec::decode_hint_record_v5(
            std::span<const std::byte>(out.data(), cut), d_end, d_ord);
        ASSERT_FALSE(rec.has_value()) << "cut=" << cut;
        EXPECT_EQ(rec.error(), codec::DecodeError::kBufferTooShort);
    }
}
