# BitcaskSanitizers.cmake
# Per-target sanitizer toggle. Usage:
#
#   cmake -S . -B build -DBITCASK_SANITIZE=address
#   cmake -S . -B build -DBITCASK_SANITIZE=undefined
#   cmake -S . -B build -DBITCASK_SANITIZE=address,undefined
#   cmake -S . -B build -DBITCASK_SANITIZE=thread
#
# Targets that link against bitcask_sanitizers (INTERFACE) inherit the flags.
# Tests must link this transitively; the NIF .so should *not* (sanitizers
# trip the BEAM if loaded).

set(BITCASK_SANITIZE "" CACHE STRING
    "Comma-separated sanitizer list: address,undefined,thread,leak (or empty)")

add_library(bitcask_sanitizers INTERFACE)

if(NOT "${BITCASK_SANITIZE}" STREQUAL "")
    string(REPLACE "," ";" _san_list "${BITCASK_SANITIZE}")

    # ASan and TSan are mutually exclusive.
    list(FIND _san_list "address" _has_asan)
    list(FIND _san_list "thread"  _has_tsan)
    if(NOT _has_asan EQUAL -1 AND NOT _has_tsan EQUAL -1)
        message(FATAL_ERROR
            "BITCASK_SANITIZE: address and thread sanitizers are mutually exclusive")
    endif()

    set(_san_flags "")
    foreach(s IN LISTS _san_list)
        if(MSVC)
            # S37-4：MSVC 只有 ASan。**未支持的档位报错退出而非降级**——
            # 「以为在跑 TSan、实际没插桩」比「构建失败」危险得多：那个 job
            # 会全绿并被当作并发正确性的证据。护栏损失见设计稿 §5.3
            # （并发正确性继续以 Linux TSan 为准）。
            if(s STREQUAL "address")
                # /fsanitize=address 与 /RTC1（CMake Debug 默认带）互斥，
                # 且要求 /INCREMENTAL:NO。两者在此一并处理。
                list(APPEND _san_flags /fsanitize=address)
            else()
                message(FATAL_ERROR
                    "BITCASK_SANITIZE=${s} 在 MSVC 下不可用（仅支持 address）。"
                    "thread/undefined/leak 无 MSVC 实现——见 doc/windows-port-design-zh.md §5.3。")
            endif()
        else()
            if(s STREQUAL "address")
                list(APPEND _san_flags -fsanitize=address -fno-omit-frame-pointer)
            elseif(s STREQUAL "undefined")
                list(APPEND _san_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
            elseif(s STREQUAL "thread")
                list(APPEND _san_flags -fsanitize=thread)
            elseif(s STREQUAL "leak")
                list(APPEND _san_flags -fsanitize=leak)
            else()
                message(FATAL_ERROR "BITCASK_SANITIZE: unknown sanitizer '${s}'")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _san_flags)

    target_compile_options(bitcask_sanitizers INTERFACE ${_san_flags})
    if(MSVC)
        # cl 的 /fsanitize=address 不是链接器开关；链接侧要的是 /INCREMENTAL:NO
        # （增量链接与 ASan 的 thunk 冲突）。同时清掉 Debug 默认的 /RTC1。
        target_link_options(bitcask_sanitizers INTERFACE /INCREMENTAL:NO)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}" CACHE STRING "" FORCE)
    else()
        target_link_options(bitcask_sanitizers INTERFACE ${_san_flags})
    endif()

    # C1 注:TSan 抑制不在此注入——动态 libtsan 不回调可执行文件内的
    # __tsan_default_suppressions 覆盖(实测零匹配),改由
    # cpp/tests/CMakeLists.txt 经测试 ENVIRONMENT 属性传
    # TSAN_OPTIONS=suppressions=cmake/tsan.supp。

    message(STATUS "bitcask: sanitizers enabled -> ${BITCASK_SANITIZE}")
endif()
