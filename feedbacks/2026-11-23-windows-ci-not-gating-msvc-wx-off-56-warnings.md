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
