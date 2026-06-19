# Google Benchmark via submodule (third_party/benchmark)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/benchmark/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/benchmark 未初始化。请运行:\n"
        "  git submodule update --init third_party/benchmark")
endif()

# Google Benchmark v1.9.x ships its own gtest dep; suppress that since we
# already pull googletest in tests/.
set(BENCHMARK_ENABLE_TESTING       OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS   OFF CACHE BOOL "" FORCE)
set(BENCHMARK_USE_BUNDLED_GTEST    OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/benchmark"
                 "${CMAKE_BINARY_DIR}/_deps/benchmark-build" EXCLUDE_FROM_ALL)

# Bench binary: keydir + cask micro-benchmarks.
# Sanitizers are deliberately NOT linked here — perf numbers under sanitizers
# are misleading and have nothing to do with steady-state throughput.
add_executable(bitcask_bench
    keydir_bench.cpp
    cask_bench.cpp
    inverted_bench.cpp
    intersect_u64_proto_bench.cpp
    analyzer_bench.cpp
    hnsw_bench.cpp
    crc32_bench.cpp
    gate_bench.cpp
    bench_main.cpp
)
target_link_libraries(bitcask_bench
    PRIVATE
        benchmark::benchmark
        bitcask_keydir
        bitcask_cask
        bitcask_bm25
        bitcask_index
        bitcask_text
        bitcask_vector
        bitcask_format
        bitcask_warnings
)
