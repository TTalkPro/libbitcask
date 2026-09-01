# S37-4：3.20 → 3.21。合并静态库改用 `$<TARGET_OBJECTS:>` 取 STATIC 目标的对象
# 文件（替代原 bash+ar 脚本），该用法自 3.21 起才不限于 OBJECT 库。
cmake_minimum_required(VERSION 3.21)
# 版本单一真源（S12-7）：project(VERSION ...) 是唯一手写版本处。
# 库 SOVERSION 与 C API bitcask_version_* 均从此派生，杜绝三处手工同步漂移。
project(libbitcask VERSION 6.2.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# ⚠️ 本文件（连同 tests/ 与 bench/ 的两份）一律用 `PROJECT_SOURCE_DIR` /
#    `PROJECT_BINARY_DIR` 定位自己的东西，**不用 `CMAKE_SOURCE_DIR` /
#    `CMAKE_BINARY_DIR`**。
#
# 两者在**独立构建时完全等价**，所以这不是风格问题，是「能不能被别人
# add_subdirectory」的问题：被嵌入时 `CMAKE_SOURCE_DIR` 指的是**下游的仓库根**，
# 于是 `${CMAKE_SOURCE_DIR}/third_party/zlib` 找去了下游的树里，configure 期报
#
#     third_party/zlib 未初始化。请运行: git submodule update --init third_party/zlib
#
# —— 而子模块明明检出了，错的是找它的路径。这条错误消息**指向了错的原因**，
# 是最贵的那一类。
#
# ⚠️ 下游**不能**在自己那边绕过去：CMake 进入子目录作用域时会重新设
#    `CMAKE_SOURCE_DIR`，父作用域覆盖它无效；而唯一能在子目录里改它的注入点
#    （`CMAKE_PROJECT_<name>_INCLUDE`）会连带让本项目的 `PROJECT_IS_TOP_LEVEL`
#    判成真，进而污染下游的构建目录（实测：下游的 CMakeCache.txt 被换成了本项目
#    的）。所以这件事**只有在这里能修**。
#
# 加新路径时照抄这条：`PROJECT_SOURCE_DIR`，不是 `CMAKE_SOURCE_DIR`。
# ---------------------------------------------------------------------------

# 由 project 版本生成 C API 版本头（configure 时展开 @PROJECT_VERSION*@）。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/c_api/bitcask_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bitcask_version.h"
    @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(BitcaskSanitizers)

enable_testing()

# ---------------------------------------------------------------------------
# S37-4：MSVC 全局必需项。**目录作用域，且必须在任何 add_library /
# add_subdirectory 之前**——目录级编译选项在 target 创建时固化，放晚了
# third_party（utf8proc / googletest / benchmark）拿不到。
#
# 这三项是设计稿 §1.1「编译第一天必炸」的全部：
#
# ① /utf-8 —— 187 个源文件中 181 个含中文注释/字符串。不给此项，MSVC 按系统
#    ANSI 代码页（本机 936/GBK）解析源码：中文注释末字节若落在 0x5C（'\'），
#    会把下一行**续接进注释**吃掉真实代码，报出 C2001/C4819 且行号指向别处。
#    等价于 /source-charset:utf-8 /execution-charset:utf-8。
# ② NOMINMAX + WIN32_LEAN_AND_MEAN —— windows.h 的 min/max 函数式宏 vs 全库
#    94 处 std::min/std::max。**全局定义而非只在移植层给**：windows.h 只准进
#    移植层 TU，但第三方头（TBB / googletest）也可能间接引入，全局定义才封死。
# ③ /bigobj —— C++23 重模板 + segment.hpp 一类大 TU 会超过 COFF 的 65279 段上限。
#
# 附带的一致性开关（设计稿 §1.2 末行）：
# - /permissive- —— 关闭 MSVC 的宽松模式，两阶段名字查找与 GCC/Clang 对齐，
#   否则模板里的错误要到 Windows 实例化时才暴露。
# - /Zc:preprocessor —— 传统 MSVC 预处理器对 __VA_ARGS__ 的展开与标准不符
#   （整包当一个参数），库内可变参宏在其下行为会漂。
# - /Zc:__cplusplus —— 不给此项 MSVC 恒报 __cplusplus == 199711L，任何按该宏
#   分支的代码（含标准库探测）会走错分支。
# ---------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8 /bigobj /permissive- /Zc:preprocessor /Zc:__cplusplus)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # MSVC 对 std::fopen / strdup 报 C4996「不安全，请改用 fopen_s」。这两个
    # 都是**标准/POSIX 明文规定**的函数，弃用是微软单方面的；改用 _s 变体会
    # 让代码只能在 MSVC 上编。故按其自己的文档关掉这两类噪音，而不是改代码。
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()

if(MSVC)
    # ---------------------------------------------------------------------
    # S37-6：CRT 模型检查。**要求全进程共用一份 CRT（/MD），这是默认值。**
    #
    # `/MT` 下每个模块各带一份静态 CRT、**各有一份自己的堆与 fd 表**。而本库的
    # **C++ API 本就不是模块边界**——只有 C API 是。W5 之后仍跨界的是：
    #   - io::File::pread 返回 std::vector（库这侧分配、调用方析构）→ 跨堆 free。
    # （两处 FILE* 跨界已在 W5 消除：io.hpp 现在只交换内核句柄，FILE* 的生成与
    #  拆解由 detail/file_util.hpp 的 inline adopt_stream / stream_handle 在调用方
    #  模块内完成。）
    # 在 /MT + 多模块（尤其是 bitcask.dll）下，跨堆释放会触发 CRT 的
    # invalid-parameter handler → __fastfail：**进程无声消失、
    # 退出码 0xC0000409、terminate/abort 钩子都不触发**，现场只剩一个退出码。
    # （该状态码名为 STACK_BUFFER_OVERRUN，与栈无关，只是 CRT 致命退出的统一出口。）
    #
    # 给的是**告警而非硬错**：纯静态、单模块的 /MT 构建（整个进程一份 CRT），
    # 以及「跨模块但只经 C API」都成立；会出事的是「/MT + 跨模块传 C++/CRT 对象」。
    # ---------------------------------------------------------------------
    if(CMAKE_MSVC_RUNTIME_LIBRARY AND
       NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        message(WARNING
            "libbitcask: CMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY} "
            "选的是静态 CRT(/MT)。本库的 C++ API 会按值返回 STL 容器"
            "（io::File::pread 等），库这侧分配、调用方析构；若同时使用 "
            "bitcask.dll 或把本库与宿主分在不同模块，就是跨堆释放，"
            "会以 __fastfail(0xC0000409) 无声退出。仅当整个进程是单模块静态链接、"
            "或只经 C API 使用时才安全。详见 doc/api-c.md §2.1。")
    endif()
endif()

if(WIN32)
    # Windows 没有 RPATH：DLL 只按「exe 所在目录 → PATH」搜索。把所有可执行
    # 文件与 DLL（bitcask.dll / tbb12.dll / zlib1.dll）收进同一个 bin/，
    # ctest 直接跑 exe 全路径即可加载到依赖，不必给每个测试改 PATH。
    # 变量会被子目录继承，故 third_party 的 DLL 也落在这里。
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Common warning flags for all bitcask C++ targets.
# ---------------------------------------------------------------------------
add_library(bitcask_warnings INTERFACE)
if(MSVC)
    # S37-4：设计稿 §1.2 映射表。/Wall 不可用——它把整套 STL 头的告警也算进来，
    # 噪音量级远超可清理范围；/W4 是 MSVC 侧实际能收敛的最高档。
    # -fvisibility=hidden 无等价物：Windows 默认就不导出，靠 BITCASK_API 显式
    # dllexport（已就位），故映射表此行为空。
    target_compile_options(bitcask_warnings INTERFACE /W4)
else()
    target_compile_options(bitcask_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Woverloaded-virtual
        -fvisibility=hidden -fvisibility-inlines-hidden
    )
endif()
# S12-7：把告警升为错误。**默认关**——避免新编译器版本的新告警破坏下游/本地构建；
# CI 的库构建（BUILD_TESTING=OFF，只建 first-party）开启作回归护栏。third_party
# 头已标 SYSTEM，-Werror 不作用于它们。
#
# S37-4：MSVC 侧首轮**刻意不开** /WX。C4267（size_t→窄整型）/ C4244 / C4100
# （未引用形参）是 MSVC 独有告警，首轮必然刷屏；开 /WX 会让整个移植卡在
# 清告警上。收口挪到 S37-7。
option(BITCASK_WERROR "Treat first-party warnings as errors（CI 库构建开启）" OFF)
if(BITCASK_WERROR)
    if(MSVC)
        target_compile_options(bitcask_warnings INTERFACE /WX)
    else()
        target_compile_options(bitcask_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# zlib。两种来源，分流规则与上面的 oneTBB 一致：
#
# ① 系统/包管理器安装的 zlib（find_package）—— **Linux / BSD / macOS 默认**。
#    这些平台上 zlib 是基础系统库，`find_package(ZLIB)` 一直就能用；而且发行版
#    打包明确反对 vendored 副本——安全补丁由发行版统一推，钉死一个子模块版本
#    反而会把自己排除在补丁之外。此前的行为就是这条，保持不变。
# ② third_party/zlib 子模块现编（v1.3.1）—— **Windows 默认**，因为那里根本没有
#    「系统 zlib」这一说。这正是 vcpkg 此前存在的唯一理由：vcpkg.json 只列了
#    zlib 一个依赖，却要求装 vcpkg、设 VCPKG_ROOT、拉 x64-windows 三元组。
#    改走子模块后 Windows 的依赖来源与其余平台的其余依赖统一：
#    `git submodule update --init`，版本钉在本仓库的提交里。
#
# 为什么不 add_subdirectory(third_party/zlib) —— 那才是本仓库其余子模块的做法：
# zlib 自带的 CMakeLists 在 out-of-source 构建时会**把源码树里的 zconf.h 改名成
# zconf.h.included**（无条件，见其 CMakeLists 第 67-77 行），子模块的工作区从此
# 永远是 dirty，`git status` 与 `git submodule update` 都要跟着受影响。顺带还会
# 建共享库、examples、install/pkg-config 规则，我们一个都不要。
# 自建目标是十几行的事，换掉这些副作用划算。
#
# 代价：源文件清单得自己维护。zlib 的文件集多年未变，且下面对每个文件都做了
# 存在性检查——版本一升导致清单对不上，是**配置期报错**而不是静默漏编。
# zconf.h 直接用 zlib 仓库自带的那份（无 ./configure 的消费方本来就用它）。
# ---------------------------------------------------------------------------
if(WIN32)
    set(_bitcask_bundled_zlib_default ON)
else()
    set(_bitcask_bundled_zlib_default OFF)
endif()
option(BITCASK_BUNDLED_ZLIB
    "从 third_party/zlib 子模块编 zlib，而非 find_package（Windows 默认 ON）"
    ${_bitcask_bundled_zlib_default})

if(BITCASK_BUNDLED_ZLIB)
    # 顶层 project() 只启用了 CXX，而 zlib 是 C。其余 C 依赖（utf8proc）走
    # add_subdirectory，由它自己的 project() 启用 C，顶层无感；这里是顶层直接
    # add_library(.c)，必须显式启用，否则配置期就报「未知源文件扩展名」。
    # 只在这一支启用：Linux/BSD 走系统 zlib 时不必平白多探测一个编译器。
    enable_language(C)
    set(_bitcask_zlib_dir "${PROJECT_SOURCE_DIR}/third_party/zlib")
    if(NOT EXISTS "${_bitcask_zlib_dir}/zlib.h")
        message(FATAL_ERROR
            "third_party/zlib 未初始化。请运行:\n"
            "  git submodule update --init third_party/zlib")
    endif()
    set(_bitcask_zlib_srcs
        adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c
        gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c
        zutil.c)
    set(_bitcask_zlib_abs "")
    foreach(_f IN LISTS _bitcask_zlib_srcs)
        if(NOT EXISTS "${_bitcask_zlib_dir}/${_f}")
            message(FATAL_ERROR
                "third_party/zlib 里没有 ${_f}——子模块版本与本清单不符。"
                "核对 zlib 的文件集后更新 CMakeLists 里的 _bitcask_zlib_srcs。")
        endif()
        list(APPEND _bitcask_zlib_abs "${_bitcask_zlib_dir}/${_f}")
    endforeach()

    add_library(bitcask_zlib STATIC ${_bitcask_zlib_abs})
    add_library(ZLIB::ZLIB ALIAS bitcask_zlib)
    target_include_directories(bitcask_zlib PUBLIC "${_bitcask_zlib_dir}")
    # bitcask_shared（bitcask.dll / libbitcask.so）要链它，故必须 PIC。
    set_target_properties(bitcask_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # 第三方代码不受本项目的告警纪律约束（同 utf8proc/googletest 的处置）：
    # 它按自己的标准写就行，不该因为我们开了 /W4 -Werror 而编不过。
    if(MSVC)
        target_compile_definitions(bitcask_zlib
            PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
        target_compile_options(bitcask_zlib PRIVATE /W0)
    else()
        target_compile_options(bitcask_zlib PRIVATE -w)
    endif()
else()
    find_package(ZLIB REQUIRED)
endif()

# ---------------------------------------------------------------------------
# oneTBB。两种来源：
#
# ① 系统/包管理器安装的 TBB（find_package）—— Linux 默认，行为与既往一致。
# ② third_party/oneTBB 子模块现编（v2022.0.0）—— 两种情况下必须走这条：
#    - **TSan 构建**：系统 libtbb 未插桩，必须自己带 -fsanitize=thread 编。
#    - **Windows/MSVC**（S37-4）：Windows 上没有「系统 TBB」这一说；而 vcpkg
#      的 tbb 端口经 hwloc 依赖拖进 msys2（m4/perl/autotools）工具链，与本项目
#      「纯 MSVC」的构建约束冲突。oneTBB 自身用 MSVC + CMake 直接可编，
#      故从子模块现编，vcpkg 只留 zlib。
# ---------------------------------------------------------------------------
if(BITCASK_SANITIZE MATCHES "thread")
    set(_bitcask_bundled_tbb_default ON)
    set(_bitcask_bundled_tbb_why "TSan 需要插桩版")
elseif(WIN32)
    set(_bitcask_bundled_tbb_default ON)
    set(_bitcask_bundled_tbb_why "Windows 无系统 TBB，且不引 vcpkg tbb→hwloc→msys2")
else()
    set(_bitcask_bundled_tbb_default OFF)
    set(_bitcask_bundled_tbb_why "")
endif()
option(BITCASK_BUNDLED_TBB
    "从 third_party/oneTBB 子模块现编 TBB，而非 find_package"
    ${_bitcask_bundled_tbb_default})

if(BITCASK_BUNDLED_TBB)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/oneTBB/CMakeLists.txt")
        message(FATAL_ERROR
            "third_party/oneTBB 未初始化（本构建需要现编 TBB：${_bitcask_bundled_tbb_why}）。请运行:\n"
            "  git submodule update --init third_party/oneTBB")
    endif()
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    set(_bitcask_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    if(BITCASK_SANITIZE MATCHES "thread")
        # 只有 TSan 这条需要给 TBB 自身加插桩开关；Windows 那条不加任何东西。
        set(CMAKE_CXX_FLAGS
            "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    endif()
    add_subdirectory(third_party/oneTBB EXCLUDE_FROM_ALL)
    set(CMAKE_CXX_FLAGS "${_bitcask_saved_cxx_flags}")
    # S37-5：oneTBB 把自己的产物放进 <build>/msvc_<ver>_cxx23_64_md_<cfg>/，
    # 覆盖了上面给 WIN32 设的 CMAKE_RUNTIME_OUTPUT_DIRECTORY。Windows 没有
    # RPATH，tbb12.dll 不与 exe 同目录就加载不到——**而且失败点在构建期**：
    # gtest_discover_tests 会运行每个测试 exe 去枚举用例，exe 起不来直接
    # 让构建失败（报「Error running test executable」，与 DLL 缺失毫无字面关联）。
    # 逐目标把运行时产物拉回 bin/；per-config 变体一并设，多配置生成器下
    # 只有它们生效。
    #
    # 另一件必须在这里补的事：把 oneTBB 的公开头改标 SYSTEM。
    # 上面 BITCASK_WERROR 处写着「third_party 头已标 SYSTEM，-Werror 不作用于
    # 它们」——对 find_package(TBB) 这条为真（导入目标带 SYSTEM 语义），对
    # add_subdirectory 这条**为假**：源码构建的 tbb 目标只有普通
    # INTERFACE_INCLUDE_DIRECTORIES，于是 oneapi/tbb/*.h 里的 old-style cast 与
    # 隐式符号转换全算进我们的 -Wold-style-cast / -Wsign-conversion。
    # 平时只是刷屏，开 BITCASK_WERROR 就直接编不过——而 **FreeBSD / macOS 上
    # 没有「系统 TBB」这一说，bundled 是唯一可选项**，等于在这些平台上
    # -Werror 恒定不可用。Windows 与 TSan 构建同理。
    foreach(_tbb_t tbb tbbmalloc tbbmalloc_proxy)
        if(TARGET ${_tbb_t})
            get_target_property(_tbb_inc ${_tbb_t} INTERFACE_INCLUDE_DIRECTORIES)
            if(_tbb_inc)
                set_target_properties(${_tbb_t} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_tbb_inc}")
            endif()
        endif()
    endforeach()
    foreach(_tbb_t tbb tbbmalloc tbbmalloc_proxy)
        if(TARGET ${_tbb_t})
            set_target_properties(${_tbb_t} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY                "${PROJECT_BINARY_DIR}/bin"
                RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${PROJECT_BINARY_DIR}/bin"
                RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${PROJECT_BINARY_DIR}/bin"
                RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PROJECT_BINARY_DIR}/bin"
                RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${PROJECT_BINARY_DIR}/bin")
        endif()
    endforeach()
    message(STATUS "libbitcask: bundled oneTBB from third_party/oneTBB (${_bitcask_bundled_tbb_why})")
else()
    find_package(TBB REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Release 性能选项（-falign-functions=64 + LTO/IPO）。
# 需在所有 add_library 之前设置（目录属性在 target 创建时固化）。
# ---------------------------------------------------------------------------
# S37-4 补 `AND NOT MSVC`：clang-cl 下 CMAKE_CXX_COMPILER_ID 是 "Clang" 而
# CMake 的 MSVC 变量**同时为真**，两个分支会一起生效 —— 下面 GNU 语法的
# -f/-Wl 选项就会跟着 /W4 一起发出去（-Wl,-z,relro 到 lld-link 直接报错）。
# Linux 上 MSVC 恒假，此条件不改变任何既有行为。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    add_compile_options($<$<CONFIG:Release>:-falign-functions=64>)
endif()

# ---------------------------------------------------------------------------
# H1-H4 构建加固（栈保护 / FORTIFY / Full RELRO / PIE）。目录作用域——必须在
# 任何 add_library/add_executable 之前设置（目录级编译/链接选项在 target 创建
# 时固化）。sanitizer 构建按需排除 FORTIFY（与运行时 interceptor 冲突）。
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)   # NOT MSVC：见上（clang-cl）
    # H1:栈溢出探测——strong 覆盖含数组/大局部对象/取址局部变量的函数
    # （比默认 -fstack-protector 覆盖面大，开销可忽略）。全配置启用。
    add_compile_options(-fstack-protector-strong)

    # H3:Full RELRO——GOT 启动时全解析后置只读。默认链接仅 partial RELRO
    # （有 GNU_RELRO 段但无 BIND_NOW）；-z,now 补齐 BIND_NOW。覆盖 .so + 可执行。
    add_link_options(-Wl,-z,relro,-z,now)

    # H2:_FORTIFY_SOURCE=2——glibc 对 memcpy/strcpy 等加边界检查。需 -O1+
    # （Debug -O0 仅触发无效告警），且与 ASAN/TSan 运行时 interceptor 冲突，
    # 故仅在优化的非 sanitizer 构建启用。先 -U 防发行版预定义导致重定义告警。
    if("${BITCASK_SANITIZE}" STREQUAL "")
        add_compile_options(
            $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
    endif()

    # H4:可执行文件 PIE 已由顶部 CMAKE_POSITION_INDEPENDENT_CODE ON +
    # CMP0083=NEW（cmake≥3.14，本项目要求 3.20）保证——migrate_le/gen_inert_table
    # 实测为 DYN/PIE，无需额外 -pie。
endif()

# ⑰ 可选 -march=native（默认 OFF）。SIMD 内核本就运行时按 CPUID 派发，故
# 通用构建故意不带 -march；此开关仅给「在部署机上现编」场景榨取 scalar glue
# 的 ~3-8%（让非 SIMD 代码也用本机 ISA）。勿与 -ffast-math 混用：HNSW/int8
# 依赖精确可复现浮点（int8 kernel self-test 校验 ULP）。
option(BITCASK_NATIVE "为本机现编启用 -march=native（默认 OFF，破坏二进制可移植性）" OFF)
if(BITCASK_NATIVE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)   # NOT MSVC：见上（clang-cl）
        add_compile_options(-march=native)
        message(STATUS "libbitcask: BITCASK_NATIVE -> -march=native（二进制仅限本机 ISA）")
    else()
        # S37-4：MSVC 驱动器无 -march=native 等价物（/arch: 只有固定档位，
        # 且不探测本机）。**报错退出而非静默忽略**——本开关的语义是「我明确
        # 要为本机榨性能」，静默忽略会让人以为拿到了本机码而实际没有，
        # 且无从察觉。
        message(FATAL_ERROR
            "BITCASK_NATIVE 在当前编译器驱动下无等价物"
            "（ID=${CMAKE_CXX_COMPILER_ID}，MSVC 兼容驱动器）：/arch: 是固定档位，"
            "不按本机 CPU 探测。请关闭该选项；SIMD 内核本就运行时按 CPUID 派发，"
            "通用构建不受影响。")
    endif()
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
# bitcask_simd — 运行时 ISA 探测（CPUID/XGETBV）+ BITCASK_SIMD_MAX 档位钳制。
#
# S37-3：替代 __builtin_cpu_supports（MSVC 不支持）。刻意**不带任何 -m/-arch
# 开关**——本 TU 只做探测，绝不能让编译器有权在此生成宽指令。
# ---------------------------------------------------------------------------
add_library(bitcask_simd STATIC
    src/simd/cpu_features.cpp
    src/simd/crc32_sse42.cpp     # S37-3.b：分 ISA TU（见下 bitcask_simd_tu）
)
target_include_directories(bitcask_simd PUBLIC include)
# S37-4：ZLIB PUBLIC —— 公开头 hw_crc32.hpp 里 `#include <zlib.h>`（scalar CRC32
# 与 zlib::crc32 逐位一致，需要它的原型），crc32_sse42.cpp 就是包含者之一。
# 此前无人显式声明该依赖：Linux 上 zlib.h 躺在 /usr/include，编译器**无需任何
# -I 就能找到**，缺失的依赖声明因此永远不报错。Windows 上没有这种全局头目录，
# 一编即炸——本届暴露的第一类「被系统路径掩盖的依赖缺失」。
target_link_libraries(bitcask_simd
    PUBLIC  ZLIB::ZLIB
    PRIVATE bitcask_warnings bitcask_sanitizers)

# ---------------------------------------------------------------------------
# bitcask_simd_tu(<源文件> <档位>) — 给单个 TU 施加该 ISA 档的编译选项。
# 档位取值：avx2 | avx512 | vnni | vnni512 | sse42
#
# S37-3.b：SIMD 内核按 ISA 分 TU 的落点。
#
# **为什么必须分 TU 而不是继续用 __attribute__((target))**：
#   MSVC 既无该属性也无 __builtin_cpu_supports，`/arch:` 是**每 TU** 生效的
#   唯一手段。GCC/Clang 侧同样改用 -m 开关，两边一套结构，免得维护两份。
#
# ⚠️ **被施加了 -m/-arch 的 TU 里，编译器有权在任何函数中生成该 ISA 的指令**
# （不止 intrinsic，还包括自动向量化的胶水代码）。因此这类 TU 必须满足：
#   ① 只放运行时门后才会被调用的内核；
#   ② **不得有静态初始化器 / 全局构造** —— 它们在 main 之前无条件执行，
#      在不支持该 ISA 的 CPU 上直接 SIGILL，且崩在库加载期极难定位；
#   ③ 不得放 scalar 回退实现（那是要在任何 CPU 上都能跑的）。
#
# MSVC 无 /arch:SSE4.2——x64 基线已含 SSE2，SSE4.2/PCLMUL intrinsic 在 MSVC 下
# 无需开关即可用，故 sse42 档在 MSVC 侧不加任何选项。
# ---------------------------------------------------------------------------
function(bitcask_simd_tu src tier)
    if(MSVC)
        if(tier STREQUAL "avx2" OR tier STREQUAL "vnni")
            set(_opts "/arch:AVX2")
        elseif(tier STREQUAL "avx512" OR tier STREQUAL "vnni512")
            set(_opts "/arch:AVX512")
        else()
            set(_opts "")          # sse42：见上
        endif()
    else()
        if(tier STREQUAL "avx2")
            set(_opts -mavx2 -mfma)
        elseif(tier STREQUAL "vnni")
            set(_opts -mavx2 -mfma -mavxvnni)
        elseif(tier STREQUAL "avx512")
            # 与 MSVC /arch:AVX512 的隐含集合对齐（F+CD+BW+DQ+VL）——运行时门
            # 也按此整集放行（见 detail/cpu_features.hpp）。两边一致才不会出现
            # 「一侧编出了另一侧门不放行的指令」。
            set(_opts -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl)
        elseif(tier STREQUAL "vnni512")
            set(_opts -mavx512f -mavx512cd -mavx512bw -mavx512dq -mavx512vl
                      -mavx512vnni)
        elseif(tier STREQUAL "sse42")
            set(_opts -msse4.2 -mpclmul)
        else()
            message(FATAL_ERROR "bitcask_simd_tu: 未知档位 '${tier}'")
        endif()
    endif()
    # SKIP_PRECOMPILE_HEADERS 必开：PCH 在**目标级**用统一选项预编译，而本函数
    # 给单个 TU 追加 -m/-arch。GCC 会报
    #   "cmake_pch.hxx.gch: created and used with differing settings of '-mavx'"
    # 并退化/失效；更糟的是这类不一致属于「警告而非错误」，极易被忽略后
    # 演变成难以定位的行为差异。分 ISA TU 本就很小，不用 PCH 无性能顾虑。
    set_source_files_properties("${src}" PROPERTIES
        COMPILE_OPTIONS "${_opts}"
        SKIP_PRECOMPILE_HEADERS ON)
endfunction()

bitcask_simd_tu(src/simd/crc32_sse42.cpp sse42)

# ---------------------------------------------------------------------------
# bitcask_format — codec for data file / hint file records.
# ---------------------------------------------------------------------------
add_library(bitcask_format STATIC
    src/fileops/codec.cpp
)
target_include_directories(bitcask_format PUBLIC include)
# S37-1：detail/file_util.hpp 的宿主原语（atomic_rename / sync_directory /
# flush_and_sync_stream）下沉到 io seam 后，任何包含该头的 target 都需要 io
# 符号——而它经 index_manifest.hpp / field_schema.hpp / oki_run.hpp /
# search_checkpoint.hpp 等公共头传播到几乎所有下游。由最低公共层 PUBLIC
# 传递，避免逐个 target 补链接的打地鼠。无环：bitcask_io 不依赖 format。
# S37-3：codec.cpp 同时包含 hw_crc32.hpp 与 int8_kernels.hpp，两者的派发都要
# 用 bitcask_simd；与 bitcask_io 同理由 PUBLIC 向下游传播。
target_link_libraries(bitcask_format
    PUBLIC ZLIB::ZLIB bitcask_io bitcask_simd
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_io — 平台 seam 的实现侧。S37-1 起，**全库唯一允许直接调用宿主原语的
# 地方就是这里的后端 TU**（第一方 src/ include/ c_api/ tools/ 的裸 POSIX 头已
# 归零，唯一命中是本目录）。换平台 = 换一个后端文件，而不是改 20 个文件。
# ---------------------------------------------------------------------------
if(WIN32)
    set(_bitcask_io_backend src/io/win32_file.cpp)
else()
    set(_bitcask_io_backend src/io/posix_file.cpp)
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_bitcask_io_backend}")
    # S37-4 阶段性状态：Windows 后端（S37-5）尚未落地。
    # 静态库目标不做符号解析，故去掉后端后**其余全部第一方 TU 照样编译**——
    # 这正是 S37-4 用来量清 MSVC 编译面的形态。链接期目标
    # （bitcask_shared / 各测试 exe）会缺 io 符号，属预期，不是回归。
    message(WARNING
        "libbitcask: 平台 I/O 后端 '${_bitcask_io_backend}' 不存在——本次构建"
        "不含 io seam 实现。库目标可编译，链接期目标会缺符号（S37-5 待落地）。")
    set(_bitcask_io_backend "")
endif()

add_library(bitcask_io STATIC
    ${_bitcask_io_backend}
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
    src/fileops/oki_run.cpp
    src/fileops/oki_state.cpp
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
# ankerl::unordered_dense（单头 INTERFACE 库）：KeyDir 分片表用它替代
# std::unordered_map（开放寻址 + 稠密存储，消除每键节点 malloc + 指针追逐）。
# header-only，EXCLUDE_FROM_ALL 不会引入测试/benchmark 目标。
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/unordered_dense/include/ankerl/unordered_dense.h")
    message(FATAL_ERROR
        "third_party/unordered_dense 未初始化。请运行:\n"
        "  git submodule update --init third_party/unordered_dense")
endif()
add_subdirectory(third_party/unordered_dense EXCLUDE_FROM_ALL)

add_library(bitcask_keydir STATIC
    src/keydir/keydir.cpp
    src/keydir/keydir_registry.cpp
)
target_include_directories(bitcask_keydir PUBLIC include)
target_link_libraries(bitcask_keydir
    PUBLIC  bitcask_fileops bitcask_io unordered_dense::unordered_dense
    # S37-4：keydir_registry.cpp 包含 thread_pool.hpp（→ oneapi/tbb/*）。
    # PRIVATE 即可——keydir 自身的公开头不暴露 TBB。同为「被 /usr/include 掩盖」类。
    PRIVATE bitcask_warnings bitcask_sanitizers TBB::tbb
)

# ---------------------------------------------------------------------------
# bitcask_docmap — DocMap 宿主服务（ord↔ext/live/meta 身份表 + 组件 ckpt）。
# S18-10：自 bitcask_index 改名（设计 §8/§10-4 终态名）；旧名 ALIAS 兼容
# 一个版本期，下游链接名平滑迁移。
# ---------------------------------------------------------------------------
add_library(bitcask_docmap STATIC
    src/keydir/index.cpp
    src/keydir/index_kernels_avx2.cpp   # S37-3.b：分 ISA TU
    src/keydir/docmap_ckpt.cpp   # S18-2：docmap 组件 ckpt（宿主侧持久化）
)
bitcask_simd_tu(src/keydir/index_kernels_avx2.cpp avx2)
target_include_directories(bitcask_docmap PUBLIC include)
target_link_libraries(bitcask_docmap
    PUBLIC  bitcask_text bitcask_format   # format：sidecar/组件容器 CRC（S18-2）
    PRIVATE bitcask_warnings bitcask_sanitizers
)
add_library(bitcask_index ALIAS bitcask_docmap)  # 兼容别名（P5 撤）

# ---------------------------------------------------------------------------
# bitcask_plugin_api — KV 存储层插件回调接口（S15 P1，纯头文件、自包含：
# 只依赖标准库）。Cask/merge/thread_pool 经它与索引插件解耦。
# ---------------------------------------------------------------------------
add_library(bitcask_plugin_api INTERFACE)
target_include_directories(bitcask_plugin_api INTERFACE include)

# ---------------------------------------------------------------------------
# bitcask_bm25 — BM25 倒排索引（InvertedIndex + DAAT 评分）。
# ---------------------------------------------------------------------------
add_library(bitcask_bm25 STATIC
    src/bm25/intersect.cpp
    src/bm25/bm25_kernels_avx2.cpp        # S37-3.b：分 ISA TU
    src/bm25/bm25_kernels_avx512.cpp
    src/bm25/intersect_kernels_avx2.cpp
    src/bm25/intersect_kernels_avx512.cpp
    src/bm25/inverted.cpp
    src/bm25/query_parser.cpp
    src/bm25/segment_v2.cpp
)
bitcask_simd_tu(src/bm25/bm25_kernels_avx2.cpp        avx2)
bitcask_simd_tu(src/bm25/bm25_kernels_avx512.cpp      avx512)
bitcask_simd_tu(src/bm25/intersect_kernels_avx2.cpp   avx2)
bitcask_simd_tu(src/bm25/intersect_kernels_avx512.cpp avx512)
target_include_directories(bitcask_bm25 PUBLIC include)
# S37-4：TBB 由 PRIVATE 改 PUBLIC —— 公开头 inverted.hpp 直接
# `#include <oneapi/tbb/concurrent_hash_map.h>`，而它经 segment.hpp /
# text_plugin.hpp / search_cache.hpp / segment_query.hpp 传播到大量下游。
# 同 zlib 那条：Linux 上系统 TBB 头在 /usr/include，缺声明也能编，
# Windows 上 bitcask_hybrid 首先炸出来。
target_link_libraries(bitcask_bm25
    PUBLIC bitcask_format bitcask_io   # B3：MmapSegment 用 io::MappedFile
           TBB::tbb
    PRIVATE bitcask_warnings bitcask_sanitizers)

# ---------------------------------------------------------------------------
# bitcask_vector — 向量索引（HNSW + IVF 段，S32-M3）。
# ---------------------------------------------------------------------------
add_library(bitcask_vector STATIC
    src/vector/hnsw.cpp
    src/vector/hnsw_kernels_avx2.cpp     # S37-3.b：分 ISA TU（见下 bitcask_simd_tu）
    src/vector/hnsw_kernels_avx512.cpp
    src/vector/int8_kernels_avx2.cpp
    src/vector/int8_kernels_vnni.cpp
    src/vector/int8_kernels_vnni512.cpp
    src/vector/ivf_rq.cpp
    src/vector/diskann.cpp
)
bitcask_simd_tu(src/vector/hnsw_kernels_avx2.cpp   avx2)
bitcask_simd_tu(src/vector/hnsw_kernels_avx512.cpp avx512)
bitcask_simd_tu(src/vector/int8_kernels_avx2.cpp    avx2)
bitcask_simd_tu(src/vector/int8_kernels_vnni.cpp    vnni)
bitcask_simd_tu(src/vector/int8_kernels_vnni512.cpp vnni512)
target_include_directories(bitcask_vector
    PUBLIC  include
    PRIVATE src/vector
)
target_link_libraries(bitcask_vector
    PUBLIC  bitcask_format bitcask_io  # B3：payload mmap 用 io::MappedFile
    PRIVATE bitcask_warnings bitcask_sanitizers TBB::tbb
)

# ---------------------------------------------------------------------------
# bitcask_search — SearchLayer (Index + InvertedIndex + Analyzer wrapper).
# ---------------------------------------------------------------------------
# S18-10（设计 §8 终态）：搜索域拆三目标——TextPlugin（BM25 全家）、
# VectorPlugin（HNSW）、HybridSearcher（可选融合器）；bitcask_search 退化
# 为 SearchLayer shim 过渡目标（P5 随 shim 删除）。解锁独立装配：
# KV+BM25（链 text_plugin，无向量依赖）/ KV+HNSW（链 vector_plugin，
# 无 jieba/utf8proc——经 bm25 的 DocTable 头只读消费，不链 bitcask_text）。
add_library(bitcask_text_plugin STATIC
    src/search/text_plugin.cpp
    src/search/search_arena.cpp    # S19-1：inter-query Search 池（生产件）
    src/search/search_cache.cpp
    src/search/highlighter.cpp
)
target_include_directories(bitcask_text_plugin PUBLIC include)
target_link_libraries(bitcask_text_plugin
    PUBLIC  bitcask_docmap bitcask_bm25 bitcask_text bitcask_plugin_api
    PRIVATE bitcask_warnings bitcask_sanitizers TBB::tbb  # TBB：search_arena
)

add_library(bitcask_vector_plugin STATIC
    src/search/vector_plugin.cpp
    src/search/vector_plugin_kernels_avx2.cpp    # S37-3.b：分 ISA TU
    src/search/vector_plugin_kernels_avx512.cpp
    src/search/ivf_plugin.cpp
    src/search/diskann_plugin.cpp
)
bitcask_simd_tu(src/search/vector_plugin_kernels_avx2.cpp   avx2)
bitcask_simd_tu(src/search/vector_plugin_kernels_avx512.cpp avx512)
target_include_directories(bitcask_vector_plugin PUBLIC include)
target_link_libraries(bitcask_vector_plugin
    PUBLIC  bitcask_vector bitcask_format bitcask_plugin_api
    PRIVATE bitcask_warnings bitcask_sanitizers
)

add_library(bitcask_hybrid STATIC
    src/search/hybrid_searcher.cpp
)
target_include_directories(bitcask_hybrid PUBLIC include)
target_link_libraries(bitcask_hybrid
    PUBLIC  bitcask_text_plugin bitcask_vector_plugin
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# S19-3：bitcask_search（SearchLayer shim）已降级为测试夹具——源码移
# tests/support/，目标定义在 tests/CMakeLists.txt（bitcask_search_shim）。

# ---------------------------------------------------------------------------
# bitcask_merge — merge 策略 + 执行。
# ---------------------------------------------------------------------------
add_library(bitcask_merge STATIC
    src/merge/merger.cpp
    src/merge/merge_policy.cpp
)
target_include_directories(bitcask_merge PUBLIC include)
target_link_libraries(bitcask_merge
    PUBLIC  bitcask_keydir bitcask_fileops bitcask_io bitcask_format bitcask_plugin_api   # S18-7：merge 去搜索化（设计 §8）
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# bitcask_cask — Cask KV store (CRUD + merge + iterators).
# ---------------------------------------------------------------------------
add_library(bitcask_cask STATIC
    src/cask/cask.cpp
    src/cask/cask_iter.cpp     # S21-3 B1：CaskIter（fold 迭代器）拆分 TU
    src/cask/cask_range_iter.cpp  # S33-5：OKI 有序 Range 迭代器
    src/cask/cask_search.cpp   # S21-3 B1：搜索门面拆分 TU
    src/cask/cask_recovery.cpp # S21-3 B1：恢复域拆分 TU
    src/cask/txn.cpp           # S34：TxnCask 多键事务 helper
    src/cask/meta_file.cpp
    src/cask/legacy_ckpt.cpp   # S19-2：pre-S17 统一 ckpt 迁移读取器
)
target_include_directories(bitcask_cask PUBLIC include)
# S37-4：TBB 由 PRIVATE 改 PUBLIC —— **公开头** cask.hpp 包含 thread_pool.hpp，
# 后者直接 `#include <oneapi/tbb/concurrent_queue.h>`。任何 include 了 cask.hpp
# 的下游（近乎全部测试与 c_api）都需要 TBB 的头路径。
target_link_libraries(bitcask_cask
    PUBLIC  bitcask_keydir bitcask_fileops bitcask_io bitcask_format bitcask_merge bitcask_hybrid  # S19-2：去 shim 依赖
            TBB::tbb
    PRIVATE bitcask_warnings bitcask_sanitizers
)

# ---------------------------------------------------------------------------
# utf8proc — Unicode NFKC 归一化 + case fold（submodule: third_party/utf8proc）。
# ---------------------------------------------------------------------------
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/utf8proc/CMakeLists.txt")
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
# migrate_le — v1 大端 → 当前纪元离线迁移工具（旧入口,保留兼容;新增迁移
# 请走统一入口 bitcask_migrate）。
# ---------------------------------------------------------------------------
add_executable(migrate_le tools/migrate_le.cpp)
target_link_libraries(migrate_le
    PRIVATE bitcask_fileops bitcask_format bitcask_io
            bitcask_warnings bitcask_sanitizers)

# ---------------------------------------------------------------------------
# bitcask_migrate — 格式纪元统一迁移入口（detect / be2le / tstamp64）。
# ---------------------------------------------------------------------------
add_executable(bitcask_migrate tools/bitcask_migrate.cpp)
target_link_libraries(bitcask_migrate
    PRIVATE bitcask_fileops bitcask_format bitcask_io
            bitcask_warnings bitcask_sanitizers)

# vec_engine_migrate — 向量引擎离线切换（S32-M4;设计 §6.4）。只改 meta,
# 目标引擎索引由首次 open 全量 fold 重建（属性测试守护）。
add_executable(vec_engine_migrate tools/vec_engine_migrate.cpp)
target_link_libraries(vec_engine_migrate
    PRIVATE bitcask_cask bitcask_io
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
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/cppjieba/include/cppjieba/Jieba.hpp" OR
   NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/limonp/include/limonp/StringUtil.hpp")
    message(FATAL_ERROR
        "third_party/cppjieba 或 third_party/limonp 未初始化。请运行:\n"
        "  git submodule update --init third_party/cppjieba third_party/limonp")
endif()

if(NOT TARGET cppjieba)
    add_library(cppjieba INTERFACE)
    # SYSTEM：cppjieba/limonp 是 vendored 第三方头，含大量 -Wold-style-cast/
    # -Wsign-conversion 等告警；标 SYSTEM 让编译器视为系统头、不对其告警，
    # 使 BITCASK_WERROR（-Werror）只作用于 first-party 代码。
    target_include_directories(cppjieba SYSTEM INTERFACE
        "${PROJECT_SOURCE_DIR}/third_party/cppjieba/include"
        "${PROJECT_SOURCE_DIR}/third_party/limonp/include"
    )
endif()

set(BITCASK_JIEBA_DICT_DIR "${PROJECT_SOURCE_DIR}/third_party/cppjieba/dict"
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
# H5:预编译头（PCH）——预编译重型 STL 公共头，编译时间 −20~30%。重型 TU
# （cask/search/keydir/bm25/text）各自 PCH 同一组头（均共享 warnings/sanitizers
# 选项 → flags 一致）。仅 STL 头入 PCH（恒可用、与各 TU 无冲突）。排查 PCH 相关
# 编译异常时可 -DBITCASK_PCH=OFF 关闭。
# ---------------------------------------------------------------------------
option(BITCASK_PCH "预编译头加速编译（默认 ON）" ON)
if(BITCASK_PCH)
    set(_bitcask_pch_headers
        <algorithm> <cstdint> <expected> <memory> <optional>
        <span> <string> <string_view> <unordered_map> <vector>)
    foreach(_t bitcask_cask bitcask_text_plugin bitcask_keydir bitcask_bm25 bitcask_text)
        target_precompile_headers(${_t} PRIVATE ${_bitcask_pch_headers})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# libbitcask 聚合 target（libbitcask.a + libbitcask.so）
# ---------------------------------------------------------------------------

# libbitcask.so — 共享库, C API (extern "C") 跨 ABI 稳定
add_library(bitcask_shared SHARED
    c_api/bitcask_kv.cpp     # S19-5：C API 分文件（KV/生命周期/迭代/管理）
    c_api/bitcask_text.cpp   # S19-5：BM25 文本搜索
    c_api/bitcask_vec.cpp    # S19-5：向量/混合检索
)
target_include_directories(bitcask_shared
    PUBLIC include c_api
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")  # 生成的 bitcask_version.h
target_link_libraries(bitcask_shared PRIVATE bitcask_cask)
target_compile_definitions(bitcask_shared PRIVATE BITCASK_DLL_EXPORTS)
set_target_properties(bitcask_shared PROPERTIES
    OUTPUT_NAME "bitcask"
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
)
add_library(Bitcask::shared ALIAS bitcask_shared)

# ---------------------------------------------------------------------------
# libbitcask.a / bitcask_static.lib — 合并全部第一方归档为单一静态库。
#
# S37-4：原实现**在配置期生成 bash 脚本**跑 `ar x` 解包 13 个归档到同一临时
# 目录再 `ar rcs` 重打包。Windows 既无 bash 也无 ar，必须重写。
#
# 现改为 CMake 原生：把各库的**对象文件**直接喂给一个真正的 STATIC target。
# `$<TARGET_OBJECTS:>` 自 CMake 3.21 起对 STATIC/SHARED 目标同样可用（不再限于
# OBJECT 库），故**无需把 13 个库改成 OBJECT 库**——链接图与 Linux 侧现有行为
# 逐字不变，只有「怎么产出这一个归档」变了。
#
# 顺带修掉的既有隐患：`ar x` 把 13 个归档解到**同一个**临时目录，不同库中的
# **同名 .o 会静默相互覆盖**（后解包的赢），产物里少一份实现且不报错。当前 43
# 个源文件基名恰好互不相同，故尚未发生——属于「加一个重名文件就中招」的定时
# 炸弹。对象文件按目标各自成路径喂进来后，结构上不再可能。
#
# 顺带修掉的一个现行缺失：**bitcask_simd 原本不在合并列表里**（S37-3 新增该库
# 时漏补）。即当前发布的 libbitcask.a 缺 cpu_features / crc32_sse42 两个 TU，
# 静态链接的下游会撞未定义符号。已补入。
# ---------------------------------------------------------------------------
add_library(bitcask_static STATIC
    $<TARGET_OBJECTS:bitcask_simd>          # S37-4 补：原漏
    $<TARGET_OBJECTS:bitcask_format>
    $<TARGET_OBJECTS:bitcask_io>
    $<TARGET_OBJECTS:bitcask_fileops>
    $<TARGET_OBJECTS:bitcask_keydir>
    $<TARGET_OBJECTS:bitcask_docmap>
    $<TARGET_OBJECTS:bitcask_bm25>
    $<TARGET_OBJECTS:bitcask_vector>
    $<TARGET_OBJECTS:bitcask_text>
    $<TARGET_OBJECTS:bitcask_text_plugin>
    $<TARGET_OBJECTS:bitcask_vector_plugin>
    $<TARGET_OBJECTS:bitcask_hybrid>
    $<TARGET_OBJECTS:bitcask_merge>
    $<TARGET_OBJECTS:bitcask_cask>
)
# Windows：bitcask_shared 的**导入库**已叫 bitcask.lib，同目录不能再有同名的
# 合并静态库。Unix 侧 libbitcask.so 与 libbitcask.a 后缀不同，可沿用原名。
if(WIN32)
    set_target_properties(bitcask_static PROPERTIES OUTPUT_NAME "bitcask_static")
else()
    set_target_properties(bitcask_static PROPERTIES OUTPUT_NAME "bitcask")
endif()
# 归档不含第三方对象（utf8proc / zlib / TBB 各自成库），故静态链接的下游必须
# 一并链上它们。原 custom-target 版没有任何 usage requirement，下游只能手工
# 猜；这里补成 INTERFACE 依赖（纯增量，不改任何既有目标的链接）。
target_include_directories(bitcask_static INTERFACE include)
target_link_libraries(bitcask_static INTERFACE
    ZLIB::ZLIB TBB::tbb ${UTF8PROC_TARGET} unordered_dense::unordered_dense)

# ---------------------------------------------------------------------------
# Install 规则
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
# S37-4：Windows 的 DLL 走 RUNTIME（→ bin/，须与 exe 同目录才能被加载器找到），
# 而**导入库** bitcask.lib 走 ARCHIVE（→ lib/）。Unix 侧 .so 走 LIBRARY，
# ARCHIVE/RUNTIME 两条对它无效——故三条并存即可两平台通吃。
# 原规则已有这三条且方向正确；真正缺的是合并静态库那条（下方），
# 它此前是 install(FILES <硬编码 .a 路径>)，在 Windows 上指向不存在的文件。
install(TARGETS bitcask_shared
    EXPORT BitcaskTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS bitcask_static
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
# Windows 上 zlib 由子模块现编，于是它成了**我们的产物**。合并静态库不含第三方
# 对象（见 bitcask_static 上方注释），所以静态链接的下游还得拿到这一份，否则装出来
# 的包缺 crc32 等符号。Linux/BSD 走系统 zlib（BITCASK_BUNDLED_ZLIB=OFF），
# ZLIB::ZLIB 是外部 imported target，不该也无法由我们安装。
if(BITCASK_BUNDLED_ZLIB)
    install(TARGETS bitcask_zlib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
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
