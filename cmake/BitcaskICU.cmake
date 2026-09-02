# BitcaskICU — ICU 探测与接入（S38）。
#
# 来源由 BITCASK_ICU_PROVIDER 显式控制（auto / system / vendored，见下方开关
# 段落）。默认 auto：系统优先，找不到才回落 vendored。
#
# 产出：
#   BITCASK_ICU_TARGETS   —— 供 target_link_libraries 使用的目标列表
#   BITCASK_ICU_VENDORED  —— ON/OFF，走的哪条路（install 规则要据此分支）
#   BITCASK_ICU_VENDORED_LIBS / _INCLUDE —— 仅 vendored 时设，供 install 用
#   BITCASK_ICU_VERSION   —— 实际使用的 ICU 版本
#
# 只需要 ICU 的 common 组件（uc + data）：
#   - icu::Normalizer2 / unorm2_*   → NFKC_Casefold
#   - u_charType / u_getCombiningClass / u_hasBinaryProperty → 字符属性
#   - ucnv_*                        → GB18030 等编码转换
# 三者全在 libicuuc + libicudata 里，**不需要 i18n**（collation/format 才要），
# 这让链接面比"整套 ICU"小一大截。

# Normalizer2::normalizeUTF8（我们的归一化热路径，直接吃 UTF-8 免 UTF-16
# 往返）起于 ICU 60；再往下就得自己做 u_strFromUTF8/u_strToUTF8 双转换。
set(BITCASK_ICU_MIN_VERSION 60 CACHE STRING "最低可接受的 ICU 版本")

# ---------------------------------------------------------------------------
# 来源开关：三态，显式可控。
#
#   auto      系统优先，找不到（或版本过低）自动回落 vendored。默认。
#   system    只用系统 ICU；找不到就**配置期报错**，绝不静默回落。
#             给发行版打包用——vendored 副本会绕开发行版的安全补丁流。
#   vendored  只用 third_party/icu，跳过系统探测。给以下场景用：
#             · 需要钉死 ICU 版本以保证索引可复现（ICU 版本 = Unicode 版本
#               = NFKC_Casefold 表 = 分词结果）
#             · 交叉编译 / 目标机无 ICU 开发包
#             · 系统 ICU 版本过老或被打过非上游补丁
#
# 兼容别名：BITCASK_FORCE_VENDORED_ICU=ON 等价于 PROVIDER=vendored。
# ---------------------------------------------------------------------------
set(BITCASK_ICU_PROVIDER "auto" CACHE STRING
    "ICU 来源: auto(系统优先,回落 vendored) | system(只用系统) | vendored(只用 third_party/icu)")
set_property(CACHE BITCASK_ICU_PROVIDER PROPERTY STRINGS auto system vendored)

option(BITCASK_FORCE_VENDORED_ICU
    "[兼容别名] 等价于 BITCASK_ICU_PROVIDER=vendored" OFF)
if(BITCASK_FORCE_VENDORED_ICU)
    set(BITCASK_ICU_PROVIDER "vendored")
endif()

if(NOT BITCASK_ICU_PROVIDER MATCHES "^(auto|system|vendored)$")
    message(FATAL_ERROR
        "BITCASK_ICU_PROVIDER 只接受 auto / system / vendored，"
        "收到的是 '${BITCASK_ICU_PROVIDER}'")
endif()

set(BITCASK_ICU_TARGETS "")
set(BITCASK_ICU_VENDORED OFF)
set(BITCASK_ICU_VERSION "")

# ---------------------------------------------------------------------------
# 1. 系统 ICU（auto / system）
# ---------------------------------------------------------------------------
if(BITCASK_ICU_PROVIDER STREQUAL "auto" OR BITCASK_ICU_PROVIDER STREQUAL "system")
    find_package(ICU ${BITCASK_ICU_MIN_VERSION} COMPONENTS uc data QUIET)

    if(ICU_FOUND)
        set(BITCASK_ICU_TARGETS ICU::uc ICU::data)
        set(BITCASK_ICU_VERSION "${ICU_VERSION}")
        message(STATUS "ICU: 系统安装 ${ICU_VERSION} (${ICU_INCLUDE_DIRS})")
        return()
    endif()

    if(BITCASK_ICU_PROVIDER STREQUAL "system")
        message(FATAL_ERROR
            "BITCASK_ICU_PROVIDER=system，但未找到 ICU >= ${BITCASK_ICU_MIN_VERSION}。\n"
            "装开发包：\n"
            "  Debian/Ubuntu:  sudo apt install libicu-dev\n"
            "  Fedora/RHEL:    sudo dnf install libicu-devel\n"
            "  FreeBSD:        pkg install icu\n"
            "  macOS:          brew install icu4c   (可能需要 -DICU_ROOT=$(brew --prefix icu4c))\n"
            "或改用 -DBITCASK_ICU_PROVIDER=auto / vendored。")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. Vendored ICU（third_party/icu 子模块）
# ---------------------------------------------------------------------------
# icu4c **不提供** CMake 构建，官方只有 autoconf（Unix）与 MSBuild 解决方案
# （Windows）。故这里只能 ExternalProject_Add 驱动它自己的构建系统，把产物
# 装进 ${CMAKE_BINARY_DIR}/icu-install，再包成 IMPORTED 目标给下游用。
#
# 代价（已知，非缺陷）：
#   - 顶层的 warning / sanitizer / IPO flag **不传播**进 ICU，它按自己的默认档
#     编。第三方代码本就不受本项目告警纪律约束（同 zlib/googletest 的处置）。
#   - ExternalProject 在 configure 期不产出 imported target 的文件，故下面用
#     显式 BUILD_BYPRODUCTS + add_dependencies 建立顺序。
set(BITCASK_ICU_VENDORED ON)
set(_icu_src "${PROJECT_SOURCE_DIR}/third_party/icu/icu4c/source")

if(NOT EXISTS "${_icu_src}/common/unicode/uversion.h")
    message(FATAL_ERROR
        "需要 vendored ICU（BITCASK_ICU_PROVIDER=${BITCASK_ICU_PROVIDER}），"
        "但 third_party/icu 子模块未初始化。\n"
        "二选一：\n"
        "  a) 装系统 ICU 开发包：\n"
        "       Debian/Ubuntu:  sudo apt install libicu-dev\n"
        "       Fedora/RHEL:    sudo dnf install libicu-devel\n"
        "       FreeBSD:        pkg install icu\n"
        "       macOS:          brew install icu4c\n"
        "  b) 拉 vendored 子模块：\n"
        "       git submodule update --init third_party/icu")
endif()

include(ExternalProject)
include(ProcessorCount)

# ICU 的构建系统是 GNU make（Unix）。不能用 CMAKE_MAKE_PROGRAM——Ninja 生成器
# 下它是 ninja，喂不了 ICU 的 Makefile；也不能写 $(MAKE)，那个只在 Makefile
# 规则里展开，作为 ExternalProject 的参数会被原样当成字面量传下去。
if(NOT WIN32)
    find_program(BITCASK_ICU_MAKE NAMES gmake make)
    if(NOT BITCASK_ICU_MAKE)
        message(FATAL_ERROR
            "vendored ICU 需要 GNU make（ICU 官方只提供 autoconf 构建），"
            "但 PATH 里找不到 gmake/make。")
    endif()
endif()

# ICU 的数据裁剪：完整数据表 31 MB，静态打包后整块进 libbitcask.so（实测
# .so 1.7 MB → 34 MB）。我们只用 normalization + 编码转换，其余（collation /
# 时区 / 区域 / 音译 / 断词 / 货币 / 字符名）全不需要。裁剪由 ICU 自己的
# buildtool 做，走 ICU_DATA_FILTER_FILE 环境变量，需要 Python 3。
#
# 默认开。关掉它（=OFF）只在两种情况下有意义：机器上没有 Python 3，或怀疑
# 裁剪清单裁错了东西要用全量数据对照排查。
option(BITCASK_ICU_TRIM_DATA
    "裁剪 vendored ICU 的数据表（只留归一化与编码转换，需 Python 3）" ON)

set(_icu_filter_env "")
if(BITCASK_ICU_TRIM_DATA)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        set(_icu_filter_env
            "ICU_DATA_FILTER_FILE=${PROJECT_SOURCE_DIR}/cmake/icu-data-filter.json")
        message(STATUS "ICU: 启用数据裁剪（cmake/icu-data-filter.json）")
    else()
        message(WARNING
            "BITCASK_ICU_TRIM_DATA=ON 但找不到 Python 3——ICU 的数据裁剪工具链需要它。"
            "改用全量数据：libbitcask.so 会因此大约 31 MB。"
            "装 Python 3，或显式 -DBITCASK_ICU_TRIM_DATA=OFF 以消除此告警。")
    endif()
endif()

ProcessorCount(_icu_jobs)
if(_icu_jobs EQUAL 0)
    set(_icu_jobs 1)
endif()

set(_icu_prefix "${CMAKE_BINARY_DIR}/icu-install")
set(_icu_libdir "${_icu_prefix}/lib")

# ICU 的静态库名在 Unix 与 Windows 上不同；且静态构建时 ICU 要求下游定义
# U_STATIC_IMPLEMENTATION，否则符号按 dllimport 解析（Windows）。
if(WIN32)
    set(_icu_uc_lib   "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}sicuuc${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_icu_data_lib "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}sicudt${CMAKE_STATIC_LIBRARY_SUFFIX}")
else()
    set(_icu_uc_lib   "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}icuuc${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_icu_data_lib "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}icudata${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()

if(WIN32)
    # MSVC：走 allinone 解决方案。ICU 的 .sln 只认 Win32/x64 平台名。
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_icu_msvc_platform "x64")
    else()
        set(_icu_msvc_platform "Win32")
    endif()
    ExternalProject_Add(icu_vendored
        SOURCE_DIR        "${PROJECT_SOURCE_DIR}/third_party/icu"
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE   1
        BUILD_COMMAND     msbuild "${_icu_src}/allinone/allinone.sln"
                          /p:Configuration=Release
                          /p:Platform=${_icu_msvc_platform}
                          /p:SkipUWP=true
        INSTALL_COMMAND   ${CMAKE_COMMAND} -E copy_directory
                          "${PROJECT_SOURCE_DIR}/third_party/icu/icu4c" "${_icu_prefix}"
        BUILD_BYPRODUCTS  "${_icu_uc_lib}" "${_icu_data_lib}"
        LOG_BUILD         ON
        LOG_INSTALL       ON
    )
else()
    # Unix：autoconf。--enable-static/--disable-shared 让产物能被我们的静态
    # 归档下游一并链上；--disable-extras/tests/samples 砍掉无关目标。
    # --with-data-packaging=static 把 icudt 打成静态库（否则要在运行期找
    # .dat 文件，对一个被嵌进别人进程的库是部署负担）。
    ExternalProject_Add(icu_vendored
        SOURCE_DIR        "${PROJECT_SOURCE_DIR}/third_party/icu"
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${_icu_filter_env}
                          "${_icu_src}/configure"
                          --prefix=${_icu_prefix}
                          --enable-static --disable-shared
                          --disable-extras --disable-tests --disable-samples
                          --with-data-packaging=static
                          "CFLAGS=-fPIC -O2"
                          "CXXFLAGS=-fPIC -O2 -std=c++17"
        BUILD_COMMAND     ${BITCASK_ICU_MAKE} -j${_icu_jobs}
        INSTALL_COMMAND   ${BITCASK_ICU_MAKE} install
        BUILD_BYPRODUCTS  "${_icu_uc_lib}" "${_icu_data_lib}"
        LOG_CONFIGURE     ON
        LOG_BUILD         ON
        LOG_INSTALL       ON
    )
endif()

# ICU 的头装在 <prefix>/include，但 ExternalProject 在 configure 期还没建它，
# 而 INTERFACE_INCLUDE_DIRECTORIES 要求路径存在 → 先建空目录占位。
file(MAKE_DIRECTORY "${_icu_prefix}/include")

add_library(bitcask_icu_data STATIC IMPORTED GLOBAL)
set_target_properties(bitcask_icu_data PROPERTIES
    IMPORTED_LOCATION "${_icu_data_lib}")
add_dependencies(bitcask_icu_data icu_vendored)

add_library(bitcask_icu_uc STATIC IMPORTED GLOBAL)
set_target_properties(bitcask_icu_uc PROPERTIES
    IMPORTED_LOCATION "${_icu_uc_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_icu_prefix}/include"
    # 静态 ICU 必须给下游这个宏，否则 Windows 上按 dllimport 解析符号；
    # Unix 上无害。ICU 自身 C++ 是 C++17 编的，ABI 与我们的 C++23 兼容
    # （只用 C 风格与 POD 接口 + Normalizer2 这种纯虚基类）。
    INTERFACE_COMPILE_DEFINITIONS "U_STATIC_IMPLEMENTATION"
    INTERFACE_LINK_LIBRARIES "bitcask_icu_data")
add_dependencies(bitcask_icu_uc icu_vendored)

if(NOT WIN32)
    # ICU 的 common 用 dlopen/pthread；Unix 上要显式补。
    find_package(Threads REQUIRED)
    set_property(TARGET bitcask_icu_uc APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})
endif()

# IMPORTED 目标上的 add_dependencies **不会**给消费者建立构建顺序。若直接把
# bitcask_icu_uc 交给 target_link_libraries，我们的 TU 可能先于 ICU 编完——
# 而 vendored 的 include 目录此刻还是空的，编译器于是顺着默认搜索路径抓到
# /usr/include/unicode/*.h（系统 ICU 的头）。两边版本一旦不同就是静默的
# ODR/ABI 错配，且症状出现在运行期。包一层**非 IMPORTED** 的 INTERFACE 目标，
# 让顺序真正传导给每个链接它的目标（CMake >= 3.19 支持，本项目要求 3.21）。
add_library(bitcask_icu INTERFACE)
add_dependencies(bitcask_icu icu_vendored)
target_link_libraries(bitcask_icu INTERFACE bitcask_icu_uc bitcask_icu_data)

set(BITCASK_ICU_TARGETS bitcask_icu)
set(BITCASK_ICU_VERSION "vendored")
# 顶层的 install 规则要把这两个静态库和头一起装出去（见 CMakeLists.txt 的
# BITCASK_ICU_VENDORED 分支）——合并归档不含第三方对象，不装下游就缺符号。
set(BITCASK_ICU_VENDORED_LIBS "${_icu_uc_lib}" "${_icu_data_lib}")
set(BITCASK_ICU_VENDORED_INCLUDE "${_icu_prefix}/include")
if(BITCASK_ICU_PROVIDER STREQUAL "vendored")
    message(STATUS "ICU: 按 BITCASK_ICU_PROVIDER=vendored 强制使用 third_party/icu"
                   "（跳过系统探测，ExternalProject 现编）")
else()
    message(STATUS "ICU: 未找到系统安装（>= ${BITCASK_ICU_MIN_VERSION}），"
                   "回落 vendored third_party/icu（ExternalProject 现编）")
endif()
