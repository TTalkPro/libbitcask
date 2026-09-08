// C API 内部共享助手（S19-5 自 bitcask_c.cpp 抽出）——句柄转换、
// 错误/结果翻译、extern "C" 异常隔离。三个实现 TU（bitcask_kv.cpp /
// bitcask_text.cpp / bitcask_vec.cpp）共用。
#pragma once

#include "bitcask_c.h"

// 版本单一真源（S12-7）：由 CMake configure_file 从 project(VERSION) 生成。
// 非 CMake 构建（少见）时回退到占位，避免编译失败。
#if __has_include("bitcask_version.h")
#  include "bitcask_version.h"
#endif
#ifndef BITCASK_VERSION_MAJOR
#  define BITCASK_VERSION_MAJOR  0
#  define BITCASK_VERSION_MINOR  0
#  define BITCASK_VERSION_PATCH  0
#  define BITCASK_VERSION_STRING "0.0.0-unknown"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/cask.hpp"
#include "bitcask/keydir_registry.hpp"
#include "bitcask/meta_codec.hpp"   // S39：meta blob 编解码（C 侧 lookup/iter）
#include "bitcask/search_config.hpp"  // S19-3：shim 已离开产品库
#include "bitcask/synonym_map.hpp"


// 助手仅供三个 C API TU 内部使用——显式 hidden，杜绝 mangled 符号泄进
// 动态表（原匿名 namespace 的内链接语义等效替代；具名 namespace 保证
// c_api_registry() 的静态局部跨 TU 单实例）。
//
// S37-4：加编译器守卫而非按 S37-3.5 原计划直接删除。删除会**改 Linux 行为**：
// bitcask_shared 是唯一不链 bitcask_warnings 的目标（见 CMakeLists），故它拿
// 不到 -fvisibility=hidden，这条 pragma 是这些助手符号唯一的隐藏来源。
// MSVC 侧无需对应物——Windows 默认不导出，只有 BITCASK_API 的 dllexport 进
// 导出表；不加守卫则每个 C API TU 刷一条 C4068「无法识别的 pragma」。
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(hidden)
#endif
namespace bitcask::capi {


namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

// S6-P0-pre：open() 强制非空 registry。C API 是进程级 host——一个全局 registry
// 共享给所有经本 FFI 打开的句柄（即「每个共享库实例一个全局 registry」生产形态）。
// 同目录多次 open 共享同一 keydir（refcount），与既有 NIF host 语义一致。
inline bitcask::keydir::KeyDirRegistry& c_api_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

struct bitcask_impl_t {
    std::unique_ptr<bitcask::Cask> cask;
};

struct bitcask_iter_impl_t {
    std::unique_ptr<bitcask::CaskIter> iter;
};

// S33-6：range 迭代器句柄（CaskRangeIter 无 release()——析构即释放，
// 见 cask.hpp 类注释；包装层与 iter 对称，便于将来加内部状态）。
struct bitcask_range_iter_impl_t {
    std::unique_ptr<bitcask::CaskRangeIter> iter;
};

inline bitcask_error_t to_c_error_kind(bitcask::CaskError e) {
    switch (e) {
        case bitcask::CaskError::kIo:               return BITCASK_ERR_IO;
        case bitcask::CaskError::kBadCrc:           return BITCASK_ERR_BAD_CRC;
        case bitcask::CaskError::kNotFound:         return BITCASK_ERR_NOT_FOUND;
        case bitcask::CaskError::kKeyTooLarge:      return BITCASK_ERR_KEY_TOO_LARGE;
        case bitcask::CaskError::kValueTooLarge:    return BITCASK_ERR_VALUE_TOO_LARGE;
        case bitcask::CaskError::kAlreadyExists:    return BITCASK_ERR_ALREADY_EXISTS;
        case bitcask::CaskError::kReadOnly:         return BITCASK_ERR_READ_ONLY;
        case bitcask::CaskError::kWriteLocked:      return BITCASK_ERR_WRITE_LOCKED;
        case bitcask::CaskError::kInvalidOption:    return BITCASK_ERR_INVALID_OPTION;
        case bitcask::CaskError::kNoIndex:          return BITCASK_ERR_NO_INDEX;
        case bitcask::CaskError::kModeMismatch:     return BITCASK_ERR_MODE_MISMATCH;
        case bitcask::CaskError::kAnalyzerMismatch: return BITCASK_ERR_ANALYZER_MISMATCH;
        case bitcask::CaskError::kClosed:           return BITCASK_ERR_CLOSED;
        case bitcask::CaskError::kIndexRebuildFailed:
            return BITCASK_ERR_INDEX_REBUILD_FAILED;
    }
    return BITCASK_ERR_IO;
}

inline void to_c_error(const bitcask::CaskFault& f, bitcask_fault_t* out) {
    if (!out) return;
    out->code = to_c_error_kind(f.kind);
    out->errnum = f.errnum;
    snprintf(out->detail, BITCASK_DETAIL_MAX, "%s", f.detail.c_str());
}

// S13-M2：extern "C" 边界异常隔离。C++ 异常穿越 C 栈帧是 UB（通常直接
// terminate）；bad_alloc（含内部 string/vector 分配失败）与任何意外异常
// 在此翻译为 BITCASK_ERR_IO + fault 详情。
inline bitcask_error_t fault_from_exception(bitcask_fault_t* fault) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        if (fault) {
            fault->code = BITCASK_ERR_IO;
            fault->errnum = ENOMEM;
            snprintf(fault->detail, BITCASK_DETAIL_MAX, "out of memory");
        }
    } catch (const std::exception& e) {
        if (fault) {
            fault->code = BITCASK_ERR_IO;
            fault->errnum = 0;
            snprintf(fault->detail, BITCASK_DETAIL_MAX,
                     "unexpected exception: %s", e.what());
        }
    } catch (...) {
        if (fault) {
            fault->code = BITCASK_ERR_IO;
            fault->errnum = 0;
            snprintf(fault->detail, BITCASK_DETAIL_MAX,
                     "unexpected exception");
        }
    }
    return BITCASK_ERR_IO;
}

template <typename Fn>
bitcask_error_t guarded(bitcask_fault_t* fault, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (...) {
        return fault_from_exception(fault);
    }
}

inline void set_oom_fault(bitcask_fault_t* fault) {
    if (!fault) return;
    fault->code = BITCASK_ERR_IO;
    fault->errnum = ENOMEM;
    snprintf(fault->detail, BITCASK_DETAIL_MAX, "out of memory");
}

inline meta::VectorEngine to_cpp_vector_engine(bitcask_vector_engine_t e) {
    switch (e) {
        case BITCASK_VECTOR_ENGINE_HNSW:    return meta::VectorEngine::kHnsw;
        case BITCASK_VECTOR_ENGINE_IVFRQ:   return meta::VectorEngine::kIvfRq;
        case BITCASK_VECTOR_ENGINE_DISKANN: return meta::VectorEngine::kDiskann;
    }
    return meta::VectorEngine::kHnsw;
}

inline meta::VectorMetric to_cpp_vector_metric(bitcask_vector_metric_t m) {
    switch (m) {
        case BITCASK_VECTOR_METRIC_NONE:    return meta::VectorMetric::kNone;
        case BITCASK_VECTOR_METRIC_COSINE:  return meta::VectorMetric::kCosineNormalized;
        case BITCASK_VECTOR_METRIC_L2:      return meta::VectorMetric::kL2;
        case BITCASK_VECTOR_METRIC_DOT:     return meta::VectorMetric::kDot;
    }
    return meta::VectorMetric::kNone;
}

inline text::AnalyzerType to_cpp_analyzer_type(bitcask_analyzer_type_t t) {
    switch (t) {
        case BITCASK_ANALYZER_NONE:      return text::AnalyzerType::Ngram;
        case BITCASK_ANALYZER_NGRAM:     return text::AnalyzerType::Ngram;
        case BITCASK_ANALYZER_WHITESPACE: return text::AnalyzerType::Whitespace;
        case BITCASK_ANALYZER_JIEBA:     return text::AnalyzerType::Jieba;
    }
    return text::AnalyzerType::Ngram;
}

// S13-M2：malloc/strdup 检查——OOM 时释放半成品并返回 false（此前直接
// 解引用 nullptr）。失败时 *out 保持 NULL。
inline bool to_search_result(bitcask::TextSearchResult&& src, bitcask_search_result_t** out) {
    auto* r = static_cast<bitcask_search_result_t*>(std::malloc(sizeof(bitcask_search_result_t)));
    if (!r) return false;
    r->count = src.hits.size();
    r->hits = static_cast<bitcask_search_hit_t*>(std::malloc(sizeof(bitcask_search_hit_t) * (r->count ? r->count : 1)));
    if (!r->hits) {
        std::free(r);
        return false;
    }
    for (std::size_t i = 0; i < r->count; ++i) {
        r->hits[i].key = strdup(src.hits[i].key.c_str());
        if (!r->hits[i].key) {
            for (std::size_t j = 0; j < i; ++j) std::free(r->hits[j].key);
            std::free(r->hits);
            std::free(r);
            return false;
        }
        r->hits[i].ord = src.hits[i].ord;
        r->hits[i].score = src.hits[i].score;
    }
    *out = r;
    return true;
}

// 单结果检索尾块（S20-1 R1）：C++ expected → C 结果物化 + 错误/OOM 翻译。
// 12 个单查询入口（text ×8 / vec ×4）共用；caller 已做参数校验并置 *out=NULL。
inline bitcask_error_t finish_single(
    std::expected<bitcask::TextSearchResult, bitcask::CaskFault>&& result,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
}

// S39：高亮结果物化。形状同 to_search_result，多一层片段数组——每个 hit 的
// key 与每个片段的 text 都是独立 strdup，故 OOM 回滚要逐层放。失败时 *out
// 保持 NULL 且不泄漏任何半成品。
inline void free_hit_ex_range(bitcask_search_hit_ex_t* hits, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        std::free(hits[i].key);
        for (std::size_t j = 0; j < hits[i].highlights_count; ++j) {
            std::free(hits[i].highlights[j].text);
        }
        std::free(hits[i].highlights);
    }
}

inline bool to_search_result_ex(bitcask::Cask::HighlightSearchResult&& src,
                                bitcask_search_result_ex_t** out) {
    auto* r = static_cast<bitcask_search_result_ex_t*>(
        std::malloc(sizeof(bitcask_search_result_ex_t)));
    if (!r) return false;
    r->count = src.hits.size();
    r->hits = static_cast<bitcask_search_hit_ex_t*>(
        std::malloc(sizeof(bitcask_search_hit_ex_t) * (r->count ? r->count : 1)));
    if (!r->hits) {
        std::free(r);
        return false;
    }
    for (std::size_t i = 0; i < r->count; ++i) {
        auto& dst = r->hits[i];
        auto& hit = src.hits[i];
        // 先清零：任何一步失败时 free_hit_ex_range 只会看到已初始化的字段。
        dst.key = nullptr;
        dst.ord = hit.ord;
        dst.score = hit.score;
        dst.highlights = nullptr;
        dst.highlights_count = 0;

        dst.key = strdup(hit.key.c_str());
        if (!dst.key) {
            free_hit_ex_range(r->hits, i);
            std::free(r->hits);
            std::free(r);
            return false;
        }
        const std::size_t ns = hit.highlights.size();
        if (ns > 0) {
            dst.highlights = static_cast<bitcask_snippet_t*>(
                std::malloc(sizeof(bitcask_snippet_t) * ns));
            if (!dst.highlights) {
                free_hit_ex_range(r->hits, i + 1);  // 含本条已 strdup 的 key
                std::free(r->hits);
                std::free(r);
                return false;
            }
            for (std::size_t j = 0; j < ns; ++j) {
                dst.highlights[j].text = strdup(hit.highlights[j].text.c_str());
                dst.highlights[j].score = hit.highlights[j].score;
                if (!dst.highlights[j].text) {
                    dst.highlights_count = j;  // 只回滚已成功的 j 个
                    free_hit_ex_range(r->hits, i + 1);
                    std::free(r->hits);
                    std::free(r);
                    return false;
                }
            }
            dst.highlights_count = ns;
        }
    }
    *out = r;
    return true;
}

inline bitcask_error_t finish_single_ex(
    std::expected<bitcask::Cask::HighlightSearchResult, bitcask::CaskFault>&& result,
    bitcask_search_result_ex_t** out, bitcask_fault_t* fault) {
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result_ex(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
}

// 把 C++ 批量搜索的 n 个 expected 物化为 malloc 的 n 元结果数组（S12-5）。三种批量
// （text/vector/hybrid）返回类型相同，故共用。out_results[i]：成功=result 指针，
// 失败=NULL；fault 回填首个失败查询详情。calloc 失败返回 BITCASK_ERR_IO。
inline bitcask_error_t fill_batch_results(
    std::vector<std::expected<bitcask::TextSearchResult, bitcask::CaskFault>>&& batch,
    size_t n, bitcask_search_result_t*** out_results, bitcask_fault_t* fault) {
    auto** arr = static_cast<bitcask_search_result_t**>(
        std::calloc(n, sizeof(bitcask_search_result_t*)));
    if (!arr) return BITCASK_ERR_IO;
    bool fault_set = false;
    for (size_t i = 0; i < batch.size(); ++i) {
        if (batch[i]) {
            // S13-M2：OOM 时该槽保持 NULL（calloc 已清零），fault 回填。
            if (!to_search_result(std::move(*batch[i]), &arr[i]) &&
                !fault_set) {
                set_oom_fault(fault);
                fault_set = true;
            }
        } else {
            arr[i] = nullptr;
            if (!fault_set && fault) {
                to_c_error(batch[i].error(), fault);
                fault_set = true;
            }
        }
    }
    *out_results = arr;
    return BITCASK_OK;
}

// S13-M2：iter entry 填充公共 helper（iter_next / iter_next_batch 共用），
// malloc 检查——OOM 时释放半成品并返回 false。
inline bool fill_iter_entry(const bitcask::CaskIter::Entry& e,
                     bitcask_iter_entry_t* entry) {
    entry->key.data = nullptr;
    entry->key.size = 0;
    entry->value.data = nullptr;
    entry->value.size = 0;
    if (!e.key.empty()) {
        auto* p = std::malloc(e.key.size());
        if (!p) return false;
        std::memcpy(p, e.key.data(), e.key.size());
        entry->key.data = p;
        entry->key.size = e.key.size();
    }
    if (!e.value.empty()) {
        auto* p = std::malloc(e.value.size());
        if (!p) {
            std::free(const_cast<void*>(entry->key.data));
            entry->key.data = nullptr;
            entry->key.size = 0;
            return false;
        }
        std::memcpy(p, e.value.data(), e.value.size());
        entry->value.data = p;
        entry->value.size = e.value.size();
    }
    entry->tstamp = e.tstamp;
    entry->file_id = e.file_id;
    entry->offset = e.offset;
    entry->total_sz = e.total_sz;
    entry->is_tombstone = e.is_tombstone ? 1 : 0;
    entry->ord = e.ord;
    return true;
}

// S33-6：range entry 填充（range_iter_next / _next_batch 共用）。malloc
// 检查——OOM 时释放半成品并返回 false（同 fill_iter_entry 的纪律）。
inline bool fill_range_entry(bitcask::CaskRangeIter::Entry&& e,
                             bitcask_range_entry_t* entry) {
    entry->key.data = nullptr;
    entry->key.size = 0;
    entry->value.data = nullptr;
    entry->value.size = 0;
    if (!e.key.empty()) {
        auto* p = std::malloc(e.key.size());
        if (!p) return false;
        std::memcpy(p, e.key.data(), e.key.size());
        entry->key.data = p;
        entry->key.size = e.key.size();
    }
    if (!e.value.empty()) {
        auto* p = std::malloc(e.value.size());
        if (!p) {
            std::free(const_cast<void*>(entry->key.data));
            entry->key.data = nullptr;
            entry->key.size = 0;
            return false;
        }
        std::memcpy(p, e.value.data(), e.value.size());
        entry->value.data = p;
        entry->value.size = e.value.size();
    }
    entry->tstamp = e.tstamp;
    entry->ord = e.ord;
    return true;
}

// S13-D2：C 过滤树 → C++ MetaFilter。返回 false = 输入非法（key 为 NULL、
// STRING 缺 str、op/type 越界、嵌套深度超限）。
constexpr int kMetaFilterMaxDepth = 32;

inline bool to_cpp_meta_value(const bitcask_meta_value_t& v, meta::MetaValue& out) {
    switch (v.type) {
        case BITCASK_META_VALUE_NULL:    out = std::monostate{}; return true;
        case BITCASK_META_VALUE_BOOL:    out = (v.i64 != 0);     return true;
        case BITCASK_META_VALUE_INT64:   out = static_cast<std::int64_t>(v.i64); return true;
        case BITCASK_META_VALUE_FLOAT64: out = v.f64;            return true;
        case BITCASK_META_VALUE_STRING:
            if (!v.str) return false;
            out = std::string(v.str);
            return true;
    }
    return false;
}

inline bool to_cpp_meta_filter(const bitcask_meta_filter_t& src,
                        meta::MetaFilter& out, int depth) {
    if (depth > kMetaFilterMaxDepth) return false;
    out.logic = src.logic_or ? meta::MetaFilter::Logic::Or
                             : meta::MetaFilter::Logic::And;
    if (src.conditions_count > 0 && !src.conditions) return false;
    out.conditions.reserve(src.conditions_count);
    for (size_t i = 0; i < src.conditions_count; ++i) {
        const auto& c = src.conditions[i];
        if (!c.key) return false;
        if (static_cast<unsigned>(c.op) >
            static_cast<unsigned>(BITCASK_META_OP_EXISTS)) {
            return false;
        }
        meta::MetaCondition mc;
        mc.key = c.key;
        mc.op  = static_cast<meta::MetaOp>(c.op);
        if (mc.op == meta::MetaOp::In) {
            if (c.values_count > 0 && !c.values) return false;
            mc.values.reserve(c.values_count);
            for (size_t j = 0; j < c.values_count; ++j) {
                meta::MetaValue mv;
                if (!to_cpp_meta_value(c.values[j], mv)) return false;
                mc.values.push_back(std::move(mv));
            }
        } else if (mc.op != meta::MetaOp::Exists) {
            if (!to_cpp_meta_value(c.value, mc.value)) return false;
        }
        out.conditions.push_back(std::move(mc));
    }
    if (src.children_count > 0 && !src.children) return false;
    out.children.reserve(src.children_count);
    for (size_t i = 0; i < src.children_count; ++i) {
        auto child = std::make_unique<meta::MetaFilter>();
        if (!to_cpp_meta_filter(src.children[i], *child, depth + 1)) {
            return false;
        }
        out.children.push_back(std::move(child));
    }
    return true;
}

// filtered 检索变体前导（S20-1 R1）：C 过滤树转换三态——
// 无过滤（filter==NULL，get()=nullptr）/ 转换成功 / 非法（ok=false，
// caller 返回 INVALID_OPTION）。storage 随本对象存活，get() 不悬垂。
struct ParsedFilter {
    meta::MetaFilter storage;
    bool has = false;
    bool ok = true;
    [[nodiscard]] const meta::MetaFilter* get() const {
        return has ? &storage : nullptr;
    }
};

inline ParsedFilter parse_meta_filter(const bitcask_meta_filter_t* filter) {
    ParsedFilter r;
    if (!filter) return r;
    if (!to_cpp_meta_filter(*filter, r.storage, 0)) {
        r.ok = false;
        return r;
    }
    r.has = true;
    return r;
}

// S13-P8.9：从零拷贝 GetResultView 直接 malloc+memcpy（跳过 to_owned 的
// 中间 vector 拷贝）。结构与 fill_get_result 一致。
inline bool fill_get_result_view(const bitcask::GetResultView& src,
                          bitcask_get_result_t* out) {
    out->value.data = nullptr;
    out->value.size = 0;
    out->meta.data = nullptr;
    out->meta.size = 0;
    out->vector = nullptr;
    out->vector_len = 0;

    auto cleanup = [out]() {
        if (out->value.data) std::free(const_cast<void*>(out->value.data));
        if (out->meta.data) std::free(const_cast<void*>(out->meta.data));
        if (out->vector) std::free(const_cast<float*>(out->vector));
    };

    if (!src.value.empty()) {
        auto* p = std::malloc(src.value.size());
        if (!p) return false;
        std::memcpy(p, src.value.data(), src.value.size());
        out->value.data = p;
        out->value.size = src.value.size();
    }
    if (!src.meta.empty()) {
        auto* p = std::malloc(src.meta.size());
        if (!p) { cleanup(); return false; }
        std::memcpy(p, src.meta.data(), src.meta.size());
        out->meta.data = p;
        out->meta.size = src.meta.size();
    }
    if (!src.vector.empty()) {
        auto* p = std::malloc(sizeof(float) * src.vector.size());
        if (!p) { cleanup(); return false; }
        std::memcpy(p, src.vector.data(), sizeof(float) * src.vector.size());
        out->vector = static_cast<const float*>(p);
        out->vector_len = src.vector.size();
    }
    out->tstamp = src.tstamp;
    out->ord = src.ord;
    return true;
}

// S39：meta blob 的就地单条解析——C 侧 bitcask_meta_lookup / bitcask_meta_iter_*
// 共用的**唯一**解析器（两个入口一份代码，不各写一遍）。
//
// 格式权威定义在 include/bitcask/meta_codec.hpp 顶部：
//   [Ver:u8=1][NumEntries:varint] × { [KeyLen:varint][key][Type:u8][ValueData] }
// C++ 侧的 meta_lookup 出于热路径考虑把「解析 + 比较 + 提前退出」揉在一个循环
// 里并直接产出 owning 的 MetaValue；C 侧要的是零拷贝视图 + 可暂停的游标，故这里
// 单独实现「解析一条 + 前进」。**格式若变，两处须同步**。
//
// off 指向一条 entry 的起始（KeyLen 的第一个字节）。返回 false = blob 到此为止
// 或损坏——调用方一律按「遍历结束」处理（与 meta_lookup 的「非法即未命中」
// 一致，不向 C 侧报错）。
struct MetaEntryRaw {
    std::string_view          key;
    bitcask_meta_value_view_t value;
    std::size_t               next = 0;
};

// 小端读 8 字节（int64 / float64 共用；镜像 meta_lookup 的手写循环，
// 不假设主机字节序）。
inline std::uint64_t meta_read_u64_le(std::span<const std::byte> blob,
                                      std::size_t at) noexcept {
    std::uint64_t bits = 0;
    for (std::size_t j = 0; j < 8; ++j) {
        bits |= static_cast<std::uint64_t>(
                    static_cast<std::uint8_t>(blob[at + j]))
                << (8 * j);
    }
    return bits;
}

inline bool meta_parse_entry(std::span<const std::byte> blob, std::size_t off,
                             MetaEntryRaw& out) noexcept {
    if (off >= blob.size()) return false;

    auto kl_vr = codec::vbyte_read_checked(blob, off);
    if (!kl_vr) return false;
    const auto kl = kl_vr->first;
    const std::size_t kp = kl_vr->second;
    if (kl > blob.size() - kp) return false;
    out.key = std::string_view(reinterpret_cast<const char*>(blob.data() + kp),
                               static_cast<std::size_t>(kl));

    std::size_t cur = kp + static_cast<std::size_t>(kl);
    if (cur >= blob.size()) return false;
    const auto tag = static_cast<meta::MetaType>(
        static_cast<std::uint8_t>(blob[cur]));
    ++cur;

    out.value = bitcask_meta_value_view_t{};
    switch (tag) {
        case meta::MetaType::Null:
            out.value.type = BITCASK_META_VALUE_NULL;
            break;
        case meta::MetaType::Bool:
            if (cur >= blob.size()) return false;
            out.value.type = BITCASK_META_VALUE_BOOL;
            out.value.i64 = static_cast<std::uint8_t>(blob[cur]) != 0 ? 1 : 0;
            cur += 1;
            break;
        case meta::MetaType::Int64: {
            if (blob.size() - cur < 8) return false;
            const std::uint64_t bits = meta_read_u64_le(blob, cur);
            std::int64_t v = 0;
            std::memcpy(&v, &bits, sizeof(v));
            out.value.type = BITCASK_META_VALUE_INT64;
            out.value.i64 = v;
            cur += 8;
            break;
        }
        case meta::MetaType::Float64: {
            if (blob.size() - cur < 8) return false;
            const std::uint64_t bits = meta_read_u64_le(blob, cur);
            double v = 0.0;
            std::memcpy(&v, &bits, sizeof(v));
            out.value.type = BITCASK_META_VALUE_FLOAT64;
            out.value.f64 = v;
            cur += 8;
            break;
        }
        case meta::MetaType::String: {
            auto sl_vr = codec::vbyte_read_checked(blob, cur);
            if (!sl_vr || sl_vr->first > blob.size() - sl_vr->second) return false;
            out.value.type = BITCASK_META_VALUE_STRING;
            out.value.str.data = blob.data() + sl_vr->second;
            out.value.str.size = static_cast<std::size_t>(sl_vr->first);
            cur = sl_vr->second + static_cast<std::size_t>(sl_vr->first);
            break;
        }
        default:
            return false;  // 未知 type tag：整块按损坏处理
    }
    out.next = cur;
    return true;
}

// S25-M2:校验 C slice 的前置不变量——data 非空或 size 为零。
// 违反时 C++ span 构造为 UB（data()==nullptr && size()!=0 非法）。
inline bool slice_valid(const bitcask_slice_t& s) noexcept {
    return s.data != nullptr || s.size == 0;
}

inline bitcask::Cask* as_cpp_cask(bitcask_t* h) {
    return reinterpret_cast<bitcask_impl_t*>(h)->cask.get();
}

inline bitcask::CaskIter* as_cpp_iter(bitcask_iter_t* h) {
    return reinterpret_cast<bitcask_iter_impl_t*>(h)->iter.get();
}

inline bitcask::CaskRangeIter* as_cpp_range_iter(bitcask_range_iter_t* h) {
    return reinterpret_cast<bitcask_range_iter_impl_t*>(h)->iter.get();
}

// C slice → C++ span（空切片 → 空 span；调用方须先过 slice_valid）。
inline std::span<const std::byte> to_span(const bitcask_slice_t& s) {
    return {static_cast<const std::byte*>(s.data), s.size};
}


}  // namespace bitcask::capi
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
