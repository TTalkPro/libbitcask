# Windows CI 是 `continue-on-error`，MSVC 侧也没开 `/WX`：56 条告警、乃至整条 job 红了都不拦合并

**报的人**：codex（Windows 兼容性验证 + 审计，12.0.3 `d995557`）。
**严重度**：🟡 CI 不拦；当前本机全绿，问题是「坏了也不会有人说」。

## 症状

- `.github/workflows/ci.yml` 的 `windows-msvc` job 带 `continue-on-error: true`，注释写「直到首次跑绿」。
  本机 VS 18 / MSVC 14.51.36231、`msvc-release`：全量重编 305/305 目标通过，`ctest` 964 个通过、3 个未跑（1 个 skipped、2 个 disabled 的 RSS probe）。
- `CMakeLists.txt:163-169`：`BITCASK_WERROR` 在 MSVC 分支会加 `/WX`，但 Windows job 传的是 `-DBUILD_TESTING=ON`，没开 `BITCASK_WERROR`；
  注释「S37-4：MSVC 侧首轮刻意不开 /WX……收口挪到 S37-7」。
- 全量重编的告警共 **56** 条：

| 条数 | 编号 | 位置 | 内容 |
|---|---|---|---|
| 55 | C4324 | `include/bitcask/inverted.hpp:549` | `InvertedIndex::Shard`: structure was padded due to alignment specifier |
| 1 | C4244 | `tests/oki_only_contract_test.cpp:452` | `vector<pair<uint32_t,string>>::emplace_back` 处 `const uint64_t` → `uint32_t`，possible loss of data |

## 位置

- `.github/workflows/ci.yml`：`windows-msvc`（`runs-on: windows-2022`、`-j 4`、`continue-on-error`）；`clang-tidy`、`avx512-sde` 同样非门控。
- `.github/workflows/ci.yml`：`werror-lib` job 只跑 GCC。
- `CMakePresets.json`：`msvc-asan` 有 configure / build / test preset，CI 没有任何 job 用它。

## 怎么撞上的

本机做 Windows 兼容性验证：先增量、再 `--clean-first` 全量重编并统计 `warning C\d+`，然后串行与 `-j 8` 各跑一遍 `ctest`。

## 影响面

- Windows 上的编译错误、测试失败、`continue-on-error` 的 job 一样通过 check；README 已把 MSVC 列为支持的平台。
- MSVC 独有的告警（窄化转换等）会持续累积，等到想开 `/WX` 时又是一批存量。
- CI 用 VS 2022，本机是 VS 18：两个编译器版本只有一个被 CI 覆盖，另一个只靠人工。
- `msvc-asan` 没进 CI ⇒ Windows 下的内存错误没有自动护栏（ASan + UBSan job 只在 Linux clang 上跑）。

## 建议

- `windows-msvc` 去掉 `continue-on-error`（前提：先在 `windows-2022` 上确认当前 HEAD 是绿的）。
- `inverted.hpp:549` 的 C4324 是对齐填充的预期结果，定点 `#pragma warning(suppress: 4324)`；`oki_only_contract_test.cpp:452` 显式 `static_cast<std::uint32_t>`。
  清零后 Windows job 加 `-DBITCASK_WERROR=ON`（先只对库目标，照 `werror-lib` 的做法）。
- 加一个 `msvc-asan` job（非门控起步也行），把已有的 preset 用起来。
- Windows 的 `ctest` 加 `-j`（见
  [`2026-11-23-ctest-test-presets-serial-437s.md`](2026-11-23-ctest-test-presets-serial-437s.md)）。

## 回没回

已修（13.0.0 已发版）：

- 56 条告警清零：`InvertedIndex::Shard` 的 C4324 是 `gen_` 的 `alignas(64)` 的预期结果（尾部补齐，下一个分片不与它同行），
  只在该结构体上 `#pragma warning(push) / (disable : 4324) / (pop)`（`_MSC_VER` 下）；`oki_only_contract_test.cpp` 的 u64→u32 显式 `static_cast`。
  本机 VS 18 / MSVC 14.51：`msvc-release` 配 `-DBITCASK_WERROR=ON -DBUILD_TESTING=OFF`，`bitcask_static` + `bitcask_shared` 以 `/WX` 编过，0 告警；
  全量构建日志里另有 `bitcask_migrate` 的 14 条 C4840 / C4477（`std::string` 直接喂 `printf("%s")`，是 UB 不是噪音），已改 `.c_str()`。
- `windows-msvc` 去掉 `continue-on-error`，**改门控**。⚠️ 建议里的前提「先在 `windows-2022` 上确认 HEAD 是绿的」**没能本地核实**
  （本机只有 VS 18）；若 runner 上是红的，这一行改回去即可。
- `/WX` **没有**直接开在 `windows-msvc` 上：VS 2022 与 VS 18 的告警面未必一致，开成门控等于把没验证过的东西当闸门。
  改为新增 `werror-windows` job（库目标、`continue-on-error: true`），照 `werror-lib` 的做法；它在 runner 上绿了再并入门控 job。
- 新增 `windows-msvc-asan` job（`msvc-asan` preset，Debug + `/fsanitize=address` + 全量 ctest，`continue-on-error: true` 直到首次跑绿）。
- 三个 job 都没在 GitHub 上实跑过（本地无法触发 CI）。
