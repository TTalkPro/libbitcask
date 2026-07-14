# Google C++ Style 规范化修复任务清单

> 来源：`RISK_REPORT.md`（2026-07-14 v2 深度审计 + Phase 5 审计 + 三次复核）
> 范围：5 项 Phase 1-3 行动建议 + 7 项 Phase 4 资源/并发修复 + 5 项 Phase 5 复核确认任务
> 基线测试：641/641 ctest 通过（1 个 S30RssProbe 预存 Disabled）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警 + lsp_diagnostics 无新错误

---

## ✅ Phase 1：低风险快速修复（已完成）

### T1 — CRC32 入口统一（MED-3） ✅

把 3 个文件直接调 `hw::crc32*` 的 10 处改为 `codec::crc32*`，让硬件派发藏在 codec 内部。

- ✅ `src/bm25/segment_v2.cpp`（6 处）
- ✅ `src/cask/meta_file.cpp`（1 处）
- ✅ `include/bitcask/field_schema.hpp`（3 处）
- ✅ 641/641 ctest 通过

### T2 — vbyte 带检查版本归并（MED-2） ✅

三份带边界检查的 vbyte 解码归并到 `vbyte.hpp::vbyte_read_checked`
（`std::optional<pair<u64, size_t>>` 返回，无歧义）。无检查版 `vbyte_decode`
保持独立（热路径契约，S21-2 A3 既定）。

- ✅ `vbyte.hpp` 增加 `vbyte_read_checked` 权威实现
- ✅ `codec.cpp` 删本地 vbyte_read，5 处调用迁移
- ✅ `meta_codec.hpp` 删 detail::vbyte_read + 内联 lambda，6 处调用迁移
- ✅ 641/641 ctest 通过

---

## ✅ Phase 2：基础设施（已完成）

### T3 — `.clang-tidy` 配置 ✅

- ✅ `.clang-tidy`（google-* + modernize/bugprone/performance/readability/cert/cppcoreguidelines 基线，关闭与项目风格冲突的 ~15 项）
- ✅ `.github/workflows/ci.yml` 加 `clang-tidy` job（continue-on-error，待存量清零改门控）
- ✅ 本地 clang-tidy 21 验证：codec.cpp 从数百告警降到 7 条真问题

### T4 — `MmapRegion` RAII 基础设施 ✅（渐进式应用）

- ✅ `include/bitcask/detail/mmap_handle.hpp` 创建（MmapRegion 类 + 析构序安全文档）
- ⏩ 全量应用到 8 处 mmap 站点改为**渐进式**——每处需成员布局重写（30+ 访问点/文件），
  风险/收益不划算。后续随其他重构（如 T5、引擎迭代）自然推广。

---

## ✅ Phase 3：高风险高价值（已完成）

### T5 — `SealedSegmentVectorPlugin` 抽取（HIGH-1） ✅

IVF/DiskANN 插件 ~950 行近乎完全重复。抽 Template Method 中间基类，
子类只 override 差异点（search_sealed / ckpt_name / blob_size 等钩子）。

- ✅ `include/bitcask/detail/sealed_segment_vector_plugin.hpp` 创建
- ✅ `ivf_plugin.cpp` / `diskann_plugin.cpp` 收缩
- ✅ commit `ba99af2`
- ⚠️ HNSW 侧同构代码未回收 → 转入 T8（RED-1）

---

## 🔴 Phase 4：资源管理与并发修复（来自 RISK_REPORT v2 深度审计）

> 来源：3 路深读 agent（内存泄露 / 死锁 / 冗余），~200 次工具调用，
> 异常路径上确认 1 中危逻辑缺陷 + 3 中危死锁 + 3 低危内存。
> 主 Agent 复核裁决：**3 项升档为 HIGH**（MEM-MED-1、DL-MED-1、DL-MED-2）。

### T6 — MEM-MED-1：Registry acquire/release 不配对修复 🔴 HIGH

**症状**：`Cask::open` 在 `registry->acquire()` **之前**就设置了 `registry_`/`keydir_name_`；
kNotReady 失败路径经 `~Cask→close()` 无条件 `release()`，把初始化方的 refcount
从 1 减到 0 并 erase 槽位 → `biggest_file_id` 持久化被跳过 → 老文件 ID 复用
→ keydir 把旧 entry 误判为最新（**tombstone-resurrection 等价类**，basho/bitcask #82）。

**修复方向**：只在 acquire 成功（kReady/kCreated）后才把 `registry_`/`keydir_name_` 赋给 `cask`。

- **位置**：`src/cask/cask.cpp:220-244`
- **工作量**：1 小时
- **验收**：新增并发 open + 慢初始化测试（注入 sleep 模拟大库冷启动）→ 641+/ctest 通过
- **优先级**：🔴 极高（唯一常态可达的逻辑缺陷）

### T7 — DL-MED-1 + DL-MED-2：RunFn 路径 ord 泄漏 + 无超时等待 🔴 HIGH

**症状（双根同终态）**：
- DL-MED-1：四个 RunFn 提交点（手动 ckpt / 自动 ckpt / merge 收尾 / run_serialized）
  在 `alloc_ord()` 与 `submit()` 之间有可抛分配（snapshot vector、serialize_meta_delta、
  std::function 构造），任一抛 bad_alloc 即泄漏该 ord → reducer lane 永久空洞 →
  `flush()`/`close()`/`~Cask` 永久挂死。
- DL-MED-2：`checkpoint()` 的 `done->wait(0)` 无超时；`IndexPool::submit` 在 `stopped_`
  时静默丢弃任务，done 永不置位 → 持 `ckpt_mu_` 级联锁死。

**修复方向**：
1. 四个 RunFn 提交点套 `OrdSkipGuard`（沿用写路径既有模式）
2. `done->wait` 加 30s 超时 + 日志兜底
3. `IndexPool::submit` 在 `stopped_` 丢弃时返回错误或主动置位 done（最小入侵：检测后 log + 让调用方自行超时恢复）

- **位置**：`src/cask/cask.cpp:493-495, 2201-2248, 2414-2437, 2519-2533`；
  `include/bitcask/thread_pool.hpp:415-432`
- **工作量**：半天
- **验收**：故障注入测试（bad_alloc mock、stopped_ pool）→ ctest 通过
- **优先级**：🔴 高（消除进程级不可恢复挂死根因）

### T8 — DL-MED-3：搜索读屏障无界等待 🟡 MED ⚠️ reverted

**症状**：`prepare_search()` 调用 `flush_index()`，其谓词要求整条索引流水线排空
（`in_flight==0 && applied>=hwm`），持续写入下查询线程无界等待。

**初版修复尝试（已 revert）**：在 `IndexPool` 加 `flush_upto(lane, snapshot_hwm)` 方法，
谓词改为 `applied_ord >= snap_hwm`（不再要求 in_flight==0、不动态重读 hwm）。
**问题**：在 4 个非并发测试场景下产生搜索漏召（search 返回 0 hits 而非 1），
根因待查——可能涉及 DWPT builder 模式与 reducer apply 之间的发布时序、
或 applied_ord 推进与实际索引可见性之间的微妙错位。
thread_pool.hpp 保留 `flush_upto` 工具方法（已验证单独工作正常），
后续重新设计时复用。

**待重新设计的方向**：
- 调查 applied_ord 与搜索可见性的精确关系（是否 reducer apply 后还需 drain）
- 考虑带超时但保留 in_flight==0 谓词（仅放宽 hwm 动态重读）
- 失败注入测试覆盖持续写 + 查询并发场景

- **位置**：`src/cask/cask.cpp:1010-1016`；`include/bitcask/thread_pool.hpp:435-443`（原代码）；
  `:445-454`（保留的 flush_upto 方法）
- **工作量**：半天（须含失败注入测试设计）
- **验收**：641+/ctest 通过 + 新增持续写 + 并发查询测试
- **优先级**：🟡 高（高写入负载下搜索停顿）

### T9 — RED-4/8/11：死代码清理 🟢 LOW

- **RED-4**：`c_api/internal.h:239-286` `fill_get_result`（零调用，view 版取代）
- **RED-8**：`include/bitcask/text_utils.hpp:143-155` `to_codepoints_reuse`（零调用）
- **RED-11**：`include/bitcask/meta_codec.hpp:80-87` `detail::vbyte_append`（零调用）

- **工作量**：2 小时（合计 ~71 行删除）
- **验收**：全量编译 + ctest 通过

### T10 — RED-2/7 + MEM-LOW-1：`detail/file_util.hpp` 公共归宿 🟡 MED

**症状**：FileCloser 定义 9 份 + fread 整读样板 6 份 + tmp+fsync+rename 原子写 ~7 处；
`field_schema.hpp` 的裸 FILE* 在 bad_alloc 路径泄漏（MEM-LOW-1）正因未消费同一基建。

**修复方向**：新建 `include/bitcask/detail/file_util.hpp`：
- `FilePtr` = `unique_ptr<FILE, FileCloser>`（统一 closer）
- `read_file_bytes(path)` 替代 6 份样板
- `atomic_write_bytes(path, bytes)` 替代 ~7 处 tmp+fsync+rename
- 顺带闭合 MEM-LOW-1（field_schema.hpp 改用 FilePtr）

- **位置**：新建 `detail/file_util.hpp`；改 `field_schema.hpp` / `hnsw.cpp` / `keydir.cpp` / `inverted.cpp` / `segment_v2.cpp` 等
- **工作量**：半天
- **验收**：ctest 通过；ASan 下 field_schema 异常路径无 fd 泄漏
- **优先级**：🟡 中

### T11 — RED-9：MmapRegion 推广或删除 🟡 MED

T4 建了 RAII 基建但 8 处 mmap 站点零采用。

**决策**：
- 若 T10 期间发现只读 mmap 站点（ivf/diskann/segment_v2）易迁移 → 推广
- 否则删除待用时再建（保留为零引用代码是漂移温床）

- **位置**：`include/bitcask/detail/mmap_handle.hpp`
- **工作量**：5 分钟（删）或半天（推广）
- **验收**：0 引用确认 / 改造站点 ctest 通过

### T12 — RED-1：HNSW ckpt 去重（T5 收尾）🟡 MED（已精确审计，待独立分支）

T5 抽取了 IVF/DiskANN 但 HNSW 侧同构代码未回收。**已精确核实重复范围**：
- `VectorPlugin::flush`（`src/search/vector_plugin.cpp:511-555`，~45 行）
  与 `SealedSegmentVectorPlugin<SealedT>::flush`
  （`include/bitcask/detail/sealed_segment_vector_plugin.hpp:672-709`）
  **逐字节相同**——`cap_hit` / `window_hit` / `want_base` 三门决策 + dirty/delta
  跳过 + delta 落盘 + 链回执整段结构同构。
- save_component_delta（~40 行）高度相似（kDeltaInfo + 段型 + chain 三元组）。
- load_component / save_component_base 结构同构但段类型不同（HNSW 走 base+payload，
  sealed 走 base+sidecar）。

**约束**：`sealed_segment_vector_plugin.hpp:33-36` 头注释明确 HNSW 不应继承
SealedSegmentVectorPlugin——HNSW 无 sealed/window 双路径（HNSW 即是 window 本身）。
正确去重方向：新建**非模板** `VectorCkptDriver` 基类（持 chain_/config_/
vec_docs_since_base_/rebase_needed_/delta_ 共享状态 + final flush() 方法），
HNSW VectorPlugin 与 SealedSegmentVectorPlugin<SealedT> 均继承之，派生类只
override 真正差异化的 save/load hook（base 写法、delta 序列化、load 段类型）。

**风险与门槛**：
- 涉及 VectorPlugin / IvfPlugin / DiskannPlugin 三者的继承链改造
- 必须 HNSW + IVF + DiskANN 三引擎测试全过（参考 T5 commit `ba99af2` 工作量）
- 预估 1 天工作量

**为何不在本分支完成**：T8 教训显示无充分失败注入测试覆盖时的重构风险高，
且向量子系统为代码库最复杂部分（~5000+ 行）。本任务应独立分支、独立深度 agent、
独立测试周期完成。

- **位置**：`src/search/vector_plugin.cpp` + `include/bitcask/vector_plugin.hpp`；
  新基类建议放 `include/bitcask/detail/vector_ckpt_driver.hpp`
- **工作量**：1 天（须向量三引擎测试全过）
- **验收**：HNSW + IVF + DiskANN 测试套件全过；flush/save_delta/load 重复行归零
- **优先级**：🟡 中（已确认重复，但去重非紧迫——current code 641/641 干净运行）

### T13 — DL 陷阱 6：插件契约文档固化 🟢 LOW

`plugin_api.hpp` 明文禁止在 reducer 上下文（on_put/on_delete/RunFn 内）调用
`PluginHost::run_serialized`——当前不可达，但缺乏编译期/文档约束。

- **位置**：`include/bitcask/plugin_api.hpp`
- **工作量**：10 分钟
- **验收**：注释 + 断言（debug 模式检测调用者线程上下文）

---

## 🔴 Phase 5：复核确认的修复任务（2026-07-14 三次核对后定稿）

> 来源：Phase 5 审计（4 路并行）+ 主 Agent 逐条重读代码复核。
> 复核修订要点：P5-MEM-1 使用点 7 处非 4 处且模式源自 S13-F2；
> P5-MEM-2 新增第三处漏点；P5-DL-3 升档（死代码在热路径留有真实开销）。

### T14 — P5-MEM-1：OrdSkipGuard 析构异常安全 🔴 HIGH ✅

**症状**：`~OrdSkipGuard()`（`include/bitcask/cask.hpp:1007-1012`）隐式 noexcept，
内部调用链 `submit_index_task → IndexPool::submit → queue_.push`
（`thread_pool.hpp:431`，TBB 有界队列内部分配可抛 bad_alloc）无一处 noexcept。
submit 抛出 → 栈回退中析构再抛 → `std::terminate()`（进程立即死亡，不可恢复）。

**范围（复核修订）**：**7 个使用点**共享同一析构——
`src/cask/cask.cpp:505 / 1060 / 1801 / 1877 / 2235 / 2469 / 2575`。
模式自 S13-F2 起即存在（1060/1801/1877），T7 新增 4 处（505/2235/2469/2575）。

**修复方向**：析构内
`try { cask->submit_index_task(...); } catch (...) { /* 记录 ord 泄漏,不抛 */ }`。
代价：极端 bad_alloc 下泄漏 1 个 ord（可恢复，触发 30s 超时路径）；
收益：消除 terminate（不可恢复）。一处修改覆盖全部 7 个使用点。

- **位置**：`include/bitcask/cask.hpp:1007-1012`
- **工作量**：30 分钟
- **验收**：641/641 ctest；建议补 log_warn 记录兜底触发

### T15 — P5-MEM-2：checkpoint 三处漏更新 last_ckpt_ord_ 🟡 MED ✅

**症状**：`last_ckpt_ord_` 唯一消费点是自动 ckpt 阈值判断（`cask.cpp:2456`）。
三处成功保存 ckpt 却不推进水位 → 下次写入触发冗余 ckpt（重复劳动+日志噪音）：

1. `cask.cpp:2215-2224` is_stopped 同步分支——注释声称 "synchronous path sets
   last_ckpt_ord itself"，**与代码直接矛盾**
2. `cask.cpp:2311-2321` 无索引池分支（理论不可达，同样漏）——复核新增
3. `cask.cpp:2580-2601` merge 的 RunFn 与 else 两分支——merge 后 rebase 全量
   ckpt 已落盘却不推进水位，紧随的写入立刻触发一次冗余自动 ckpt

**修复方向**：三处成功路径后补
`last_ckpt_ord_.store(<对应 wm>, std::memory_order_relaxed);`；
同步分支的矛盾注释一并修正。

- **工作量**：15 分钟
- **验收**：641/641 ctest；merge 后自动 ckpt 不再立即触发（可加计数断言）

### T16 — P5-DL-3：删除 flush_upto + reducer 每任务通知块 🟡 MED（复核升档）✅

**症状**：`flush_upto`（`thread_pool.hpp:447`）T8 revert 后零调用；
`reducer_loop:653-656` 每 apply 一个索引事件取一次全局 `flush_mu_` + `notify_all`，
注释明示专为 flush_upto 服务——即死代码在 reducer 热路径留有**每任务全局锁开销**。
常规 `flush()` 的唤醒由 `dec_in_flight` 归零通知完全覆盖
（applied_ord 在 dec 之前 store，无丢失唤醒窗口）。

**修复方向**：删 flush_upto 定义 + 删 :653-656 通知块。
**前置确认**：unregister_lib 等待路径不依赖 per-apply 通知（只依赖 in_flight 归零）。
若 T8 重设计仍需 flush_upto，届时从 git 历史恢复。

- **工作量**：1 小时（含前置确认 + 并发测试回归）
- **验收**：641/641 ctest + TSan 树通过；merge_concurrent_writer_test 等并发套件重点回归

### T17 — NEW-P4-1 + RED-7残：T10 收尾 🟢 LOW ✅

- `field_schema.hpp:73/230` 本地别名 ReadFilePtr/WriteFilePtr → 直接用
  `detail::FilePtr`（file_util.hpp:31 已存在）
- `hnsw.cpp:1525` 本地 `pwrite_all` → 消费 `vec_disk_internal.hpp:25`
  的 `diskint::pwrite_all`（同目录内部头，5 个调用点）

- **工作量**：30 分钟
- **验收**：641/641 ctest

### T18 — P5-DL-1/2：thread-safety 文档化模式偏离 🟢 LOW ✅

- P5-DL-1：checkpoint() 在 **ckpt_mu_ 临界区内**（函数级 lock_guard，`cask.cpp:2203`）
  做 30s 有界 cv 等待，WriteOpGate 同时持有——reducer 卡住时 close() 与后续
  checkpoint 调用者最坏拖 30s。非死锁（done_mu 为 per-call 局部 + 30s 上界），
  但须在 docs/design/thread-safety.md 记录此例外及其安全论证
- P5-DL-2：reducer_loop 连续两次取 flush_mu_（notify + dec_in_flight）偏离
  "任一时刻至多持一把锁"文档模式——T16 删除通知块后自然消失，届时只需
  文档化 dec_in_flight 单点通知

- **工作量**：30 分钟（若 T16 先行，P5-DL-2 部分免除）
- **验收**：文档更新，无代码变更（或随 T16 合并）

### Phase 5 执行序

```
T14 (30min) ──┐
T15 (15min) ──┼── 可同一 commit（均为 cask 行级修改，互不冲突）
              │
T16 (1h)    ──┴── 独立 commit（须 TSan 回归），完成后 T18 的 DL-2 部分免除
T17 (30min) ───── 独立小 commit
T18 (30min) ───── 文档 commit（T16 之后做）
```

**Phase 5 不做**（转入 backlog）：RED-1（T12 独立分支既定）、RED-3/5/6/10
（冗余去重非 bug，随后续重构自然消化）、T8 重设计（须失败注入测试先行）。

---

## Phase 4 执行序与依赖图

```
并行启动（互不依赖）：
  ├── Track A: T6  (MEM-MED-1，1h，主 Agent 直接修，最小补丁)
  ├── Track B: T9  (死代码清理，2h，主 Agent 直接修)
  └── Track C: T7  (DL-MED-1/2，半天，委派 deep agent)

T7 完成后：
  └── T8  (DL-MED-3，半天，依赖 T7 的 timeout 基建)

独立但需配合：
  ├── T10 + T11 (file_util.hpp + MmapRegion 推广/删，合并执行半天)
  └── T13 (10 min 注释加固)

最后（独立分支）：
  └── T12 (HNSW 去重，须三引擎测试全过)
```

---

## 当前状态快照

| 项 | 状态 | 验证 |
|---|---|---|
| T1 CRC32 入口统一 | ✅ done | 641/641 ctest |
| T2 vbyte 归并 | ✅ done | 641/641 ctest |
| T3 .clang-tidy | ✅ done | 本地 clang-tidy 21 验证 |
| T4 MmapRegion 基础设施 | ✅ done | 类已创建，渐进式应用 |
| T5 SealedSegmentVectorPlugin | ✅ done | commit ba99af2 |
| T6 MEM-MED-1 修复 | ✅ done | 641/641 ctest（cask.cpp acquire 后置位） |
| T7 DL-MED-1/2 修复 | ✅ done | 641/641 ctest（OrdSkipGuard ×4 + cv+mutex 有界等待 + is_stopped 同步回退） |
| T9 死代码清理 | ✅ done | 641/641 ctest（fill_get_result / to_codepoints_reuse / vbyte_append） |
| T13 插件契约文档 | ✅ done | plugin_api.hpp run_serialized 死锁陷阱注释 |
| MEM-LOW-1 field_schema FILE* | ✅ done | 641/641 ctest（裸 FILE* → FilePtr RAII） |
| T8 DL-MED-3 修复 | ⚠️ revert | 初版 snapshot-hwm 方案在非并发场景下产生搜索漏召（4 测失败）；thread_pool.hpp 保留 flush_upto 工具方法供后续重新设计使用 |
| T10 file_util.hpp | ✅ done | 641/641 ctest（9 FileCloser → 1 detail/file_util.hpp；含 field_schema MEM-LOW-1 闭合） |
| T11 MmapRegion 决策 | ✅ done | 641/641 ctest（删除建而未用的 mmap_handle.hpp，71 行；可从 git 历史恢复） |
| T12 HNSW ckpt 去重 | ⚠️ 已精确审计 | 已验证 VectorPlugin::flush 与 SealedSegmentVectorPlugin::flush 逐字节相同；待独立分支执行（须向量三引擎测试全过 + 新建 VectorCkptDriver 非模板基类） |
| T14 OrdSkipGuard 析构异常安全 | ✅ done | cask.hpp 析构 try-catch + log_warn 兜底；ASan smoke/checkpoint/crash/merge_concurrent 全过（一处修复覆盖 7 使用点） |
| T15 last_ckpt_ord_ 三处漏更新 | ✅ done | 三处补 store（is_stopped/无池/merge 两分支）+ 修矛盾注释；确认纯 KV 早返回无需（auto ckpt 仅 text_ 模式）；ASan checkpoint/merge/keydir/cask_docvalue 全过 |
| T16 flush_upto + 每任务通知块删除 | ✅ done | 删 flush_upto + reducer:653-656 通知块；前置确认 flush()/unregister_lib 只依赖 dec_in_flight 归零通知；ASan+TSan 全过 + TSan ×5 无挂起 |
| T17 T10 收尾（FilePtr 别名 + pwrite_all） | ✅ done | field_schema 用 detail::FilePtr；hnsw 用 diskint::pwrite_all（含 vec_disk_internal.hpp）；ASan hnsw(20)/vector_plugin/cask_docvalue/text_plugin/smoke 全过 |
| T18 thread-safety 文档化 | ✅ done | doc/concurrency-zh.md 补 P5-DL-1（ckpt_mu_ 跨 30s 有界等待）+ P5-DL-2（随 T16 消解）；无代码改动 |

---

## 下一轮审计目标（来自主 Agent 综合分析的盲区）

报告聚焦资源管理，但 **Bitcask 协议正确性维度存在系统性盲区**——
basho/bitcask 生产史上最严重 bug 的来源。建议 Phase 5 专项审计：

1. **🔴 Tombstone/Merge 语义正确性**（basho #82 删除复活类、#149/174/175 merge 竞态）
2. **🟠 fsync/fdatasync 纪律审计**（WAL 持久性、meta/ckpt 原子性、backup 一致点）
3. **🟠 Lock 文件健壮性**（空文件、PID 复用、disk-full 失败传播）
4. **🟡 CRC 回退路径完整性**（hint 失败回退扫描时是否做 CRC 校验）
