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
# （Windows/MSVC）。所以"用包管理器拉一份 CMake 化的 ICU"这条路走不通：
#   · CPM.cmake / FetchContent 本质是"下载后 add_subdirectory"，而上游 ICU 树里
#     一个 CMakeLists.txt 都没有，拉下来也没法 add；
#   · 社区仅有的两个 CMake 移植都不覆盖本项目要的「ICU 76 + MSVC」组合——
#     LibCMaker/ICU_CMake_Files 停在 ICU 63 且仓库已归档，viaduck/icu-cmake 的
#     Windows 预编译产物是 MinGW-w64 编的、与 MSVC 的 C++ ABI 不兼容，其源码
#     构建走的也还是 autoconf（Windows 上要 MSYS）。
# 故这里只能 ExternalProject_Add 驱动 ICU 自己的构建系统，把产物装进
# ${CMAKE_BINARY_DIR}/icu-install，再包成 IMPORTED 目标给下游用。
#
# 代价（已知，非缺陷）：
#   - 顶层的 warning / sanitizer / IPO flag **不传播**进 ICU，它按自己的默认档
#     编。第三方代码本就不受本项目告警纪律约束（同 zlib/googletest 的处置）。
#   - ExternalProject 在 configure 期不产出 imported target 的文件，故下面用
#     显式 BUILD_BYPRODUCTS + add_dependencies 建立顺序。
#   - MSVC 那条只能 BUILD_IN_SOURCE：ICU 的 vcxproj 把产物路径写死成相对源码树
#     的 ..\..\bin64 等，构建会弄脏 third_party/icu 工作区（属预期，勿提交）。
set(BITCASK_ICU_VENDORED ON)
set(_icu_tree "${PROJECT_SOURCE_DIR}/third_party/icu/icu4c")
set(_icu_src "${_icu_tree}/source")

if(NOT EXISTS "${_icu_src}/common/unicode/uversion.h")
    # 注意拉取命令里的 -c submodule.<path>.update=checkout：.gitmodules 给
    # third_party/icu 标了 update = none（它 380 MB，默认不该被 --recursive
    # 拖下来），而 update = none 会让**普通的 submodule update --init 静默
    # 跳过**它（打印 "Skipping submodule"，退出码 0）。不带这个 -c 的话，
    # 用户会以为拉过了，然后又撞回这条报错。
    message(FATAL_ERROR
        "需要 vendored ICU（BITCASK_ICU_PROVIDER=${BITCASK_ICU_PROVIDER}），"
        "但 third_party/icu 子模块未初始化。\n"
        "二选一：\n"
        "  a) 装系统 ICU 开发包：\n"
        "       Debian/Ubuntu:  sudo apt install libicu-dev\n"
        "       Fedora/RHEL:    sudo dnf install libicu-devel\n"
        "       FreeBSD:        pkg install icu\n"
        "       macOS:          brew install icu4c\n"
        "  b) 拉 vendored 子模块（update = none 必须显式覆盖，否则会被跳过）：\n"
        "       git -c submodule.third_party/icu.update=checkout \\\n"
        "           submodule update --init --depth 1 third_party/icu")
endif()

include(ExternalProject)
include(ProcessorCount)

# ICU 主版本号决定 Windows 侧的产物名（icuuc76.dll / icudt76.dll）。从头文件读，
# 不写死——换 maint 分支时写死的那份就是下一个静默错配。
file(STRINGS "${_icu_src}/common/unicode/uvernum.h" _icu_major_line
     REGEX "^#define[ \t]+U_ICU_VERSION_MAJOR_NUM[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+" BITCASK_ICU_MAJOR "${_icu_major_line}")
if(NOT BITCASK_ICU_MAJOR)
    message(FATAL_ERROR
        "无法从 ${_icu_src}/common/unicode/uvernum.h 解析 U_ICU_VERSION_MAJOR_NUM")
endif()
if(BITCASK_ICU_MAJOR LESS BITCASK_ICU_MIN_VERSION)
    message(FATAL_ERROR
        "third_party/icu 是 ICU ${BITCASK_ICU_MAJOR}，低于本项目要求的 "
        "${BITCASK_ICU_MIN_VERSION}（Normalizer2::normalizeUTF8 的引入版本）。")
endif()

set(_icu_prefix "${CMAKE_BINARY_DIR}/icu-install")
# ICU 的头装在 <prefix>/include，但 ExternalProject 在 configure 期还没建它，
# 而 INTERFACE_INCLUDE_DIRECTORIES 要求路径存在 → 先建空目录占位。
file(MAKE_DIRECTORY "${_icu_prefix}/include")

ProcessorCount(_icu_jobs)
if(_icu_jobs EQUAL 0)
    set(_icu_jobs 1)
endif()

# ICU 的数据裁剪：完整数据表约 31 MB，我们实际只用 normalization + 编码转换，
# 裁完约 5.5 MB。裁剪由 ICU 自己的 buildtool 做，走 ICU_DATA_FILTER_FILE
# 环境变量，需要 Python 3。
#
# **默认关**，即用全量数据。裁剪省下的那 25 MB 换来的是一类很难查的故障：
# 裁多了不会让构建失败，只会让归一化或某个编码在**运行期**静默失效（返回空串 /
# kUnknownEncoding）。而 cmake/icu-data-filter.json 里写的是 ICU 的**内部
# category 名**，跨大版本会改——ICU 78 就已经对 brkitr_treedict / coll_tree
# 报“category 不存在”了，且只是 warning、不中断。也就是说这份清单会随 ICU 升级
# 悄悄失效，而失效的表现在运行期。用全量数据把这条路整个绕开。
#
# 需要那 25 MB 时再显式打开（-DBITCASK_ICU_TRIM_DATA=ON），并重跑
# bitcask_text_encoding_test 与 bitcask_analyzer_test——它们是这份清单的守门人。
option(BITCASK_ICU_TRIM_DATA
    "裁剪 vendored ICU 的数据表（只留归一化与编码转换，需 Python 3）" OFF)

# Windows 侧 **无论裁不裁都要 Python**：ICU 的 makedata.mak 把整个数据构建交给
# 了 `py -3 -B -m icutools.databuilder`（见 source/data/makedata.mak）。Unix 侧
# 只有开裁剪时才需要（configure 阶段跑 buildtool 生成 Makefile 片段）。
if(BITCASK_ICU_TRIM_DATA OR MSVC)
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()

set(_icu_filter_file "")
if(BITCASK_ICU_TRIM_DATA)
    if(Python3_FOUND)
        set(_icu_filter_file "${PROJECT_SOURCE_DIR}/cmake/icu-data-filter.json")
        message(STATUS "ICU: 启用数据裁剪（cmake/icu-data-filter.json）")
    else()
        message(WARNING
            "BITCASK_ICU_TRIM_DATA=ON 但找不到 Python 3——ICU 的数据裁剪工具链需要它。"
            "改用全量数据：libbitcask.so 会因此大约 31 MB。"
            "装 Python 3，或显式 -DBITCASK_ICU_TRIM_DATA=OFF 以消除此告警。")
    endif()
endif()

set(BITCASK_ICU_VENDORED_DLLS "")

if(MSVC)
    # -----------------------------------------------------------------------
    # 2a. Windows / MSVC —— MSBuild 驱动 source/allinone/allinone.sln
    #
    # 与 Unix 那条最大的不同：**Windows 上没有静态 ICU 可选**。ICU 的 vcxproj
    # 一律 ConfigurationType=DynamicLibrary，官方也没有"静态"配置（要静态得手改
    # 工程文件，而 makedata 依赖的一串 tool 仍得链 DLL 版）。所以这条路产出的是
    # icuuc<major>.dll + icudt<major>.dll 与两个导入库，**不能**给下游定义
    # U_STATIC_IMPLEMENTATION——定义了符号就不按 dllimport 解析，链接期全是
    # unresolved external。
    # -----------------------------------------------------------------------

    # ---- 目标架构 → ICU 解决方案的 Platform 名与产物目录 ----
    # ICU 按平台分产物目录（allinone/Build.Windows.ProjectConfiguration.props）：
    # Win32→bin/lib，x64→bin64/lib64，ARM→binARM/libARM，ARM64→binARM64/libARM64。
    # 原实现假定一律是 lib/，于是在 x64（也就是绝大多数情况）上找不到任何产物。
    if(CMAKE_VS_PLATFORM_NAME)
        set(_icu_arch "${CMAKE_VS_PLATFORM_NAME}")               # VS 生成器
    elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
        set(_icu_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")   # Ninja/NMake + cl
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_icu_arch "x64")
    else()
        set(_icu_arch "Win32")
    endif()
    string(TOLOWER "${_icu_arch}" _icu_arch_lc)
    if(_icu_arch_lc STREQUAL "x64" OR _icu_arch_lc STREQUAL "amd64")
        set(_icu_msvc_platform "x64")
        set(_icu_bin_sub "bin64")
        set(_icu_lib_sub "lib64")
    elseif(_icu_arch_lc STREQUAL "win32" OR _icu_arch_lc STREQUAL "x86")
        set(_icu_msvc_platform "Win32")
        set(_icu_bin_sub "bin")
        set(_icu_lib_sub "lib")
    elseif(_icu_arch_lc MATCHES "^arm64")
        # ARM64EC 也落这里：ICU 没有 EC 配置，用 ARM64 产物。
        set(_icu_msvc_platform "ARM64")
        set(_icu_bin_sub "binARM64")
        set(_icu_lib_sub "libARM64")
    elseif(_icu_arch_lc STREQUAL "arm")
        set(_icu_msvc_platform "ARM")
        set(_icu_bin_sub "binARM")
        set(_icu_lib_sub "libARM")
    else()
        message(FATAL_ERROR
            "vendored ICU：目标架构 '${_icu_arch}' 映射不到 allinone.sln 的 "
            "Platform（只有 Win32 / x64 / ARM / ARM64）。")
    endif()

    # ---- MSBuild.exe ----
    # 原实现直接写字面量 msbuild，只在 "Developer Command Prompt" 里成立；从
    # IDE、CI 的普通 shell 或 CLion 起的 CMake 里，PATH 上根本没有它。
    set(BITCASK_ICU_MSBUILD "" CACHE FILEPATH
        "MSBuild.exe 路径（vendored ICU 用；留空则自动探测）")
    if(NOT BITCASK_ICU_MSBUILD)
        set(_icu_msbuild "")
        if(CMAKE_VS_MSBUILD_COMMAND)
            set(_icu_msbuild "${CMAKE_VS_MSBUILD_COMMAND}")
        endif()
        if(NOT _icu_msbuild)
            find_program(_icu_msbuild_found NAMES MSBuild.exe msbuild)
            if(_icu_msbuild_found)
                set(_icu_msbuild "${_icu_msbuild_found}")
            endif()
        endif()
        if(NOT _icu_msbuild)
            # 兜底：vswhere 是 VS 安装器自带的定位器，路径固定，任何 VS 2017+
            # 都有。-find 直接问它要 MSBuild.exe 的绝对路径。
            find_program(_icu_vswhere NAMES vswhere.exe
                PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer"
                      "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer")
            if(_icu_vswhere)
                execute_process(
                    COMMAND "${_icu_vswhere}" -latest -prerelease -products *
                            -requires Microsoft.Component.MSBuild
                            -find "MSBuild/**/Bin/MSBuild.exe"
                    OUTPUT_VARIABLE _icu_vswhere_out
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                string(REPLACE "\r" "" _icu_vswhere_out "${_icu_vswhere_out}")
                string(REPLACE "\n" ";" _icu_vswhere_out "${_icu_vswhere_out}")
                foreach(_icu_cand IN LISTS _icu_vswhere_out)
                    if(_icu_cand AND EXISTS "${_icu_cand}")
                        set(_icu_msbuild "${_icu_cand}")
                        break()
                    endif()
                endforeach()
            endif()
        endif()
        if(NOT _icu_msbuild)
            message(FATAL_ERROR
                "vendored ICU 需要 MSBuild（ICU 在 Windows 上只提供 MSBuild 解决方案），"
                "但它既不在 PATH 上、vswhere 也没找到。\n"
                "从 Developer Command Prompt for VS 里重新配置，"
                "或显式指定：-DBITCASK_ICU_MSBUILD=<...>/MSBuild.exe")
        endif()
        set(BITCASK_ICU_MSBUILD "${_icu_msbuild}" CACHE FILEPATH
            "MSBuild.exe 路径（vendored ICU 用；留空则自动探测）" FORCE)
    endif()

    # ---- PlatformToolset ----
    # ICU 76 的 allinone/Build.Windows.PlatformToolset.props 只把
    # VisualStudioVersion 14.0/15.0/16.0/17.0 映射到 v140/v141/v142/v143。
    # VS 2026（VisualStudioVersion 18.x）落不进任何一条 →
    # AutoDetectedPlatformToolset 为空 → PlatformToolset 为空，MSBuild 报
    # "The build tools for  (Platform Toolset = '') cannot be found"，字面上
    # 完全看不出是 ICU 的 props 太老。这里显式喂一个：命令行 /p: 是全局属性、
    # 工程内不可覆盖，正好压住那段自动探测。
    set(BITCASK_ICU_MSVC_TOOLSET "" CACHE STRING
        "vendored ICU 用的 PlatformToolset（留空=跟随本次构建的工具集）")
    if(BITCASK_ICU_MSVC_TOOLSET)
        set(_icu_toolset "${BITCASK_ICU_MSVC_TOOLSET}")
    elseif(CMAKE_VS_PLATFORM_TOOLSET)
        set(_icu_toolset "${CMAKE_VS_PLATFORM_TOOLSET}")
    elseif(MSVC_TOOLSET_VERSION)
        set(_icu_toolset "v${MSVC_TOOLSET_VERSION}")
    else()
        set(_icu_toolset "v143")
    endif()

    # ---- Configuration ----
    # ICU 的 Debug 配置用 /MDd，Release 用 /MD。跟本次构建的 CRT 对齐，别让一个
    # 进程里同时出现两份 CRT——ICU 的 ByteSink 会往我们的 std::string 里写，
    # 跨 CRT 堆的分配/释放在 Windows 上是 __fastfail 级别的故障。
    set(BITCASK_ICU_MSVC_CONFIG "" CACHE STRING
        "vendored ICU 的 MSBuild 配置（Debug/Release；留空=跟随 CMAKE_BUILD_TYPE）")
    if(BITCASK_ICU_MSVC_CONFIG)
        set(_icu_cfg "${BITCASK_ICU_MSVC_CONFIG}")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_icu_cfg "Debug")
    else()
        set(_icu_cfg "Release")
    endif()
    if(NOT _icu_cfg MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR
            "BITCASK_ICU_MSVC_CONFIG 只接受 Debug / Release，收到 '${_icu_cfg}'"
            "（allinone.sln 只有这两个配置）。")
    endif()
    if(_icu_cfg STREQUAL "Debug")
        # common 的 Debug 产物带 d 后缀（icuuc76d.dll / icuucd.lib）；stubdata
        # **不带**——见 stubdata.vcxproj 里那句 "stubdata is somewhat odd in
        # that it doesn't suffix the Debug output DLL/LIB with a d"。
        set(_icu_uc_sfx "d")
    else()
        set(_icu_uc_sfx "")
    endif()
    if(NOT CMAKE_BUILD_TYPE)
        message(STATUS
            "ICU: 多配置生成器下 vendored ICU 固定按 ${_icu_cfg} 编一份"
            "（要 Debug CRT 版请设 -DBITCASK_ICU_MSVC_CONFIG=Debug）")
    endif()

    # 源码树内的构建脏状态清理，见下面 build-icu.bat 的生成逻辑。放在构建期而
    # 不是 configure 期，是因为多个 build 目录会**共用同一棵 ICU 源码树**，
    # 交替 `cmake --build` 时根本不经过 configure。
    set(_icu_data_out "${_icu_src}/data/out")
    set(_icu_stubdata_ts "${_icu_src}/stubdata/stubdatabuilt.txt")

    # ---- 产物路径：ICU 源码树内（BUILD_IN_SOURCE）→ icu-install ----
    set(_icu_built_uc_implib "${_icu_tree}/${_icu_lib_sub}/icuuc${_icu_uc_sfx}.lib")
    set(_icu_built_uc_dll    "${_icu_tree}/${_icu_bin_sub}/icuuc${BITCASK_ICU_MAJOR}${_icu_uc_sfx}.dll")
    set(_icu_built_dt_implib "${_icu_tree}/${_icu_lib_sub}/icudt.lib")
    set(_icu_built_dt_dll    "${_icu_tree}/${_icu_bin_sub}/icudt${BITCASK_ICU_MAJOR}.dll")

    set(_icu_uc_implib "${_icu_prefix}/lib/icuuc${_icu_uc_sfx}.lib")
    set(_icu_uc_dll    "${_icu_prefix}/bin/icuuc${BITCASK_ICU_MAJOR}${_icu_uc_sfx}.dll")
    set(_icu_dt_implib "${_icu_prefix}/lib/icudt.lib")
    set(_icu_dt_dll    "${_icu_prefix}/bin/icudt${BITCASK_ICU_MAJOR}.dll")

    # Windows 没有 RPATH，DLL 只按「exe 所在目录 → PATH」搜索。顶层已把所有 exe
    # 收进 <build>/bin/（见 CMakeLists.txt 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY），
    # ICU 的两个 DLL 也得跟过去——**构建期就要**：gen_inert_table 是链 ICU 的
    # 代码生成器，构建中途会被执行，DLL 不在位就是 0xC0000135 起不来，而
    # add_custom_command 只会报一个没有上下文的非零退出码。
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(_icu_runtime_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    else()
        set(_icu_runtime_dir "${PROJECT_BINARY_DIR}/bin")
    endif()
    file(MAKE_DIRECTORY "${_icu_runtime_dir}")

    # ---- py launcher 垫片 ----
    # makedata.mak 里写死的是 `py -3 -B -m icutools.databuilder`——python.org
    # 安装包附带的 py launcher。用 scoop / conda / Microsoft Store / uv 装的
    # Python 通常没有 py.exe，此时 nmake 以「'py' 不是内部或外部命令」失败，
    # 而这行错误深埋在 makedata 的 NMake 输出里，跟 Python 毫无字面关联。
    # 找不到 py 就现生成一个转发到 ${Python3_EXECUTABLE} 的 py.bat（吃掉 -3
    # 这个版本选择参数），并把它所在目录前插进 PATH。
    #
    # 光看 py.exe 在不在**不够**：scoop 的 python 包会装一个 py.exe，但它只认
    # 注册表里登记过的解释器，而 scoop 是绿色安装、不写注册表 —— 于是这个
    # py.exe 存在、可执行，一跑却是 "No installed Python found!"。所以这里实探
    # 一次 `py -3 -c ...`，探不通就照样上垫片。
    set(_icu_work_dir "${CMAKE_BINARY_DIR}/icu-build")
    set(_icu_shim_dir "${_icu_work_dir}/shim")
    find_program(_icu_py_launcher NAMES py.exe)
    set(_icu_py_usable FALSE)
    if(_icu_py_launcher)
        execute_process(
            COMMAND "${_icu_py_launcher}" -3 -c "import sys; sys.stdout.write('icu-py-ok')"
            RESULT_VARIABLE _icu_py_rc
            OUTPUT_VARIABLE _icu_py_probe
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_icu_py_rc EQUAL 0 AND _icu_py_probe STREQUAL "icu-py-ok")
            set(_icu_py_usable TRUE)
        endif()
    endif()
    set(_icu_path_prefix "")
    if(_icu_py_usable)
        message(STATUS "ICU: 数据构建走系统 py launcher（${_icu_py_launcher}）")
    elseif(Python3_FOUND)
        file(MAKE_DIRECTORY "${_icu_shim_dir}")
        file(TO_NATIVE_PATH "${Python3_EXECUTABLE}" _icu_py_native)
        # 生成的 .bat 必须是**纯 ASCII**：cmd.exe 按 OEM 代码页读批处理（中文
        # Windows 上是 CP936），文件里的 UTF-8 中文会被当 GBK 拆错字节边界，
        # 运气不好就拆出一个裸 & 把 rem 行劈成两条命令——表现是
        # 「'…' 不是内部或外部命令」，跟本意毫无关系。故这两个生成脚本里
        # 一个中文都不能有，注释一律英文；解释留在本文件里。
        file(WRITE "${_icu_shim_dir}/py.bat"
"@echo off\n"
"rem Generated by cmake/BitcaskICU.cmake. ICU's makedata.mak hardcodes the\n"
"rem Windows py launcher; this machine has none (or a broken one), so forward\n"
"rem to the Python 3 that CMake found.\n"
"rem Only job: drop the leading \"-3\" version selector, forward the rest.\n"
"setlocal EnableExtensions\n"
"set \"ICU_PY_ARGS=%*\"\n"
"if \"%ICU_PY_ARGS:~0,3%\"==\"-3 \" set \"ICU_PY_ARGS=%ICU_PY_ARGS:~3%\"\n"
"\"${_icu_py_native}\" %ICU_PY_ARGS%\n"
"exit /b %ERRORLEVEL%\n")
        set(_icu_path_prefix "${_icu_shim_dir}")
        if(_icu_py_launcher)
            set(_icu_py_why "${_icu_py_launcher} 跑不通（多半是没在注册表登记解释器）")
        else()
            set(_icu_py_why "PATH 上没有 py launcher")
        endif()
        message(STATUS
            "ICU: ${_icu_py_why}，已生成垫片 ${_icu_shim_dir}/py.bat"
            " → ${Python3_EXECUTABLE}")
    else()
        message(FATAL_ERROR
            "vendored ICU 在 Windows 上必须有 Python 3——ICU 的 makedata.mak 把整个"
            "数据构建交给了 `py -3 -m icutools.databuilder`，这跟 "
            "BITCASK_ICU_TRIM_DATA 开不开无关。\n"
            "装 Python 3（并确保 CMake 的 find_package(Python3) 能找到），"
            "或改用系统 ICU：-DBITCASK_ICU_PROVIDER=system。")
    endif()

    # ---- 构建脚本 ----
    # 不把 msbuild 直接塞进 BUILD_COMMAND，而是生成一个 .bat：
    #   · PATH / ICU_DATA_FILTER_FILE 这类含分号与反斜杠的环境变量，经
    #     `cmake -E env` 传会被 CMake 的 list 语义切碎，写进 .bat 则原样保真；
    #   · 出问题时可以单独跑这个脚本复现，不必去猜 ExternalProject 拼出来的
    #     命令行长什么样。
    file(TO_NATIVE_PATH "${BITCASK_ICU_MSBUILD}" _icu_msbuild_native)
    file(TO_NATIVE_PATH "${_icu_src}/allinone/allinone.sln" _icu_sln_native)
    set(_icu_bat "${_icu_work_dir}/build-icu.bat")
    # 同样只能是 ASCII —— 理由见上面 py.bat 处的注释。
    set(_icu_bat_body
        "@echo off\nrem Generated by cmake/BitcaskICU.cmake -- do not edit.\nsetlocal\n")

    # ---- 换版本 / 换配置时清掉源码树里的构建脏状态 ----
    # ICU 的数据构建全程在源码树内，而且**既不按版本、也不按配置分目录**：
    # icudata.lst 与各步时间戳固定落在 source/data/out/ 下，最终产物固定是
    # bin64\icudt<major>.dll。由此有两个静默故障，实测都复现过：
    #
    #  (1) 升级 submodule（76 -> 78）后重建：nmake 看到上一版留下的时间戳，
    #      判定数据已最新而**整步跳过** -> out/build/icudt78l/ 是空的，然后
    #      pkgdata 拿到上一版的 icudata.lst，报
    #      `icupkg: unable to open input file ".\brkitr\de.res"`。
    #
    #  (2) Debug / Release 在同一棵树里交替构建：stubdata.vcxproj 产出的**空桩**
    #      与 makedata 产出的真数据是同一个文件名 bin64\icudt<major>.dll（且
    #      stubdata 的 Debug 输出不带 d 后缀，ICU 源码里专门有注释说这点"有点
    #      怪"）。第二个配置构建时 stubdata 先把桩覆盖上去，makedata 再按时间戳
    #      判定"已最新"而不重新打包 -> 桩留在原地。**构建照样成功**，但 ICU 运行
    #      期没有数据：Normalizer2::getNFKCCasefoldInstance() 返回 nullptr，
    #      nfkc_fold() 一律返回空串，全部文本用例红。
    #
    # 所以按「major + 配置 + 平台」打一个身份标记；不一致就把数据构建状态、
    # stubdata 时间戳和那个 DLL 一并清掉，让 makedata 从头再来。代价是换配置时
    # 多花一次数据构建，换来的是不会有上面两种"编得过、跑不对"。
    file(TO_NATIVE_PATH "${_icu_data_out}" _icu_data_out_native)
    file(TO_NATIVE_PATH "${_icu_stubdata_ts}" _icu_stubdata_ts_native)
    file(TO_NATIVE_PATH "${_icu_built_dt_dll}" _icu_built_dt_dll_native)
    # 身份里**必须**带上裁剪开关：它直接决定数据内容，而数据构建状态是整棵
    # 源码树共享的。开关一变却不清状态，就是跟上面 (1)(2) 同类的静默陈旧——
    # 从 ON 切到 OFF 会留着裁过的数据当全量用。
    if(BITCASK_ICU_TRIM_DATA AND _icu_filter_file)
        set(_icu_trim_id "trim")
    else()
        set(_icu_trim_id "full")
    endif()
    set(_icu_build_id
        "${BITCASK_ICU_MAJOR}-${_icu_cfg}-${_icu_msvc_platform}-${_icu_trim_id}")
    string(APPEND _icu_bat_body
        "set \"ICU_MARK=${_icu_data_out_native}\\.bitcask-icu-build-id\"\n"
        "set \"ICU_ID=${_icu_build_id}\"\n"
        "set \"ICU_PREV=\"\n"
        "if exist \"%ICU_MARK%\" set /p ICU_PREV=<\"%ICU_MARK%\"\n"
        "if not \"%ICU_PREV%\"==\"%ICU_ID%\" (\n"
        "  echo [bitcask] ICU in-source build state is \"%ICU_PREV%\", need \"%ICU_ID%\" -- clearing\n"
        "  if exist \"${_icu_data_out_native}\" rmdir /s /q \"${_icu_data_out_native}\"\n"
        "  if exist \"${_icu_stubdata_ts_native}\" del /q \"${_icu_stubdata_ts_native}\"\n"
        "  if exist \"${_icu_built_dt_dll_native}\" del /q \"${_icu_built_dt_dll_native}\"\n"
        ")\n")
    if(_icu_path_prefix)
        file(TO_NATIVE_PATH "${_icu_path_prefix}" _icu_path_prefix_native)
        string(APPEND _icu_bat_body "set \"PATH=${_icu_path_prefix_native};%PATH%\"\n")
    endif()
    # ICU 的 databuilder 用裸 open(filter_file, "r") 读裁剪清单——不带 encoding，
    # 于是 Python 用 locale 默认编码。中文 Windows 上那是 GBK，而
    # cmake/icu-data-filter.json 是带中文注释的 UTF-8，直接
    # UnicodeDecodeError: 'gbk' codec can't decode byte 0xae。
    # PYTHONUTF8=1（Python ≥ 3.7 的 UTF-8 模式）让 open() 默认按 UTF-8 解，
    # 比把那份清单改成纯 ASCII 更治本：ICU 自己的数据源里也有非 ASCII 路径。
    string(APPEND _icu_bat_body "set \"PYTHONUTF8=1\"\n")
    if(_icu_filter_file)
        file(TO_NATIVE_PATH "${_icu_filter_file}" _icu_filter_native)
        string(APPEND _icu_bat_body "set \"ICU_DATA_FILTER_FILE=${_icu_filter_native}\"\n")
    endif()
    # /m 并行；/v:minimal 收敛输出（ICU 全量 build log 上万行）。
    # SkipUWP=true 砍掉 UWP 变体——我们不要，且它要额外的 SDK 组件才编得动。
    string(APPEND _icu_bat_body
        "\"${_icu_msbuild_native}\" \"${_icu_sln_native}\" /nologo /m /v:minimal"
        " /p:Configuration=${_icu_cfg}"
        " /p:Platform=${_icu_msvc_platform}"
        " /p:SkipUWP=true"
        " /p:PlatformToolset=${_icu_toolset}"
        " /p:DefaultPlatformToolset=${_icu_toolset}")
    if(NOT _icu_toolset MATCHES "^v14[0123]$")
        # props 只给 v141/v142/v143 兜底设 WindowsTargetPlatformVersion；更新的
        # 工具集上它是空的，得自己给。"10.0" = 用装着的最新 Windows 10/11 SDK。
        string(APPEND _icu_bat_body " /p:WindowsTargetPlatformVersion=10.0")
    endif()
    string(APPEND _icu_bat_body "\n"
        "if errorlevel 1 exit /b 1\n"
        # 构建后自检：确认 bin64\\icudt<major>.dll 是真数据而不是 stubdata 的空桩
        # （桩 3 KB；裁剪后的真数据 5 MB 量级，不裁是 30 MB 量级）。上面的身份
        # 标记应当已经挡住已知路径，但这个故障的形态是"编得过、跑不对"——36 个
        # 文本用例齐红、报 nfkc_fold 返回空串，从症状反查到 ICU 数据要绕很远。
        # 宁可在这里当场炸。
        "set \"ICU_DT_SIZE=\"\n"
        "for %%A in (\"${_icu_built_dt_dll_native}\") do set \"ICU_DT_SIZE=%%~zA\"\n"
        "if not defined ICU_DT_SIZE (\n"
        "  echo [bitcask] ERROR: ICU data DLL not found: ${_icu_built_dt_dll_native}\n"
        "  exit /b 1\n"
        ")\n"
        "if %ICU_DT_SIZE% LSS 262144 (\n"
        "  echo [bitcask] ERROR: ICU data DLL is only %ICU_DT_SIZE% bytes -- this is the\n"
        "  echo [bitcask]        stubdata placeholder, not real data. ICU would load with\n"
        "  echo [bitcask]        no data and every normalization would silently return \"\".\n"
        "  echo [bitcask]        Delete ${_icu_data_out_native} and rebuild.\n"
        "  exit /b 1\n"
        ")\n"
        "if not exist \"${_icu_data_out_native}\" mkdir \"${_icu_data_out_native}\"\n"
        "> \"%ICU_MARK%\" echo %ICU_ID%\n"
        "exit /b 0\n")
    file(WRITE "${_icu_bat}" "${_icu_bat_body}")

    ExternalProject_Add(icu_vendored
        SOURCE_DIR        "${PROJECT_SOURCE_DIR}/third_party/icu"
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE   1
        # 这里必须给 CMake 风格的正斜杠路径：ExternalProject 会把 BUILD_COMMAND
        # 原样 set() 进一个生成的 .cmake 脚本里，反斜杠在那儿是非法转义
        # （"Invalid character escape '\w'"）。cmd 的 call 吃正斜杠绝对路径没问题。
        BUILD_COMMAND     cmd /c call "${_icu_bat}"
        # ICU 的 MSBuild 没有 install 目标：头由 common 工程的
        # Windows.CopyUnicodeHeaderFiles.targets 复制到 icu4c/include/unicode，
        # 库与 DLL 落在 icu4c/${_icu_lib_sub} 与 icu4c/${_icu_bin_sub}。
        # 这里手工搬进 icu-install/，顺带把两个 DLL 送进 <build>/bin/。
        INSTALL_COMMAND
            ${CMAKE_COMMAND} -E copy_directory
                "${_icu_tree}/include" "${_icu_prefix}/include"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${_icu_prefix}/lib" "${_icu_prefix}/bin" "${_icu_runtime_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_built_uc_implib}" "${_icu_uc_implib}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_built_dt_implib}" "${_icu_dt_implib}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_built_uc_dll}" "${_icu_uc_dll}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_built_dt_dll}" "${_icu_dt_dll}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_uc_dll}"
                "${_icu_runtime_dir}/icuuc${BITCASK_ICU_MAJOR}${_icu_uc_sfx}.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icu_dt_dll}"
                "${_icu_runtime_dir}/icudt${BITCASK_ICU_MAJOR}.dll"
        BUILD_BYPRODUCTS
            "${_icu_uc_implib}" "${_icu_dt_implib}"
            "${_icu_uc_dll}" "${_icu_dt_dll}"
            "${_icu_runtime_dir}/icuuc${BITCASK_ICU_MAJOR}${_icu_uc_sfx}.dll"
            "${_icu_runtime_dir}/icudt${BITCASK_ICU_MAJOR}.dll"
        LOG_BUILD             ON
        LOG_INSTALL           ON
        LOG_OUTPUT_ON_FAILURE ON
    )

    add_library(bitcask_icu_data SHARED IMPORTED GLOBAL)
    set_target_properties(bitcask_icu_data PROPERTIES
        IMPORTED_IMPLIB   "${_icu_dt_implib}"
        IMPORTED_LOCATION "${_icu_dt_dll}")
    add_dependencies(bitcask_icu_data icu_vendored)

    add_library(bitcask_icu_uc SHARED IMPORTED GLOBAL)
    set_target_properties(bitcask_icu_uc PROPERTIES
        IMPORTED_IMPLIB   "${_icu_uc_implib}"
        IMPORTED_LOCATION "${_icu_uc_dll}"
        INTERFACE_INCLUDE_DIRECTORIES "${_icu_prefix}/include"
        # 注意：这里**没有** U_STATIC_IMPLEMENTATION。Windows 的 vendored ICU
        # 是 DLL，加了那个宏符号就不按 dllimport 解析，链接期全是 unresolved。
        INTERFACE_LINK_LIBRARIES "bitcask_icu_data")
    add_dependencies(bitcask_icu_uc icu_vendored)

    set(BITCASK_ICU_VENDORED_LIBS "${_icu_uc_implib}" "${_icu_dt_implib}")
    set(BITCASK_ICU_VENDORED_DLLS "${_icu_uc_dll}" "${_icu_dt_dll}")

    message(STATUS
        "ICU: vendored ICU ${BITCASK_ICU_MAJOR} via MSBuild"
        " [${_icu_cfg} | ${_icu_msvc_platform} | ${_icu_toolset}]")
else()
    # -----------------------------------------------------------------------
    # 2b. Unix（含 MinGW/MSYS）—— autoconf，静态
    # -----------------------------------------------------------------------
    # ICU 的构建系统是 GNU make。不能用 CMAKE_MAKE_PROGRAM——Ninja 生成器下它是
    # ninja，喂不了 ICU 的 Makefile；也不能写 $(MAKE)，那个只在 Makefile 规则里
    # 展开，作为 ExternalProject 的参数会被原样当成字面量传下去。
    find_program(BITCASK_ICU_MAKE NAMES gmake make)
    if(NOT BITCASK_ICU_MAKE)
        message(FATAL_ERROR
            "vendored ICU 需要 GNU make（ICU 官方只提供 autoconf 构建），"
            "但 PATH 里找不到 gmake/make。")
    endif()

    set(_icu_filter_env "")
    if(_icu_filter_file)
        set(_icu_filter_env "ICU_DATA_FILTER_FILE=${_icu_filter_file}")
    endif()

    set(_icu_libdir "${_icu_prefix}/lib")
    set(_icu_uc_lib   "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}icuuc${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(_icu_data_lib "${_icu_libdir}/${CMAKE_STATIC_LIBRARY_PREFIX}icudata${CMAKE_STATIC_LIBRARY_SUFFIX}")

    # --enable-static/--disable-shared 让产物能被我们的静态归档下游一并链上；
    # --disable-extras/tests/samples 砍掉无关目标。
    # --with-data-packaging=static 把 icudt 打成静态库（否则要在运行期找 .dat
    # 文件，对一个被嵌进别人进程的库是部署负担）。
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

    add_library(bitcask_icu_data STATIC IMPORTED GLOBAL)
    set_target_properties(bitcask_icu_data PROPERTIES
        IMPORTED_LOCATION "${_icu_data_lib}")
    add_dependencies(bitcask_icu_data icu_vendored)

    add_library(bitcask_icu_uc STATIC IMPORTED GLOBAL)
    set_target_properties(bitcask_icu_uc PROPERTIES
        IMPORTED_LOCATION "${_icu_uc_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_icu_prefix}/include"
        # 静态 ICU 必须给下游这个宏，否则符号按 dllimport 解析（Windows）；
        # Unix 上无害。ICU 自身 C++ 是 C++17 编的，ABI 与我们的 C++23 兼容
        # （只用 C 风格与 POD 接口 + Normalizer2 这种纯虚基类）。
        INTERFACE_COMPILE_DEFINITIONS "U_STATIC_IMPLEMENTATION"
        INTERFACE_LINK_LIBRARIES "bitcask_icu_data")
    add_dependencies(bitcask_icu_uc icu_vendored)

    # ICU 的 common 用 dlopen/pthread；Unix 上要显式补。
    find_package(Threads REQUIRED)
    set_property(TARGET bitcask_icu_uc APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads ${CMAKE_DL_LIBS})

    set(BITCASK_ICU_VENDORED_LIBS "${_icu_uc_lib}" "${_icu_data_lib}")

    message(STATUS "ICU: vendored ICU ${BITCASK_ICU_MAJOR} via autoconf（静态）")
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
set(BITCASK_ICU_VERSION "vendored ${BITCASK_ICU_MAJOR}")
# 顶层的 install 规则要把这些库/DLL 和头一起装出去（见 CMakeLists.txt 的
# BITCASK_ICU_VENDORED 分支）——合并归档不含第三方对象，不装下游就缺符号。
set(BITCASK_ICU_VENDORED_INCLUDE "${_icu_prefix}/include")
if(BITCASK_ICU_PROVIDER STREQUAL "vendored")
    message(STATUS "ICU: 按 BITCASK_ICU_PROVIDER=vendored 强制使用 third_party/icu"
                   "（跳过系统探测，ExternalProject 现编）")
else()
    message(STATUS "ICU: 未找到系统安装（>= ${BITCASK_ICU_MIN_VERSION}），"
                   "回落 vendored third_party/icu（ExternalProject 现编）")
endif()
