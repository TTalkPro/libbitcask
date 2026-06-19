cmake_minimum_required(VERSION 3.20)
project(libbitcask CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(BitcaskSanitizers)

enable_testing()

# ---------------------------------------------------------------------------
# Common warning flags for all bitcask C++ targets.
# ---------------------------------------------------------------------------
add_library(bitcask_warnings INTERFACE)
target_compile_options(bitcask_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Woverloaded-virtual
    -fvisibility=hidden -fvisibility-inlines-hidden
)

find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# TSan 构建改用源码编译的**插桩版** oneTBB（系统 libtbb 未插桩）。
# 源码以 submodule 形式置于 third_party/oneTBB（v2022.0.0）。
# ---------------------------------------------------------------------------
if(BITCASK_SANITIZE MATCHES "thread")
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/oneTBB/CMakeLists.txt")
        message(FATAL_ERROR
            "third_party/oneTBB 未初始化（TSan 构建需要插桩版 oneTBB）。请运行:\n"
            "  git submodule update --init third_party/oneTBB")
    endif()
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    set(_bitcask_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    add_subdirectory(third_party/oneTBB EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_FLAGS "${_bitcask_saved_cxx_flags}")
    message(STATUS "libbitcask: TSan build -> instrumented oneTBB from third_party/oneTBB")
else()
    find_package(TBB REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Release 性能选项（-falign-functions=64 + LTO/IPO）。
# 需在所有 add_library 之前设置（目录属性在 target 创建时固化）。
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:Release>:-falign-functions=64>)
endif()

option(BITCASK_LTO "Release 构建启用 LTO/IPO（sanitizer 构建自动关闭）" ON)
if(BITCASK_LTO AND "${BITCASK_SANITIZE}" STREQUAL "")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _bitcask_ipo_ok OUTPUT _bitcask_ipo_msg)
    if(_bitcask_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(STATUS "libbitcask: LTO unavailable (${_bitcask_ipo_msg}), proceeding without")
    endif()
endif()

# sanitizer 空壳 target（当顶层未注入时提供）
if(NOT TARGET bitcask_sanitizers)
    add_library(bitcask_sanitizers INTERFACE)
endif()

# ---------------------------------------------------------------------------
# bitcask_format — codec for data file / hint file records.
# ---------------------------------------------------------------------------
add_library(bitcask_format STATIC
    src/fileops/codec.cpp
)
target_include_directories(bitcask_format PUBLIC include)
target_link_libraries(bitcask_format
    PUBLIC ZLIB::ZLIB
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_io — POSIX file + advisory lock primitives.
# ---------------------------------------------------------------------------
add_library(bitcask_io STATIC
    src/io/posix_file.cpp
    src/lock/file_lock.cpp
)
target_include_directories(bitcask_io PUBLIC include)
target_link_libraries(bitcask_io PRIVATE bitcask_warnings bitcask_sanitizers)

# ---------------------------------------------------------------------------
# bitcask_fileops — DataFile + HintFile abstractions over PosixFile + codec.
# ---------------------------------------------------------------------------
add_library(bitcask_fileops STATIC
    src/fileops/data_file.cpp
    src/fileops/hint_file.cpp
    src/fileops/scanner.cpp
    src/fileops/migrate.cpp
)
target_include_directories(bitcask_fileops PUBLIC include)
target_link_libraries(bitcask_fileops
    PUBLIC bitcask_format bitcask_io
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_keydir — in-memory keydir (KeyDir class for bitcask).
# ---------------------------------------------------------------------------
add_library(bitcask_keydir STATIC
    src/keydir/keydir.cpp
    src/keydir/keydir_registry.cpp
)
target_include_directories(bitcask_keydir PUBLIC include)
target_link_libraries(bitcask_keydir
    PUBLIC  bitcask_fileops bitcask_io
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_index — in-memory Index side tables (vector DB V1, §3.1).
# ---------------------------------------------------------------------------
add_library(bitcask_index STATIC
    src/keydir/index.cpp
)
target_include_directories(bitcask_index PUBLIC include)
target_link_libraries(bitcask_index
    PUBLIC  bitcask_text
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_bm25 — BM25 倒排索引（InvertedIndex + DAAT 评分）。
# ---------------------------------------------------------------------------
add_library(bitcask_bm25 STATIC
    src/bm25/intersect.cpp
    src/bm25/inverted.cpp
    src/bm25/inverted_wal.cpp
    src/bm25/query_parser.cpp
)
target_include_directories(bitcask_bm25 PUBLIC include)
target_link_libraries(bitcask_bm25
    PUBLIC bitcask_format
    PRIVATE bitcask_warnings bitcask_sanitizers TBB::tbb)

# ---------------------------------------------------------------------------
# bitcask_vector — HNSW 向量索引。
# ---------------------------------------------------------------------------
add_library(bitcask_vector STATIC
    src/vector/hnsw.cpp
)
target_include_directories(bitcask_vector
    PUBLIC  include
    PRIVATE src/vector
)
target_link_libraries(bitcask_vector
    PUBLIC  bitcask_format
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_search — SearchLayer (Index + InvertedIndex + Analyzer wrapper).
# ---------------------------------------------------------------------------
add_library(bitcask_search STATIC
    src/search/search_cache.cpp
    src/search/search_layer.cpp
    src/search/highlighter.cpp
)
target_include_directories(bitcask_search PUBLIC include)
target_link_libraries(bitcask_search
    PUBLIC  bitcask_index bitcask_bm25 bitcask_text bitcask_vector
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_merge — merge 策略 + 执行。
# ---------------------------------------------------------------------------
add_library(bitcask_merge STATIC
    src/merge/merger.cpp
    src/merge/merge_policy.cpp
)
target_include_directories(bitcask_merge PUBLIC include)
target_link_libraries(bitcask_merge
    PUBLIC  bitcask_keydir bitcask_fileops bitcask_io bitcask_format bitcask_search
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_cask — Cask KV store (CRUD + merge + iterators).
# ---------------------------------------------------------------------------
add_library(bitcask_cask STATIC
    src/cask/cask.cpp
    src/cask/meta_file.cpp
)
target_include_directories(bitcask_cask PUBLIC include)
target_link_libraries(bitcask_cask
    PUBLIC  bitcask_keydir bitcask_fileops bitcask_io bitcask_format bitcask_merge bitcask_search
    PRIVATE bitcask_warnings bitcask_sanitizers TBB::tbb
)

# ---------------------------------------------------------------------------
# utf8proc — Unicode NFKC 归一化 + case fold（submodule: third_party/utf8proc）。
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/utf8proc/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/utf8proc 未初始化。请运行:\n"
        "  git submodule update --init")
endif()
add_subdirectory(third_party/utf8proc EXCLUDE_FROM_ALL)

if(TARGET utf8proc::utf8proc)
    set(UTF8PROC_TARGET utf8proc::utf8proc)
else()
    set(UTF8PROC_TARGET utf8proc)
endif()

# ---------------------------------------------------------------------------
# gen_inert_table — build-time code generator for the NFKC inert range table.
# ---------------------------------------------------------------------------
add_executable(gen_inert_table tools/gen_inert_table.cpp)
target_link_libraries(gen_inert_table PRIVATE ${UTF8PROC_TARGET})

# ---------------------------------------------------------------------------
# migrate_le — v1 大端 → v2 小端离线迁移工具。
# ---------------------------------------------------------------------------
add_executable(migrate_le tools/migrate_le.cpp)
target_link_libraries(migrate_le
    PRIVATE bitcask_fileops bitcask_format bitcask_io
            bitcask_warnings bitcask_sanitizers)

set(INERT_TABLE_HPP
    "${CMAKE_CURRENT_SOURCE_DIR}/include/bitcask/detail/inert_table.hpp")
add_custom_command(
    OUTPUT "${INERT_TABLE_HPP}"
    COMMAND gen_inert_table "${INERT_TABLE_HPP}"
    DEPENDS gen_inert_table
    COMMENT "Generating NFKC inert range table"
    VERBATIM
)
add_custom_target(generate_inert_table DEPENDS "${INERT_TABLE_HPP}")

# ---------------------------------------------------------------------------
# cppjieba — jieba 中文分词（header-only，依赖 limonp）。
# 两者均以 submodule 形式置于 third_party/。
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/cppjieba/include/cppjieba/Jieba.hpp" OR
   NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/limonp/include/limonp/StringUtil.hpp")
    message(FATAL_ERROR
        "third_party/cppjieba 或 third_party/limonp 未初始化。请运行:\n"
        "  git submodule update --init third_party/cppjieba third_party/limonp")
endif()

if(NOT TARGET cppjieba)
    add_library(cppjieba INTERFACE)
    target_include_directories(cppjieba INTERFACE
        "${CMAKE_SOURCE_DIR}/third_party/cppjieba/include"
        "${CMAKE_SOURCE_DIR}/third_party/limonp/include"
    )
endif()

set(BITCASK_JIEBA_DICT_DIR "${CMAKE_SOURCE_DIR}/third_party/cppjieba/dict"
    CACHE INTERNAL "cppjieba 词典目录")

# ---------------------------------------------------------------------------
# bitcask_text — 文本分词器（Analyzer 抽象基类 + NgramAnalyzer + 工厂）。
# ---------------------------------------------------------------------------
add_library(bitcask_text STATIC
    src/text/analyzer.cpp
    src/text/jieba_analyzer.cpp
)
target_include_directories(bitcask_text PUBLIC include)
target_link_libraries(bitcask_text
    PUBLIC  ${UTF8PROC_TARGET}
    PRIVATE bitcask_warnings bitcask_sanitizers cppjieba
)
add_dependencies(bitcask_text generate_inert_table)

# ---------------------------------------------------------------------------
# libbitcask 聚合 target（libbitcask.a + libbitcask.so）
# ---------------------------------------------------------------------------

# libbitcask.so — 共享库, C API (extern "C") 跨 ABI 稳定
add_library(bitcask_shared SHARED c_api/bitcask_c.cpp)
target_include_directories(bitcask_shared PUBLIC include c_api)
target_link_libraries(bitcask_shared PRIVATE bitcask_cask)
target_compile_definitions(bitcask_shared PRIVATE BITCASK_DLL_EXPORTS)
set_target_properties(bitcask_shared PROPERTIES
    OUTPUT_NAME "bitcask"
    VERSION 1.0.0
    SOVERSION 1
)
add_library(Bitcask::shared ALIAS bitcask_shared)

# libbitcask.a — 合并全部静态归档为单一 .a
set(BITCASK_STATIC_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libbitcask.a")
set(BITCASK_MERGE_SH "${CMAKE_CURRENT_BINARY_DIR}/merge_static.sh")
file(WRITE "${BITCASK_MERGE_SH}"
    "#!/bin/bash\n"
    "set -e\n"
    "OUTPUT=\"$1\"\n"
    "shift\n"
    "TMPD=$(mktemp -d)\n"
    "for lib in \"$@\"; do\n"
    "    (cd \"$TMPD\" && ${CMAKE_AR} x \"$lib\")\n"
    "done\n"
    "${CMAKE_AR} rcs \"$OUTPUT\" \"$TMPD\"/*.o\n"
    "${CMAKE_RANLIB} \"$OUTPUT\"\n"
    "rm -rf \"$TMPD\"\n"
)
add_custom_target(bitcask_static ALL
    COMMAND bash "${BITCASK_MERGE_SH}" "${BITCASK_STATIC_OUTPUT}"
            $<TARGET_FILE:bitcask_format>
            $<TARGET_FILE:bitcask_io>
            $<TARGET_FILE:bitcask_fileops>
            $<TARGET_FILE:bitcask_keydir>
            $<TARGET_FILE:bitcask_index>
            $<TARGET_FILE:bitcask_bm25>
            $<TARGET_FILE:bitcask_vector>
            $<TARGET_FILE:bitcask_text>
            $<TARGET_FILE:bitcask_search>
            $<TARGET_FILE:bitcask_merge>
            $<TARGET_FILE:bitcask_cask>
    DEPENDS bitcask_cask bitcask_text
    BYPRODUCTS "${BITCASK_STATIC_OUTPUT}"
    COMMENT "Merging libbitcask.a from all static archives"
)

# ---------------------------------------------------------------------------
# Install 规则
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS bitcask_shared
    EXPORT BitcaskTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES "${BITCASK_STATIC_OUTPUT}"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/bitcask
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)
install(FILES c_api/bitcask_c.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ---------------------------------------------------------------------------
# Tests (GoogleTest via submodule: third_party/googletest)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks (Google Benchmark via submodule: third_party/benchmark). Off by default.
# ---------------------------------------------------------------------------
option(BITCASK_BUILD_BENCHMARKS "Build C++ micro-benchmarks" OFF)
if(BITCASK_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

message(STATUS "libbitcask: C++${CMAKE_CXX_STANDARD}, build type ${CMAKE_BUILD_TYPE}")
