# libbitcask 风险报告

> 基线：Google C++ Style 规范化审计（2026-07-14）
> 范围：`src/`、`include/bitcask/`、`c_api/`（208 文件 / ~74K 行）
> 方法：3 路 explore agent 全树扫描 + 逐一阅读上下文验证 + diff 比对
> 结论：**内存与并发维度达到工业级水准（误报率 ~95%）；唯一显著问题在冗余维度**
>
> 修订（2026-07-14 二次核对）：所有实质性结论复核通过；修正若干计数
> （HIGH-1 diff 行数、MED-4 guarded 计数、cv.wait 处数），补充两处遗漏
> （MED-3 增补 `field_schema.hpp`；MED-1 增补 hnsw 重载路径与 data_file
> 错误回退路径），修正 MED-2 的合并方案（须保留两种解码契约）。
>
> ---
>
> **Phase 4-5 修订（2026-07-14，本报告下半部分）**：
> Phase 1-3（T1-T5）落地后进行 v2 深度审计（3 路深读 agent，~200 次工具调用）；
> Phase 4（T6-T13）修复了 v2 标定的 1 HIGH + 3 MED + 多项 LOW；
> Phase 5 在 Phase 4 完成后再次进行 4 路并行审计（3 explore + 1 librarian）。
> 下半部分记录 v2 深度审计、Phase 4 修复落地、Phase 5 新发现三段。
> **当前风险摘要见文末「Phase 5 风险摘要」表。**
>
> **Phase 5 复核修订（2026-07-14 三次核对）**：主 Agent 对 Phase 5 全部开放
> 发现逐条重读代码复核。核心结论全部属实（P5-MEM-1/2、P5-DL-2/3、冗余残留
> 项逐一确认仍在），但修正 4 处：① P5-MEM-1 使用点为 **7 处非 4 处**，且模式
> 源自 S13-F2 而非 Phase 4 引入；② P5-MEM-2 **新增第三处漏点**（无索引池分支）；
> ③ P5-DL-1 场景描述反了——等待发生在 ckpt_mu_ **临界区内**；④ P5-DL-3 升档：
> 死代码 flush_upto 在 reducer 热路径上留有**每任务全局锁开销**。详见各小节
> 内嵌的「复核修订」标注。

---

## 风险摘要

| 维度 | 高危 | 中危 | 低危 | 总评 |
|---|---|---|---|---|
| 内存逃逸 | 0 | 0 | 2 | ✅ 无真泄漏 |
| 死锁场景 | 0 | 0 | 0 | ✅ 无可达死锁 |
| 代码冗余 | 1 | 4 | 1 | ⚠️ 需行动 |

---

## 一、内存逃逸 — 无真问题

### 1.1 验证结论

explore agent 报出 10 类共 50+ 嫌疑点，**逐一阅读上下文后全部否定**：

| 嫌疑 | 文件:行 | 验证 |
|---|---|---|
| `new std::atomic<u32>[]` 看似裸指针 | `src/vector/hnsw.cpp:334` | 误报。成员声明为 `std::unique_ptr<std::atomic<std::uint32_t>[]> locks;`（`hnsw.hpp:245`），`= default` 析构正确 `delete[]` |
| `NodeChunk*` 裸所有权 | `include/bitcask/hnsw.hpp:157` | 误报。元素为 `std::atomic<NodeChunk*>`，析构循环 `for (auto& slot : chunks_) delete slot.load()`（`hnsw.cpp:402-404`）正确配对 |
| `void* vecs_mmap_raw_` / `qc_mmap_raw_` | `include/bitcask/hnsw.hpp:452,466` | 误报。析构里 `::munmap(...)` + `::close(fd)` 严格配对（`hnsw.cpp:381-401`），先 munmap 再 close fd 顺序正确 |
| `void* raw_` 无主 | `include/bitcask/diskann.hpp:121` `ivf_rq.hpp:148` | 误报。两个类的析构都 `~T() { close(); }`（`diskann.cpp:177`、`ivf_rq.cpp:70`），`close()` 内 `::munmap(raw_, len_)` + `::close(fd)` |
| `T* chunk = new T[kChunkSize]()` | `include/bitcask/row_chunks.hpp:81,88` | 见下方 LOW-1（风格偏好，无泄漏） |
| `static tbb::task_arena* = new ...` | `src/search/search_arena.cpp:22` | 合规。Meyer singleton，注释明示"故意泄漏，规避静态析构与 TbbLifetime::finalize 顺序坑"——Google Style 允许此模式 |
| `IndexLane* default_lane_` | `include/bitcask/thread_pool.hpp:701` | 误报。是 `lanes_` map 中 `shared_ptr<IndexLane>` 的非拥有引用（注释"facade 状态"），map 释放时自动清理 |
| `c_api/bitcask_kv.cpp` malloc/free × 15 处 | `c_api/bitcask_kv.cpp` | 必要。C ABI 跨语言边界强制 malloc/free，每个 malloc 都有对应 `bitcask_*_free` 配对 |

### 1.2 风格层面建议

#### LOW-1：`row_chunks.hpp` 裸 `new T[]` + `delete[]`

- **位置**：`include/bitcask/row_chunks.hpp:81,88,104-107`
- **现状**：自定义并发容器，`destroy()` 正确释放 chunks + graveyard + spine，无泄漏
- **建议**：改 `std::unique_ptr<T[]>`，行为等价但符合 Google Style "Prefer smart pointers"
- **优先级**：LOW（功能正确，仅风格）

#### LOW-2：`const_cast<void*>` 后 free

- **位置**：`c_api/bitcask_kv.cpp:283-285`
- **现状**：C API 出参约定为 `const void*` 给调用方读，free 时去 const——POSIX 习惯（`free((void*)ptr)` 是 C 标准允许的）
- **建议**：如要彻底干净可改 `void*` 出参，但破坏 C API 现有约定
- **优先级**：LOW（API 兼容性 > 风格）

---

## 二、死锁场景 — 无可达死锁

### 2.1 验证结论

explore agent 报了 2 个 "🔴 HIGH" 多锁风险，**全部误报**——agent 只模式匹配了 `std::unique_lock` 嵌套，未读注释里的无环证明。

#### 嫌疑 #1（否定）：`KeyDir::put_probe` 锁序 shard → meta

```
src/keydir/keydir.cpp:487  ctx.slock = unique_lock(sh.mu)
src/keydir/keydir.cpp:494  ctx.slock.unlock()         ← 关键！
src/keydir/keydir.cpp:496  unique_lock g(gate_mu_)
src/keydir/keydir.cpp:497  gate_cv_.wait(g, predicate)
src/keydir/keydir.cpp:501  ctx.slock.lock()
src/keydir/keydir.cpp:522  ctx.mlock = unique_lock(meta_mu_)
```

**无环论证**：等待 `gate_cv_` 前**显式释放了 shard 锁**（494 行），注释明示设计意图（"等待前必须放分片锁——否则排干循环与我们互等"）。`gate_mu_` 在 501 行前已离开作用域。重新拿到 shard → 再拿 meta shared，方向与读者路径一致。

#### 嫌疑 #2（否定）：`apply_pending_to_entries_barrier` 反向锁序

```
src/keydir/keydir.cpp:1047  shared_lock mlock(meta_mu_)
src/keydir/keydir.cpp:1059  lock_guard sg(sh.mu)
```

**无环论证**（`keydir.cpp:1034-1045` 原注释）：
> ⚠ 锁序例外（仅屏障内合法）：写者（put/remove，meta unique 的全部使用者）已被闸门出清；唯一并发者是读者（get），其 shard→meta 嵌套对 meta 只拿 **shared**；本阶段同样只拿 shared。shared-shared 相容且无 unique 排队者——无法构成「持 shard 等 meta / 持 meta 等 shard」的环。

### 2.2 其他并发检查（全部通过）

| 项 | 结果 |
|---|---|
| 所有 `cv.wait/wait_for/wait_until` 是否带谓词（9 处 CV wait + 1 处 `atomic::wait`） | ✅ 全部带（keydir×2、cask gc_cv×1、text_plugin×3、thread_pool×3；`cask.cpp:644` 是 `writes_in_flight_.wait()` 原子等待，无谓词需求） |
| 锁内回调（callback-while-locked） | ✅ TextPlugin builder_loop 在调 `apply_*_in` 前明确释放 `b.mu`（`text_plugin.cpp:1524-1533`）；DataFile::fold 无锁；IndexPool 只在锁内存回调，调用在 worker 线程无锁 |
| `recursive_mutex` 使用 | ✅ 全代码库零使用 |
| 原子内存序 | ✅ 全部合理：counter 用 relaxed，发布用 release/acquire，写屏障 `writes_in_flight_/barrier_active_/epoch_` 用 seq_cst |
| thread_local scratch 复用 | ✅ hnsw.cpp / inverted.cpp / segment_v2.cpp 等大量 thread_local 工作缓冲 |

---

## 三、代码冗余 — 1 处严重 + 4 处中等

### 🔴 HIGH-1：IVF / DiskANN 插件 ~950 行近乎完全重复

**证据**（直接 diff 验证）：

```
src/search/ivf_plugin.cpp      518 行
src/search/diskann_plugin.cpp  519 行
diff 行数:                     91 行（< 和 > 合计,实测 diff | grep -c '^[<>]'）
相同或仅符号不同的行数:        ~950 行
```

`diskann_plugin.cpp` 文件头自承：

> DiskannPlugin 实现（S32-M5）。**结构与 ivf_plugin.cpp 同构**（sealed 段 + 窗口 + DeltaLog + 组件链）；差异仅段类型/文件族/kDiskann 段/建图参数。

**diff 显示的全部差异**（机械替换）：

| From | To |
|---|---|
| `IvfPlugin` | `DiskannPlugin` |
| `IvfSegment` | `DiskannSegment` |
| `"ivf.ckpt"` | `"diskann.ckpt"` |
| `.biv` | `.bda` |
| `biv_path_of` | `bda_path_of` |
| `nprobe` | `l` |
| `kIvfBlobSize` | `kDiskannBlobSize` |
| `kIvfCkptName` | `kDiskannCkptName` |

**完全相同或近似的函数**：

| 函数 | 行号（两文件对应） | 行数 |
|---|---|---|
| `normalize_for_write` | 68-92 | ~25 |
| `insert` | 94-107 | ~14 |
| `search` | 109-188 | **~80** |
| `rebuild` | 190-195 | ~5 |
| `on_merge_commit` | 197-204 | ~7 |
| `make_window` | 53-65 | ~12 |
| `make_gen` | 36-42 | ~7 |
| `size/sealed_size/window_size` | 206-216 | ~10 |
| `serialize_delta_log` / `apply_delta_log` | 218-238 | ~20 |
| `save_component_base` | 240-334 | **~95** |
| `load_component` | 360-462 | **~103** |

**Google Style 违反**：[Avoid Duplication](https://google.github.io/styleguide/cppguide.html#Avoid_Duplication) 明确要求"Duplicated code should be factored into a shared helper or base class."

**修复路径**：

1. 抽取 `VecIndexPluginBase`（CRTP 或常规多态基类）到 `include/bitcask/detail/vec_plugin_base.hpp`
2. 子类只 override 少量差异点：`do_search_sealed(query, k, ef, live)` / `segment_path()` / `ckpt_name()` / `blob_size()` / `segment_open(path, ...)` / `segment_build(...)`
3. 预期：`ivf_plugin.cpp` / `diskann_plugin.cpp` 各收缩到 ~100 行（仅差异逻辑）
4. 已有先例：HNSW 路径用 `VectorEnginePlugin` 接口；DiskANN/IVF 应共用同一实现骨架
5. **工作量估算**：1-2 天（含测试调整）

**风险提醒**：当前两文件的机械对称反而便于 diff 对拍验证（本审计即用此法）；
抽基类后此优势消失，重构必须让 IVF / DiskANN 两引擎的测试套件全部通过后
才可合入。考虑到 S32 之后大概率继续加引擎，此投入仍然划算。

---

### 🟡 MED-1：mmap 释放样板重复（~8 处）

**位置**：

| 文件 | 行号 | 行数 | 场景 |
|---|---|---|---|
| `src/vector/hnsw.cpp` | 378-405 | ~24 | 析构 |
| `src/vector/hnsw.cpp` | 1675, 1855 | ~5×2 | 重载/换映射路径 |
| `src/vector/diskann.cpp` | 177-193 | ~15 | 析构（经 `close()`） |
| `src/vector/ivf_rq.cpp` | 70-89 | ~18 | 析构（经 `close()`） |
| `src/bm25/segment_v2.cpp` | 423-427 | ~4 | 析构 |
| `src/fileops/data_file.cpp` | 67-72, 86 | ~5+1 | 析构 + 错误回退 |

中途 remap 与错误回退路径正是最容易漏配对的地方——这加强了抽 RAII 的理由。

**重复模式**：

```cpp
if (ptr != nullptr) {
    ::munmap(ptr, len);
    ptr = nullptr; base = nullptr; len = 0;
}
if (fd >= 0) { ::close(fd); fd = -1; }
```

**修复路径**：抽取 `MmapHandle` RAII 类到 `include/bitcask/detail/mmap_handle.hpp`，构造持 `fd + map`，析构统一释放，提供 `release()` 逃生口。符合 Google Style "Use RAII where possible"。

**工作量**：半天。

---

### 🟡 MED-2：vbyte 解码三份独立定义

**位置**：

| 文件 | 行号 | 形态 | 契约 |
|---|---|---|---|
| `src/fileops/codec.cpp` | 22-35 | `vbyte_read` 自由函数 | 带越界检查（span） |
| `include/bitcask/meta_codec.hpp` | 88-101 | `detail::vbyte_read` 命名空间函数 | 带越界检查 |
| `include/bitcask/meta_codec.hpp` | 337-350 | `meta_lookup` 内联 lambda `read_varint` | 带越界检查 |
| `include/bitcask/vbyte.hpp` | 41 | `vbyte_decode` | **无检查（裸指针，热路径）** |

带越界检查的同一算法写了三遍。注意 `vbyte.hpp` 的无检查版本是**有意为之**
（`codec.cpp:20` 注释、S21-2 A3：损坏文件防越读 vs 热路径零开销），不在合并范围。

**修复路径**：每种契约各留一份权威实现——带检查的三份归并到一处
（`codec.hpp` 或 `vbyte.hpp` 增加 checked 变体），`meta_codec.hpp` include 复用；
无检查的 `vbyte_decode` 保持独立并保留注释说明。**不是**三合一到单个函数。

**工作量**：1 小时。

---

### 🟡 MED-3：CRC32 调用入口不一致

**现状**：

- 71 处走 `codec::crc32(...)` / `codec::crc32_update(...)`（正确统一入口）
- **3 个文件**直接调 `hw::crc32` / `hw::crc32_update`（绕过 codec 封装）：
  - `src/bm25/segment_v2.cpp`（6 处：103, 370, 471, 499, 945, 976）
  - `src/cask/meta_file.cpp`（1 处：39）
  - `include/bitcask/field_schema.hpp`（3 处：172, 199, 201）——初版报告遗漏

**问题**：硬件派发应藏在 codec 内部，调用方统一走 codec 入口，否则未来替换 CRC 实现或加预处理器需扫多处。

**修复路径**：把三个文件的 `hw::crc32*` 改为 `codec::crc32*`。`field_schema.hpp`
是头文件，改前需确认 include `codec.hpp` 不引入循环依赖；若是有意避开，
应就地注释说明。

**工作量**：30 分钟。

---

### 🟡 MED-4：C API `guarded()` 包装样板（30 处）

**位置**：`c_api/bitcask_kv.cpp`（15 处）、`c_api/bitcask_text.cpp`（9 处）、`c_api/bitcask_vec.cpp`（6 处）——实测 `grep -c "return guarded("`，初版报告的 40 处（28+12+8）为高估

**重复模式**：

```cpp
BITCASK_API bitcask_error_t bitcask_X(...) {
    return guarded(fault, [&]() -> bitcask_error_t {
        if (!cask || !out) return BITCASK_ERR_INVALID_OPTION;
        *out = nullptr;
        // 业务逻辑
        return BITCASK_OK;
    });
}
```

**评估**：

- 已抽取的部分：`guarded`、`fault_from_exception`、`set_oom_fault` 在 `c_api/internal.h` 共享
- 剩余 30 处样板是 **extern "C" 异常隔离的必要成本**——C++ 异常不能泄漏到 C 调用方，每个导出函数必须 try/catch 包裹
- 可选优化：X-macros（`BITCASK_KV_API(name, sig, body)`）能把模板缩成一行，但降低可读性
- **当前形态可接受**，列入 MED 仅作记录

---

### 🟢 LOW-3：插件路径助手函数重复

**位置**：`comp_path` / `biv_path_of` / `bda_path_of` 在 4 个插件里各写一次

```cpp
std::string comp_path(std::string_view dir) {
    return (std::filesystem::path(dir) / kXxxCkptName).string();
}
```

**修复路径**：HIGH-1 重构时一并模板化（`comp_path<Plugin>(dir)`）。

---

## 四、其他 Google Style 符合度

| 项 | 符合情况 |
|---|---|
| 文件命名、`#pragma once`、include 顺序 | ✅ 规范 |
| 命名（snake_case 变量、PascalCase 类型、kConstant、member_） | ✅ 严格 |
| `const`/`constexpr` 正确性 | ✅ 大量 `[[nodiscard]] const` |
| `= delete` 防拷贝 | ✅ Cask / KeyDir / 所有 Segment 子类显式 delete |
| 异常 vs 错误码 | ✅ `std::expected` 普遍使用，C 边界 `guarded()` 严格隔离 |
| 智能指针优先 | ✅ 整体优秀；`row_chunks.hpp` 例外有文档理由 |
| `-Werror` | ⚠️ 默认 OFF（`BITCASK_WERROR` 选项），CI `werror-lib` job 才开。当前权衡可接受 |
| `.clang-tidy` / `.clang-format` | ❌ **缺失**——无自动化 tidy 检查 |

---

## 五、行动建议（按 ROI 排序）

| # | 行动 | 文件 | 工作量 | ROI |
|---|---|---|---|---|
| 1 | **抽取 `VecIndexPluginBase`，IVF/DiskANN 改薄派生** | `src/search/{ivf,diskann}_plugin.cpp` | 1-2 天 | 🔴 极高（-950 行，未来加引擎只写 ~100 行）；须两引擎测试全过 |
| 2 | **补 `.clang-tidy` 配置 + CI tidy job** | 仓库根、`.github/workflows/ci.yml` | 2 小时 | 🟡 高（防未来回归） |
| 3 | 抽取 `MmapHandle` RAII 类 | `include/bitcask/detail/` | 半天 | 🟡 中（~8 处统一，含重载/错误回退路径） |
| 4 | 归并带检查的 vbyte 解码三份定义（无检查版保持独立） | `codec.cpp` + `meta_codec.hpp` | 1 小时 | 🟡 中 |
| 5 | `segment_v2.cpp` / `meta_file.cpp` / `field_schema.hpp` 改走 `codec::crc32` | 3 个文件 | 30 分钟 | 🟢 低 |

---

## 六、附：验证方法学

本报告所有结论均经过双重验证：

1. **explore agent 全树扫描**：3 个并行 agent 分别覆盖内存模式（new/delete/malloc/free/C-cast/RAII）、并发模式（mutex/lock/cv/atomic/thread）、冗余模式（重复代码块/样板/相似函数）
2. **直接阅读上下文**：所有被标为 "HIGH/CRITICAL" 的嫌疑点均由阅读对应源码上下文（含注释）逐一确认或否定
3. **二次独立核对**（2026-07-14）：对全部关键断言重新跑 grep/diff/源码
   抽查。实质性结论全部复核通过；修正数字类偏差（HIGH-1 diff 行数、
   MED-4 guarded 计数、cv.wait 处数），补充 MED-1/MED-3 遗漏点，修正
   MED-2 合并方案。唯一未逐点复核的大断言是 2.2 表中"原子内存序全部
   合理"——该项依赖初版审计的逐处阅读。

**关键教训**：模式匹配式的静态扫描（grep / AST-grep）在规范化的代码库上误报率极高（本库 ~95%）；真正的判断必须依赖**对代码注释和所有权文档的阅读**。`keydir.cpp:1034-1045` 的无环证明是典型案例——agent 只看到 `shared_lock` + `lock_guard` 嵌套就报 HIGH，但注释里已有严格的不变量论证。

未来在 CI 中引入 clang-tidy + 静态分析时，应同样配套**人工 review 通道**，避免告警泛滥稀释信号。

---

# Phase 4-5 修订（2026-07-14）

本部分接续 v1/v2 内容。先记录 v2 深度审计的发现（驱动了 Phase 4 的 T6-T13
修复），再追踪 Phase 4 修复落地情况，最后给出 Phase 5（post-Phase-4）审计
的新发现。

## v2 深度审计摘要（驱动 Phase 4）

v2 在 T1-T5 落地后，针对异常路径做了逐调用链深读。**核心方法学升级**：
"常态路径干净不等于异常路径干净"——bad_alloc/kNotReady/契约违反三类路径
需要专项追踪不变量是否维持。

### v2 发现（已被 Phase 4 处理）

| 编号 | 严重度 | 描述 | Phase 4 处理 |
|---|---|---|---|
| MEM-MED-1 | MED（升档 HIGH） | Registry acquire/release 不配对——kNotReady 路径漏增 refcount，close 误减初始化方 refcount 至 0 并 erase，破坏 biggest_file_id 单调性（tombstone-resurrection 等价类） | ✅ T6（commit cc30b6c） |
| DL-MED-1 | MED（升档 HIGH） | 四个 RunFn 提交点（run_serialized / 手动 ckpt / 自动 ckpt / merge）alloc_ord 与 submit 之间可抛 bad_alloc 泄漏 ord → reducer lane 永久空洞 → 进程级挂死 | ✅ T7（cc30b6c） |
| DL-MED-2 | MED（升档 HIGH） | checkpoint() 的 done->wait(0) 无超时；IndexPool::submit 在 stopped_ 时静默丢弃任务，done 永不置位 → ckpt_mu_ 级联锁死 | ✅ T7（cc30b6c） |
| DL-MED-3 | MED | prepare_search() 调用 flush_index()，谓词要求 in_flight==0 && applied>=hwm，持续写入下查询线程无界等待 | ⚠️ T8 初版 snapshot-hwm 方案在 4 测场景产生搜索漏召，已 revert；thread_pool.hpp 保留 flush_upto 工具方法供重新设计 |
| MEM-LOW-1 | LOW | field_schema.hpp 裸 FILE* 在 bad_alloc 路径泄漏 fd（同仓库其他文件已改 unique_ptr） | ✅ T10（commit 8206aed，file_util.hpp 一并收口） |
| RED-2 | MED | FileCloser 定义 9 份 + fopen/SEEK_END/fread 整读样板 6 份 + tmp+fsync+rename 原子写 ~7 处 | ✅ T10（8206aed + c4045e4） |
| RED-4 | MED | 死代码 fill_get_result（零调用，被 view 版取代） | ✅ T9（commit 4dba518） |
| RED-8 | SMALL | 死代码 to_codepoints_reuse（零调用） | ✅ T9（4dba518） |
| RED-9 | SMALL | 建而未用 MmapRegion（T4 新建 RAII 基建，8 处 mmap 站点零采用） | ✅ T11（commit de448c4，整文件删除） |
| RED-11 | SMALL | 死代码 detail::vbyte_append（零调用且与 codec::vbyte_encode 逐字相同） | ✅ T9（4dba518） |
| DL 陷阱 6 | LOW | plugin_api.hpp 未明文禁止 reducer 上下文调用 run_serialized | ✅ T13（de448c4，注释固化契约） |

### v2 已核实非问题（保留记录供后续审计复用）

- C API 句柄生命周期、fd/mmap/FILE*/文件锁、线程、shared_ptr 循环引用、
  epoch/延迟回收、search_arena never-destroyed 单例——逐点核对无发现
- KeyDir barrier 内 meta→shard 反序：闸门协议 + shared-shared 相容，无环论证成立
- IndexPool "任一时刻至多持一把锁"不变量：cv 通知配对正确
- 组提交 follower 丢失唤醒：理论窗口被 wait_for 100µs 超时兜底
- 读写锁升级、递归加锁、线程池自死锁——未发现

---

## Phase 4 落地清单（commit b0cda22 截止）

| Commit | 内容 | 文件数 | 验证 |
|---|---|---|---|
| `cc30b6c` | T6 MEM-MED-1 + T7 DL-MED-1/2 | 2 | 641/641 ctest |
| `4dba518` | T9 死代码清理（RED-4/8/11） | 3 | 641/641 ctest |
| `8206aed` | T10①file_util.hpp + 头文件迁移（含 MEM-LOW-1） | 4 | 641/641 ctest |
| `c4045e4` | T10②cpp 站点迁移 | 5 | 641/641 ctest |
| `de448c4` | T11 删 MmapRegion + T13 plugin_api 契约注释 | 2 | 641/641 ctest |
| `b0cda22` | docs: TASK.md 更新 | 1 | — |

**未完成**：
- T8 DL-MED-3：revert，待重新设计
- T12 RED-1（HNSW ckpt 去重）：已精确审计，待独立分支（须向量三引擎测试门槛）
- RED-3/5/6/7残/10/13：v2 残留冗余，未在 Phase 4 处理

---

## Phase 5 风险摘要（post-Phase-4 状态）

| 维度 | 高危 | 中危 | 低危 | 总评 |
|---|---|---|---|---|
| 内存/异常安全 | **1**（P5-MEM-1） | 1（P5-MEM-2） | 2（P5-MEM-4/5） | ⚠️ HIGH 模式源自 S13-F2，Phase 4 扩散（复核修订） |
| 死锁/并发 | 0 | 3（P5-DL-1/2/3，DL-3 复核升档） | 1（P5-DL-4） | ✅ 无实际死锁；偏离是模式非 bug |
| 代码冗余 | 0 | 3（RED-1/5/6） | 4（RED-7残/10/13/NEW-P4） | ⚠️ 可清理 ~280 行；T10 收尾欠 1 处 |
| Google Style | 0 | 0 | 3（C++23/std::expected/#pragma） | ✅ 表面合规优秀 |

---

## 七、Phase 5 — 内存与异常安全（1 HIGH + 1 MED）

### 🔴 P5-MEM-1：OrdSkipGuard 析构在异常路径可触发 std::terminate()

- **位置**：`include/bitcask/cask.hpp:1007-1012`；**7 个使用点**
  `src/cask/cask.cpp:505 / 1060 / 1801 / 1877 / 2235 / 2469 / 2575`
- **Phase 4 引入**：**否（复核修订）**——`cask.cpp:1877` 带注释 "S13-F2"，
  noexcept 析构 + 可抛 submit 的模式**从 S13-F2 起即存在**于写路径 3 个使用点
  （1060/1801/1877）；T7（cc30b6c）复制既有模式新增 4 个 RunFn 使用点
  （505/2235/2469/2575）。v2 与初版审计均未捕获。修复在析构一处即覆盖全部 7 点
- **触发条件**：`submit_index_task()` 抛 bad_alloc（TBB queue push 失败）
- **场景**：
  ```cpp
  OrdSkipGuard og(this, ord);
  // ... 任务构造 ...
  c->submit_index_task(std::move(t));  // ← 可抛 bad_alloc
  og.disarm();                          // ← 抛出则不到达
  // 析构：~OrdSkipGuard() → armed==true → submit_index_task() 再抛 → terminate()
  ```
- **后果**：`std::terminate()` = 进程立即死亡，不可恢复。Linux overcommit 下
  罕见但不可消除
- **证据**：`~OrdSkipGuard()` 隐式 noexcept，但 `submit_index_task()`
  调 `tbb::concurrent_bounded_queue::push` 非 noexcept
- **修复方向**：析构内 `try { submit_index_task(...); } catch (...) { /* 记录 ord 泄漏，不抛 */ }`。
  代价：极端 bad_alloc 下泄漏 1 个 ord（可恢复）；收益：避免 terminate（不可恢复）

### 🟡 P5-MEM-2：checkpoint() 同步回退路径漏更新 last_ckpt_ord_

- **位置（复核修订：三处漏点）**：
  1. `src/cask/cask.cpp:2215-2224`（is_stopped 同步分支——注释声称
     "synchronous path sets last_ckpt_ord itself"，与代码直接矛盾）
  2. `:2311-2321`（无索引池分支，理论不可达但同样漏 store）——复核新增
  3. merge 路径 `:2580-2601`（RunFn 与 else 两分支均漏；merge 后 rebase 全量
     ckpt 已落盘却不推进水位 → 紧随的写入立刻触发一次冗余自动 ckpt）
- **消费点核对**：`last_ckpt_ord_` 唯一消费点是自动 ckpt 阈值判断
  （`cask.cpp:2456`）；store 点现仅 open（:284）与两个异步 RunFn 成功路径
  （:2259/:2281/:2478）
- **Phase 4 引入**：是（T7 DL-MED-2 修复的副作用；merge/无池分支属遗漏而非引入）
- **问题**：注释自相矛盾
  ```cpp
  // DL-MED-2: ... 同步路径 sets last_ckpt_ord itself.   ← 注释如此声称
  if (index_pool_->is_stopped()) {
      ...
      if (!save_search_ckpt_paired(...)) { return unexpected(...); }
      // BUG: 缺少 last_ckpt_ord_.store(peek_next_ord, relaxed)
      return {};
  }
  ```
- **后果**：`last_ckpt_ord_` 是自动 ckpt 阈值的输入。漏更新 → 下次写时触发
  冗余 ckpt（重复劳动 + 日志噪音）。非崩溃，非数据损坏
- **修复方向**：成功路径加 `last_ckpt_ord_.store(keydir_->peek_next_ord(), std::memory_order_relaxed);`

### 🟢 P5-MEM-3（核验否定）：field_schema wf.reset() 顺序问题

agent 初版报告称 `wf.reset()` 在 fflush/fsync 之前导致 use-after-close。
**直接核验代码顺序**：flush/sync 在 line 241-242（wf 存活），reset 在 line 244。
**agent 误读了代码顺序**。✅ 安全。

### 🟢 P5-MEM-4：checkpoint() 30s 超时后 RunFn 异步继续

- **位置**：`src/cask/cask.cpp:2295-2309`
- **性质**：**接受的 trade-off**（commit message 已说明）；调用方应处理
  "先错后成功"语义

### 🟢 P5-MEM-5：OrdSkipGuard::disarm() 非 atomic

- **位置**：`include/bitcask/cask.hpp:1006`
- **性质**：当前使用模式均为单线程（构造/disarm/析构在同一线程），技术上
  非数据竞争。Google Style 建议加注释固化契约

### 干净项（Phase 5 直接核验无问题）

- ✅ **零** first-party `.release()` / `dynamic_cast` / `reinterpret_cast` /
  `goto` / `using namespace std` / `std::endl` / 动态异常规格 / `NULL` 宏 /
  `typedef`
- ✅ 零 first-party `printf`/`scanf` 在 src/include（仅在 tools/ 合理）
- ✅ T9 死代码删除：核验零残留引用
- ✅ T11 mmap_handle.hpp 删除：核验零残留引用
- ✅ T10 FilePtr 在 8 个迁移站点：`.get()` 使用均在 FilePtr 存活范围
- ✅ T6 MEM-MED-1 修复：局部变量暂存 + 成功后 std::move 赋值——RAII 顺序正确
- ✅ T7 DL-MED-2 shared_ptr by-value 捕获：lambda 持有副本，生命周期延长正确
- ✅ FileCloser::operator() noexcept——unique_ptr 析构安全

---

## 八、Phase 5 — 死锁与并发（无实际死锁；2 项模式偏离）

### 🟡 P5-DL-1：checkpoint() 持 ckpt_mu_ 后扩展到局部 done_mu 的 cv 等待

- **位置**：`src/cask/cask.cpp:2203, 2229-2305`
- **性质**：**非死锁**，但偏离 v2 thread-safety.md 文档化的"ckpt_mu_ 简单串行化"语义
- **场景（复核修订：原描述反了）**：`ckpt_mu_` 是函数级 `lock_guard`
  （`cask.cpp:2203`），30 秒 cv 等待（:2296-2301）发生在 ckpt_mu_
  **临界区内**，且 WriteOpGate（:2197）同时持有——即「持锁 A 等待条件 B」
  的嵌套等待，而非原文所述"在释放边界外等待"。后果：reducer 卡住时，
  后续 checkpoint 调用者与 close()（经 WriteOpGate）最坏被拖满 30 秒
- **为何不是死锁**：done_mu 是 per-call 局部 shared_ptr，无任何其他代码路径
  获取它；且等待有 30s 超时上界
- **为何仍是 MED**：偏离文档不变量；未来若 reducer 上下文错误调用 checkpoint()
  会立即构成死环
- **修复方向**：在 docs/design/thread-safety.md 补充记录此模式，或重构成
  纯 atomic+超时（须等 C++26 的 atomic_wait_for 提案）

### 🟡 P5-DL-2：reducer_loop 在 applied_ord 推进后立刻取 flush_mu_，紧接 dec_in_flight 又取

- **位置**：`include/bitcask/thread_pool.hpp:653-657`
- **性质**：**非死锁**（两次锁不重叠），但偏离"释放 A → 工作 → 取 B"的单锁文档模式
- **场景**：
  ```cpp
  lane->applied_ord.store(...); ++lane->next_apply_ord;
  { std::lock_guard fl(flush_mu_); flush_cv_.notify_all(); }  // 第一次取 flush_mu_
  dec_in_flight(lane.get());                                   // 第二次取 flush_mu_
  ```
- **为何仍是 MED**：文档化的不变量是"任一线程任一时刻最多持一把锁"——这里两次
  取同一锁违反"释放 → 工作"模式；未来误读此模式可能引入真锁序问题
- **修复方向**：合并为单次 dec_in_flight 内部通知，或文档化此例外

### 🟡 P5-DL-3：flush_upto() 死代码在 reducer 热路径留有每任务全局锁开销（复核升档 🟢→🟡）

- **位置**：`thread_pool.hpp:447`（flush_upto 定义，零调用）；
  `:653-656`（reducer_loop 每 apply 一个索引事件即取一次全局 `flush_mu_`
  + `notify_all`，注释明示专为 flush_upto 的 applied_ord 推进通知服务）
- **复核升档理由**：不只是死代码——常规 `flush()` 的唤醒已由 `dec_in_flight`
  归零通知完全覆盖（applied_ord 在 dec 之前 store，谓词在 flush_mu_ 下检查，
  无丢失唤醒窗口）。即 :653-656 是**纯开销**：reducer 每 apply 一个任务白付
  一次全局 mutex 加锁 + notify_all
- **修复方向**：删除 flush_upto + 连带删除 :653-656 通知块（动手前需确认
  unregister_lib 等待路径不依赖 per-apply 通知），从"5 分钟标注"变为有实际
  吞吐收益的清理项

### 🟢 P5-DL-4：v2 的 15 组件审计未覆盖 S32 新增组件

DiskANN/IVF/VectorDeltaLog/SearchCheckpoint/IndexManifest——未审计≠有 bug，
但建议下轮专项覆盖

### 已排除的疑点

- ✅ `run_serialized` 在 reducer 上下文调用：grep 全部 caller（TextPlugin/
  VectorPlugin 的 on_merge_commit）均在 merge 线程，不在 reducer。T13 文档
  契约未被违反
- ✅ `flush_cv_` 双 notify（applied_ord 推进 + in_flight==0）：谓词正确处理
  spurious wake，无丢失唤醒

---

## 九、Phase 5 — 代码冗余（v2 残留 + Phase 4 新增）

### v2 残留（Phase 4 未处理）

| # | 项 | 位置 | 规模 | 状态 |
|---|---|---|---|---|
| RED-1 | HNSW flush() 与 SealedSegmentVectorPlugin flush() 逐字节相同 | `vector_plugin.cpp:511-555` vs `sealed_segment_vector_plugin.hpp:670-710` | ~90 行 | T12 待独立分支 |
| RED-3 | 小端编解码三套并行 | `byte_order.hpp:14-48` vs `search_checkpoint.hpp:135-163` vs `index_manifest.hpp:75-99` | ~55 行 | 未处理 |
| RED-5 | hnsw.cpp search_layer / search_layer_int8 成对复制 | `hnsw.cpp:846-910 / 952-1023` | ~130 行 | 未处理 |
| RED-6 | IvfSegment::open vs DiskannSegment::open 骨架同构 | `ivf_rq.cpp:475-549` vs `diskann.cpp:472-569` | ~45 行 | 未处理 |
| RED-7（残） | hnsw.cpp 仍有本地 pwrite_all，未消费 diskint::pwrite_all | `hnsw.cpp:1523-1536` vs `vec_disk_internal.hpp:25-37` | ~14 行 | T10 未覆盖 |
| RED-10 | SnapCursor::vb() 与 codec::vbyte_read_checked 语义等价 | `keydir.cpp:1297-1307` | ~11 行 | 未处理 |
| RED-12 | Manifest::min_chain_watermark 生产零调用（仅测试引用） | `index_manifest.hpp:61-65` | ~5 行 | 维持 |
| RED-13 | c_api 错误翻译样板 ×16 | `bitcask_kv.cpp` 等 | ~32 行 | **有意保留**（C ABI 隔离代价） |

### Phase 4 新增（已核验）

| # | 项 | 位置 | 规模 |
|---|---|---|---|
| **NEW-P4-1** | field_schema.hpp 重复本地别名 ReadFilePtr/WriteFilePtr，等价于 detail::FilePtr | `field_schema.hpp:73, 230` | 2 行 |
| **NEW-P4-2** | hnsw.cpp 未使用 diskint::pwrite_all（与 RED-7 残同根） | `hnsw.cpp:1523-1536` | ~14 行 |

### 已核实非冗余（保留记录）

- inverted 的 MSB-first FOR 与 segment_v2 的 LSB-first BitWriter 分属两个已定盘格式
  （字节序相反），合并破坏兼容
- `vbyte_decode`（无检查，热路径）与 `vbyte_read_checked`（带检查）双版本契约明确
- bm25 三层已经 bm25_search_impl.hpp/walk_chain/SectionWriter 充分共享

---

## 十、Phase 5 — Google C++ Style 合规

### 表面合规（直接工具核验，全部 ✅）

- 零 first-party `goto` / `NULL` / `typedef` / 动态异常规格 / `using namespace std` / `std::endl`
- 零 first-party `dynamic_cast` / `reinterpret_cast`（Google Style 限制 RTTI）
- 零 first-party `.release()` on smart pointer（无所有权逃逸）
- `.clang-tidy` 配置完备，关闭项均有文档化理由

### 🟢 低危（有意识偏离，建议文档化）

| # | 项 | Google Style | libbitcask 现状 | 评估 |
|---|---|---|---|---|
| GS-1 | C++ 版本 | C++20（C++23 features 禁用） | **C++23**（CMakeLists 强制） | 有意识偏离。独立项目非 Google 内部，可接受 |
| GS-2 | std::expected | 禁用（C++23 特性） | 重度使用（103 处，10 文件） | 同上。是核心 API 设计支柱，不可回退 |
| GS-3 | #pragma once vs #define 守卫 | 推荐 #define 守卫 | 全用 #pragma once | 主流编译器全支持，可接受 |
| GS-4 | `<filesystem>` 头 | **禁用**（安全/可移植性） | 生产代码 **零使用**；15 处全在 tests/ | ✅ 合规 |

### 2025 Google Style 更新（lib-bitcask 影响）

- **PR #937**：`int* p` 而非 `int *p`——libbitcask 已合规
- **PR #914**：模板参数命名规范——需专项审

---

## 十一、Phase 5 行动建议（按 ROI 排序）

| # | 行动 | 对应发现 | 工作量 | ROI |
|---|---|---|---|---|
| 1 | **修 OrdSkipGuard 析构异常安全**（try-catch 包 submit；析构一处覆盖全部 7 使用点） | P5-MEM-1（HIGH） | 30 分钟 | 🔴 极高（消除 terminate 风险） |
| 2 | **修 checkpoint 漏更新 last_ckpt_ord_**（三处：is_stopped 同步分支 / 无池分支 / merge 两分支） | P5-MEM-2（MED） | 15 分钟 | 🔴 高（消除冗余 ckpt 触发） |
| 3 | **field_schema 去冗余别名 + hnsw.cpp 用 diskint::pwrite_all** | NEW-P4-1 + RED-7 残 | 30 分钟 | 🟡 中（T10 收尾） |
| 4 | **删除 flush_upto + reducer_loop:653-656 每任务通知块**（需先确认 unregister 路径不依赖） | P5-DL-3（复核升档） | 1 小时 | 🟡 中（reducer 热路径去每任务全局锁） |
| 5 | **thread-safety.md 文档化 done_mu/双-flush_mu_ 模式偏离** | P5-DL-1 + P5-DL-2 | 30 分钟 | 🟡 中（防回归） |
| 6 | **RED-5 搜索内核模板化**（须基准测试） | v2-leftover | 1 天 | 🟡 中（-130 行，热路径） |
| 7 | **RED-10 SnapCursor::vb 重构** | v2-leftover | 2 小时 | 🟢 低 |
| 8 | **RED-6 IVF/DiskANN open 骨架抽取** | v2-leftover | 半天 | 🟢 低 |
| 9 | **T12 HNSW ckpt 去重**（独立分支 + 三引擎测试门槛） | v2-leftover | 1 天 | 🟡 中 |

---

## 十二、Phase 5 — 验证方法学

1. **4 路并行深读 agent**：内存安全（10m42s）、死锁/并发（5m45s）、
   代码冗余（4m47s）、Google Style 最佳实践（2m29s，librarian）
2. **主 Agent 直接工具核验**：grep 全树检查 9 类 Google Style 表面合规项
   （goto/NULL/typedef/using namespace std/std::endl/dynamic_cast/
   reinterpret_cast/.release()/printf in src），全部 ✅
3. **关键发现交叉验证**：
   - P5-MEM-1（OrdSkipGuard 析构）：主 Agent 重读 `cask.hpp:1007-1012` + 4 个
     使用点逐个验证 armed 状态机
   - P5-MEM-2（last_ckpt_ord_ 漏更新）：主 Agent 重读 `cask.cpp:2215-2224`
     注释与代码逐行对比
   - P5-MEM-3（agent 误报 fflush/fsync 顺序）：主 Agent 重读
     `field_schema.hpp:240-249` 否定 agent 结论
4. **Phase 4 commit 逐一审计**：cc30b6c / 4dba518 / 8206aed / c4045e4 / de448c4
   每个 commit 的 diff 均经过内存 agent 深读

**核心教训（Phase 5 新增）**：

1. **"修一处带一处"是修复阶段的经典风险**：OrdSkipGuard 是 T7 的核心 fix，
   其析构却是新引入的 terminate 风险。RAII guard 的析构默认 noexcept，但调用
   了可抛的 submit。**根因：析构异常安全未在 review checklist 中**
2. **表面合规≠深层语义安全**：所有表面 idiom 检查全过，但异常路径不变量、
   文档化锁序模式偏离仍可被深读发现
3. **agent 也会误报**：P5-MEM-3（fflush/fsync 顺序）是 agent 误读代码顺序
   的典型案例。**主 Agent 必须复核关键 HIGH/MED 发现，不能全盘接受 agent 输出**
4. **Phase 4 → Phase 5 的最大价值是发现了共同盲区**：v2 不审异常路径不变量；
   Phase 4 修复时也未审"修复本身的异常安全"。下一轮（Phase 6）应建立
   "修复后回归审计"制度——每个修复 commit 必须经异常安全专项 review

---

## 十三、协议盲区（Phase 6 候选）

报告聚焦资源管理与并发，但 **Bitcask 协议正确性维度存在系统性盲区**——
basho/bitcask 生产史上最严重 bug 的来源。建议 Phase 6 专项审计：

1. **🔴 Tombstone/Merge 语义正确性**（basho #82 删除复活类、#149/174/175
   merge 竞态）
2. **🟠 fsync/fdatasync 纪律审计**（WAL 持久性、meta/ckpt 原子性、backup 一致点）
3. **🟠 Lock 文件健壮性**（空文件、PID 复用、disk-full 失败传播）
4. **🟡 CRC 回退路径完整性**（hint 失败回退扫描时是否做 CRC 校验）

这些是真正的 Bitcask 协议正确性问题，远比资源管理更危险。资源管理修复
（Phase 1-5）让代码库的"骨架"健康；Phase 6 应专注于"灵魂"。
