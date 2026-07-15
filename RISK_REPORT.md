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
>
> **Phase 6 修订（2026-07-15，文末新增）**：Phase 5 落地（T14-T18）后第三轮
> 3 路深读审计 + 主 Agent 对全部 HIGH/MED 发现逐条对抗复核。新增 2 项并发
> 确认发现（P6-MEM-1 / P6-DL-1，同根于 `IndexPool::flush()` 无界等待）、
> 1 项持久性发现（P6-DUR-1：hnsw 三处原子写缺 fdatasync）、6 项冗余；
> **推翻 1 项 agent 发现**（P6-DL-2，主 Agent 穷举论证不成立）；**修正本报告
> 自身 2 处基线错误**（reinterpret_cast「零」断言假阴性、RED-1 行数）。
> 详见文末「Phase 6 修订」段。

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

- ✅ **零** first-party `.release()` / `dynamic_cast` /
  `goto` / `using namespace std` / `std::endl` / 动态异常规格 / `NULL` 宏 /
  `typedef`
- ❌ ~~零 first-party `reinterpret_cast`~~ **Phase 6 修正：假阴性**——实测
  **183 处 / 33 文件**（POD 字节视图、SIMD 内核为主，用法本身多数合理，
  但「零」断言错误，见文末「基线修正」）
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
| RED-1 | HNSW flush() 与 SealedSegmentVectorPlugin flush() 逐字节相同 | `vector_plugin.cpp:511-555` vs `sealed_segment_vector_plugin.hpp:670-710` | ~115 行（Phase 6 实测修正：flush 35 + delta 31 + load ~50；原 ~90 系跨度重计） | T12 待独立分支 |
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
- 零 first-party `dynamic_cast`（Google Style 限制 RTTI）；
  ❌ ~~零 `reinterpret_cast`~~ **Phase 6 修正：实测 183 处 / 33 文件**（假阴性）
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
   （**Phase 6 修正**：其中 reinterpret_cast 一项为假阴性——实测 183 处，
   当时的 grep 未实际执行或统计口径错误。教训：核验清单里每一项都要留
   命令与输出存档，「✅」不能只凭记忆勾选）
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

---

# Phase 6 修订（2026-07-15）

> 方法：3 路并行深读 agent（内存 / 死锁 / 冗余，Google C++ Style 透镜，
> 明确排除 Phase 1-5 已修项）+ **主 Agent 对全部 HIGH/MED 发现逐环节对抗
> 复核**（本轮推翻 1 项、细化 1 项触发窗口——延续 Phase 5 教训 3）。
> 基线：Phase 5 落地后（commit 57b9878），641/641 ctest。
> 生产代码实测规模：src/ + include/ + c_api/ 共 **133 文件 / 41,466 行**
> （首页「208 文件 / ~74K 行」含 tests/tools/bench）。

## Phase 6 风险摘要

| 维度 | 高危 | 中危 | 低危 | 总评 |
|---|---|---|---|---|
| 内存/资源 | 0 | 1 | 2 | 无常态可达泄漏；bad_alloc 路径 1 处后果不可恢复 |
| 死锁 | 0 | 1 | 0 | 无新增可达死锁；close 逃生门存在结构缺口 |
| 持久性 | 0 | 1 | 0 | hnsw 三处原子写偏离全库 fdatasync 规范 |
| 冗余 | 0 | 3 | 3 | RED-2 残留 ~280 行，其中 3 处已实际漂移 |

**核心结构性结论（三路独立收敛）**：内存与死锁维度的全部确认发现落在
**同一个函数**——`IndexPool::flush()`（`thread_pool.hpp:435-443`）是全池
唯一既**无超时**、又**无 stopped_ 旁路**、且谓词依赖两个由不同站点维护的
原子变量的等待点。同池对照：`map_cv_` 有 stopped_ 旁路（:591-594，注释明言
"防与 sentinel 死锁"）、`checkpoint()` 有 T7 的 30s 超时、`close()` 第一段
有 S25-T1 的 30s 超时——唯独 `flush()` 三样都没有。一处修复（有界超时 +
stopped_ 旁路 + submit 异常补偿）同时消解 P6-MEM-1 与 P6-DL-1。

---

## 十四、Phase 6 — 内存与资源

### 🟠 P6-MEM-1：`IndexPool::submit` 的 in_flight 泄漏 → close() 永久挂死（MED，CONFIRMED）

**证据链（主 Agent 逐环节复核）**：
1. `thread_pool.hpp:428` 先 `in_flight.fetch_add(1)`，`:431` 才 `queue_.push`；
2. `queue_` 是 `tbb::concurrent_bounded_queue`（:217），push 内部按需分配
   segment，可抛 bad_alloc；
3. 上游无兜底：`Cask::submit_index_task`（`cask.cpp:782-785`）直通，put 路径
   全部调用点无 try/catch；
4. 泄漏后 `flush()` 谓词 `in_flight==0` 永假、wait 无超时、唯一 notify 站点
   `dec_in_flight`（:681）的配对 dec 永不到来 → `close()`/`~Cask` 永久挂死。

**对 T14 结论的修正**：T14 的 catch 防住了 terminate，但 in_flight 在抛出
**之前**已递增，catch 救不了它。T14 记录的代价「泄漏 1 个 ord（**可恢复**，
触发 30s 超时路径）」（`cask.hpp:1019-1023`）对 ord 成立、**对 in_flight
不成立**——后果是永久挂死，不可恢复。

**佐证**：`:565-567` 对 ring_put 拒收路径已有完全相同的补偿模式（注释
「补偿 in_flight，防 flush 悬挂」）——失效模式已被维护者理解，只是未覆盖
push 抛出窗口。

**修复方向**：`submit` 的 push 套 try/catch，catch 内 `dec_in_flight(lane)`
后重抛；同时给 `flush()` 加 30s 有界超时 + `|| stopped_` 旁路（与 T7 的
checkpoint 30s、map_cv_ 旁路同款模式）。

### 🟢 P6-MEM-2：`RowChunks::ensure_slot` 分配到接管之间两个异常窗口（LOW）

`row_chunks.hpp:81-93`：`new T[kChunkSize]` 后 `chunks_.push_back` 扩容可抛
→ chunk 无人持有；spine 表 `new T*[cap]` 后 `graveyard_.push_back` 可抛 →
ns 泄漏。`destroy()` 只遍历 chunks_/graveyard_/spine_pub_，泄漏对象从未进入
三者。触发限 bad_alloc。修法：`unique_ptr` 暂存再 `release()`。

### 🟢 P6-MEM-3：`MmapSegment::open` 在 `new` 抛出时泄漏整文件映射（LOW）

`segment_v2.cpp:444-448`：mmap 成功、fd 已关后，
`new MmapSegment()` 抛 bad_alloc → 映射（可达 GB 级虚拟地址空间）永久泄漏。
窗口仅 :444→:448 之间（其后所有早返回由 `~MmapSegment` 的 munmap 兜住，
那部分正确）。修法：把 `new` 提到 mmap 之前。

### 已排除嫌疑（主动否决记录，供后续审计复用）

- 裸 `FILE*` 写站点（hnsw.cpp:2016 / keydir.cpp:1561 / inverted.cpp:1261 /
  segment.hpp:525）：serialize 全在 fopen 之前，fopen→fclose 之间无抛出点
  → 风格问题非泄漏
- IvfSegment::open / HNSW load_vec_payload / load_qc_payload：逐条早返回
  核对，失败分支全部 close → 干净
- NodeChunk：locks 按声明序早于可抛成员初始化，成员回退正确 delete[]
- DataFile 移动语义：源 map_base_ 置空、赋值前 munmap → 无双重释放
- 循环引用：CaskPluginHost 持 Cask* 裸指针，插件持 PluginHost* 裸指针 → 无环
- read_files_ LRU / keydir limbo / text_plugin tomb 保留：文档化有意软上限
  （P9 / S29-6 / S27-4）

---

## 十五、Phase 6 — 死锁与并发

### 🟡 P6-DL-1：close() 的 30s 逃生门被紧随的无超时 flush() 抵消（MED，CONFIRMED-结构性）

`cask.cpp:645-660` 为 close 加 30s 超时（S25-T1，注释宣称"close 不再永久
阻塞"），超时 `break` 后 `:685-689` 的 `unregister_lib` 内含**无超时**
`flush(lane)`——挂死不是异常，外层 try/catch 兜不住。

**主 Agent 细化触发窗口**（`writes_in_flight_` 卡住 ≠ `lane->in_flight` 卡住）：
- 写线程被 kill 在 submit **之前** → 仅 writes_in_flight_ 卡住，逃生门有效；
- kill 在 `in_flight++`（:428）之后、push 返回之前 → **两计数俱卡，逃生门
  失效，永久挂死**；
- kill 在 push 之后 → 任务已入队，池照常消费，无事。

中间窗口有放大器：队列满（容量 10240）时 push **阻塞**，写者长时间停留在
该窗口内——而"线程看起来卡死所以被 kill"恰恰最易发生在此时。close 注释
自己论证的"被背压挡住的写者也会收敛，push 必然返回"（:641-643）以写者
存活为前提。另外 P6-MEM-1 的 bad_alloc 泄漏也进入同一挂死点。

**修复方向**：与 P6-MEM-1 同一处——flush() 有界超时 + stopped_ 旁路。

### ❌ P6-DL-2（主 Agent 推翻）：push_reorder 拒绝路径谓词永久不可满足——**不成立**

审计 agent 报告（PLAUSIBLE）：`:561-568` 拒绝分支 dec_in_flight 但不推进
applied_ord，而 submit 已 CAS 抬 hwm → 谓词永久不可满足。
**主 Agent 读 `ring_put`（:290-301）后穷举推翻**：
- **拒绝原因 A（`ord < ring_base` 回退）**：ring_base 只在 apply 时推进，
  故该 ord **已被 apply**，applied_ord ≥ ord；hwm 本次不会被抬（若该 ord 为
  历史最高早已在 hwm 内）。反证：要让本次把 hwm 抬到 X 且被回退拒绝，需
  X 为历史最高且 X < ring_base ≤ applied+1 → applied ≥ X = hwm，谓词为真。
- **拒绝原因 B（槽位已占，重复 ord）**：原始 entry 仍在 ring 中待 apply，
  其自身的 in_flight++ 尚未释放 → 此刻 in_flight ≥ 1，不存在"归零时带永假
  谓词 notify"的时刻；原始 entry apply 后 applied 恰推到 X = hwm。
- 无第三种拒绝：ord 超前走 `ring_grow`（:294）不拒绝。

agent 混淆了"这份副本被丢"与"这个 ord 无人 apply"。真正的 ord 空洞
（alloc 后从未 submit）是 T7/T14 已处理的已知类。**从发现清单划除，
记录在案防止后续轮次重报。**

### 已排除疑点（全部核实为真才排除）

- 锁序反转：无。concurrency-zh.md:530 两处反向嵌套的无环论证成立；
  BarrierGuard 逐分片排干协议正确（keydir.cpp:149-160 / 493-500 / 724-733）
- 持锁外调：无。reducer apply 前 unlock（thread_pool.hpp:632）、error_fn 在
  lock_guard 作用域外（:566）、builder 弹出 job 后即释放 b.mu
  （text_plugin.cpp:1514-1530）
- run_serialized 环死锁：契约「所有调用点均在 merge 线程」逐点核实为真
  （text_plugin.cpp:1392 / vector_plugin.cpp:564 /
  sealed_segment_vector_plugin.hpp:395 均在 on_merge_commit 内；
  merger.cpp:400 派发时不持 keydir 锁）
- BuilderPool cv 协议：所有唤醒点一律 notify_all，无丢唤醒

---

## 十六、Phase 6 — 持久性（对应「十三、协议盲区」第 2 项的提前兑现）

### 🟠 P6-DUR-1：hnsw 三处原子写缺 fdatasync——同文件两套持久性纪律（MED，CONFIRMED，主 Agent 亲验）

全库原子写规范由 `keydir.cpp:1565-1567` 定义并预先驳回"可重建就不用 sync"
的抗辩（「可重建 ≠ 可以不 fdatasync」，与 SearchCheckpoint / write_manifest
同款语义）。9 个原子写站点中 6 个遵守，**hnsw.cpp 三处偏离且无豁免注释**：

| 站点 | rename 前同步 |
|---|---|
| `keydir.cpp:1559-1581` | fflush + fdatasync ✅ |
| `search_checkpoint.hpp:210-230` | fflush + fdatasync ✅ |
| `index_manifest.hpp:167-186` | fdatasync + 目录 fsync ✅（唯一做目录 sync 的） |
| `field_schema.hpp:228-243` | ::fsync ✅（唯一用 fsync 非 fdatasync） |
| `segment_v2.cpp:942-949` / `:376-382` | fflush + fdatasync ✅ |
| **`hnsw.cpp:2015-2023`（save）** | **无** ❌ |
| **`hnsw.cpp:491-538`（save_vec_payload）** | **无** ❌ |
| **`hnsw.cpp:564-588`（write_bcq8_file）** | **无** ❌ |

**同文件自相矛盾（决定性证据）**：hnsw.cpp 的 fd 增量追加路径
（:1613 / :1636 / :1782 / :1816）**全部 fdatasync**；FILE* save 路径全不做。

**后果比"少一次加速"重**：rename 前不 sync，崩溃后**最终文件名下可能是
零长/半截文件，而旧的好文件已被 rename 覆盖**。实际严重度取决于 load 对
损坏文件的容错（CRC 失败退回重建则为重建成本，否则为 load 失败），但与
规范的偏离确凿。

**修复方向**：三处补 fflush + fdatasync（30 分钟）；结构性防复发见 P6-RED-1。

---

## 十七、Phase 6 — 代码冗余（RED-2 残留 + 新发现）

> 根因单一：**T10 只做了 RED-2 的一半**。`file_util.hpp` 仅 33 行
> （FileCloser + FilePtr），其头注释 :12-13 自承欠 `read_file_bytes` /
> `atomic_write_bytes`——两者全仓库不存在。「漂移温床」预言已兑现三处
> （P6-DUR-1 的 fsync 分叉、P6-RED-3 的 need 公式漂移、P6-RED-4 的注释断言
> 失去结构保证）。

### 🔴 P6-RED-1：原子写样板 ×9，fsync 策略已分叉（~55-70 行，CONFIRMED）

站点清单见 P6-DUR-1 表。归并需**两个** helper：
`atomic_write_bytes(path, span)`（4 份 buffer 式）+ `AtomicFileWriter` RAII
（5 份流式：构造开 tmp，commit() 做 flush+fdatasync+rename，析构未 commit
则 remove）。收益不止行数：sync 策略从 9 处可审收敛为 1 处可审。

### 🔴 P6-RED-2：整文件读样板 ×6，轮子已造好被关在 migrate.cpp（~49 行，CONFIRMED）

`migrate.cpp:45-59` 的 `read_all(path) -> expected<vector<byte>, string>`
**就是**缺失的 read_file_bytes，困在匿名 namespace。其余 5 站点：
hnsw.cpp:2031-2041 / keydir.cpp:1588-1599 / inverted.cpp:1272-1286 /
segment_v2.cpp:954-966 / search_checkpoint.hpp:326-338。
障碍（小）：元素类型分叉 `vector<uint8_t>` vs `vector<byte>`，零
reinterpret_cast 策略下需 `template <class Byte>`。
已排除：search_checkpoint 的 read_selected 按 section seek 非整读；
segment.hpp:525 是 4 字节 magic 探针。

### 🟡 P6-RED-3：chunked-pread refill lambda ×3，已漂移（~55 行，CONFIRMED）

hint_file.cpp:143-173 / :244-267、data_file.cpp:309-335。两份注释自承抄袭
（data_file.cpp:295「照搬 hint_file.cpp 的 refill 模式」）。
**漂移实证**：`need` 公式 data_file.cpp:322 掉了 `buf_len +`
（hint_file 两份均为 `std::max(desired, buf_len + read_size_hint)`）。
逐行核实**当前无害**（memmove 后 record 从 buf 头起算），但证明三份在被
分别维护。归并：`detail::ChunkedReader{file_, end_bound}`，唯一参数化点是
文件末界。

### 🟡 P6-RED-4：Analyzer 双出口成对复制 ×2 组（~88 行，CONFIRMED）

- **4a** `NgramAnalyzer::analyze_with_positions`（analyzer.cpp:211-285）vs
  `analyze`（:292-350）：~48 行重复。**真实风险在注释**——:287-291 的 S29-8
  注释断言两版「term 集与 tf 值逐位一致」，这是索引路径（positions 版）与
  BOW 查询路径（tf 版）必须成立的不变量，却靠复制粘贴维护——改一处过滤
  语义忘另一处 → 索引与查询 term 集静默分叉，评分错误无人察觉。
  S29-8 对双入口的性能论证成立（tf 版避免每 n-gram 一次 positions 堆分配），
  归并用 `template <class Sink>` + `if constexpr` 可保零分配。
- **4b** `WhitespaceAnalyzer::analyze_with_positions`（:356-398）vs
  `analyze_with_offsets`（:400-443）：前 39 行完全相同，仅 4 行 sink 差异。
  **仓库内已有先例**：JiebaAnalyzer 的 `collect_tokens(text, need_offsets)`
  （jieba_analyzer.cpp:126）已解决同一问题，两个公开入口各剩 8 行。
  ~40 行，照抄先例 1 小时。

### 🟡 P6-RED-5：`decode_rec` vs `decode_rec_list` 共享解包段（~35 行，CONFIRMED）

segment_v2.cpp:619-653 vs :1007-1041 逐字节相同（块循环 + BitReader 解包 +
全部边界守卫）。`PostingList` 是 `FlatPostings` 的结构超集（ords/tfs/blocks/
max_tf 同名同类型），`template <class Out>` 单态化零性能代价，
`if constexpr (requires { out.dls; })` 保住热路径 dl 跳过。真分叉部分
（blocks 重建 vs dls+positions 解码）留在 helper 外。

### 🟢 P6-RED-6：死代码 7 行（全部 exact-grep 验证）

| 项 | 位置 | 行 |
|---|---|---|
| `detail::ends_with_vowel` | porter_stemmer.hpp:52-55 | 4（全树仅定义） |
| `KeyDir::newest_folder_epoch_` | keydir.hpp:585 + keydir.cpp:923 | 2（仅写零读；疑为漏掉的 IterInfo 导出字段而非有意诊断位） |
| `codec::DecodeError::kValueSizeOverflow` | codec.hpp:34 | 1（构造不可达，codec.cpp:465-467 折进 kKeySizeOverflow） |

### backlog 复核（RED-3/5/6/10 全部仍成立）

- RED-3：三份 LE 编解码并存（byte_order.hpp:14-48 / search_checkpoint.hpp:136-163 /
  index_manifest.hpp:76-99），byte_order.hpp 补 vector-append 形态即可归并
- RED-5：hnsw.cpp:847 / :953 成对复制（:951 注释自承）
- RED-6：ivf_rq.cpp:475 / diskann.cpp:472 骨架同构
- RED-10：keydir.cpp:1297-1307 SnapCursor::vb，价值低维持 LOW

### 已核实非冗余（新增记录）

- SIMD 内核家族（bm25_kernels / int8_kernels / hw_crc32）：每份挂不同
  `target()` 属性、用不同 intrinsic 类型——非冗余
- 冗余状态维度零发现：dead_count_ / has_pending_ / row_chunks::size_ 等
  嫌疑点均为 O(1) 缓存或无锁发布点，且注释预先驳回了归并方案
  （keydir.hpp:576-579 典型）

---

## 十八、基线修正（对本报告与 TASK.md 自身）

1. **reinterpret_cast 假阴性**：「零 first-party reinterpret_cast」（原
   §干净项、§十、§十二）实测为 **183 处 / 33 文件**。dynamic_cast 确为 0。
   已在原文标注修正。教训：核验清单每项须留命令与输出存档。
2. **RED-1 行数**：原 ~90 行系对 35 行函数的跨度重计；实测可回收 ~115 行
   （flush 35 + save_component_delta 31 + load_component ~50，其中链重放
   lambda 逐字节相同）。已在 §九 表格标注修正。
3. **T14「可恢复」表述**：TASK.md 与 cask.hpp:1019-1023 记录的「泄漏 1 个
   ord（可恢复，触发 30s 超时路径）」对 in_flight 计数器不成立
   （见 P6-MEM-1）。TASK.md 已随 Phase 6 重写修正；cask.hpp 注释随 T19 修正。
4. **规模口径**：首页「208 文件 / ~74K 行」含 tests/tools/bench；生产代码
   实测 133 文件 / 41,466 行。

---

## 十九、Phase 6 行动建议（按 ROI 排序）

| # | 行动 | 对应发现 | 工作量 | ROI |
|---|---|---|---|---|
| 1 | **flush() 有界超时 + stopped_ 旁路；submit push 异常补偿 dec** | P6-MEM-1 + P6-DL-1（一处修复双消解） | 半天 | 🔴 极高（消除两条永久挂死路径；顺带补齐 T8 重设计欠缺的超时基建） |
| 2 | **hnsw 三处补 fdatasync** | P6-DUR-1 | 30 分钟 | 🔴 高（独立持久性修复，不依赖重构） |
| 3 | **T10 收尾：read_file_bytes + AtomicFileWriter** | P6-RED-1/2 | 1 天 | 🟡 高（sync 纪律 9 处→1 处可审，结构性防 P6-DUR-1 复发） |
| 4 | **WhitespaceAnalyzer 照 Jieba collect_tokens 先例归并** | P6-RED-4b | 1 小时 | 🟡 中 |
| 5 | **NgramAnalyzer sink 模板归并**（保零分配，须对拍 term/tf 逐位一致） | P6-RED-4a | 半天 | 🟡 中（把注释断言变成结构保证） |
| 6 | **ChunkedReader 归并 refill ×3** | P6-RED-3 | 半天 | 🟡 中 |
| 7 | **decode_rec 模板归并**（须基准回归） | P6-RED-5 | 半天 | 🟢 低 |
| 8 | **死代码 7 行删除** | P6-RED-6 | 10 分钟 | 🟢 低 |
| 9 | RowChunks / MmapSegment 异常窗口修补 | P6-MEM-2/3 | 各 15 分钟 | 🟢 低（可随 #1 同 commit） |

**Phase 6 不做**：T8 重设计（前置：可复现饥饿的失败注入测试 + applied_ord
与搜索可见性根因调查——#1 的超时基建是其前置之一）、T12（独立分支既定）、
RED-3/5/6/10（backlog 维持）。

**方法学教训（Phase 6 新增）**：
1. **agent 发现必须逐环节对抗复核**：P6-DL-2 推理链条看似完整（拒绝分支
   确实不推进 applied_ord），但对 ring_put 拒绝语义做穷举后两个分支均
   构造不出终态——"这份副本被丢"≠"这个 ord 无人 apply"。
2. **冗余审计能挖出正确性问题**：P6-DUR-1（fdatasync 分叉）是"找重复代码"
   的副产物——样板未归并导致纪律靠人肉复制，复制必漂移。
3. **超时/旁路要按等待点清点，不能按函数清点**：close() 有超时、checkpoint()
   有超时、map_cv_ 有旁路，全部单独看都"已修"——但同一线程先后两个等待点，
   只要有一个无界，前面所有超时都归零。
