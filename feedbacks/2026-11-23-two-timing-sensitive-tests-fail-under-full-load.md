# 照核数满载跑测试有两个带时序假设的用例会挂：单跑 5 次全过，负载越高越现形

**报的人**：codex（Windows 兼容性验证 + 审计，12.0.3 `d995557`，Windows x64 / 14 逻辑核）。
**严重度**：🟡 测试假红，间歇、负载越高越现形；不影响库行为。

## 症状

`ctest -j 14`（照 14 逻辑核满载）偶发失败（从 [`2026-11-23-ctest-test-presets-serial-437s.md`](2026-11-23-ctest-test-presets-serial-437s.md)
的并行化验证里撞出来）：

- `AtomicBatchVisibilityTest.HeadThenTailNeverShowsHalfBatch`
- `OkiCompactionModelTest.ConcurrentLoaderNeverFallsBackToUnloaded`

两个用例各自单跑 5 次全过；`-j 8` 反复跑也全绿。挂的是用例自身对调度时序的假设
（写者 / loader 线程没在假设的窗口内交叠），不是可见性不变量被破坏。

## 位置

- `tests/atomic_batch_visibility_test.cpp`：head/tail 半批可见性用例依赖两个写线程按假设顺序交叠。
- `tests/oki_compaction_model_test.cpp:483-570`：并发 loader 用例靠多处 1～5 ms 的
  `sleep_for` 给调度留窗（ctest 反馈记录里已点过名）。

（行号以 12.0.3 工作树为准，后续挪动以测试名索引。）

## 怎么撞上的

`check` 目标用 `-j <ProcessorCount>` 封顶 8；为验证封顶值合不合理，手动开 `-j 14` 跑全量，两个用例间歇挂。

## 影响面

- 本地照核数开满跑测试 / CI 把 `-j` 开到物理核数：偶发假红，浪费排查时间。
- `check` 目标已封顶 8 绕开（`tests/CMakeLists.txt`）；显式 `ctest -j 14` 仍会撞。
- 库行为不受影响：两个用例断言的都是可见性不变量，挂时复跑即绿。

## 建议

- 两处都对调度时序去敏感：假时钟，或用显式同步点（promise / latch）把线程带到已知交叠点，别靠 sleep 留窗。
- 或者在测试入口把「需要交叠」的前提显式断言出来：前提没达成就 skip / 标 unsupported，别让前提不满足时变成断言失败。
