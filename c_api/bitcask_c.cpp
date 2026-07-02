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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/cask.hpp"
#include "bitcask/keydir_registry.hpp"
#include "bitcask/search_layer.hpp"
#include "bitcask/synonym_map.hpp"

namespace {

namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

// S6-P0-pre：open() 强制非空 registry。C API 是进程级 host——一个全局 registry
// 共享给所有经本 FFI 打开的句柄（即「每个共享库实例一个全局 registry」生产形态）。
// 同目录多次 open 共享同一 keydir（refcount），与既有 NIF host 语义一致。
bitcask::keydir::KeyDirRegistry& c_api_registry() {
    static bitcask::keydir::KeyDirRegistry reg;
    return reg;
}

struct bitcask_impl_t {
    std::unique_ptr<bitcask::Cask> cask;
};

struct bitcask_iter_impl_t {
    std::unique_ptr<bitcask::CaskIter> iter;
};

bitcask_error_t to_c_error_kind(bitcask::CaskError e) {
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

void to_c_error(const bitcask::CaskFault& f, bitcask_fault_t* out) {
    if (!out) return;
    out->code = to_c_error_kind(f.kind);
    out->errnum = f.errnum;
    snprintf(out->detail, BITCASK_DETAIL_MAX, "%s", f.detail.c_str());
}

// S13-M2：extern "C" 边界异常隔离。C++ 异常穿越 C 栈帧是 UB（通常直接
// terminate）；bad_alloc（含内部 string/vector 分配失败）与任何意外异常
// 在此翻译为 BITCASK_ERR_IO + fault 详情。
bitcask_error_t fault_from_exception(bitcask_fault_t* fault) noexcept {
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

void set_oom_fault(bitcask_fault_t* fault) {
    if (!fault) return;
    fault->code = BITCASK_ERR_IO;
    fault->errnum = ENOMEM;
    snprintf(fault->detail, BITCASK_DETAIL_MAX, "out of memory");
}

meta::VectorMetric to_cpp_vector_metric(bitcask_vector_metric_t m) {
    switch (m) {
        case BITCASK_VECTOR_METRIC_NONE:    return meta::VectorMetric::kNone;
        case BITCASK_VECTOR_METRIC_COSINE:  return meta::VectorMetric::kCosineNormalized;
        case BITCASK_VECTOR_METRIC_L2:      return meta::VectorMetric::kL2;
        case BITCASK_VECTOR_METRIC_DOT:     return meta::VectorMetric::kDot;
    }
    return meta::VectorMetric::kNone;
}

text::AnalyzerType to_cpp_analyzer_type(bitcask_analyzer_type_t t) {
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
bool to_search_result(bitcask::TextSearchResult&& src, bitcask_search_result_t** out) {
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

// 把 C++ 批量搜索的 n 个 expected 物化为 malloc 的 n 元结果数组（S12-5）。三种批量
// （text/vector/hybrid）返回类型相同，故共用。out_results[i]：成功=result 指针，
// 失败=NULL；fault 回填首个失败查询详情。calloc 失败返回 BITCASK_ERR_IO。
bitcask_error_t fill_batch_results(
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
bool fill_get_result(const bitcask::GetResult& src, bitcask_get_result_t* out) {
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
bool fill_iter_entry(const bitcask::CaskIter::Entry& e,
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

bool to_cpp_meta_value(const bitcask_meta_value_t& v, meta::MetaValue& out) {
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

bool to_cpp_meta_filter(const bitcask_meta_filter_t& src,
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

// S13-P8.9：从零拷贝 GetResultView 直接 malloc+memcpy（跳过 to_owned 的
// 中间 vector 拷贝）。结构与 fill_get_result 一致。
bool fill_get_result_view(const bitcask::GetResultView& src,
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

bitcask::Cask* as_cpp_cask(bitcask_t* h) {
    return reinterpret_cast<bitcask_impl_t*>(h)->cask.get();
}

bitcask::CaskIter* as_cpp_iter(bitcask_iter_t* h) {
    return reinterpret_cast<bitcask_iter_impl_t*>(h)->iter.get();
}

}

extern "C" {

BITCASK_API int bitcask_version_major(void) { return BITCASK_VERSION_MAJOR; }
BITCASK_API int bitcask_version_minor(void) { return BITCASK_VERSION_MINOR; }
BITCASK_API int bitcask_version_patch(void) { return BITCASK_VERSION_PATCH; }
BITCASK_API const char* bitcask_version_string(void) { return BITCASK_VERSION_STRING; }

BITCASK_API void bitcask_options_init(bitcask_options_t* opts) {
    if (!opts) return;
    opts->read_write = 0;
    opts->max_file_size = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    opts->max_read_handles = 0;
    opts->o_sync = 0;
    opts->sync_every_n = 0;
    opts->expiry_secs = 0;
    opts->merge_only = 0;
    opts->tombstone_version = 0;
    opts->enable_search = 0;
    opts->analyzer_type = BITCASK_ANALYZER_NONE;
    opts->analyzer_min_n = 2;
    opts->analyzer_max_n = 3;
    opts->jieba_dict_path = nullptr;
    opts->enable_stop_words = 0;
    opts->stop_words = nullptr;
    opts->min_token_length = 1;
    opts->enable_stemming = 0;
    opts->synonym_file_path = nullptr;
    opts->vector_dim = 0;
    opts->vector_metric = BITCASK_VECTOR_METRIC_NONE;
    opts->vector_quantized = 0;
    opts->vector_inmem_int8 = 0;
    opts->hnsw_m = 0;                 // S13-D11：0 = HnswConfig 默认
    opts->hnsw_ef_construction = 0;
    opts->log_fn = NULL;              // S13-D7：默认不上报
    opts->log_ctx = NULL;
}

BITCASK_API bitcask_error_t bitcask_open(const char* dirname,
                                          const bitcask_options_t* opts,
                                          bitcask_t** out,
                                          bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    bitcask::CaskOptions cpp_opts;
    if (opts) {
        cpp_opts.read_write = opts->read_write != 0;
        cpp_opts.max_file_size = opts->max_file_size;
        cpp_opts.max_read_handles = opts->max_read_handles;
        cpp_opts.o_sync = opts->o_sync != 0;
        cpp_opts.sync_every_n = opts->sync_every_n;
        cpp_opts.expiry_secs = opts->expiry_secs;
        cpp_opts.merge_only = opts->merge_only != 0;
        cpp_opts.tombstone_version = opts->tombstone_version;
        cpp_opts.vector_dim = opts->vector_dim;
        cpp_opts.vector_metric = to_cpp_vector_metric(opts->vector_metric);
        cpp_opts.vector_quantized = opts->vector_quantized != 0;
        cpp_opts.vector_inmem_int8 = opts->vector_inmem_int8 != 0;
        // S13-D7：C 函数指针 + ctx 包成 std::function（open-time 不可变）。
        if (opts->log_fn) {
            cpp_opts.log_fn =
                [fn = opts->log_fn, ctx = opts->log_ctx](
                    bitcask::CaskOptions::LogLevel lvl, std::string_view msg) {
                    // C 侧要 NUL 结尾——msg 是引擎构造的临时串，拷一份。
                    const std::string owned(msg);
                    fn(static_cast<int>(lvl), owned.c_str(), ctx);
                };
        }

        if (opts->enable_search) {
            search::SearchLayerConfig search_cfg;
            search_cfg.analyzer_config.type = to_cpp_analyzer_type(opts->analyzer_type);
            search_cfg.analyzer_config.min_n = opts->analyzer_min_n;
            search_cfg.analyzer_config.max_n = opts->analyzer_max_n;
            search_cfg.analyzer_config.enable_stop_words = opts->enable_stop_words != 0;
            search_cfg.analyzer_config.min_token_length = opts->min_token_length;
            search_cfg.analyzer_config.enable_stemming = opts->enable_stemming != 0;
            search_cfg.hnsw_m = opts->hnsw_m;                              // S13-D11
            search_cfg.hnsw_ef_construction = opts->hnsw_ef_construction;  // S13-D11
            if (opts->jieba_dict_path) {
                search_cfg.analyzer_config.dict_path = opts->jieba_dict_path;
            }
            if (opts->stop_words) {
                for (const char* const* p = opts->stop_words; *p; ++p) {
                    search_cfg.analyzer_config.stop_words.emplace_back(*p);
                }
            }
            cpp_opts.search_config = search_cfg;
            cpp_opts.enable_search = true;
            // 同义词词典：open 时一次性加载（不可变、并发安全）。文件无法打开 →
            // 干净拒绝（INVALID_OPTION），不静默忽略。
            if (opts->synonym_file_path) {
                auto sm = std::make_shared<text::SynonymMap>();
                if (!sm->load_from_file(opts->synonym_file_path)) {
                    if (fault) {
                        fault->code = BITCASK_ERR_INVALID_OPTION;
                        fault->errnum = 0;
                        snprintf(fault->detail, BITCASK_DETAIL_MAX,
                                 "synonym_file_path load failed: %s",
                                 opts->synonym_file_path);
                    }
                    return BITCASK_ERR_INVALID_OPTION;
                }
                cpp_opts.synonym_map = std::move(sm);
            }
        }
    }

    auto result = bitcask::Cask::open(dirname, cpp_opts, &c_api_registry());
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }

    // S9-P1-a：构造期用 unique_ptr 持有（异常安全），跨 C 边界前 release 转交
    // 裸句柄所有权给调用方；bitcask_close 再 adopt 回 unique_ptr 自动析构。
    auto wrapper = std::make_unique<bitcask_impl_t>();
    wrapper->cask = std::move(*result);
    *out = reinterpret_cast<bitcask_t*>(wrapper.release());
    return BITCASK_OK;
    });
}

BITCASK_API void bitcask_close(bitcask_t* cask) {
    // S13-M2：extern "C" 异常隔离（无可报告通道，吞掉）
    try {
    if (!cask) return;
    // adopt 回 unique_ptr：close() 后作用域结束自动 delete（与 open 的 release 对称）。
    std::unique_ptr<bitcask_impl_t> owned(reinterpret_cast<bitcask_impl_t*>(cask));
    owned->cask->close();
    } catch (...) {
    }
}

BITCASK_API bitcask_error_t bitcask_get(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_get_result_t** out,
                                          bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    // S13-P8.9：直接消费零拷贝 view（此前 get_owned 先 span→vector 拷一次、
    // fill_get_result 再 malloc+memcpy 一次——每次 C get 多付整份 value 拷贝）。
    auto view = as_cpp_cask(cask)->get(key_span);
    if (!view) {
        to_c_error(view.error(), fault);
        return to_c_error_kind(view.error().kind);
    }

    auto* r = static_cast<bitcask_get_result_t*>(std::malloc(sizeof(bitcask_get_result_t)));
    if (!r || !fill_get_result_view(*view, r)) {  // S13-M2：OOM 检查
        std::free(r);
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    *out = r;
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_put(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_slice_t value,
                                          uint32_t tstamp,
                                          bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    std::span<const std::byte> value_span{static_cast<const std::byte*>(value.data), value.size};
    auto result = as_cpp_cask(cask)->put(key_span, value_span, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

// S13-D5：带 per-key TTL 的写入。
BITCASK_API bitcask_error_t bitcask_put_ex(bitcask_t* cask,
                                           bitcask_slice_t key,
                                           bitcask_slice_t value,
                                           uint32_t tstamp,
                                           uint32_t expiry_at,
                                           bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    std::span<const std::byte> value_span{static_cast<const std::byte*>(value.data), value.size};
    auto result = as_cpp_cask(cask)->put(key_span, value_span, tstamp, expiry_at);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_delete(bitcask_t* cask,
                                             bitcask_slice_t key,
                                             uint32_t tstamp,
                                             bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    auto result = as_cpp_cask(cask)->remove(key_span, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_sync(bitcask_t* cask,
                                           bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    auto result = as_cpp_cask(cask)->sync();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_close_write_file(bitcask_t* cask,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    auto result = as_cpp_cask(cask)->close_write_file();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API void bitcask_get_result_free(bitcask_get_result_t* result) {
    if (!result) return;
    if (result->value.data) std::free(const_cast<void*>(result->value.data));
    if (result->meta.data) std::free(const_cast<void*>(result->meta.data));
    if (result->vector) std::free(const_cast<float*>(result->vector));
    std::free(result);
}

BITCASK_API bitcask_error_t bitcask_put_doc(bitcask_t* cask,
                                              bitcask_slice_t key,
                                              const bitcask_doc_input_t* doc,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !doc) return BITCASK_ERR_INVALID_OPTION;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    bitcask::DocInput doc_input;
    doc_input.text = {static_cast<const std::byte*>(doc->text.data), doc->text.size};
    if (doc->meta.data && doc->meta.size > 0) {
        doc_input.meta = {static_cast<const std::byte*>(doc->meta.data), doc->meta.size};
    }
    if (doc->vector && doc->vector_len > 0) {
        doc_input.vector = {doc->vector, doc->vector_len};
    }
    doc_input.expiry_at = doc->expiry_at;  // S13-D5

    auto result = as_cpp_cask(cask)->put_doc(key_span, doc_input, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_text(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_text(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_text_batch(bitcask_t* cask,
                                                      const char* const* queries,
                                                      size_t n,
                                                      size_t k,
                                                      bitcask_search_result_t*** out_results,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out_results) return BITCASK_ERR_INVALID_OPTION;
    *out_results = nullptr;
    if (n == 0) return BITCASK_OK;
    if (!queries) return BITCASK_ERR_INVALID_OPTION;

    std::vector<std::string_view> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!queries[i]) return BITCASK_ERR_INVALID_OPTION;
        qv.emplace_back(queries[i]);
    }

    // C++ 返回 n 个 expected（每查询一个结果或错误）。
    return fill_batch_results(as_cpp_cask(cask)->search_text_batch(qv, k),
                              n, out_results, fault);
    });
}

BITCASK_API void bitcask_search_result_batch_free(bitcask_search_result_t** results,
                                                  size_t n) {
    if (!results) return;
    for (size_t i = 0; i < n; ++i) bitcask_search_result_free(results[i]);
    std::free(results);
}

BITCASK_API bitcask_error_t bitcask_search_phrase(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_phrase(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_bool_search(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->bool_search(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_fields(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_fields(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_near(bitcask_t* cask,
                                                   const char* query,
                                                   uint32_t slop,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_near(query, slop, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask,
                                                     const char* query,
                                                     size_t k,
                                                     uint32_t max_edit_distance,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_fuzzy(query, k, max_edit_distance);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_wildcard(bitcask_t* cask,
                                                        const char* pattern,
                                                        size_t k,
                                                        bitcask_search_result_t** out,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !pattern || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_wildcard(pattern, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                                      const float* query,
                                                      size_t query_len,
                                                      size_t k,
                                                      size_t ef,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::span<const float> query_span{query, query_len};
    auto result = as_cpp_cask(cask)->search_vector(query_span, k, ef);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                                      const char* text_query,
                                                      const float* vec_query,
                                                      size_t vec_len,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::string_view text_sv;
    if (text_query) text_sv = text_query;
    std::span<const float> vec_span;
    if (vec_query && vec_len > 0) vec_span = {vec_query, vec_len};

    auto result = as_cpp_cask(cask)->search_hybrid(text_sv, vec_span, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_vector_batch(bitcask_t* cask,
                                                        const float* const* queries,
                                                        size_t n,
                                                        size_t query_len,
                                                        size_t k,
                                                        size_t ef,
                                                        bitcask_search_result_t*** out_results,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out_results) return BITCASK_ERR_INVALID_OPTION;
    *out_results = nullptr;
    if (n == 0) return BITCASK_OK;
    if (!queries) return BITCASK_ERR_INVALID_OPTION;

    std::vector<std::span<const float>> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!queries[i] && query_len > 0) return BITCASK_ERR_INVALID_OPTION;
        qv.emplace_back(queries[i], query_len);
    }
    return fill_batch_results(as_cpp_cask(cask)->search_vector_batch(qv, k, ef),
                              n, out_results, fault);
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid_batch(bitcask_t* cask,
                                                        const bitcask_hybrid_query_t* queries,
                                                        size_t n,
                                                        size_t k,
                                                        bitcask_search_result_t*** out_results,
                                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out_results) return BITCASK_ERR_INVALID_OPTION;
    *out_results = nullptr;
    if (n == 0) return BITCASK_OK;
    if (!queries) return BITCASK_ERR_INVALID_OPTION;

    std::vector<bitcask::Cask::HybridQuery> qv;
    qv.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        bitcask::Cask::HybridQuery hq;
        if (queries[i].text) hq.text = queries[i].text;
        if (queries[i].vector && queries[i].vector_len > 0) {
            hq.vec = std::span<const float>(queries[i].vector, queries[i].vector_len);
        }
        qv.push_back(hq);
    }
    return fill_batch_results(as_cpp_cask(cask)->search_hybrid_batch(qv, k),
                              n, out_results, fault);
    });
}

// S13-D1：批量写。
BITCASK_API bitcask_error_t bitcask_put_batch(bitcask_t* cask,
                                              const bitcask_kv_pair_t* items,
                                              size_t n,
                                              uint32_t tstamp,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    if (n == 0) return BITCASK_OK;
    if (!items) return BITCASK_ERR_INVALID_OPTION;

    std::vector<bitcask::Cask::BatchItem> batch;
    batch.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        batch.push_back(bitcask::Cask::BatchItem{
            {static_cast<const std::byte*>(items[i].key.data),
             items[i].key.size},
            {static_cast<const std::byte*>(items[i].value.data),
             items[i].value.size}});
    }
    auto result = as_cpp_cask(cask)->put_batch(batch, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

// S13-D2：带 meta 过滤的检索变体。filter==NULL 退化为无过滤；非法 filter →
// INVALID_OPTION。过滤树在调用期间转换为 C++ MetaFilter（调用返回后 C 侧
// 存储即可释放）。
BITCASK_API bitcask_error_t bitcask_search_text_filtered(
    bitcask_t* cask, const char* query, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    meta::MetaFilter mf;
    if (filter && !to_cpp_meta_filter(*filter, mf, 0)) {
        return BITCASK_ERR_INVALID_OPTION;
    }
    auto result = as_cpp_cask(cask)->search_text(query, k,
                                                 filter ? &mf : nullptr);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_vector_filtered(
    bitcask_t* cask, const float* query, size_t query_len, size_t k, size_t ef,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    meta::MetaFilter mf;
    if (filter && !to_cpp_meta_filter(*filter, mf, 0)) {
        return BITCASK_ERR_INVALID_OPTION;
    }
    std::span<const float> query_span{query, query_len};
    auto result = as_cpp_cask(cask)->search_vector(query_span, k, ef,
                                                   filter ? &mf : nullptr);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_search_hybrid_filtered(
    bitcask_t* cask, const char* text_query,
    const float* vec_query, size_t vec_len, size_t k,
    const bitcask_meta_filter_t* filter,
    bitcask_search_result_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    meta::MetaFilter mf;
    if (filter && !to_cpp_meta_filter(*filter, mf, 0)) {
        return BITCASK_ERR_INVALID_OPTION;
    }
    std::string_view text_sv;
    if (text_query) text_sv = text_query;
    std::span<const float> vec_span;
    if (vec_query && vec_len > 0) vec_span = {vec_query, vec_len};

    auto result = as_cpp_cask(cask)->search_hybrid(text_sv, vec_span, k,
                                                   filter ? &mf : nullptr);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (!to_search_result(std::move(*result), out)) {
        set_oom_fault(fault);
        return BITCASK_ERR_IO;
    }
    return BITCASK_OK;
    });
}

// 同义词词典已改为 open 时配置（bitcask_options_t::synonym_file_path）；
// 运行期 bitcask_set_synonym_map 已移除。

BITCASK_API void bitcask_search_result_free(bitcask_search_result_t* result) {
    if (!result) return;
    for (std::size_t i = 0; i < result->count; ++i) {
        std::free(result->hits[i].key);
    }
    std::free(result->hits);
    std::free(result);
}

BITCASK_API bitcask_error_t bitcask_iter_start(bitcask_t* cask,
                                                  int maxage,
                                                  int maxputs,
                                                  int see_tombstones,
                                                  bitcask_iter_t** out,
                                                  bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto iter = as_cpp_cask(cask)->make_iter();
    auto start_result = iter->start(maxage, maxputs, 0, see_tombstones != 0);
    if (!start_result) {
        if (start_result.error().kind == bitcask::CaskError::kInvalidOption) {
            return BITCASK_ERR_INVALID_OPTION;
        }
        to_c_error(start_result.error(), fault);
        return to_c_error_kind(start_result.error().kind);
    }
    if (*start_result == bitcask::keydir::StartIterResult::kOutOfDate) {
        return BITCASK_ERR_INVALID_OPTION;
    }

    // S9-P1-a：同 open——构造期 unique_ptr 持有，release 转交裸句柄给调用方。
    auto wrapper = std::make_unique<bitcask_iter_impl_t>();
    wrapper->iter = std::move(iter);
    *out = reinterpret_cast<bitcask_iter_t*>(wrapper.release());
    return BITCASK_OK;
    });
}

BITCASK_API int bitcask_iter_next(bitcask_iter_t* iter,
                                    bitcask_iter_entry_t* entry,
                                    bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!iter || !entry) return -1;

    auto result = as_cpp_iter(iter)->next();
    if (!result) {
        to_c_error(result.error(), fault);
        return -1;
    }
    if (!*result) {
        return 0;
    }

    if (!fill_iter_entry(**result, entry)) {  // S13-M2：OOM 检查
        set_oom_fault(fault);
        return -1;
    }
    return 1;
    } catch (...) {
        (void)fault_from_exception(fault);
        return -1;
    }
}

BITCASK_API int bitcask_iter_next_batch(bitcask_iter_t* iter,
                                          bitcask_iter_entry_t* entries,
                                          size_t max_n,
                                          bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!iter || !entries || max_n == 0) return -1;

    std::size_t count = 0;
    while (count < max_n) {
        auto result = as_cpp_iter(iter)->next();
        if (!result) {
            // S13-M1：中途失败时已填充的 entries[0..count-1] 持有 malloc
            // 缓冲，而契约只返回 -1、caller 无从得知已填充多少条——必须在
            // 此处释放，否则必然泄漏。
            for (std::size_t i = 0; i < count; ++i) {
                bitcask_iter_entry_free(&entries[i]);
            }
            to_c_error(result.error(), fault);
            return -1;
        }
        if (!*result) {
            break;
        }
        if (!fill_iter_entry(**result, &entries[count])) {  // S13-M2：OOM
            for (std::size_t i = 0; i < count; ++i) {
                bitcask_iter_entry_free(&entries[i]);
            }
            set_oom_fault(fault);
            return -1;
        }
        ++count;
    }
    return static_cast<int>(count);
    } catch (...) {
        (void)fault_from_exception(fault);
        return -1;
    }
}

BITCASK_API void bitcask_iter_release(bitcask_iter_t* iter) {
    // S13-M2：extern "C" 异常隔离（无可报告通道，吞掉）
    try {
    if (!iter) return;
    // adopt 回 unique_ptr：release() 后作用域结束自动 delete（与创建处 release 对称）。
    std::unique_ptr<bitcask_iter_impl_t> owned(reinterpret_cast<bitcask_iter_impl_t*>(iter));
    owned->iter->release();
    } catch (...) {
    }
}

BITCASK_API void bitcask_iter_entry_free(bitcask_iter_entry_t* entry) {
    if (!entry) return;
    if (entry->key.data) std::free(const_cast<void*>(entry->key.data));
    if (entry->value.data) std::free(const_cast<void*>(entry->value.data));
    entry->key.data = nullptr;
    entry->key.size = 0;
    entry->value.data = nullptr;
    entry->value.size = 0;
}

BITCASK_API bitcask_error_t bitcask_parallel_scan(bitcask_t* cask,
                                                  size_t n_threads,
                                                  bitcask_scan_fn fn,
                                                  void* ctx,
                                                  size_t* out_count,
                                                  bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !fn) return BITCASK_ERR_INVALID_OPTION;
    if (out_count) *out_count = 0;

    // fn+ctx 按值捕获（函数指针 + void*，平凡可拷、多线程读安全）；wrapper 无共享
    // 可变态。回调本身的线程安全由 C 消费方负责（见头文件契约）。key/value 是零拷贝
    // view，仅在本次回调内有效。
    auto result = as_cpp_cask(cask)->parallel_scan(
        n_threads,
        [fn, ctx](std::span<const std::byte> key,
                  const bitcask::GetResultView& view) {
            bitcask_slice_t k{key.data(), key.size()};
            bitcask_slice_t v{view.value.data(), view.value.size()};
            fn(ctx, k, v);
        });
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (out_count) *out_count = *result;
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_status(bitcask_t* cask,
                                             bitcask_status_t* out,
                                             bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;

    bitcask::StatusInfo info = as_cpp_cask(cask)->status();
    out->key_count = info.key_count;
    out->key_bytes = info.key_bytes;
    out->epoch = info.epoch;
    out->index_errors = info.index_errors;
    return BITCASK_OK;
    });
}

// S13-D8：扩展观测（additive）。
BITCASK_API bitcask_error_t bitcask_status_ex(bitcask_t* cask,
                                              bitcask_status_ex_t* out,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;

    bitcask::StatusInfo info = as_cpp_cask(cask)->status();
    out->key_count = info.key_count;
    out->key_bytes = info.key_bytes;
    out->epoch = info.epoch;
    out->index_errors = info.index_errors;
    out->hnsw_nodes = info.hnsw_nodes;
    out->search_cache_entries = info.search_cache_entries;
    out->read_handles = info.read_handles;
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_needs_merge(bitcask_t* cask,
                                                  bitcask_needs_merge_t* out,
                                                  bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;

    bitcask::Cask::NeedsMerge result = as_cpp_cask(cask)->needs_merge();
    out->needs = result.needs ? 1 : 0;
    out->files_count = result.files.size();
    if (result.files.empty()) {
        out->files = nullptr;
    } else {
        out->files = static_cast<char**>(std::malloc(sizeof(char*) * result.files.size()));
        if (!out->files) {  // S13-M2：OOM 检查
            out->files_count = 0;
            set_oom_fault(fault);
            return BITCASK_ERR_IO;
        }
        for (std::size_t i = 0; i < result.files.size(); ++i) {
            out->files[i] = strdup(result.files[i].c_str());
            if (!out->files[i]) {
                for (std::size_t j = 0; j < i; ++j) std::free(out->files[j]);
                std::free(out->files);
                out->files = nullptr;
                out->files_count = 0;
                set_oom_fault(fault);
                return BITCASK_ERR_IO;
            }
        }
    }
    return BITCASK_OK;
    });
}

BITCASK_API void bitcask_needs_merge_free(bitcask_needs_merge_t* nm) {
    if (!nm) return;
    if (nm->files) {
        for (std::size_t i = 0; i < nm->files_count; ++i) {
            std::free(nm->files[i]);
        }
        std::free(nm->files);
        nm->files = nullptr;
        nm->files_count = 0;
    }
}

BITCASK_API bitcask_error_t bitcask_merge(bitcask_t* cask,
                                            bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    auto result = as_cpp_cask(cask)->merge();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
    });
}

BITCASK_API int bitcask_is_empty(bitcask_t* cask) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!cask) return 1;
    return as_cpp_cask(cask)->is_empty_estimate() ? 1 : 0;
    } catch (...) {
        return 1;
    }
}

BITCASK_API int bitcask_is_frozen(bitcask_t* cask) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!cask) return 0;
    return as_cpp_cask(cask)->is_frozen() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

BITCASK_API void bitcask_flush_index(bitcask_t* cask) {
    // S13-M2：extern "C" 异常隔离（无可报告通道，吞掉）
    try {
    if (!cask) return;
    as_cpp_cask(cask)->flush_index();
    } catch (...) {
    }
}

}