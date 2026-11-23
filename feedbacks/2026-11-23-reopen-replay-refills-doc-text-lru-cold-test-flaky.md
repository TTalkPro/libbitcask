# 重开时的恢复重放把原文填回 LRU：冷文档变热，`HighlightColdTest` 偶发红

**报的人**：codex（Windows 兼容性验证，合并 `origin/main` 后跑并行 `check` 撞上，12.2.0 `1b2e241`）。
**严重度**：🟡 测试偶发假红（本机约 5%，`ctest -j 8` 两轮都挂）；库侧行为与「重开即冷」的文档口径不符，不报错。

## 症状

- `HighlightColdTest.ReopenedLibraryHasNoSnippetsUnlessLoaded` 偶发失败，断言是「重开后缺省（`kSkip`）片段为空」，
  实际 d1 / k2 / f3 里有 1～2 篇拿到了 `<em>fox</em>` 片段——它们在重开后的进程里是**热**的。
- CHANGELOG 12.2.0 已知问题栏记为「机器有负载时偶发失败 4/60，未处理」。本机单跑循环实测约 5%
  （60 次挂 2～3 次），`check -j 8` 两轮都挂；失败迭代与通过迭代在 close 后的目录文件字节数完全一致。

## 位置

- `include/bitcask/plugin_api.hpp:74`：`replay_floor_of(wm) = wm − kReplayFloorMargin(4096)`。
- `src/search/text_plugin.cpp:336`（`apply_job_impl_in`）：`doc_texts_.put(job.ord, ...)` 排在同 LSN 判重（`:356`）**之前**。
- `src/search/text_plugin.cpp:162`（`apply_text_in`）：同病，put 在判重（`:175`）之前。
- `src/cask/cask_recovery.cpp:1015`、`:698` / `:708`：重放起点与 `view.ord >= replay_floor` 投递。
- `tests/search_highlight_cold_test.cpp:41-43`：注释「1500 篇重开直接载段、不重放 ⇒ 冷」是误归因。

## 怎么撞上的

合并 `origin/main` 后跑 `cmake --build build-msvc-release --target check`（并行 ctest，封顶 `-j 8`），
两轮都在这个用例上红；单跑 `--gtest_repeat` 约 1/20 红。

用临时诊断用例（未提交）量了上百次重开，失败和通过的迭代对照：

| | 重开后 LRU 里热的 filler | d1 / k2 / f3 |
|---|---|---|
| 通过的迭代 | 1024 / 1500 | 全冷 |
| 失败的迭代 | 1022～1023 / 1500 | 1～2 篇热（d1+k2、只 k2、只 f3 都见过） |

## 病因

三件事叠起来，缺一不发作：

1. **每次重开都全量重放，不看负载。** 重放起点是 `wm − 4096`，库里只有 1503 篇，水位不到余量 ⇒ 起点恒为 0。
   CHANGELOG 里「高负载下重开仍从数据重放」只说对了一半：重放什么时候都在发生，通过的迭代同样重放了
   ≥ 1024 篇（LRU 装满 1024 篇 filler 就是证据）。
2. **重放任务照样填原文 LRU。** `apply_job_impl_in` / `apply_text_in` 先 `doc_texts_.put`，再查 `key_to_location_`
   发现同 LSN 已索引而 return。put 已经做了。于是重放的最后 1024 篇留在 LRU 里。
3. **留下哪 1024 篇由分片线程的调度决定。** 重放任务按 key 哈希落分片并行应用；d1 / k2 / f3 的 ord 最小，
   通常先被应用、最后被挤出。负责它们所在分片的线程若起得晚（其它分片已 put 了 ≥ 480 篇之后才轮到），
   它们就留在 LRU 里 ⇒ 用例红。并发越高越容易晚，所以 `-j 8` 下几乎必挂。

不是合并 / S70 引入的：余量是 S45-5 加的，CHANGELOG 记 9.1.0 基线就 29 / 32 次失败；合并前 7 轮全绿只是运气
（0.95⁷ ≈ 70%）。

## 影响面

- `windows-msvc` 已改成门控 job（见
  [`2026-11-23-windows-ci-not-gating-msvc-wx-off-56-warnings.md`](2026-11-23-windows-ci-not-gating-msvc-wx-off-56-warnings.md)），
  这个用例会让 CI 随机红。
- 库侧：每次 open 都把重放窗口内的正文拷一遍进 LRU（S45-5 的设计是「多重放的部分幂等跳过」，LRU 这一半没跳过）——
  白付一份正文深拷，且重开后的「冷文档」语义取决于调度：同一份库，这次重开 d1 冷、下次热。
- `ColdText::kLoad` 的结果不受影响（热冷两条路切片逐字相同）；只是 `kSkip` 对「重开后理应冷」的文档偶尔给出片段。

## 建议

**方案 A（已选，修库）**：把 `text_plugin.cpp` 两处 `doc_texts_.put` 挪到同 LSN 判重**之后**。

- 重放的重复文档（`it->second.ord >= job.ord`）不再进 LRU，重开后确实全冷；用例不再依赖调度，自然稳定。
- 省掉每次 open 对重放窗口内正文的无谓深拷。
- 真·覆盖写（更高 ord）路径不变，`put` 照做；`HotSnippetsAreUntouchedByLoad` / `ColdSnippetsMatchHotOnes` 不受影响。
- 盘上格式 / ABI 不变。

备选（未选，记在这里备查）：

- B：`kFiller` 提到 5200 以上，让目标文档落在重放窗口（最后 4096 个 ord）之外。不动库，测试变慢。
- C：这个用例设 `doc_text_cache_max = 0`，照 S55 自家冷路径用例的做法。只改测试一行。
- D：不修，`windows-msvc` 恢复 `continue-on-error`。

## 回没回

已修（S73，未发版），按方案 A：

- `apply_text_in` / `apply_job_impl_in` 的 `doc_texts_.put` 挪到同 LSN 早读判重**之后**、`bld->add` **之前**：
  重放 / 乱序到达的旧版本在判重处直接 return，不再 put。放在 `add` 之前是保住原来的顺序——
  原文先进 LRU、文档再进段，并发查不会看到「有命中、没片段」的窗口。
- 无判重可查的两条路原文照进 LRU，与修前一致：无在建段（shim / 空 job 降级）；无可索引词项的文档
  （向量-only、全停用词——`key_to_location_` 里本来就没有它们的条目）。
- 终检败者（`lost`，并发写了同 key 更高 ord）仍 put 一份：LRU 按 ord 键，死 ord 的条目读不到，只占一格；
  生产流水线同 key 同分片 FIFO，实际走不到，未动。
- 回归：`HighlightColdTest.ReopenedLibraryHasNoSnippetsUnlessLoaded` 本机 MSVC Release `--gtest_repeat=60` 60/60（修前约 1/20 红）；
  全量 ctest `-j 8`（修前两轮都挂在这个用例上）987 / 987 通过。
- Windows 上按 CI 三个 job 的配置补跑（本机 MSVC 14.51）：`msvc-release` 全量 `ctest -j 4` 987 / 987；
  `msvc-debug` 该用例 `--gtest_repeat=60` 60 / 60；`msvc-asan` 该用例 `--gtest_repeat=20` 20 / 20、全量 `ctest -j 8`
  987 / 987 无 ASan 报告。后两个 preset 原本编不过，是另一条
  （[`2026-11-23-msvc-debug-asan-presets-do-not-build.md`](2026-11-23-msvc-debug-asan-presets-do-not-build.md)，S74），
  修了才跑起来的。
- 测试注释（`search_highlight_cold_test.cpp:41-43`）改写：冷的根据是「重放不填 LRU」，不是「落进封好的段、不重放」。
  `kFiller = 1500` 不再是必要条件，留作回归（临时改 `kFiller = 3` 实测重开同样全冷，20 / 20）。
