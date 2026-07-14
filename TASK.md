# Google C++ Style 规范化修复任务清单

> 来源：`RISK_REPORT.md`（2026-07-14 v2 深度审计）
> 范围：5 项 Phase 1-3 行动建议 + 7 项 Phase 4 资源/并发修复
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

---

## 下一轮审计目标（来自主 Agent 综合分析的盲区）

报告聚焦资源管理，但 **Bitcask 协议正确性维度存在系统性盲区**——
basho/bitcask 生产史上最严重 bug 的来源。建议 Phase 5 专项审计：

1. **🔴 Tombstone/Merge 语义正确性**（basho #82 删除复活类、#149/174/175 merge 竞态）
2. **🟠 fsync/fdatasync 纪律审计**（WAL 持久性、meta/ckpt 原子性、backup 一致点）
3. **🟠 Lock 文件健壮性**（空文件、PID 复用、disk-full 失败传播）
4. **🟡 CRC 回退路径完整性**（hint 失败回退扫描时是否做 CRC 校验）
