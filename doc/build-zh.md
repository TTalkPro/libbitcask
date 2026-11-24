# 构建与安装

> 从 README 拆出。[← 返回 README](../README.md)


ICU 的来源（`BITCASK_ICU_PROVIDER`）与索引可复现性的关系见 [`text-encoding-zh.md`](text-encoding-zh.md)。


## 系统依赖

| 依赖 | 版本 | 说明 | Debian/Ubuntu 包 |
|------|------|------|------------------|
| C++ 编译器 | GCC 13+ / Clang 17+ / **MSVC 19.4x+（VS 2022 17.6+）** | 需要 C++23 支持 | `gcc` `g++` |
| CMake | **≥ 3.21** | 构建系统（合并静态库用 `$<TARGET_OBJECTS:>` 取 STATIC 目标对象，该用法自 3.21 起可用）| `cmake` |
| ZLIB | v1.3.1（Windows 走子模块）| CRC32 / 数据压缩 | Linux/BSD `zlib1g-dev`；Windows 由 `third_party/zlib` 现编 |
| oneTBB | — | 并发容器；普通构建用系统包 | `libtbb-dev` |

> 普通构建用系统的 `libtbb`（`find_package(TBB)`）；仅 TSan 构建会改用 `third_party/oneTBB` 源码编译插桩版——系统 libtbb 未插桩，TSan 下会漏报/误报。

## Windows（MSVC）

平台支持范围：**Windows x64 / MSVC 原生 ABI**。不支持 MinGW，不支持 ARM64 Windows 与 32 位 x86。

- **依赖**：**无外部包管理器**。zlib 与 oneTBB 都从 `third_party/` 子模块现编——Windows 上这两者都没有「系统版」可用，也不走 vcpkg（oneTBB 的 `tbb` 端口经 `hwloc` 会拖进一整套 msys2 工具链，与「纯 MSVC」构建冲突）。配置前先：

```bat
git submodule update --init --recursive
```

- **配置**：用 `CMakePresets.json`，不必手填 toolchain 与 triplet：

```bat
cmake --preset msvc-debug
cmake --build --preset msvc-debug
cmake --build --preset msvc-debug --target check
```

- **产物**：`bitcask.dll` + `bitcask.lib`（导入库）+ `bitcask_static.lib`（合并静态库；Windows 下不叫 `bitcask.lib` 是为了不与导入库撞名）。所有 exe 与 DLL 统一落到 `<build>/bin/`——Windows 没有 RPATH，DLL 必须与 exe 同目录。
- **Sanitizer**：只有 ASan（`BITCASK_SANITIZE=address`）。TSan / UBSan 在 MSVC 下不存在，指定它们会在**配置期报错退出**而不是静默降级——「以为在跑 TSan 而实际没插桩」比构建失败危险得多。并发正确性仍以 Linux TSan 为准。

> **跨模块使用请只用 C API。** C++ 头（`include/bitcask/*.hpp`）要求调用方与库共用同一份 CRT；`/MT`（静态 CRT）下每个模块各有一份自己的堆，而 `io::File::pread` 一类按值返回 `std::vector` 的接口会让「库这侧分配、调用方析构」跨堆发生。默认的 `/MD` 下没有这个问题。失败形态是**进程无声消失、退出码 `0xC0000409`、`terminate`/`abort` 都不触发**，极难定位——详见 [`doc/api-c.md` §2.1](api-c.md)。`CMakeLists.txt` 在配置期检查 CRT 选择并告警。

## Vendored 依赖（git submodule）

`.gitmodules` 全部条目位于 `third_party/`；首次 clone 后无需手动安装，构建无需联网。

| submodule | 来源 | 用途 |
|-----------|------|------|
| `third_party/icu` | https://github.com/unicode-org/icu | Unicode NFKC_Casefold + 字符属性 + 编码转换。**可选**：标了 `update = none`，`--recursive` 会跳过它（380 MB）；只有 `BITCASK_ICU_PROVIDER=vendored` 或系统无 ICU 时才需要，见下 |
| `third_party/cppjieba` | https://github.com/yanyiwu/cppjieba | 中文分词 |
| `third_party/limonp` | https://github.com/yanyiwu/limonp | cppjieba 的 header-only 依赖 |
| `third_party/googletest` | https://github.com/google/googletest | 测试（`BUILD_TESTING=ON` 时编） |
| `third_party/benchmark` | https://github.com/google/benchmark | 微基准（`BITCASK_BUILD_BENCHMARKS=ON` 时编） |
| `third_party/oneTBB` | https://github.com/uxlfoundation/oneTBB | TSan 插桩版（`BITCASK_SANITIZE=thread` 时编） |
| `third_party/unordered_dense` | https://github.com/martinus/unordered_dense | DocMap live/meta/delta 表使用的稠密哈希容器（header-only INTERFACE 库） |

## 获取代码与构建

```bash
# 首次 clone —— 带 submodule
git clone --recurse-submodules <repo-url>
# 已 clone 的仓库补拉 submodule
git submodule update --init --recursive
```

## 构建与测试

```bash
# Release 构建（含 LTO / -falign-functions=64 / _FORTIFY_SOURCE=2 / Full RELRO）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 测试 + 基准
cmake -S . -B build -DBUILD_TESTING=ON -DBITCASK_BUILD_BENCHMARKS=ON
cmake --build build -j
cmake --build build --target check   # 并行 ctest，-j 照核数
```

跑测试用顶层目标 `check`（`BUILD_TESTING=ON` 时才有）：它就是 `ctest --output-on-failure -j <核数>`。直接跑 `ctest` 不带 `-j` 是串行的（14 逻辑核上实测串行 437 s、`-j 8` 91 s）。`CMakePresets.json` 的 `test-*` preset 没法配 `jobs`——那要 preset version 6（CMake ≥ 3.29），本项目下限是 3.21。

## Sanitizers

```bash
# ASan + UBSan（ASan 与 TSan 互斥，一次设置一种）
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=address,undefined -DBUILD_TESTING=ON

# TSan 构建改用 third_party/oneTBB 源码编译插桩版（系统 libtbb 未插桩）
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
    -DBITCASK_SANITIZE=thread -DBUILD_TESTING=ON
```

## `-Werror` 库构建

默认关闭以避免新编译器新告警破坏下游；开启时只对 first-party 库目标生效，third_party 头（cppjieba / limonp）已标 SYSTEM 不受影响。CI `werror-lib` job 开启（GCC 13 + Release + 只建 `bitcask_static` / `bitcask_shared`）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DBITCASK_WERROR=ON -DBUILD_TESTING=OFF
cmake --build build -j --target bitcask_static bitcask_shared
```

## ICU 的来源（`BITCASK_ICU_PROVIDER`）

ICU 提供 NFKC_Casefold 归一化、Unicode 字符属性与编码转换（GB18030 等 → UTF-8）。
需要 ICU **≥ 60**（`Normalizer2::normalizeUTF8` 的引入版本），只用 `uc` + `data`
两个组件，不链 `i18n`。
它是**唯一默认走系统安装**的依赖——ICU 带约 30 MB 数据表，vendored 一份会让
部署形态和发行版的安全补丁流都变样。三态显式可控：

| 取值 | 行为 |
|------|------|
| `auto`（默认） | 先 `find_package(ICU)`；系统没有（或 < 60）才回落 `third_party/icu` |
| `system` | 只用系统 ICU；找不到就**配置期报错**，绝不静默回落。发行版打包用 |
| `vendored` | 只用 `third_party/icu`，跳过系统探测——即使系统装了也不用 |

```bash
cmake -S . -B build -DBITCASK_ICU_PROVIDER=vendored   # 强制用子模块里的 ICU
cmake -S . -B build -DBITCASK_ICU_PROVIDER=system     # 只认系统 ICU
```

`vendored` 的典型场景：需要把 ICU 版本钉死以保证索引可复现（ICU 版本 = Unicode
版本 = NFKC_Casefold 表 = 分词结果）、交叉编译、目标机无开发包、或系统 ICU 过老。
这个子模块标了 `update = none`，所以 `git submodule update --init --recursive` **不会**
拉它（380 MB，其余依赖加起来也没这么大）。要用 vendored 就显式拉——注意
`update = none` 会让**普通的 `--init` 也静默跳过**（只打印 `Skipping submodule`，
退出码 0），必须用 `-c` 就地覆盖：

```bash
git -c submodule.third_party/icu.update=checkout \
    submodule update --init --depth 1 third_party/icu
```

ICU 自带 autoconf（Unix）/ MSBuild（Windows）构建，官方不提供 CMake，故由
`ExternalProject_Add` 驱动它自己的构建系统——首次构建要额外花十几分钟编 ICU，
且顶层的 warning / sanitizer / IPO flag 不传播进去（同 zlib/googletest 的处置）。
用 CPM.cmake / FetchContent 换掉这段也不行：上游 ICU 树里没有任何
`CMakeLists.txt`，而社区仅有的两个 CMake 移植都不覆盖本项目要的组合
（[LibCMaker/ICU_CMake_Files](https://github.com/LibCMaker/ICU_CMake_Files) 停在
ICU 63 且仓库已归档；[viaduck/icu-cmake](https://github.com/viaduck/icu-cmake)
的 Windows 预编译产物是 MinGW-w64 的、与 MSVC ABI 不兼容，源码构建走的还是
autoconf）。

**Windows / MSVC 上的 vendored ICU**：ICU 的 `allinone.sln` 里所有工程都是
`DynamicLibrary`，官方**没有静态配置**，所以这条路产出的是
`icuuc<major>.dll` + `icudt<major>.dll` 与两个导入库（Unix 侧则是静态
`libicuuc.a` + `libicudata.a`）。两个 DLL 会被自动复制到 `<build>/bin/`，
`install` 时进 `bin/`。除 MSBuild 外还**必须有 Python 3**——ICU 的
`source/data/makedata.mak` 把整个数据构建交给了 `py -3 -m icutools.databuilder`，
与 `BITCASK_ICU_TRIM_DATA` 开关无关；机器上没有可用的 `py` launcher 时，构建系统
会自动生成一个转发到 CMake 找到的 Python 的垫片。相关旋钮（都可留空自动探测）：

| 变量 | 作用 |
|------|------|
| `BITCASK_ICU_MSBUILD` | `MSBuild.exe` 路径。默认按 `CMAKE_VS_MSBUILD_COMMAND` → `PATH` → `vswhere` 顺序找 |
| `BITCASK_ICU_MSVC_TOOLSET` | 编 ICU 用的 `PlatformToolset`。默认跟随本次构建的工具集。ICU 76 自带的 props 只认到 `v143`（VS 2022），更新的 VS 必须显式喂，否则 MSBuild 报 `Platform Toolset = ''` |
| `BITCASK_ICU_MSVC_CONFIG` | `Debug` / `Release`。默认跟随 `CMAKE_BUILD_TYPE`——ICU 的 Debug 用 `/MDd`、Release 用 `/MD`，务必与本次构建的 CRT 一致 |

`VCToolsVersion` 无旋钮、自动钉：构建系统从当前编译器路径切出
`…/VC/Tools/MSVC/<ver>/bin/` 里的版本（`VCToolsVersion` 环境变量只作兜底；
都拿不到不钉），经 `/p:` 全局属性喂给 MSBuild。不钉的话 VS 18 的 MSBuild 会
自选最新的 14.5x，与 `/p:PlatformToolset=v143` 撞 `MSB8052`——而报文指的
「改工具集 / 修 VS 安装」两条都不通。configure 输出第四格显示钉的是哪版：

```
ICU: vendored ICU 78 via MSBuild [Release | x64 | v143 | VCToolsVersion 14.44.35207]
```

MSBuild 只能在 ICU 源码树里就地构建（vcxproj 把产物路径写死成 `..\..\bin64` 等），
故 vendored 构建会弄脏 `third_party/icu` 工作区，属预期。

**数据裁剪（`BITCASK_ICU_TRIM_DATA`，默认 OFF）**：ICU 的完整数据表约 31 MB，
只留归一化与编码转换的话约 5.5 MB。**默认不裁，直接用全量数据。**

不裁的理由是风险不对等：裁多了**不会让构建失败**，只会让归一化或某个编码在
**运行期**静默失效（返回空串 / `kUnknownEncoding`）。而 `cmake/icu-data-filter.json`
里写的是 ICU 的**内部 category 名**，属实现细节、跨大版本会改——ICU 78 就已经
对其中的 `brkitr_treedict` / `coll_tree` 报“category 不存在”，且只是 warning、
不中断构建。等于这份清单会随 ICU 升级悄悄失效，而失效点在运行期。为省 25 MB
背这个，不划算。

需要那 25 MB 时再显式打开（`-DBITCASK_ICU_TRIM_DATA=ON`）：裁剪由 ICU 自己的
buildtool 完成，**需要 Python 3**，找不到会告警并回落全量数据。开启或改动清单后
必须重跑 `bitcask_text_encoding_test` 与 `bitcask_analyzer_test`——它们是这份清单
唯一的守门人。

## 主要 CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTING` | OFF | 编测试二进制 + ctest（CMake 内建开关） |
| `BITCASK_BUILD_BENCHMARKS` | OFF | 编 Google Benchmark 微基准 |
| `BITCASK_SANITIZE` | 空 | `address,undefined` / `thread`——ASan 与 TSan 互斥 |
| `BITCASK_WERROR` | OFF | first-party 告警升错（CI `werror-lib` 开启） |
| `BITCASK_NATIVE` | OFF | 为本机现编启用 `-march=native`（破坏二进制可移植性，勿与 `-ffast-math` 混用） |
| `BITCASK_LTO` | ON | Release 启用 LTO / IPO；sanitizer 构建自动关闭 |
| `BITCASK_PCH` | ON | 预编译头加速编译；排查 PCH 异常可临时关闭 |
| `BITCASK_ICU_PROVIDER` | `auto` | ICU 来源：`auto` / `system` / `vendored`，见上一节 |
| `BITCASK_ICU_TRIM_DATA` | OFF | vendored ICU 只留归一化与编码转换的数据（约 31 MB → 5.5 MB），见上一节 |
| `BITCASK_BUNDLED_ZLIB` | Windows ON，其余 OFF | 从 `third_party/zlib` 现编，而非 `find_package(ZLIB)` |
| `BITCASK_BUNDLED_TBB` | Windows / TSan ON，其余 OFF | 从 `third_party/oneTBB` 现编，而非 `find_package(TBB)` |

版本信息由 `CMakeLists.txt` 的 `project(libbitcask VERSION …)` 单一真源派生：`libbitcask.so` 的 `SOVERSION` = major，`VERSION` = 完整版本，C 端 `bitcask_version_{major,minor,patch,string}()` 同步。当前版本见 [`CHANGELOG.md`](../CHANGELOG.md) 顶部。

## 产物

- `libbitcask.so` — 共享库，导出 C API（`extern "C"`；`SOVERSION` = major）；Windows 为 `bitcask.dll` + `bitcask.lib`（导入库）
- `libbitcask.a` — 把全部静态归档合并为单一 `.a`；Windows 为 `bitcask_static.lib`（换名避免与导入库撞名）
- `bitcask_migrate` — 统一纪元迁移入口：`detect` / `be2le`（v1 大端 → 当前）/ `tstamp64`（u32 → u64，5.0 flag-day）/ `hintord`（hint 补 ord，5.1 flag-day）
- `migrate_le` — v1 大端 → 当前纪元（旧入口，等价于 `bitcask_migrate be2le`）
- `vec_engine_migrate` — HNSW / IVF-RaBitQ / DiskANN 离线引擎切换（S32-M4）
- `gen_inert_table` — NFKC 惰性区间表代码生成器（构建期自动执行）

## 安装

```bash
cmake --install build   # 头文件、libbitcask.{so,a}、bitcask_c.h
```

