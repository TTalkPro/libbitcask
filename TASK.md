# Google C++ Style 规范化修复任务清单

> 来源：`RISK_REPORT.md`（2026-07-14 v2 深度审计 + Phase 5 审计 + **Phase 6 审计 2026-07-15**）
> 当前活跃范围：**Phase 6（T19-T26）** + 2 项遗留（T8、T12）
> 基线测试：641/641 ctest 通过（1 个 S30RssProbe 预存 Disabled）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警 + lsp_diagnostics 无新错误

---

## ✅ Phase 1-5 归档（T1-T18，全部落地或明确处置）

> 详细记录见 git 历史（本文件在 commit 57b9878 之前的版本）与 RISK_REPORT.md 对应章节。

| 项 | 内容 | 状态 |
|---|---|---|
| T1 | CRC32 入口统一（codec:: 收口） | ✅ 641/641 |
| T2 | vbyte 带检查版本归并（vbyte_read_checked） | ✅ 641/641 |
| T3 | .clang-tidy 配置 + CI job | ✅ |
| T4 | MmapRegion RAII 基建 | ✅（后由 T11 决策删除） |
| T5 | SealedSegmentVectorPlugin 抽取（IVF/DiskANN ~950 行回收） | ✅ commit ba99af2 |
| T6 | MEM-MED-1 Registry acquire/release 配对 | ✅ 641/641 |
| T7 | DL-MED-1/2 RunFn ord 泄漏 + checkpoint 30s 超时 | ✅ 641/641 |
| T9 | 死代码清理（fill_get_result 等 ~71 行） | ✅ 641/641 |
| T10 | file_util.hpp（9 份 FileCloser → 1）+ MEM-LOW-1 闭合 | ✅ 641/641（**RED-2 另两个模板未做 → T21**） |
| T11 | mmap_handle.hpp 删除（建而未用） | ✅ |
| T13 | plugin_api run_serialized 契约注释 | ✅ |
| T14 | OrdSkipGuard 析构 try-catch 防 terminate | ✅（**表述修正见下**） |
| T15 | last_ckpt_ord_ 三处漏更新 | ✅ ASan 全过 |
| T16 | flush_upto 死代码 + reducer 每任务通知块删除 | ✅ ASan+TSan×5 |
| T17 | FilePtr 别名 + diskint::pwrite_all 收口 | ✅ ASan 全过 |
| T18 | thread-safety 文档化 P5-DL-1/2 | ✅ doc/concurrency-zh.md |

**⚠️ T14 表述修正（Phase 6 / P6-MEM-1）**：T14 记录的代价「泄漏 1 个 ord
（可恢复，触发 30s 超时路径）」只对 ord 成立。同一次 push 抛出**同时**泄漏
`in_flight` 计数，而 `IndexPool::flush()` 无超时 → `close()`/`~Cask` 永久
挂死，**不可恢复**。修复归入 T19；`cask.hpp:1019-1023` 注释随 T19 一并修正。

---

## 🔴 Phase 6：新任务（来自 RISK_REPORT「Phase 6 修订」段，2026-07-15）

> 审计方法：3 路深读 agent + 主 Agent 逐环节对抗复核（推翻 1 项 P6-DL-2、
> 细化 1 项触发窗口、亲验 fdatasync 与 reinterpret_cast 两条关键断言）。

### T19 — P6-MEM-1 + P6-DL-1：IndexPool::flush 有界等待 + submit 异常补偿 🔴 HIGH

**症状（两条独立进入路径，同一挂死点）**：
- P6-MEM-1：`submit()` 先 `in_flight.fetch_add`（`thread_pool.hpp:428`）再
  `queue_.push`（:431，TBB 有界队列分配可抛 bad_alloc），抛出即泄漏计数，
  全类仅两处 dec（:567/:648）均不覆盖此窗口 → `flush()` 谓词永假。
- P6-DL-1：`close()` 的 30s 逃生门（`cask.cpp:645-660`）break 后紧接
  `unregister_lib` → 无超时 `flush()`（`cask.cpp:685-689`）——逃生门被
  25 行后的等待抵消。触发窗口：写线程在 `in_flight++` 与 push 返回之间被
  kill（含背压阻塞期,队列满时 push 阻塞、窗口拉长）。

**结构根因**：`flush()`（`thread_pool.hpp:435-443`）是全池唯一既无超时、
又无 `stopped_` 旁路的等待点（对比：map_cv_ :591 有旁路、checkpoint 有
30s、close 第一段有 30s）。

**修复方向**：
1. `submit` 的 `queue_.push` 套 try/catch，catch 内 `dec_in_flight(lane)` 后
   重抛（模式照抄 :565-567 既有补偿）
2. `flush()` 改有界等待，超时上报
3. 修正 `cask.hpp:1019-1023` T14 注释的「可恢复」表述
4. （顺手，同 commit）P6-MEM-2：`row_chunks.hpp:81-93` 用 unique_ptr 暂存再
   release；P6-MEM-3：`segment_v2.cpp:444-448` 把 `new` 提到 mmap 之前

- **位置**：`include/bitcask/thread_pool.hpp:415-443`；`include/bitcask/cask.hpp:1019-1023`；
  `include/bitcask/row_chunks.hpp:81-93`；`src/bm25/segment_v2.cpp:444-448`
- **工作量**：半天
- **验收**：641/641 ctest + TSan 树；建议故障注入（mock push 抛 bad_alloc →
  close 在 30s 超时后仍能返回）
- **备注**：此超时基建同时是 T8 重设计的前置之一
- **优先级**：🔴 极高（消除两条进程级永久挂死路径）

#### ✅ T19 落地记录（2026-07-15）

**两处与原计划的偏离，均为实施中发现的更优/更安全解**：

1. **超时只上在拆卸路径，不做全局**。原计划「flush() 改 wait_for 30s」会
   连带 `Cask::flush_index()`（`cask.hpp:667` → `prepare_search` 的搜索读
   屏障）——谓词里的 `in_flight==0` **兼任索引可见性屏障**，有界化 =
   prepare_search 静默返回未排空的索引 = **漏召**，正是 T8 初版翻车的形状。
   故 `flush(lane, timeout=nullopt)` 默认仍为无界（搜索路径语义不变），
   仅 `unregister_lib` 传 30s。P6-MEM-1 的根因由 submit 的补偿 dec 独立闭合，
   不依赖超时；超时是 P6-DL-1（写线程被 kill）的兜底。
2. **不加 `|| stopped_` 旁路**（原计划有）。核实后否决：`stopped_=true` 到
   `stop()` join 完 map worker 之间存在窗口，此时旁路会让 `unregister_lib`
   提前 erase lane，而在途 map worker 仍持 `task.lane` 裸指针 → **新引入
   UAF**（reducer 有 shared_ptr 拷活保护，map worker 没有）。30s 超时已覆盖
   该场景的活性需求（stopped_ 后 submit 本就早返回、不再累加 in_flight），
   旁路只省一次边缘场景的 30s 等待，不值这个风险。

- **改动**：`thread_pool.hpp`（submit try/catch + dec 重抛；flush 加
  `optional<ms>` 参数；`unregister_lib` 改 `[[nodiscard]] bool`；
  `kUnregisterFlushTimeout` 常量；`<chrono>`）；`cask.cpp:686` 超时 log_error；
  `cask.hpp` T14 注释修正；`row_chunks.hpp` unique_ptr 暂存 + `<memory>`；
  `segment_v2.cpp` new 提到 **open 之前**（提到 mmap 前仍会漏 fd——见下）
- **P6-MEM-3 实施修正**：RISK_REPORT 建议「把 new 提到 mmap 之前」**不充分**
  ——fd 在 :436 已打开、close 在 mmap 之后，new 抛出仍泄漏 fd（只是从泄漏
  映射降级为泄漏 fd）。实际提到 `::open` **之前**才无窗口（new 是本函数唯一
  抛出点，此时无任何 OS 资源在手）。
- **验收**：ASan 641/641 ✅ | TSan 全量 0 告警 ✅（按 CI 门控排除两项既知
  假阳性）| TSan 并发套件 ×3 各 86/86 无挂起 ✅ | Debug/Release/ASan/TSan
  四树编译零新告警 ✅

### T20 — P6-DUR-1：hnsw 三处原子写补 fdatasync 🔴 HIGH

**症状**：全库原子写规范（`keydir.cpp:1565-1567`，「可重建 ≠ 可以不
fdatasync」）9 站点中 6 个遵守；`hnsw.cpp` 三处 FILE* save 路径
（`:2015-2023` save / `:491-538` save_vec_payload / `:564-588`
write_bcq8_file）rename 前**无任何 sync**，且无豁免注释。同文件 fd 增量
路径（:1613/:1636/:1782/:1816）全部 fdatasync——一个文件两套纪律。
后果：崩溃后最终文件名下可能是半截文件，**旧的好文件已被 rename 覆盖**。

**修复方向**：三处在 fclose 前补 `fflush` + `::fdatasync(::fileno(f))`，
失败走既有 remove(tmp) 路径。与 keydir.cpp:1568-1571 逐字同款。

- **位置**：`src/vector/hnsw.cpp:2015-2023, 491-538, 564-588`
- **工作量**：30 分钟
- **验收**：641/641 ctest（hnsw/vector_plugin 套件重点）
- **优先级**：🔴 高（独立持久性修复，不依赖任何重构）

#### ✅ T20 落地记录（2026-07-15）

三处均在 rename 前补 `fflush` + `::fdatasync(::fileno(f))`，失败走既有
remove(tmp) 路径。`<unistd.h>` 原已 include（hnsw.cpp:26），无新依赖。
**比 keydir 原型多一处加固**：`keydir.cpp:1568` 忽略 `fflush` 返回值
（disk-full 时 fflush 失败但 fdatasync 可能对已落盘部分成功 → rename 出
半截文件）；三处新代码均检查 `fflush() == 0 && fdatasync() == 0`。
建议 T21 归并 `AtomicFileWriter` 时把这个加固回灌 keydir。

- **验收**：ASan `Hnsw|Vector|Ivf|Diskann|SegmentV2|Checkpoint` 105/105 ✅
  （零 ASan 报告）| ASan 全量 641/641 ✅

### T21 — P6-RED-1/2：T10 真正收尾——read_file_bytes + AtomicFileWriter 🟡 MED

**症状**：`file_util.hpp` 头注释 :12-13 自承的欠债兑现为三处漂移
（T20 的 fsync 分叉、T23 的 need 公式、T22 的注释断言）。

**修复方向**（`detail/file_util.hpp` 扩展）：
1. `read_file_bytes`：把 `migrate.cpp:45-59` 现成的 `read_all` 搬进
   file_util.hpp,`template <class Byte>` 吸收 uint8_t/byte 分叉;迁移 5 站点
   （hnsw.cpp:2031 / keydir.cpp:1588 / inverted.cpp:1272 / segment_v2.cpp:954 /
   search_checkpoint.hpp:326）,尺寸谓词由调用方查 `.size()` 保留
2. `atomic_write_bytes(path, span)`：迁移 4 份 buffer 式（keydir /
   search_checkpoint / index_manifest（保留其目录 fsync 增强）/ segment_v2:942）
3. `AtomicFileWriter` RAII：迁移 5 份流式（field_schema / segment_v2:376 /
   hnsw ×3——T20 先行后此处是纯收编）

- **工作量**：1 天
- **验收**：641/641 ctest;sync 纪律从 9 处可审收敛为 1 处可审
- **依赖**：T20 先行(持久性修复不等重构)
- **优先级**：🟡 高（结构性防 P6-DUR-1 类漂移复发,~100 行回收）

#### ✅ T21 落地记录（2026-07-15）

**归并前实测四套 fsync 纪律**（比 RISK_REPORT 记的三套还多一层）：
① keydir/search_checkpoint 检查 fdatasync 但丢弃 fflush 返回值；
② **`index_manifest.hpp:176` 连 fdatasync 返回值都丢弃**（`if (wrote) ::fdatasync(...)`）；
③ field_schema 用 `::fsync`；④ hnsw ×3 完全不 sync（T20 已修）。
仅 segment_v2 两处两个返回值都检查。

**新增基建**（`detail/file_util.hpp`，33 → 175 行，header-only inline，
沿用 index_manifest 既有风格，不动 CMakeLists）：
- `read_file_bytes<Byte>(path)` → `optional<vector<Byte>>`；`Byte` 模板化
  （keydir/hnsw 的 deserialize 吃 uint8_t，其余吃 byte，统一类型反而逼出
  更多 cast）；**尺寸谓词留给调用方**查 `.size()`（各站点门槛互不相同）
- `atomic_write_bytes(path, span, fsync_dir=false)` — buffer 式 ×4
- `AtomicFileWriter` RAII — 流式 ×3（tmp 后缀可定制，保住 `.upgrade.tmp`
  的诊断价值：残留文件名一眼指认是 schema 升级路径崩的）
- `flush_and_sync()` / `fsync_parent_dir()`

**统一后的纪律**：fflush 与 fdatasync **两个返回值都检查**（disk-full 下
fflush 失败而 fdatasync 对已落盘部分成功 → 静默 rename 出半截文件）。
field_schema 的 `::fsync` → `fdatasync`：新文件的尺寸元数据属「取回数据
所必需」，fdatasync 同样保证，差别仅 mtime（无人依赖）。

**刻意不做的**：目录 fsync 未全面铺开，`fsync_dir` 默认 false，仅
`write_manifest`（唯一 commit 点）传 true——保持 T21 为**纯重构**，
不夹带性能/行为变更。是否铺开属 Phase 7「目录 fsync 专项」。

**迁移站点**：整读 ×6（hnsw.cpp:2031 / keydir.cpp:1588 / inverted.cpp:1272 /
segment_v2.cpp:954 / search_checkpoint.hpp:326 / migrate.cpp 的 read_all 由
原型降为 expected 语义的薄包装）；原子写 ×9（keydir / search_checkpoint /
index_manifest / segment_v2 ×2 / field_schema / hnsw ×3）。
`index_manifest::fsync_directory_of` 随 write_manifest 归并后零残留，已删。
`uint8_t` 缓冲区经 `std::as_bytes(std::span(buf))` 转换——C++20 惯用法，
**未新增 reinterpret_cast**。

- **验收**：ASan 全量 641/641 ✅ | 落盘/载入路径套件 219/219 + **零 .tmp
  残留** ✅（AtomicFileWriter 失败路径新契约）| TSan 639/639 零告警 ✅ |
  Debug/ASan/TSan/Release 四树编译，改动文件零新告警 ✅
  （Release 的 ftruncate/unused-param/memaccess 告警经日志对比确认为预存）

### T22 — P6-RED-4：Analyzer 双出口归并 ×2 组 🟡 MED

- **4b（先做,1 小时)**：`WhitespaceAnalyzer` 照抄 JiebaAnalyzer 既有先例
  （`jieba_analyzer.cpp:126` `collect_tokens(text, need_offsets)`）,
  `analyzer.cpp:356-398` 与 `:400-443` 前 39 行完全相同,仅 4 行 sink 差异。
- **4a（半天)**：`NgramAnalyzer::analyze_with_positions`（:211-285）与
  `analyze`（:292-350）——S29-8 注释断言两版「term 集与 tf 值逐位一致」,
  该不变量是索引路径与 BOW 查询路径的一致性前提,目前靠复制粘贴维护。
  `template <class Sink>` + `if constexpr` 归并,保住 tf 版零 positions
  分配特性。**验收须含 term/tf 逐位一致对拍测试**（把注释断言变成测试断言）。

- **位置**：`src/text/analyzer.cpp`
- **工作量**：合计 1 天内
- **验收**：641/641 ctest + text 套件重点回归 + 4a 的对拍测试
- **优先级**：🟡 中（4a 是"等着被踩的雷"——过滤语义单边修改即静默分叉）

#### ✅ T22 落地记录（2026-07-15）

**4b（WhitespaceAnalyzer）**：抽 `whitespace_tokenize(cps, normalized,
min_len, max_bytes, sink)`，两个出口各剩 ~8 行。**未照搬 Jieba
collect_tokens 的物化 token 向量**——直接回调省一次中间分配（Jieba 那样做
可接受是因为词数少；见 4a 的理由）。

**4a（NgramAnalyzer）**：抽 `ngram_collect`（含**全部过滤语义**）+
`materialize_and_filter`（含物化与停用词）。tf 版 sink 忽略 pos 形参 →
**零 positions 分配，S29-8 的性能取舍完整保留**。
**明确否决 Jieba 先例**：物化 token 向量会抵消 S29-8 的全部收益——一篇
CJK 文档数千个 n-gram，正是该覆写存在的理由。

**对拍测试**（`tests/analyzer_test.cpp`，+3 测试 ~95 行）：把 S29-8 注释里
「term 集与 tf 值逐位一致」的断言变成可执行断言——覆盖 ngram_tokenize 每条
分支（CJK/拉丁/空白/CJK 标点/ASCII 标点/混排/重复/单字/纯标点/空输入）、
停用词、以及 min_token_length × max_token_bytes 的 3×3 参数矩阵。
额外断言 `tf == positions.size()`（positions 版自身一致性）。

**变异测试验证测试有效性**（"在正确代码上通过"不等于"抓得住回归"）：
注入三种单边分叉——① tf 版多一条过滤、② positions 版多一条过滤、
③ tf 版 min_token_length 门槛 off-by-one——**新对拍三种全抓**。
诚实记录：三种变异**旧测试也各抓到 1 个**，故新测试的增量不是"从 0 到 1"，
而是①直接定位分叉点（旧测试只报某个具体值不符）②覆盖旧测试没有的 9 组
参数矩阵③即使将来有人重新拆开两版也立刻失败。变异残留已确认清零。

- **验收**：ASan **644/644** ✅（641 基线 + 3 新测试）| TSan 642/642 零告警 ✅
  | Debug/ASan/TSan/Release 四树编译，analyzer 零新告警 ✅

### T23 — P6-RED-3：ChunkedReader 归并 refill ×3 🟡 MED

`hint_file.cpp:143-173 / :244-267`、`data_file.cpp:309-335` 三份 refill,
注释自承抄袭,`need` 公式已漂移（data_file.cpp:322 掉了 `buf_len +`,
当前无害但证明分别维护）。归并为 `detail::ChunkedReader{file_, end_bound}`,
唯一参数化点是文件末界（total vs body_end）。

- **工作量**：半天
- **验收**：641/641 ctest（fold/恢复路径套件重点）
- **优先级**：🟡 中（~55 行,冷启动路径无性能顾虑）

### T24 — P6-RED-5：decode_rec 共享解包段模板归并 🟢 LOW

`segment_v2.cpp:619-653` vs `:1007-1041` 逐字节相同。`template <class Out>`
（PostingList 是 FlatPostings 结构超集）+ `if constexpr (requires { out.dls; })`
保住热路径 dl 跳过,单态化零开销。真分叉部分（blocks 重建 vs dls+positions）
留在 helper 外。

- **工作量**：半天
- **验收**：641/641 ctest + **bench 基准回归**（decode_rec 在 WAND/bool/
  wildcard 热路径,:809 等 6 调用点）;须 build-rel 双树验证（见 memory:
  build-rel 编 bench 能抓 Debug 套件漏掉的问题）
- **优先级**：🟢 低（重复确凿但有基准门槛）

### T25 — P6-RED-6：死代码 7 行删除 🟢 LOW

- `porter_stemmer.hpp:52-55` `detail::ends_with_vowel`（4 行,全树仅定义）
- `keydir.hpp:585` + `keydir.cpp:923` `newest_folder_epoch_`（2 行,仅写零读;
  删除前确认:疑为漏掉的 IterInfo 导出字段,若是补导出而非删除——问一下
  设计意图或查 S 系列注释）
- `codec.hpp:34` `kValueSizeOverflow`（1 行,构造不可达）

- **工作量**：10 分钟（+ newest_folder_epoch_ 的 5 分钟考据）
- **验收**：全量编译 + ctest

### T26 — 文档收尾 🟢 LOW

- RISK_REPORT.md 基线修正 ✅（随本次 Phase 6 更新已完成:reinterpret_cast
  假阴性 ×2 处、RED-1 行数、方法学备注）
- TASK.md T14 表述修正 ✅（本文件归档表已注明）
- 剩余：`cask.hpp:1019-1023` 注释修正 → 归入 T19 第 3 步

---

## Phase 6 执行序

```
T20 (30min) ───── 最先,独立 commit（持久性修复,不等任何重构）
T19 (半天)  ───── 独立 commit（须 TSan 回归;含 P6-MEM-2/3 顺手项 + T14 注释修正）
T25 (10min) ───── 随手,可并入任意 commit
T21 (1天)   ───── T20 之后（hnsw 三处变纯收编）
T22 (1天)   ───── 4b 先行 1 小时,4a 须对拍测试
T23 (半天)  ───── 独立
T24 (半天)  ───── 最后（须 bench 基准 + build-rel 双树）
```

---

## ⏸ 遗留任务（Phase 6 明确不做,前置条件未满足）

### T8 — DL-MED-3：搜索读屏障无界等待（⚠️ 暂缓,前置未满足）

`prepare_search()` → `flush()` 谓词 `in_flight==0 && applied>=hwm`,hwm
**活读**无自然终止边界;饱和写入下为**饥饿**（非死锁,写方停手即恢复）。
现有 3 个持续写+并发查询测试全过（写者磁盘 IO 主导,天然慢于索引排空,
触发需索引侧成为瓶颈）。初版 snapshot-hwm 方案曾致 4 个非并发测试漏召
（测试名未留档,改动未进 git）——说明 `applied>=hwm` **不蕴含可搜**,
`in_flight==0` 兼任可见性屏障,放宽谓词的前提本身待证。

**重启前置（缺一不可）**：
1. 可复现饥饿的失败注入测试（多写线程/大文档/重 analyzer 让索引侧成瓶颈）
2. applied_ord 推进与搜索可见性精确关系调查（DWPT builder 发布时序）
3. T19 的 flush 超时基建（已排入本轮）
4. 若恢复 flush_upto,须**连同** reducer 每任务通知块一起恢复（T16 删除后
   flush_cv_ 仅剩 dec_in_flight 1→0 单点 notify,光恢复方法体谓词无人唤醒）

### T12 — RED-1：HNSW ckpt 去重（独立分支既定）

重复已精确核实 ~115 行（flush 35 逐字节 + delta 31 + load ~50）。
非模板 `VectorCkptDriver` 基类**无技术障碍**（SealedT 不进入共享方法,
10 个共享成员全为具体类型;replay_gate_ 留派生类）。
**默认不做**——115 行换 1 天 + 磁盘格式兼容面风险,不划算,且不修任何 bug。
**替代动作（5 分钟,建议随 T20 顺手做）**：把三门决策的理由注释
（S32-M1/S20-3 B-B2）从 vector_plugin.cpp 同步到 sealed 侧,双向标注
「改此处须同步另一处」——拿走大部分漂移风险,零测试成本。
若未来真做：独立分支 + HNSW/IVF/DiskANN 三引擎测试全过。

### Backlog（随后续重构自然消化,Phase 6 复核全部仍成立）

RED-3（三份 LE 编解码,byte_order.hpp 补 vector-append 形态）、
RED-5（search_layer f32/int8 成对,须基准）、RED-6（IVF/DiskANN open 骨架）、
RED-10（SnapCursor::vb,价值低）。

---

## 当前状态快照（Phase 6 起点）

| 项 | 状态 |
|---|---|
| T1-T18 | ✅ 归档（见上表;T14 表述已修正） |
| T19 flush 有界等待 + submit 补偿 | ✅ done（含 P6-MEM-2/3;两处计划偏离见落地记录） |
| T20 hnsw fdatasync ×3 | ✅ done（ASan hnsw/vector/segment 105/105） |
| T21 read_file_bytes + AtomicFileWriter | ✅ done（整读 ×6 + 原子写 ×9 归并;四套 fsync 纪律收敛为一） |
| T22 Analyzer 双出口 ×2 | ✅ done（4a+4b 归并;+3 对拍测试,经变异测试验证有效） |
| T23 ChunkedReader | 🟡 待做 |
| T24 decode_rec 模板 | 🟢 待做 |
| T25 死代码 7 行 | ✅ done（ends_with_vowel / newest_folder_epoch_ / kValueSizeOverflow） |
| T26 文档收尾 | 🟢 大部分已随本次更新完成 |
| T8 | ⏸ 暂缓（4 项前置见上） |
| T12 | ⏸ 默认不做（注释同步替代） |

---

## 下一轮审计目标（Phase 7 候选,继承自 Phase 5 提出的协议盲区）

1. **🔴 Tombstone/Merge 语义正确性**（basho #82 删除复活类、#149/174/175
   merge 竞态）——仍是最大盲区
2. **🟠 fsync/fdatasync 纪律审计**——P6-DUR-1 已提前兑现一部分（原子写
   站点清点完毕）;剩余:WAL put 路径持久性、backup 一致点、**目录 fsync**
   （9 站点中仅 index_manifest 做了,rename 的目录项持久性普遍缺失,值得专项）
3. **🟠 Lock 文件健壮性**（空文件、PID 复用、disk-full 失败传播）
4. **🟡 CRC 回退路径完整性**（hint 失败回退扫描时是否做 CRC 校验）
