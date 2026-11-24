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

## 回没回

已修（S77，13.1.1 已发版），按建议 1 做（显式同步点），建议 2 只用在兜底上。

**先复现**：Linux 8 核上 64 份并发（8 倍超额）各跑几百次全过，撞不出来（反馈是 Windows 14 核）。改为往用例里临时注入
「最不利的调度」逐个验证失败模式，两条都坐实：

- `HeadThenTail`：读线程开头睡 300 ms ⇒ 三个读者都在 700 批写完、`stop` 置位后才第一次被调度，进门就退出，
  `EXPECT_GT(seen, 0)` 三条全红。
- `ConcurrentLoader`：写线程开头睡 500 ms ⇒ 1000 次 load（平时 179 ms 做完）在一次归并都没发生时就做完，
  `compactions >= 1` 红。`unloaded` 仍是 0——「写者还没 flush、目录里没 manifest」不是失败模式（`fresh()` 的 `rebuild` 已写出空 manifest）。

**更要紧的**：调度不利时这两个用例即使不红，也没测到交叠，不变量的断言是空转的。而且 `HeadThenTail` / `TailThenHead` 的
撕裂判据写的是 `h.size() == 2 && tv.size() == 2`，只对 `h0`～`h9` 成立——700 批里只有前 9 批真的被检查过。

**改法**：

- `atomic_batch_visibility_test.cpp` 新增 `run_overlapped()`，三个并发用例（`HeadThenTail` / `TailThenHead` / `RemoveSwitches`）共用：
  读者各自先做成一次一致观察才 `std::latch::count_down`，写者等齐再开写（`seen > 0` 由此确定）；写者至少写原定批数，并一直写到
  每个读者在写入期间都做成 200 次观察（上限 10 万批，到上限只打一条 `note:`、不判失败）；停写后读者再观察一次才退出。
- 撕裂判据改为：两次读到同一个 head ⇒ 期间无批提交 ⇒ tail 的代数必须**等于** head 的代数（新头旧尾、旧头新尾都抓），不再只查前 9 批。
  `RemoveSwitches` 的奇偶判据本来就对，只改结构。写批失败从子 lambda 里的 `ASSERT`（只会让 lambda 返回）改成 `ADD_FAILURE` + 提前停写。
- `oki_compaction_model_test.cpp` 的 `ConcurrentLoader`：写者做完第一次 flush 才开始 load；load 一直做到「≥ 1000 次，且 load 期间写者
  提交了 ≥ 3 次归并」（归并后才有 sweep，load 撞 sweep 才是要测的），期限 120 s；期限到仍不够 ⇒ `GTEST_SKIP`（前提没达成），
  `unloaded == 0` 在跳过之前照样严格断言。写者 flush 失败同样不再用子线程里的 `ASSERT`。
- 顶层 `check` 目标去掉封顶 8，照核数开。

**验证**（Linux，gcc Release `-Werror`）：

- 同样的注入下全过：读者晚到 300 ms ⇒ 写者在 latch 上等齐；写者晚到 500 ms ⇒ load 等第一次 flush；读者每次观察都睡 1 ms（整段饥饿）⇒
  写到 10 万批上限、打 `note:`、通过。
- 不注入：64 份并发 × 5 轮（`AtomicBatchVisibilityTest.*` 320 次）、× 4 轮（`ConcurrentLoader` 256 次）全过，无 `note:` / 跳过。
- TSan（`build-tsan`）两个文件的用例无报告；全量 `check` 1002 / 1002；`ctest -j 16`（8 核，2 倍超额）两轮 1002 / 1002。
- ⚠️ **Windows 14 核 `-j 14` 未实测**；改严后的判据没抓到库的问题（S63 的整批可见性成立）。

