# MSVC 的 Debug / ASan 两个 preset 编不过：`is_standard_layout` 断言 + 插桩目标与没插桩目标的 STL 容器标注不一致

**报的人**：codex（Windows 验证：给 S73 的修复补跑 CI 的 `windows-msvc-asan` job，本机 VS 18 / MSVC 14.51）。
**严重度**：🔴 两个 preset 在编译期就断，不产生产物。`msvc-debug` 自 12.0.2 起、`msvc-asan` 在本机一直是编不过的；
CI 的 `windows-msvc-asan` job 一上跑就是红（目前非门控，没在 GitHub 上实跑过）。

## 症状

`cmake --preset msvc-asan -DBUILD_TESTING=ON && cmake --build build-msvc-asan` 一处修掉下一处才现形，共三层；
三层都没有 S73 改动的事：

1. `src/fileops/oki_state.cpp(190): error C2607: static assertion failed`
2. ```
   bitcask_io.lib(win32_file.cpp.obj) : error LNK2038: mismatch detected for 'annotate_string': value '1' doesn't match value '0' in gen_inert_table.cpp.obj
   ... 'annotate_vector' ... 'annotate_optional'
   bin\gen_inert_table.exe : fatal error LNK1319: 3 mismatches detected
   ```
   修掉后同一报错换人再出现两轮：`bitcask.dll`（`bitcask_kv.cpp.obj` = 0 对 `bitcask_cask` = 1），
   然后是**每一个**测试可执行文件（`gtest.lib` = 0 对测试 TU = 1）。

`msvc-debug` preset（不带 ASan）停在同一处 1。

## 位置

- `src/fileops/oki_state.cpp:190`：`static_assert(std::is_standard_layout_v<ActiveSorted>)`（S64，`ab9b51f`，12.0.2）。
- `cmake/BitcaskSanitizers.cmake`：MSVC 分支只加了 `/fsanitize=address`，没处理 STL 的容器标注。
- `CMakeLists.txt:769`：`gen_inert_table` 链 `bitcask_io`（S72，`e87cdbd`，13.0.0 已发版）而不链 `bitcask_sanitizers`。
- `CMakeLists.txt:904`：`bitcask_shared` 只链 `bitcask_cask`，是第一方目标里唯一没带 `bitcask_sanitizers` 的。

## 怎么撞上的

修 `HighlightColdTest` 偶发红（S73）之后，按 CI 三个 Windows job 的配置在本机逐个跑：
`windows-msvc`（Release 全量构建 + ctest）、`werror-windows`（`/WX`）都过；`windows-msvc-asan` 从头配置，连断三层。

## 病因

1. **断言比它守的不变式更严。** `ActiveSorted` 要的是「`tail` 是首成员、偏移 0」——`sorted_view_` 存的是
   `&cur->tail` 别名指针，`:1503` / `:1581` 靠 `reinterpret_cast` 转回整个对象。standard-layout 蕴含这一点，
   但 MSVC Debug（`/MDd`，`_ITERATOR_DEBUG_LEVEL=2`）下 `std::vector` 带调试迭代器代理、不是 standard-layout
   ⇒ `ActiveSorted` 也不是 ⇒ 断言红。布局与 Release 完全一致，别名转换照样成立。
   Release 下 `_ITERATOR_DEBUG_LEVEL=0`，`std::vector` 是 standard-layout，所以 Release 一直绿。
2. **`annotate_*` 不是 `/analyze` 的提示，是 ASan 容器标注。** `/fsanitize=address` 定义 `__SANITIZE_ADDRESS__`，
   STL 据此给 string / vector / optional 开容器标注（size～capacity 之间的越界检测）；标注会改容器布局，
   所以 STL 用 `detect_mismatch` 把「插桩的 .obj」与「没插桩的 .obj」链到一起判成硬错误
   （`__msvc_sanitizer_annotate_container.hpp`）。插桩是 per-target（`bitcask_sanitizers` INTERFACE），
   于是凡是「链了插桩库、自己不带」的目标都撞：漏带的 `gen_inert_table` / `bitcask_shared`，
   以及**不可能带**的第三方 gtest（每个测试可执行文件都链它）。
3. `gen_inert_table` 在 GCC / Clang 下**按理也过不了链接**：它是可执行文件，链的 `bitcask_io` 目标文件引用
   `__asan_*` / `__tsan_*`，自己没带 `-fsanitize=` 就没有运行时（`.so` 允许未定义符号，可执行文件不允许）。
   即 CI 的 `sanitizer` job（门控）自 S72 起应当也是红的。⚠️ 这一条是读代码推的，本机没有 Linux 工具链，未实测。

## 影响面

- 本机：`msvc-debug` / `msvc-asan` 两个 preset 不可用；Windows 下没有调试构建，也没有 ASan 护栏。
- 发布产物不受影响（Release 一直能编）。库侧行为不变。
- 可能波及 Linux CI `sanitizer` job（未实测，见病因 3）。

## 建议

1. 断言改守真正要的东西：`std::is_standard_layout_v<ActiveSorted> || offsetof(ActiveSorted, tail) == 0`。
2. `bitcask_sanitizers` 的 MSVC 分支带上 `_DISABLE_STL_ANNOTATION`：所有插桩目标关掉容器标注，
   与 gtest / ICU / TBB 回到同一布局。代价是 MSVC ASan 构建少了容器越界这一项检测（其余照旧）。
3. 漏带的目标补上 `bitcask_sanitizers`（`gen_inert_table`、`bitcask_shared`），与其余第一方目标一致。
   非 Windows 下生成器会在构建期被跑一次，给它加 `ASAN_OPTIONS=detect_leaks=0`：它是构建工具，
   ICU 的静态缓存若被 LSan 判泄漏不该打断构建。

## 回没回

已修（S74，13.0.0 已发版），三条都按建议做：

- `oki_state.cpp`：断言改为 `is_standard_layout || offsetof(tail) == 0`，加 `<cstddef>`。
- `BitcaskSanitizers.cmake`：MSVC 分支 `target_compile_definitions(... _DISABLE_STL_ANNOTATION)`。
- `CMakeLists.txt`：`gen_inert_table`、`bitcask_shared` 链 `bitcask_sanitizers`；
  `generate_inert_table` 的命令在非 Windows + sanitizer 构建下经 `cmake -E env ASAN_OPTIONS=detect_leaks=0` 启动。
- 没动：`bench/` 的两个可执行文件也不链 `bitcask_sanitizers`，`BITCASK_BUILD_BENCHMARKS=ON` + MSVC ASan 下
  仍会在链接时缺 `__asan_*`（默认不编，CI 不跑，未实测）。
- ⚠️ Linux / GCC / Clang 未实测；VS 2022（CI runner）未实测。

### 验证（本机 MSVC 14.51，`windows-msvc-asan` job 的配置）

- `msvc-asan`：全量构建 244 / 244；全量 `ctest -j 8`，`ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1`
  ⇒ 987 / 987 通过，无 ASan 报告（473 s）。`HighlightColdTest` 单跑 `--gtest_repeat=20` 20 / 20。
- `msvc-debug`：原先断在 `oki_state.cpp` 的目标全部编过（`msvc-asan` 同为 Debug，`_ITERATOR_DEBUG_LEVEL` 一致）；
  `HighlightColdTest` `--gtest_repeat=60` 60 / 60。全量 Debug ctest 没跑。
- 改动后的非 sanitizer 路路重验：`msvc-release` 全量构建 + `ctest -j 4` 987 / 987；
  `BITCASK_WERROR=ON` 的 `bitcask_static` / `bitcask_shared` 无告警。
- 跑 ASan 测试要在 VS 开发环境里（CI 的 `msvc-dev-cmd` 是）：`clang_rt.asan_dynamic-x86_64.dll` 在
  `VC\Tools\MSVC\<ver>\bin\Hostx64\x64\` 下，不在 PATH 上时测试进程起来就停着（加载器在等一个看不见的
  「找不到 DLL」弹窗），ctest 不报错、只是不动。本机用 `scripts/Enter-MsvcEnv.ps1` 进环境。
