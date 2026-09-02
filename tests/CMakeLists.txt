# GoogleTest via submodule (third_party/googletest)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/googletest 未初始化。请运行:\n"
        "  git submodule update --init third_party/googletest")
endif()
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/googletest"
                 "${PROJECT_BINARY_DIR}/_deps/googletest-build" EXCLUDE_FROM_ALL)

# S37-2：测试公共 helper（support/test_paths.hpp、support/crash_child.hpp）
# 以 "support/..." 形式引用，把 tests/ 自身放进包含路径。目录级——所有测试
# 目标一致可见，免得每加一个用到 helper 的测试就补一次 include 目录。
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# bitcask_test_crash_child — 崩溃注入测试的 exec-self 骨架 + main（S37-2）。
#
# 用它的测试目标链 GTest::gtest（**不是** gtest_main）——本库自带 main，
# 需在 InitGoogleTest 之前拦截 --bitcask-crash-child 分发。契约见
# support/crash_child.hpp。
# ---------------------------------------------------------------------------
add_library(bitcask_test_crash_child STATIC
    support/crash_child.cpp
    support/crash_main.cpp
)
target_include_directories(bitcask_test_crash_child PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bitcask_test_crash_child
    PUBLIC  GTest::gtest
    PRIVATE bitcask_warnings bitcask_sanitizers
)

add_executable(bitcask_smoke_test smoke_test.cpp)
target_link_libraries(bitcask_smoke_test PRIVATE
    GTest::gtest_main
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_cpu_features_test cpu_features_test.cpp)
target_link_libraries(bitcask_cpu_features_test PRIVATE
    GTest::gtest_main
    bitcask_simd
    bitcask_warnings
    bitcask_sanitizers
)
add_executable(bitcask_codec_test codec_test.cpp)
target_link_libraries(bitcask_codec_test PRIVATE
    GTest::gtest_main
    bitcask_format
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_posix_file_test posix_file_test.cpp)
target_link_libraries(bitcask_posix_file_test PRIVATE
    GTest::gtest_main
    bitcask_io
    bitcask_warnings
    bitcask_sanitizers
)

# P0：窄路径 = UTF-8 的库级约定。链 fileops 是为了用 scan_dir 覆盖
# 「directory_iterator 产出的真·宽路径转窄」这条形态（见文件头）。
add_executable(bitcask_path_utf8_test path_utf8_test.cpp)
target_link_libraries(bitcask_path_utf8_test PRIVATE
    GTest::gtest_main
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_file_lock_test file_lock_test.cpp)
target_link_libraries(bitcask_file_lock_test PRIVATE
    GTest::gtest_main
    bitcask_io
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_index_test index_test.cpp)
target_link_libraries(bitcask_index_test PRIVATE
    GTest::gtest_main
    bitcask_index
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_data_file_test data_file_test.cpp)
target_link_libraries(bitcask_data_file_test PRIVATE
    GTest::gtest_main
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_oki_run_v2_test oki_run_v2_test.cpp)
target_link_libraries(bitcask_oki_run_v2_test PRIVATE
    GTest::gtest_main
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_oki_run_test oki_run_test.cpp)
target_link_libraries(bitcask_oki_run_test PRIVATE
    GTest::gtest_main
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_scanner_test scanner_test.cpp)
target_link_libraries(bitcask_scanner_test PRIVATE
    GTest::gtest_main
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
)

# NOTE(V1): legacy 测试 keydir*/merge*/cask/NIF 已移除（见 cpp/CMakeLists.txt）。

add_executable(bitcask_analyzer_test analyzer_test.cpp)
target_link_libraries(bitcask_analyzer_test PRIVATE
    GTest::gtest_main
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

# S38：ICU 编码转换层（GB18030 等 → UTF-8）。
add_executable(bitcask_text_encoding_test text_encoding_test.cpp)
target_link_libraries(bitcask_text_encoding_test PRIVATE
    GTest::gtest_main
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_porter_stemmer_test porter_stemmer_test.cpp)
target_link_libraries(bitcask_porter_stemmer_test PRIVATE
    GTest::gtest_main
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_stemming_analyzer_test stemming_analyzer_test.cpp)
target_link_libraries(bitcask_stemming_analyzer_test PRIVATE
    GTest::gtest_main
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_segment_v2_test segment_v2_test.cpp)
target_link_libraries(bitcask_segment_v2_test PRIVATE
    GTest::gtest_main
    bitcask_bm25
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_inverted_test inverted_test.cpp)
target_link_libraries(bitcask_inverted_test PRIVATE
    GTest::gtest_main
    bitcask_bm25
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_jieba_analyzer_test jieba_analyzer_test.cpp)
target_link_libraries(bitcask_jieba_analyzer_test PRIVATE
    GTest::gtest_main
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)
# 注入 cppjieba 词典目录的真实路径，替代源码里硬编码的 /tmp 旧构建路径。
target_compile_definitions(bitcask_jieba_analyzer_test PRIVATE
    BITCASK_JIEBA_DICT_DIR="${BITCASK_JIEBA_DICT_DIR}")

add_executable(bitcask_keydir_test keydir_test.cpp)
target_link_libraries(bitcask_keydir_test PRIVATE
    GTest::gtest_main
    bitcask_keydir
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_cask_docvalue_test cask_docvalue_test.cpp)
target_link_libraries(bitcask_cask_docvalue_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)
target_compile_definitions(bitcask_cask_docvalue_test PRIVATE
    BITCASK_JIEBA_DICT_DIR="${BITCASK_JIEBA_DICT_DIR}")

add_executable(bitcask_oki_range_test oki_range_test.cpp)
target_link_libraries(bitcask_oki_range_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_oki_recovery_test oki_recovery_test.cpp)
target_link_libraries(bitcask_oki_recovery_test PRIVATE
    GTest::gtest
    bitcask_test_crash_child
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_oki_levelb_test oki_levelb_test.cpp)
target_link_libraries(bitcask_oki_levelb_test PRIVATE
    GTest::gtest
    bitcask_test_crash_child
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_oki_locate_test oki_locate_test.cpp)
target_link_libraries(bitcask_oki_locate_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_atomic_batch_test atomic_batch_test.cpp)
target_link_libraries(bitcask_atomic_batch_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_txn_test txn_test.cpp)
target_link_libraries(bitcask_txn_test PRIVATE
    GTest::gtest
    bitcask_test_crash_child
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_crash_recovery_test crash_recovery_test.cpp)
target_link_libraries(bitcask_crash_recovery_test PRIVATE
    GTest::gtest
    bitcask_test_crash_child
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_merge_concurrent_writer_test merge_concurrent_writer_test.cpp)
target_link_libraries(bitcask_merge_concurrent_writer_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_checkpoint_recovery_test checkpoint_recovery_test.cpp)
target_link_libraries(bitcask_checkpoint_recovery_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)


# S19-3：SearchLayer shim 测试夹具（原产品 bitcask_search 目标降级至此）。
# 聚合 docmap+TextPlugin+VectorPlugin+HybridSearcher 的兼容门面 + legacy
# 统一 search.ckpt 读写端——save 半边专供迁移测试生成 pre-S17 格式文件。
add_library(bitcask_search_shim STATIC support/search_layer.cpp)
target_include_directories(bitcask_search_shim PUBLIC support)
target_link_libraries(bitcask_search_shim
    PUBLIC  bitcask_hybrid
    PRIVATE bitcask_warnings bitcask_sanitizers
)

add_executable(bitcask_search_layer_test search_layer_test.cpp)
target_link_libraries(bitcask_search_layer_test PRIVATE
    GTest::gtest_main
    bitcask_search_shim
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_highlighter_test highlighter_test.cpp)
target_link_libraries(bitcask_highlighter_test PRIVATE
    GTest::gtest_main
    bitcask_text_plugin   # S24-B4：TextPlugin 直连（docmap 经 PUBLIC 传递）
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_wildcard_test wildcard_test.cpp)
target_link_libraries(bitcask_wildcard_test PRIVATE
    GTest::gtest_main
    bitcask_bm25
    bitcask_text_plugin   # S24-B4：TextPlugin 直连（docmap 经 PUBLIC 传递）
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_synonym_test synonym_test.cpp)
target_link_libraries(bitcask_synonym_test PRIVATE
    GTest::gtest_main
    bitcask_text_plugin   # S24-B4：TextPlugin 直连（docmap 经 PUBLIC 传递）
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_fuzzy_test fuzzy_test.cpp)
target_link_libraries(bitcask_fuzzy_test PRIVATE
    GTest::gtest_main
    bitcask_bm25
    bitcask_text_plugin   # S24-B4：TextPlugin 直连（docmap 经 PUBLIC 传递）
    bitcask_text
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_hnsw_test hnsw_test.cpp)
target_link_libraries(bitcask_hnsw_test PRIVATE
    GTest::gtest_main
    bitcask_vector
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_diskann_test diskann_test.cpp)
target_link_libraries(bitcask_diskann_test PRIVATE
    GTest::gtest_main
    bitcask_vector
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_ivf_rq_test ivf_rq_test.cpp)
target_link_libraries(bitcask_ivf_rq_test PRIVATE
    GTest::gtest_main
    bitcask_vector
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_diskann_plugin_test diskann_plugin_test.cpp)
target_link_libraries(bitcask_diskann_plugin_test PRIVATE
    GTest::gtest_main
    bitcask_vector_plugin
    bitcask_warnings
    bitcask_sanitizers
)

add_executable(bitcask_ivf_plugin_test ivf_plugin_test.cpp)
target_link_libraries(bitcask_ivf_plugin_test PRIVATE
    GTest::gtest_main
    bitcask_vector_plugin
    bitcask_warnings
    bitcask_sanitizers
)

include(GoogleTest)

# C1:TSan 下给所有测试注入 suppression 文件(见 cmake/tsan.supp 头注释:
# TBB 的 fence 同步对 TSan 不可见,插桩自编 oneTBB 后仍有残余假阳性)。
# 动态 libtsan 不回调二进制内的 __tsan_default_suppressions,env 是
# 唯一可靠通道。
# detect_deadlocks=1 固化(屏障 v2 验收门禁):KeyDir 屏障曾因同时持
# 257 把锁撞 TSan 死锁检测器 64 持锁硬上限崩溃,该开关保证回归可见。
set(_bitcask_test_props "")
if(BITCASK_SANITIZE MATCHES "thread")
    set(_bitcask_test_props PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/cmake/tsan.supp:detect_deadlocks=1")
endif()
gtest_discover_tests(bitcask_smoke_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_cpu_features_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_codec_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_posix_file_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_path_utf8_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_file_lock_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_index_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_data_file_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_run_v2_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_run_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_recovery_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_locate_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_levelb_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_oki_range_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_scanner_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_analyzer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_text_encoding_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_porter_stemmer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_stemming_analyzer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_inverted_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_segment_v2_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_jieba_analyzer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_keydir_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_cask_docvalue_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_atomic_batch_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_txn_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_crash_recovery_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_merge_concurrent_writer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_checkpoint_recovery_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_search_layer_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_highlighter_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_wildcard_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_synonym_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_fuzzy_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_hnsw_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_diskann_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_ivf_rq_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_diskann_plugin_test ${_bitcask_test_props})
gtest_discover_tests(bitcask_ivf_plugin_test ${_bitcask_test_props})

# S15-1：plugin_api 头自包含测试——只链 bitcask_plugin_api（+gtest），
# 编译通过即证明接口层对 search/bm25/vector 零依赖。
add_executable(bitcask_plugin_api_test plugin_api_test.cpp)
target_link_libraries(bitcask_plugin_api_test PRIVATE
    GTest::gtest_main
    bitcask_plugin_api
    bitcask_warnings
    bitcask_sanitizers
)
gtest_discover_tests(bitcask_plugin_api_test ${_bitcask_test_props})

# S15-4：插件契约 Mock 测试——不链 search（thread_pool.hpp 已去搜索化），
# 守护 IndexPool 分发通路对 CaskPlugin 的契约。S16-2 增链 bitcask_index
# （docmap 宿主服务）以验证「宿主先 apply DocMap、再广播插件」顺序不变量。
# S19-1：Searcher 门面等价测试。
add_executable(bitcask_searcher_test searcher_test.cpp)
target_link_libraries(bitcask_searcher_test PRIVATE
    GTest::gtest_main
    bitcask_cask
    bitcask_warnings
    bitcask_sanitizers
)
gtest_discover_tests(bitcask_searcher_test ${_bitcask_test_props})

# S18-11：插件级测试（flush/open 自治、replay 路由、归一化契约）。
add_executable(bitcask_text_plugin_test text_plugin_test.cpp)
target_link_libraries(bitcask_text_plugin_test PRIVATE
    GTest::gtest_main
    bitcask_text_plugin
    bitcask_docmap
    bitcask_warnings
    bitcask_sanitizers
)
gtest_discover_tests(bitcask_text_plugin_test ${_bitcask_test_props})

add_executable(bitcask_vector_plugin_test vector_plugin_test.cpp)
target_link_libraries(bitcask_vector_plugin_test PRIVATE
    GTest::gtest_main
    bitcask_vector_plugin
    bitcask_bm25   # DocTable/meta_filter 头 + MetaFilter 求值符号
    bitcask_warnings
    bitcask_sanitizers
)
gtest_discover_tests(bitcask_vector_plugin_test ${_bitcask_test_props})

add_executable(bitcask_plugin_contract_test plugin_contract_test.cpp)
target_link_libraries(bitcask_plugin_contract_test PRIVATE
    GTest::gtest_main
    bitcask_plugin_api
    bitcask_index
    bitcask_merge     # S18-11：merge 广播契约（run_merge 直调 + mock 插件）
    bitcask_keydir
    bitcask_fileops
    bitcask_warnings
    bitcask_sanitizers
    TBB::tbb
)
gtest_discover_tests(bitcask_plugin_contract_test ${_bitcask_test_props})

add_executable(bitcask_thread_pool_test thread_pool_test.cpp)
target_link_libraries(bitcask_thread_pool_test PRIVATE
    GTest::gtest_main
    bitcask_hybrid
    bitcask_warnings
    bitcask_sanitizers
    TBB::tbb
)
gtest_discover_tests(bitcask_thread_pool_test ${_bitcask_test_props})

# S17-1：index.manifest 格式测试——header-only（只需 gtest + zlib via codec）。
add_executable(bitcask_index_manifest_test index_manifest_test.cpp)
target_link_libraries(bitcask_index_manifest_test PRIVATE
    GTest::gtest_main
    bitcask_format
    bitcask_warnings
    bitcask_sanitizers
)
gtest_discover_tests(bitcask_index_manifest_test ${_bitcask_test_props})

# ---------------------------------------------------------------------------
# C API smoke test — 纯 C, 链接 libbitcask.so (bitcask_shared)
# ---------------------------------------------------------------------------
enable_language(C)
add_executable(bitcask_c_api_test c_api_test.c)
# bitcask_sanitizers（INTERFACE）：与所有 C++ 测试目标一致，让 C 测试也被 sanitizer
# 插桩。否则 sanitize 构建下，未插桩的 C 主程序链接已插桩的 .so，一旦 .so 内部起线程
# （如 search 模式的 IndexPool worker）就会因运行时缺失而 SEGV（S12-5 修）。
target_link_libraries(bitcask_c_api_test PRIVATE bitcask_shared bitcask_sanitizers)
# S24 补：强制 CXX 链接器——clang 下 C 驱动器只链 UBSan 的 C runtime，
# 缺 .so 内 vptr 检查引用的 C++ 符号（__ubsan_vptr_type_cache 等）致链接
# 失败；clang++ 驱动链 ubsan_standalone_cxx 补齐。对 gcc/非 sanitizer
# 构建无影响（C 程序用 C++ 链接器无害）。
set_target_properties(bitcask_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
# S37-4：c_api_test.c 用 <stdatomic.h>，要求 C11。GCC 默认 gnu17 故一直没事；
# MSVC 的 .c 默认走 C89/C90，其 <stdatomic.h> 首行就 #error「C atomics require
# C11 or later」。只钉这一个目标，不动全局 CMAKE_C_STANDARD——那会连带改掉
# zlib 的 C 方言档位（GCC 侧从 gnu17 降到 gnu11），属无谓的波及。
set_target_properties(bitcask_c_api_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    # 光有 /std:c11 还不够：MSVC 的 C11 atomics 至今在开关后面，不给此项
    # <stdatomic.h> 第 12 行就 #error「C atomic support is not enabled」。
    # 不改测试去用 Interlocked* —— 那会把一个平台无关的并发断言拆成两份实现；
    # 这些 atomic 是 parallel_scan 多工作线程回调的计数/校验和，属实质用途。
    target_compile_options(bitcask_c_api_test PRIVATE /experimental:c11atomics)
endif()
add_test(NAME CApi.SmokeTest COMMAND bitcask_c_api_test)
