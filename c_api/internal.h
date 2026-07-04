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
#include "bitcask/search_config.hpp"  // S19-3：shim 已离开产品库
#include "bitcask/synonym_map.hpp"


// 助手仅供三个 C API TU 内部使用——显式 hidden，杜绝 mangled 符号泄进
// 动态表（原匿名 namespace 的内链接语义等效替代；具名 namespace 保证
// c_api_registry() 的静态局部跨 TU 单实例）。
#pragma GCC visibility push(hidden)
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

// S13-M2：malloc 检查——OOM 时释放已分配段并返回 false（此前直接
// memcpy 到 nullptr）。
inline bool fill_get_result(const bitcask::GetResult& src, bitcask_get_result_t* out) {
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
        if (!p) {
            cleanup();
            return false;
        }
        std::memcpy(p, src.meta.data(), src.meta.size());
        out->meta.data = p;
        out->meta.size = src.meta.size();
    }

    if (!src.vector.empty()) {
        auto* p = std::malloc(sizeof(float) * src.vector.size());
        if (!p) {
            cleanup();
            return false;
        }
        std::memcpy(p, src.vector.data(), sizeof(float) * src.vector.size());
        out->vector = static_cast<const float*>(p);
        out->vector_len = src.vector.size();
    }

    out->tstamp = src.tstamp;
    out->ord = src.ord;
    return true;
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

inline bitcask::Cask* as_cpp_cask(bitcask_t* h) {
    return reinterpret_cast<bitcask_impl_t*>(h)->cask.get();
}

inline bitcask::CaskIter* as_cpp_iter(bitcask_iter_t* h) {
    return reinterpret_cast<bitcask_iter_impl_t*>(h)->iter.get();
}


}  // namespace bitcask::capi
#pragma GCC visibility pop
