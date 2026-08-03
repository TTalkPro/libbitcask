// C API — KV/生命周期/迭代/管理（S19-5 自 bitcask_c.cpp 拆分，符号与实现不变）。
#include "internal.h"

using namespace bitcask::capi;

namespace meta = bitcask::meta;
namespace search = bitcask::search;
namespace text = bitcask::text;

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
    opts->vector_engine = BITCASK_VECTOR_ENGINE_HNSW;  // S32：默认 HNSW
    opts->hnsw_m = 0;                 // S13-D11：0 = HnswConfig 默认
    opts->hnsw_ef_construction = 0;
    opts->hnsw_build_nav_int8 = 1;    // S29-11-②：默认开
    opts->vector_rebase_min_docs = 262144;  // S32-M1：恢复窗口默认 256K
    opts->vector_ivf_nlist = 0;             // S32-M3：0 = 自动
    opts->vector_ivf_nprobe = 0;
    opts->vector_diskann_r = 0;             // S32-M5：0 = 自动
    opts->vector_diskann_l_build = 0;
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
        cpp_opts.vector_engine = to_cpp_vector_engine(opts->vector_engine);
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
            search_cfg.hnsw_build_nav_int8 = opts->hnsw_build_nav_int8 != 0;  // S29-11-②
            // S32：向量引擎调优透传（0 = 各自动默认）。
            search_cfg.vector_rebase_min_docs = opts->vector_rebase_min_docs;
            search_cfg.vector_ivf_nlist = opts->vector_ivf_nlist;
            search_cfg.vector_ivf_nprobe = opts->vector_ivf_nprobe;
            search_cfg.vector_diskann_r = opts->vector_diskann_r;
            search_cfg.vector_diskann_l_build = opts->vector_diskann_l_build;
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
    if (!slice_valid(key)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2
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
                                          uint64_t tstamp,
                                          bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key) || !slice_valid(value)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2

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
                                           uint64_t tstamp,
                                           uint64_t expiry_at,
                                           bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key) || !slice_valid(value)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2
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
                                             uint64_t tstamp,
                                             bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2

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
                                              uint64_t tstamp,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !doc) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key) || !slice_valid(doc->text) ||
        !slice_valid(doc->meta)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2

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

BITCASK_API void bitcask_search_result_batch_free(bitcask_search_result_t** results,
                                                  size_t n) {
    if (!results) return;
    for (size_t i = 0; i < n; ++i) bitcask_search_result_free(results[i]);
    std::free(results);
}

// S13-D1：批量写。
BITCASK_API bitcask_error_t bitcask_put_batch(bitcask_t* cask,
                                              const bitcask_kv_pair_t* items,
                                              size_t n,
                                              uint64_t tstamp,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask) return BITCASK_ERR_INVALID_OPTION;
    if (n == 0) return BITCASK_OK;
    if (!items) return BITCASK_ERR_INVALID_OPTION;

    std::vector<bitcask::Cask::BatchItem> batch;
    batch.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!slice_valid(items[i].key) || !slice_valid(items[i].value))  // S25-M2
            return BITCASK_ERR_INVALID_OPTION;
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

// S33-6：带前缀版是实现主体，无前缀版 = 空切片特例（见头文件契约）。
BITCASK_API bitcask_error_t bitcask_iter_start_prefix(
    bitcask_t* cask, int maxage, int maxputs, int see_tombstones,
    bitcask_slice_t key_prefix, bitcask_iter_t** out,
    bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key_prefix)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2
    *out = nullptr;

    auto iter = as_cpp_cask(cask)->make_iter();
    auto start_result = iter->start(maxage, maxputs, 0, see_tombstones != 0,
                                    to_span(key_prefix));
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

BITCASK_API bitcask_error_t bitcask_iter_start(bitcask_t* cask,
                                                  int maxage,
                                                  int maxputs,
                                                  int see_tombstones,
                                                  bitcask_iter_t** out,
                                                  bitcask_fault_t* fault) {
    bitcask_slice_t none = {NULL, 0};
    return bitcask_iter_start_prefix(cask, maxage, maxputs, see_tombstones,
                                     none, out, fault);
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

/* ---------------------------------------------------------------------------
 *  S33-6：有序 range 迭代（OKI）
 * ------------------------------------------------------------------------- */

BITCASK_API void bitcask_range_options_init(bitcask_range_options_t* opts) {
    if (!opts) return;
    opts->lo.data = NULL;
    opts->lo.size = 0;
    opts->hi.data = NULL;
    opts->hi.size = 0;
    opts->prefetch = 0;
    opts->prefetch_threads = 0;
}

BITCASK_API bitcask_error_t bitcask_range_iter_start(
    bitcask_t* cask, const bitcask_range_options_t* opts,
    bitcask_range_iter_t** out, bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
    *out = nullptr;

    bitcask::RangeOptions ro;
    if (opts) {
        if (!slice_valid(opts->lo) || !slice_valid(opts->hi)) {  // S25-M2
            return BITCASK_ERR_INVALID_OPTION;
        }
        ro.lo = to_span(opts->lo);
        ro.hi = to_span(opts->hi);
        ro.prefetch = opts->prefetch;
        ro.prefetch_threads = opts->prefetch_threads;
    }
    auto iter = as_cpp_cask(cask)->make_range_iter(ro);
    if (!iter) {
        to_c_error(iter.error(), fault);
        return to_c_error_kind(iter.error().kind);
    }

    // 同 iter_start：构造期 unique_ptr 持有，release 转交裸句柄给调用方。
    auto wrapper = std::make_unique<bitcask_range_iter_impl_t>();
    wrapper->iter = std::move(*iter);
    *out = reinterpret_cast<bitcask_range_iter_t*>(wrapper.release());
    return BITCASK_OK;
    });
}

BITCASK_API int bitcask_range_iter_next(bitcask_range_iter_t* iter,
                                        bitcask_range_entry_t* entry,
                                        bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!iter || !entry) return -1;

    auto result = as_cpp_range_iter(iter)->next();
    if (!result) {
        to_c_error(result.error(), fault);
        return -1;
    }
    if (!*result) return 0;

    if (!fill_range_entry(*std::move(*result), entry)) {  // S13-M2：OOM
        set_oom_fault(fault);
        return -1;
    }
    return 1;
    } catch (...) {
        (void)fault_from_exception(fault);
        return -1;
    }
}

BITCASK_API int bitcask_range_iter_next_batch(bitcask_range_iter_t* iter,
                                              bitcask_range_entry_t* entries,
                                              size_t max_n,
                                              bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    try {
    if (!iter || !entries || max_n == 0) return -1;

    std::size_t count = 0;
    while (count < max_n) {
        auto result = as_cpp_range_iter(iter)->next();
        if (!result) {
            // S13-M1：中途失败必须释放已填充条目——契约只返回 -1，
            // caller 无从得知已填多少条。
            for (std::size_t i = 0; i < count; ++i) {
                bitcask_range_entry_free(&entries[i]);
            }
            to_c_error(result.error(), fault);
            return -1;
        }
        if (!*result) break;
        if (!fill_range_entry(*std::move(*result), &entries[count])) {
            for (std::size_t i = 0; i < count; ++i) {
                bitcask_range_entry_free(&entries[i]);
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

BITCASK_API void bitcask_range_iter_release(bitcask_range_iter_t* iter) {
    // S13-M2：extern "C" 异常隔离（无可报告通道，吞掉）
    try {
    if (!iter) return;
    // adopt 回 unique_ptr：作用域结束自动 delete（与创建处 release 对称）。
    std::unique_ptr<bitcask_range_iter_impl_t> owned(
        reinterpret_cast<bitcask_range_iter_impl_t*>(iter));
    } catch (...) {
    }
}

BITCASK_API void bitcask_range_entry_free(bitcask_range_entry_t* entry) {
    if (!entry) return;
    if (entry->key.data) std::free(const_cast<void*>(entry->key.data));
    if (entry->value.data) std::free(const_cast<void*>(entry->value.data));
    entry->key.data = nullptr;
    entry->key.size = 0;
    entry->value.data = nullptr;
    entry->value.size = 0;
}

BITCASK_API bitcask_error_t bitcask_parallel_scan_prefix(
    bitcask_t* cask, size_t n_threads, bitcask_slice_t key_prefix,
    bitcask_scan_fn fn, void* ctx, size_t* out_count,
    bitcask_fault_t* fault) {
    // S13-M2：extern "C" 异常隔离
    return guarded(fault, [&]() -> bitcask_error_t {
    if (!cask || !fn) return BITCASK_ERR_INVALID_OPTION;
    if (!slice_valid(key_prefix)) return BITCASK_ERR_INVALID_OPTION;  // S25-M2
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
        },
        to_span(key_prefix));
    if (!result) {
        to_c_error(result.error(), fault);
        return to_c_error_kind(result.error().kind);
    }
    if (out_count) *out_count = *result;
    return BITCASK_OK;
    });
}

BITCASK_API bitcask_error_t bitcask_parallel_scan(bitcask_t* cask,
                                                  size_t n_threads,
                                                  bitcask_scan_fn fn,
                                                  void* ctx,
                                                  size_t* out_count,
                                                  bitcask_fault_t* fault) {
    bitcask_slice_t none = {NULL, 0};
    return bitcask_parallel_scan_prefix(cask, n_threads, none, fn, ctx,
                                        out_count, fault);
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

}  // extern "C"
