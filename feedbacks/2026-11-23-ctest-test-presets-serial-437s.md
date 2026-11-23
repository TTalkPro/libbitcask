# `ctest` 不带 `-j` 是串行的：966 个用例 437 秒，`-j 8` 91 秒，test preset 里没配并行

**报的人**：codex（Windows 兼容性验证 + 审计，12.0.3 `d995557`，Windows x64 / 14 逻辑核）。
**严重度**：🟡 本地验证慢 4.8 倍，不报错。

## 症状

同一份 `build-msvc-release`，同一批用例：

| 跑法 | 结果 | 总耗时 |
|---|---|---|
| `ctest --test-dir build-msvc-release --output-on-failure` | 964/964 通过 | 437.27 s |
| `ctest --test-dir build-msvc-release -j 8` | 964/964 通过 | 91.20 s |

`-j 8` 下没有出现因并发而失败的用例。

## 位置

- `CMakePresets.json` 的 `test-base`：`execution` 里只有 `{"timeout": 300}`，没有 `jobs`；`msvc-debug` / `msvc-release` / `gcc-release` / `clang-debug` / `asan` / `tsan` 都继承它。
  ⇒ `ctest --preset msvc-release` 是串行的。
- `tests/CMakeLists.txt`：75 个 `add_executable` + 74 处 `gtest_discover_tests`，966 个 ctest 条目里 **859 个单跑不到 0.3 秒**（主要是进程启动开销）。

## 怎么撞上的

Windows 上跑全量测试：先默认串行，再 `-j 8`；耗时取自 `ctest` 的 `Total Test time` 与 `Testing/Temporary/LastTest.log` 的逐用例 `Test time`。

## 影响面

- 串行时两个套件占了一半以上：`OkiCompactionModelTest` 12 个用例 136.4 s、`UnorderedApplyTest` 3 个用例 95.2 s（合计 53%）。
  全量跑一遍要七分多钟，Windows 上每回改完等结果都付这个钱。
- CI 的 job 自己写了 `-j$(nproc)` / `-j 4`，不受影响；受影响的是照 README / preset 在本地跑的人。
- 用例是「一个 gtest case 一个进程」注册的，按进程隔离（进程级的 `IndexPool` / search arena 不会互相串）；实测 `-j 8` 全绿。

## 建议

- `test-base` 加 `"execution": {"timeout": 300, "jobs": 8}`（或给个够用的缺省，`tsan` 单独覆盖）。
- `OkiCompactionModelTest` / `UnorderedApplyTest` 看能否缩小数据规模，或用假时钟替代 `sleep_for`（`oki_compaction_model_test.cpp:483-570` 多处 1～5 ms 睡眠）。
