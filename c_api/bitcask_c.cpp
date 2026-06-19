#include "bitcask_c.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bitcask/cask.hpp"
#include "bitcask/search_layer.hpp"
#include "bitcask/synonym_map.hpp"

namespace {

namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

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
    }
    return BITCASK_ERR_IO;
}

void to_c_error(const bitcask::CaskFault& f, bitcask_fault_t* out) {
    if (!out) return;
    out->code = to_c_error_kind(f.kind);
    out->errnum = f.errnum;
    snprintf(out->detail, BITCASK_DETAIL_MAX, "%s", f.detail.c_str());
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

void to_search_result(bitcask::TextSearchResult&& src, bitcask_search_result_t** out) {
    auto* r = static_cast<bitcask_search_result_t*>(std::malloc(sizeof(bitcask_search_result_t)));
    r->count = src.hits.size();
    r->hits = static_cast<bitcask_search_hit_t*>(std::malloc(sizeof(bitcask_search_hit_t) * r->count));
    for (std::size_t i = 0; i < r->count; ++i) {
        r->hits[i].key = strdup(src.hits[i].key.c_str());
        r->hits[i].ord = src.hits[i].ord;
        r->hits[i].score = src.hits[i].score;
    }
    *out = r;
}

void fill_get_result(const bitcask::GetResult& src, bitcask_get_result_t* out) {
    if (src.value.empty()) {
        out->value.data = nullptr;
        out->value.size = 0;
    } else {
        auto* p = std::malloc(src.value.size());
        std::memcpy(p, src.value.data(), src.value.size());
        out->value.data = p;
        out->value.size = src.value.size();
    }

    if (src.meta.empty()) {
        out->meta.data = nullptr;
        out->meta.size = 0;
    } else {
        auto* p = std::malloc(src.meta.size());
        std::memcpy(p, src.meta.data(), src.meta.size());
        out->meta.data = p;
        out->meta.size = src.meta.size();
    }

    if (src.vector.empty()) {
        out->vector = nullptr;
        out->vector_len = 0;
    } else {
        auto* p = std::malloc(sizeof(float) * src.vector.size());
        std::memcpy(p, src.vector.data(), sizeof(float) * src.vector.size());
        out->vector = static_cast<const float*>(p);
        out->vector_len = src.vector.size();
    }

    out->tstamp = src.tstamp;
    out->ord = src.ord;
}

bitcask::Cask* as_cpp_cask(bitcask_t* h) {
    return reinterpret_cast<bitcask_impl_t*>(h)->cask.get();
}

bitcask::CaskIter* as_cpp_iter(bitcask_iter_t* h) {
    return reinterpret_cast<bitcask_iter_impl_t*>(h)->iter.get();
}

}

extern "C" {

BITCASK_API int bitcask_version_major(void) { return 2; }
BITCASK_API int bitcask_version_minor(void) { return 2; }
BITCASK_API int bitcask_version_patch(void) { return 0; }
BITCASK_API const char* bitcask_version_string(void) { return "2.2.0"; }

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
    opts->vector_dim = 0;
    opts->vector_metric = BITCASK_VECTOR_METRIC_NONE;
    opts->vector_quantized = 0;
    opts->vector_inmem_int8 = 0;
}

BITCASK_API bitcask_error_t bitcask_open(const char* dirname,
                                          const bitcask_options_t* opts,
                                          bitcask_t** out,
                                          bitcask_fault_t* fault) {
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

        if (opts->enable_search) {
            search::SearchLayerConfig search_cfg;
            search_cfg.analyzer_config.type = to_cpp_analyzer_type(opts->analyzer_type);
            search_cfg.analyzer_config.min_n = opts->analyzer_min_n;
            search_cfg.analyzer_config.max_n = opts->analyzer_max_n;
            search_cfg.analyzer_config.enable_stop_words = opts->enable_stop_words != 0;
            search_cfg.analyzer_config.min_token_length = opts->min_token_length;
            search_cfg.analyzer_config.enable_stemming = opts->enable_stemming != 0;
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
        }
    }

    auto result = bitcask::Cask::open(dirname, cpp_opts);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }

    auto* wrapper = new bitcask_impl_t;
    wrapper->cask = std::move(*result);
    *out = reinterpret_cast<bitcask_t*>(wrapper);
    return BITCASK_OK;
}

BITCASK_API void bitcask_close(bitcask_t* cask) {
    if (!cask) return;
    as_cpp_cask(cask)->close();
    delete reinterpret_cast<bitcask_impl_t*>(cask);
}

BITCASK_API bitcask_error_t bitcask_get(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_get_result_t** out,
                                          bitcask_fault_t* fault) {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    auto result = as_cpp_cask(cask)->get_owned(key_span);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }

    auto* r = static_cast<bitcask_get_result_t*>(std::malloc(sizeof(bitcask_get_result_t)));
    fill_get_result(*result, r);
    *out = r;
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_put(bitcask_t* cask,
                                          bitcask_slice_t key,
                                          bitcask_slice_t value,
                                          uint32_t tstamp,
                                          bitcask_fault_t* fault) {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    std::span<const std::byte> value_span{static_cast<const std::byte*>(value.data), value.size};
    auto result = as_cpp_cask(cask)->put(key_span, value_span, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_delete(bitcask_t* cask,
                                             bitcask_slice_t key,
                                             uint32_t tstamp,
                                             bitcask_fault_t* fault) {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    std::span<const std::byte> key_span{static_cast<const std::byte*>(key.data), key.size};
    auto result = as_cpp_cask(cask)->remove(key_span, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_sync(bitcask_t* cask,
                                           bitcask_fault_t* fault) {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    auto result = as_cpp_cask(cask)->sync();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_close_write_file(bitcask_t* cask,
                                                        bitcask_fault_t* fault) {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    auto result = as_cpp_cask(cask)->close_write_file();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
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

    auto result = as_cpp_cask(cask)->put_doc(key_span, doc_input, tstamp);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_text(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_text(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_phrase(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_phrase(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_bool_search(bitcask_t* cask,
                                                   const char* query,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->bool_search(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_fields(bitcask_t* cask,
                                                      const char* query,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_fields(query, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_near(bitcask_t* cask,
                                                   const char* query,
                                                   uint32_t slop,
                                                   size_t k,
                                                   bitcask_search_result_t** out,
                                                   bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_near(query, slop, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_fuzzy(bitcask_t* cask,
                                                     const char* query,
                                                     size_t k,
                                                     uint32_t max_edit_distance,
                                                     bitcask_search_result_t** out,
                                                     bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_fuzzy(query, k, max_edit_distance);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_wildcard(bitcask_t* cask,
                                                        const char* pattern,
                                                        size_t k,
                                                        bitcask_search_result_t** out,
                                                        bitcask_fault_t* fault) {
    if (!cask || !pattern || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    auto result = as_cpp_cask(cask)->search_wildcard(pattern, k);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_vector(bitcask_t* cask,
                                                      const float* query,
                                                      size_t query_len,
                                                      size_t k,
                                                      size_t ef,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
    if (!cask || !query || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    std::span<const float> query_span{query, query_len};
    auto result = as_cpp_cask(cask)->search_vector(query_span, k, ef);
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_search_hybrid(bitcask_t* cask,
                                                      const char* text_query,
                                                      const float* vec_query,
                                                      size_t vec_len,
                                                      size_t k,
                                                      bitcask_search_result_t** out,
                                                      bitcask_fault_t* fault) {
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
    to_search_result(std::move(*result), out);
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_set_synonym_map(bitcask_t* cask,
                                                       const char* path,
                                                       bitcask_fault_t* fault) {
    if (!cask || !path) return BITCASK_ERR_INVALID_OPTION;

    auto map = std::make_unique<text::SynonymMap>();
    map->load_from_file(path);
    as_cpp_cask(cask)->set_synonym_map(std::move(map));
    return BITCASK_OK;
}

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

    auto* wrapper = new bitcask_iter_impl_t;
    wrapper->iter = std::move(iter);
    *out = reinterpret_cast<bitcask_iter_t*>(wrapper);
    return BITCASK_OK;
}

BITCASK_API int bitcask_iter_next(bitcask_iter_t* iter,
                                    bitcask_iter_entry_t* entry,
                                    bitcask_fault_t* fault) {
    if (!iter || !entry) return -1;

    auto result = as_cpp_iter(iter)->next();
    if (!result) {
        to_c_error(result.error(), fault);
        return -1;
    }
    if (!*result) {
        return 0;
    }

    auto& e = **result;
    if (e.key.empty()) {
        entry->key.data = nullptr;
        entry->key.size = 0;
    } else {
        auto* p = std::malloc(e.key.size());
        std::memcpy(p, e.key.data(), e.key.size());
        entry->key.data = p;
        entry->key.size = e.key.size();
    }
    if (e.value.empty()) {
        entry->value.data = nullptr;
        entry->value.size = 0;
    } else {
        auto* p = std::malloc(e.value.size());
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
    return 1;
}

BITCASK_API int bitcask_iter_next_batch(bitcask_iter_t* iter,
                                          bitcask_iter_entry_t* entries,
                                          size_t max_n,
                                          bitcask_fault_t* fault) {
    if (!iter || !entries || max_n == 0) return -1;

    std::size_t count = 0;
    while (count < max_n) {
        auto result = as_cpp_iter(iter)->next();
        if (!result) {
            to_c_error(result.error(), fault);
            return -1;
        }
        if (!*result) {
            break;
        }
        auto& e = **result;
        if (e.key.empty()) {
            entries[count].key.data = nullptr;
            entries[count].key.size = 0;
        } else {
            auto* p = std::malloc(e.key.size());
            std::memcpy(p, e.key.data(), e.key.size());
            entries[count].key.data = p;
            entries[count].key.size = e.key.size();
        }
        if (e.value.empty()) {
            entries[count].value.data = nullptr;
            entries[count].value.size = 0;
        } else {
            auto* p = std::malloc(e.value.size());
            std::memcpy(p, e.value.data(), e.value.size());
            entries[count].value.data = p;
            entries[count].value.size = e.value.size();
        }
        entries[count].tstamp = e.tstamp;
        entries[count].file_id = e.file_id;
        entries[count].offset = e.offset;
        entries[count].total_sz = e.total_sz;
        entries[count].is_tombstone = e.is_tombstone ? 1 : 0;
        entries[count].ord = e.ord;
        ++count;
    }
    return static_cast<int>(count);
}

BITCASK_API void bitcask_iter_release(bitcask_iter_t* iter) {
    if (!iter) return;
    as_cpp_iter(iter)->release();
    delete reinterpret_cast<bitcask_iter_impl_t*>(iter);
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

BITCASK_API bitcask_error_t bitcask_status(bitcask_t* cask,
                                             bitcask_status_t* out,
                                             bitcask_fault_t* fault) {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;

    bitcask::StatusInfo info = as_cpp_cask(cask)->status();
    out->key_count = info.key_count;
    out->key_bytes = info.key_bytes;
    out->epoch = info.epoch;
    return BITCASK_OK;
}

BITCASK_API bitcask_error_t bitcask_needs_merge(bitcask_t* cask,
                                                  bitcask_needs_merge_t* out,
                                                  bitcask_fault_t* fault) {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;

    bitcask::Cask::NeedsMerge result = as_cpp_cask(cask)->needs_merge();
    out->needs = result.needs ? 1 : 0;
    out->files_count = result.files.size();
    if (result.files.empty()) {
        out->files = nullptr;
    } else {
        out->files = static_cast<char**>(std::malloc(sizeof(char*) * result.files.size()));
        for (std::size_t i = 0; i < result.files.size(); ++i) {
            out->files[i] = strdup(result.files[i].c_str());
        }
    }
    return BITCASK_OK;
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
    if (!cask) return BITCASK_ERR_INVALID_OPTION;

    auto result = as_cpp_cask(cask)->merge();
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    return BITCASK_OK;
}

BITCASK_API int bitcask_is_empty(bitcask_t* cask) {
    if (!cask) return 1;
    return as_cpp_cask(cask)->is_empty_estimate() ? 1 : 0;
}

BITCASK_API int bitcask_is_frozen(bitcask_t* cask) {
    if (!cask) return 0;
    return as_cpp_cask(cask)->is_frozen() ? 1 : 0;
}

BITCASK_API void bitcask_flush_index(bitcask_t* cask) {
    if (!cask) return;
    as_cpp_cask(cask)->flush_index();
}

}