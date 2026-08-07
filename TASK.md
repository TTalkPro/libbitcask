# S37：Windows 移植（MSVC 原生 · x64 · 保 SIMD）开发任务清单

> 来源：[`doc/windows-port-design-zh.md`](doc/windows-port-design-zh.md)（设计稿已定稿）
> 决策基线（2026-08-07 拍板）：**MSVC 原生 ABI**（不考虑 MinGW）、**Windows 上保留 SIMD**
> （不接受 scalar 退化）、**不做 ARM64 Windows / 32 位 x86**
> 目标平台：Windows x64 / VS2022 17.6+；Linux 侧行为与性能**零回归**是全程硬约束
> 基线测试：ctest 全绿（v6.1.0 基线，Debug 全量）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警；公共结构体改动须 build-rel 双树验证；
> I/O 语义改动须过 crash 注入用例

---

## 📁 上一清单归档

S33（有序 Key 索引 OKI）、S34（TxnCask）、S35（引擎原生原子批）、S36（keydir 磁盘驻留
Level B）四届清单已全部收官，随 **v6.1.0** 发布，详见 git 历史（本文件在 056127b
之前的版本）与 `CHANGELOG.md`。

**未完项带入本清单遗留区**：T8（搜索读屏障，⏸ 4 项前置未满足）、
T12（HNSW ckpt 去重，⏸ 默认不做）。两项均与本届无耦合。

---

## 🧭 本届总纲

设计稿把移植拆成 A（构建）/ B（SIMD 派发）/ C（系统调用）/ D（测试）四段，
落到执行序上是 7 个任务。**关键排期事实：S37-1 / S37-2 / S37-3 三项共约 5 周
可完全在 Linux 上完成并验证**，且每项对现有代码库都是净收益 —— 应先行吃掉，
把风险最高的 S37-4 / S37-5 攻击面压到最小。

### 面积实测（2026-08-07，开工前重新量过）

初版设计稿的 `::open` 计数把 `HintFile::open` / `MmapSegment::open` /
`OkiRunReader::open` 一类**成员函数**误算在内。剔除后的真实裸 POSIX 调用面：

| 调用 | 处数 | 主要分布 |
|---|---|---|
| `::close` | 53 | 向量插件错误路径（一个 open 对应 6-8 个 close）|
| `::open` | **13** | hnsw ×4、ivf_rq ×2、diskann ×2、segment_v2 ×1、file_lock ×1、file_util ×1、sealed_segment_vector_plugin ×1、posix_file ×1 |
| `::fstat` | 8 | 同上，全部用于取文件大小（1 处取 dev/ino）|
| `::fdatasync` | 8 | file_util `flush_and_sync` 为主 |
| `::ftruncate` | 4 | file_lock、vec 追加回滚 |
| `::pread`/`::pwrite` | 4 / 3 | posix_file + file_lock |
| `::lseek` | 3 | posix_file |
| `::stat`/`::unlink`/`::madvise` | 2 / 2 / 2 | — |
| `::mmap`/`::munmap`/`::fsync`/`::fileno` | 各 1 | 已收敛（S36-B3）|

**结论：核心 KV 路径（data_file / hint_file / oki_run / segment）早已走
`io::PosixFile`，裸调用高度集中在向量插件（13 个 open 里占 8 个）。**
S37-1 的实际工作量因此低于设计稿估计。

---

## 🔴 S37 主线

### 🟡 S37-1 — 平台抽象层收编 🔴 HIGH（先行，纯 Linux 可交付）

把散落的裸 POSIX 调用全部收进 `bitcask::io`，使「Windows 后端」= 新增一个
`src/io/win32_file.cpp`，而不是改 20 个文件。**Linux 行为零变化**。

`io.hpp` 需补齐的 API（当前缺口）：

1. `OpenFlag` 补 `kTruncate`(O_TRUNC) / `kWriteOnly`(O_WRONLY) —— 向量插件的
   `O_RDWR|O_CREAT|O_TRUNC` 与 `O_WRONLY` 形态现无法表达，是这些站点绕过抽象的直接原因。
2. `PosixFile::size()` —— 收编 8 处 `::fstat`（其中 7 处只为取大小）。
3. `PosixFile::identity()` → `io::FileIdentity{dev, ino}` —— 收编 hnsw `.vec`
   追加目标身份校验，同时把 `<sys/types.h>` 逐出**公开头** `hnsw.hpp:44`。
4. `PosixFile::truncate(len)` —— 显式长度版（现仅有 `truncate_here()`），
   收编 `file_lock` 的 `ftruncate(fd,0)` 与 vec 追加回滚。
5. `io::sync_data(int fd)` —— 收编 8 处 `::fdatasync`。
6. `io::sync_directory(path)` —— 收编 `file_util.hpp:96` 的 `O_DIRECTORY` 开目录 fsync
   （Windows 下将降为 no-op，见设计稿 C5）。
7. **`io::atomic_rename(from, to)`** —— 收编 `file_util.hpp:128/200` 的 `std::rename`。
   **这是 C1（最高危项）的落点**：Windows 上 `std::rename` 目标存在即失败，
   9 个原子写站点会全线挂。提前把它放到 seam 后面，Windows 侧只需改这一个函数。
8. `MappedFile::prefetch(off, len)` —— 收编 `hnsw.cpp:1406` 的 `MADV_WILLNEED`。
9. `io::page_size()` —— 收编 `hnsw.cpp:1380` 的 `::sysconf(_SC_PAGESIZE)`
   （Windows 侧需区分 `dwPageSize` 与 `dwAllocationGranularity`，见设计稿 C7）。

- **工作量**：2 周（面积实测后下修，原估 2 周含 51 个 open 的误算，实际净工作量约 1 周 + 验证）
- **验收**：ctest 全绿（Debug 全量 + ASan）；bench 零回归（hot get / BOW / hnsw）；
  build-rel 双树（`hnsw.hpp` 公开头改动）；`grep` 确认第一方裸 POSIX 调用归零

#### ✅ S37-1 落地记录（2026-08-07）

**seam 建立**：`io.hpp` 从「POSIX 文件 I/O 包装」升格为平台 seam——全库唯一允许
直接调用宿主原语的地方是其实现文件（POSIX `src/io/posix_file.cpp`；Windows 将是
`src/io/win32_file.cpp`）。核验：第一方 `src/` `include/` `c_api/` `tools/` 的裸宿主
原语与 POSIX 头**双双归零**（唯一 grep 命中是成员函数 `SealedSegmentVectorPlugin::open`
的误报）。

**新增 API**：
- 类型：`FileHandle`（+ `kInvalidHandle` / `handle_valid`）、`FileIdentity`、`FileMode`
- `PosixFile` → `File`（`PosixFile` 留别名，既有 65 处引用零改动）+ `release()` /
  `truncate(len)` / `size()` / `identity()`
- `OpenFlag` 补 4 位：`kWriteOnly` / `kTruncate` / `kCloseOnExec` / `kSyncAll` / `kNoAppend`
- 句柄级：`open_handle` / `close_handle` / `sync_data` / `truncate_handle` /
  `handle_size` / `handle_identity` / `path_identity` / `pread_all` / `pwrite_all` /
  `pread_once` / `pwrite_once`
- 路径级：**`atomic_rename`（C1 落点）** / `sync_directory` / `flush_and_sync_stream` /
  `remove_file` / `prefetch_range` / `page_size`
- 进程级：`current_process_id` / **`process_alive`（C4/风险#2 落点）** / `max_open_files`

**收编站点**：`file_util.hpp`（原子写 ×2 + 目录 fsync + fdatasync）、`vec_disk_internal.hpp`
（`pread_all`/`pwrite_all` 下沉，`TmpFile` 换句柄类型；数十处调用站点零改动）、
`ivf_rq.cpp`、`diskann.cpp`、`hnsw.cpp`（站点最多）、`segment_v2.cpp`、`file_lock.cpp`、
`cask.cpp`（进程原语）。`hnsw.hpp` 的 `dev_t`/`ino_t` 换 `io::FileIdentity`，
**`<sys/types.h>` 逐出公开头**（原污染所有下游用户）。

**三处「像机械替换、实则会漂」的语义，已原样保留**（收编的主要风险都在这里，
非 API 翻译）：
1. **`O_APPEND` × `pwrite`**：`file_lock` 原用 `O_CREAT|O_EXCL|O_RDWR|O_SYNC`
   （**无** `O_APPEND`），而 `OpenFlag::kCreate` 带 `O_APPEND`。POSIX 下
   **`O_APPEND` 会让 `pwrite` 忽略 offset 改为追加**——锁文件的 `ftruncate(0)` +
   `pwrite(0,…)` 覆盖写语义会被悄悄改掉（当前因先截断到 0 而结果碰巧一致，
   但已不由 offset 决定）。为此新增 `kNoAppend`。
2. **`O_SYNC` ≠ `O_DSYNC`**：库内其余写路径的 `kOSync` 映射到 `O_DSYNC`（S13-P2），
   而写锁需要元数据也同步。为此新增 `kSyncAll`，未合并两者。
3. **单次 pwrite ≠ 循环 pwrite**：`file_lock::write_data` 的 legacy 契约明写
   「不循环，部分写也算成功」，套 `pwrite_all`（重试补齐）或 `File::pwrite`（循环）
   都会改掉它。为此新增 `pread_once`/`pwrite_once` 与 `*_all` 并存。

**两处刻意的收紧**（非纯搬运，已在代码注释标注）：
- 4 处「开文件后 `::read` 读头部」改为 `pread_all` 定位读：消除对 fd 内部偏移的
  依赖（正是 C3 要解决的形态），且短读由失败改为先重试补齐（更严，非放松）。
- hnsw 两处 payload load 的身份收养由「按路径 `::stat`」改为「按已持有句柄
  `handle_identity`」：排除「校验与收养之间路径被外部替换」的窗口。

**构建**：`bitcask_format` 补 `PUBLIC bitcask_io`——`file_util.hpp` 经 `index_manifest.hpp` /
`field_schema.hpp` / `oki_run.hpp` / `search_checkpoint.hpp` 传播到几乎所有下游，
逐 target 补链接会变成打地鼠。无环（`bitcask_io` 不依赖 format）。

**验收**：Debug 全量 **725/725**、**ASan 全量 725/725**、build-rel 双树零错误
（1 项 S30RssProbe 预存 Disabled）。

### S37-2 — 测试解耦：fork → exec-self 🔴 HIGH（纯 Linux 可交付）

4 个测试文件用 `fork()` + `_exit()` 模拟崩溃恢复，是全套中最有价值的一批
（WAL / torn tail / 墓碑复活门 / 原子批区间提交）：
`crash_recovery_test.cpp`、`oki_levelb_test.cpp`（`:476` `:565` `:787`）、
`oki_recovery_test.cpp`、`txn_test.cpp`（`:182`）。

改为「子进程 = 重新 exec 测试二进制自身 + `--bitcask-child-scenario=<name>` argv 分发」，
父进程 `posix_spawn` + `waitpid`（Windows 侧换 `CreateProcess` + `WaitForSingleObject`）。

> ⚠️ **实质工作量在语义差异**：exec-self 下子进程不再继承 fork 时刻的内存状态
> （已建好的 keydir、已打开的句柄）。每个 case 需逐个确认能否改为「子进程自己重建」，
> 不能的要重新设计断言。这不是机械改写。

顺带清理：`::getpid()` 造临时目录（4 处）、硬编码 `/tmp`（`scanner_test.cpp:53`）、
`<dirent.h>`（`thread_pool_test.cpp`）、直接 `stat` 断言 inode
（`cask_docvalue_test.cpp:38`、`hnsw_test.cpp:38`，改用 S37-1 的 `FileIdentity`）。

- **工作量**：1 周
- **验收**：ctest 全绿；4 个崩溃场景的原有断言意图全部保留（逐条对照 review）

#### ✅ S37-2 落地记录（2026-08-07）

**骨架**：`tests/support/crash_child.{hpp,cpp}` + `crash_main.cpp`，聚成
`bitcask_test_crash_child` 静态库（用它的目标链 `GTest::gtest` 而非 `gtest_main`
——需在 `InitGoogleTest` 之前拦截参数分发）。子进程 = `posix_spawn` 本测试二进制
自身 + `--bitcask-crash-child=<场景> --bitcask-crash-dir=<路径>`。
Windows 分支（`CreateProcessW`）已留位并标注 S37-5。

**6 个 fork 点全部转换**：`crash_recovery`(mid_put)、`txn`(txn_crash_after_commit)、
`oki_recovery`(oki_crash_tail)、`oki_levelb`(b1_checkpoint_fsync /
crash_after_merge / retired_files_crash)。事前担心的「依赖 fork 时刻内存快照」
**未发生**——6 个场景全部形如「`Cask::open(dir)` → 干活 → `_exit(N)`」，唯一
输入是目录路径。

**⚠️ 本届最值得记住的一条：`return N` ≠ `_exit(N)`，且失败形态是静默的**

改造初版让场景函数 `return` 退出码。**编译通过、6 个里 4 个照样 PASS。**
但 `return` 会正常展开栈 → 局部 `Cask` 析构 → `close()` → **OKI flush +
写锁释放**，注入的崩溃态当场消失：那 4 个测试的断言在「干净关闭」下同样成立，
于是**不声不响地不再检验崩溃路径**。只有 2 个（OKI tail 重放 / B1 不变量）
因为断言了「必须存在未固化的尾巴」才炸出来。

排查证据：子进程跑完后目录里**没有 `bitcask.write.lock`**、且已出现 `seg-2`
（第二段被 flush 成 run）——正常关闭的指纹。

**修法是把契约做成强制的**，而非写进注释靠自觉：
- 场景函数返回类型改 `void`，退出一律经 `crash_exit(code)`（`_Exit`，不展开栈）；
- 场景函数若正常走到结尾（忘了调），骨架带诊断以 122 退出，**不当作成功**。

修后复验：子进程目录里写锁残留、只有 `seg-1`，与原 fork 版指纹一致。

**顺带清理的 POSIX 依赖**（新增 `tests/support/test_paths.hpp`）：
- `::getpid()` ×9 文件 → `test_pid()`（走库的 io seam，Windows 已覆盖）
- 硬编码 `/tmp` 负面路径（`scanner_test`）→ `nonexistent_path()`
- `<dirent.h>` 枚举 `/proc/self/task`（`thread_pool_test`）→ `std::filesystem`；
  **`/proc` 本身无 Windows 对应物**，已标注 S37-5 需换
  `CreateToolhelp32Snapshot` 或把该用例整体标 Linux-only
- `::stat` 断言 inode/size（`hnsw_test` ×4、`cask_docvalue_test` ×3）→
  `identity_of()` / `file_size_of()`；`::truncate` → `fs::resize_file`
- `tests/` 下 POSIX 头（`<unistd.h>`/`<sys/wait.h>`/`<sys/stat.h>`/`<dirent.h>`）
  **归零**

**W1 现状复核**：exec-self 已让 fd 继承成为真实语义面，但**当前 6 个场景不触发**
——父进程都是「先 spawn 后重开」，spawn 时手里没有打开的 Cask。W1 仍待评估。

**验收**：Debug 全量 **725/725**（含 6 个崩溃场景）；ASan 全量 725/725。

### S37-3 — SIMD 派发层分 ISA TU 🔴 HIGH（大部分纯 Linux 可交付）

**当前状态：MSVC 下所有 SIMD 内核被 25 处 `#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))`
守卫整体编译掉，静默退化为 scalar 且不报错。** 即「能编过」与「有 SIMD」差一整个阶段。

1. 新建 `src/simd/`：`cpu_features`（CPUID 探测）+ `kernels_{sse42,avx2,avx512,vnni}.cpp`
   + `dispatch.cpp`（函数指针表）。CMake 按 TU 施加 `/arch:`（MSVC）或 `-m*`/target 属性（GCC/Clang）。
2. **3 个公开头 + 1 个内部头交出内联内核**：`bm25_kernels.hpp`、`detail/int8_kernels.hpp`、
   `hw_crc32.hpp`、`src/vector/hnsw_kernels.hpp` —— 跨 TU 边界迁移，内联被切断处须实测无回退
   （`bm25_kernels` 在 BOW 热路径上最需盯）。
3. `__builtin_cpu_supports`（18 处）→ 自实现 CPUID。
   **必须同时检查 `XCR0`**（AVX 需 OSXSAVE + XCR0[2:1]，AVX-512 还需 XCR0[7:5]）——
   `__builtin_cpu_supports` 替我们做了这步，手写漏掉会在「CPU 支持但 OS/hypervisor
   未启用 YMM/ZMM 保存」的机器上直接 `#UD`，且只在特定虚拟化环境复现。**须写单测**。
4. **收紧 AVX-512 运行时门为 `F && CD && BW && DQ && VL`**：MSVC 的 `/arch:AVX512`
   隐含此集合，比代码当前 `target("avx512f")` 的窄集更宽，编译器可能在胶水代码里
   生成 BW/DQ/VL 指令而门只查了 F → 在仅 F+CD 的 CPU 上 `#UD`。
5. 机械替换：`__builtin_ia32_pause`→`_mm_pause`、`__builtin_popcount*`→`std::popcount`、
   `__builtin_memcpy`→`std::memcpy`、`__attribute__((noinline))`→宏、
   `#pragma GCC visibility`（`c_api/internal.h`）删除（靠已有 `BITCASK_API`）、
   25 处 `__x86_64__` 守卫 → `BITCASK_X86_64` 宏（注意 `cask.cpp:1253` 未带编译器条件）。
6. **新增 `BITCASK_SIMD_MAX=scalar|sse42|avx2|avx512` 强制降档开关**：让 CI 在同一台机器上
   把所有 ISA 档位都跑一遍对拍，而不是听天由命看 runner 的 CPU 型号。Linux 侧同样受益。

- **工作量**：2-2.5 天设计 + 2 周实现
- **验收**：全 ISA 档位对拍（`int8_kernels` 既有 `self_test` + BM25/intersect/crc32
  scalar-vs-SIMD）；bench 零回归；**仓库既有纪律「改评分算法必须过三方穷举对拍」在此适用**

> **执行拆分（2026-08-07）**：本项拆两步做。搬内核之前**必须先有对拍手段**——
> 否则搬完无从验证（见下方 3.a 的发现）。
> - **3.a** `cpu_features` + `BITCASK_SIMD_MAX` + 单测 + 派发点切换（纯增量，
>   派发逻辑不变，只换探测来源）✅ done
> - **3.b** 内核出头文件 → 分 ISA TU + `/arch:` 施加 + 机械替换 ⬜ 未开始

#### ✅ S37-3.a 落地记录（2026-08-07）

**开工即发现的盲区**：本机 CPU 为 `avx2 avx_vnni fma pclmulqdq sse4_2`，
**无 AVX-512**；`ci.yml` 也无任何 ISA 相关配置。即：仓库里的 **AVX-512 内核
从未在本地或 CI 上被执行过**——改错了不会红。这比设计稿预估的更严重，
也正是「先建对拍手段再搬内核」的直接理由。

**新增**：`include/bitcask/detail/cpu_features.hpp` + `src/simd/cpu_features.cpp`
（新 `bitcask_simd` 库，由 `bitcask_format` PUBLIC 传播——`codec.cpp` 同时包含
`hw_crc32.hpp` 与 `int8_kernels.hpp`）。三重过滤后的能力位：
CPUID 报告 ∧ XCR0 表明 OS 已启用相应寄存器状态 ∧ 未被 `BITCASK_SIMD_MAX` 钳掉。

**验证策略——与 `__builtin_cpu_supports` 逐位对拍**：`cpu_features_test` 的
`MatchesCompilerBuiltin` 把手写 CPUID 的 11 个位与编译器内建对拍。这是关键的
一步：GCC/Clang 的内建是**替我们做过 XCR0 检查**的权威实现，MSVC 上没有——
只有在 GCC/Clang 上对拍通过，才有底气到 MSVC 上只留手写版。本机通过，
含最易抄错的 **AVX-VNNI（leaf 7 subleaf **1** 的 EAX bit 4，与 AVX512-VNNI 的
leaf 7.0 ECX bit 11 位置迥异）**。

**AVX-512 门已收紧**为 `F && CD && BW && DQ && VL`（原仅查 `avx512f`），
理由见设计稿 §2.5。`have_avx512()` 单一出口，5 个派发点全部随之收紧。

**`BITCASK_SIMD_MAX` 行为**（实测）：`scalar/sse42/avx2/avxvnni` 下调生效；
`avx512/avx512vnni` 在无该硬件的机器上正确保持 `avxvnni`（**只降不升**）；
拼错档位 **SIGABRT**（不做「警告后忽略」——CI 矩阵里一个拼错的档位若被静默
忽略，那个 job 会在满档下跑却显示为在测低档，即「测了个寂寞还报绿」）。

**派发点切换**：`src/` + `include/` 的 18 处 `__builtin_cpu_supports` 与
3 处 `__builtin_cpu_init` **归零**（`hw_crc32` / `bm25_kernels` / `int8_kernels` /
`hnsw.cpp` / `vector_plugin.cpp` / `intersect.cpp` / `index.cpp`）。
`bench/avx512_verify_bench.cpp` 等诊断 bench 暂留（默认不构建，S37-3.b 一并处理）。

**验收——本届第一次真正的跨档对拍**：全套 **732/732** 在
`scalar` / `sse42` / `avx2` / `avxvnni` 四档下**逐档全绿**（此前只有 avxvnni
一条码路被执行过）。build-rel 双树零错误（三个公开头改动）；ASan 732/732。

**仍未覆盖**：AVX-512 / AVX512-VNNI 两档——降档开关只能往下钳，覆盖它们必须
有带 AVX-512 的机器（S37-7 的 CI 矩阵）。这是 3.b 搬内核时的**已知风险敞口**。

#### ✅ S37-3.b 落地记录（2026-08-07）

**24 个 `__attribute__((target))` 函数全部搬进分 ISA TU**，第一方 GCC 扩展归零。

新增 CMake 函数 `bitcask_simd_tu(<源文件> <档位>)`（`avx2|avx512|vnni|vnni512|sse42`）
统一施加 `-m*`（GCC/Clang）或 `/arch:`（MSVC）。**GCC/Clang 侧也改用 `-m` 开关而
非保留 target 属性**——两边一套结构，免得维护两份。

新增 12 个分 ISA TU：

| 模块 | 新 TU | 档 |
|---|---|---|
| hnsw 距离 | `hnsw_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| int8 点积 | `int8_kernels_avx2/_vnni/_vnni512.cpp` | avx2 / vnni / vnni512 |
| BM25 打分 | `bm25_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| 求交 | `intersect_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| 向量归一化 | `vector_plugin_kernels_avx2/_avx512.cpp` | avx2 / avx512 |
| CRC32 折叠 | `simd/crc32_sse42.cpp` | sse42 |
| Index gather | `index_kernels_avx2.cpp` | avx2 |

**3 个公开头 + 1 个内部头交出内联内核**（`bm25_kernels.hpp` / `detail/int8_kernels.hpp` /
`hw_crc32.hpp` / `hnsw_kernels.hpp`）——它们经 `format.hpp`/`hnsw.hpp`/`codec.cpp`
传播到大量 TU，内核留在头里就无法对包含者分别施加 ISA 开关。

**顺带修掉的一个潜在 SIGILL**：`Index::fill_is_live` / `fill_doc_lens` 原**整个成员
函数**带 `target("avx2")` 却被**无条件调用**——该属性允许编译器在函数任何位置
（含运行时门之后才该走的 scalar 回退）生成 AVX2，在无 AVX2 的 CPU 上即 SIGILL。
反汇编确认当前 GCC -O3 未真的越界生成（**潜在风险而非现行 bug**），但结构上不
该依赖编译器的克制；内核拆出后调用方是普通 TU，编译器无权在其中生成 AVX2。

**机械替换**：`__builtin_ia32_pause`→`_mm_pause`（2 处）、`__builtin_popcount*`→
`std::popcount`、`__builtin_memcpy`→`std::memcpy`、`__attribute__((no_sanitize))`→
`BITCASK_NO_SANITIZE` 宏、25 处 `#if defined(__x86_64__) && (GNUC||clang)` →
`BITCASK_X86_64`（后半个条件本就只因内核用了 GCC 扩展而存在）。

**踩到的一个构建坑**：分 ISA TU 与**预编译头冲突**——PCH 在目标级用统一选项
预编译，而本函数给单个 TU 追加 `-m`，GCC 报
`cmake_pch.hxx.gch: created and used with differing settings of '-mavx'`。
这属**警告而非错误**，极易被忽略后演变成难定位的行为差异。已在
`bitcask_simd_tu` 内固定 `SKIP_PRECOMPILE_HEADERS ON`。

**验收**：
- **ISA 泄漏检查**：反汇编本次构建的 **42 个非 SIMD TU，宽指令（zmm/vfmadd/
  vpgather/vpclmul/ymm）泄漏 0**；5 个 avx512/vnni512 TU 确实含 zmm。隔离
  从「靠编译器自觉」变成结构性成立。
  （首轮检查曾报 2 处泄漏，追查为 7 月 4 日的**陈旧 .o**——`bitcask_index.dir` /
  `bitcask_search.dir` 是已不存在的旧目标遗留目录。）
- **四档跨档回归**：`scalar`/`sse42`/`avx2`/`avxvnni` 逐档 **732/732**。
- **CRC32 内核体与原版逐字 diff 一致**（唯一差异是删掉的守卫行）——搬运忠实。
- **bench 零回归**：同机交替 A/B + `taskset` 绑核 + 3 轮取最小值，
  crc32(4 档尺寸)/BOW/intersect 全部落在 **±2%** 内。
  ⚠️ **首轮非绑核测量曾报 crc32 +27%、intersect +40%**，但**未改动的 scalar 路径
  也「回退」13%**——据此判定为负载噪声（load avg >10 / 8 核），改用绑核交替
  后全部消失。**这类噪声足以让人误判并去「优化」一个并不存在的回退。**

### S37-4 — MSVC 构建适配 🟡 MED（首个需要 Windows 环境的任务）

1. **编译第一天必炸三项**：`/utf-8`（187 源文件中 **181 个含中文注释**，不给此项 MSVC
   按系统 ANSI 代码页解析，中文字节吃掉后续代码报无从溯源的 C2001/C4819）；
   `NOMINMAX` + `WIN32_LEAN_AND_MEAN`（`windows.h` 的 `min`/`max` 宏 vs 全库 94 处
   `std::min/max`，且 `windows.h` **只准进移植层 TU**）；`/bigobj`。
2. `CMakeLists.txt:30-113` 全部 GCC/Clang 选项加 `if(MSVC)` 分流（映射表见设计稿 §1.2）。
   `/W4` 首轮**不开 `/WX`**（MSVC 独有告警 C4267/C4244/C4100 会刷屏并阻塞进度），
   逐条清理后再开。
3. **静态库合并重写**：`CMakeLists.txt:458-471` 在配置期生成 bash 脚本跑 `ar x`，
   Windows 无 bash/ar。改 `OBJECT` 库 + `$<TARGET_OBJECTS:>`。
   **顺带修既有隐患**：`ar x` 把 13 个归档解到同一临时目录，不同库中的**同名 `.o`
   会静默相互覆盖** —— 当前是否已发生取决于文件名巧合，属定时炸弹。
4. install 规则补 Windows 布局（`RUNTIME`→`bin/`、`ARCHIVE`(.lib)→`lib/`；
   现二者都指向 `LIBDIR`）。
5. vcpkg manifest 承载 zlib + TBB；cppjieba/limonp 在 MSVC 下的可编译性实测。

- **工作量**：4-6 天
- **验收**：MSVC 能编出 `bitcask.dll` + `bitcask.lib`；ctest 能跑起来（不要求全绿）

#### ✅ S37-4 落地记录（2026-08-07）

环境：VS 18 / **MSVC 14.51（cl 19.51）** / Windows SDK 10.0.28000 / CMake 4.3 / Ninja 1.13。
`scripts/Enter-MsvcEnv.ps1` 把 VsDevCmd 的环境导进当前会话（本届新增的开发脚本）。

**结论先行：全部 188 个 TU（第一方 + 61 个源 + 40 个测试 + tools + 第三方）
在 MSVC 下编译零错误；剩余 1851 条链接错误 100% 是 `bitcask::io` 的 34 个符号
——即 S37-5 的完整工作面，无一例外。** 编译面比设计稿预估小得多。

**依赖策略（决策变更）**：设计稿 §1.4 建议 vcpkg 承载 zlib + TBB。**TBB 改从
`third_party/oneTBB` 子模块现编**——vcpkg 的 `tbb` 端口经 `hwloc` 依赖会拖进
msys2（m4/perl/autotools/ncurses…）整套工具链，与「纯 MSVC」构建约束冲突。
oneTBB 自身用 MSVC + CMake 直接可编（无 hwloc 时只跳过 `tbbbind` 的 NUMA 绑定）。
新增 `BITCASK_BUNDLED_TBB` 开关，Windows 与 TSan 构建默认 ON，Linux 默认 OFF
（`find_package` 行为逐字不变）。`vcpkg.json` 最终只剩 zlib，14 秒装完。

**构建系统改动**：
- 全局 MSVC 分流：`/utf-8 /bigobj /permissive- /Zc:preprocessor /Zc:__cplusplus`
  + `NOMINMAX` / `WIN32_LEAN_AND_MEAN` / `_CRT_SECURE_NO_WARNINGS` /
  `_CRT_NONSTDC_NO_WARNINGS`（目录作用域，置于所有 `add_library`/`add_subdirectory` 之前）
- `bitcask_warnings` 分流 `/W4`（**首轮不开 `/WX`**）；`BITCASK_WERROR`→`/WX`；
  `BITCASK_NATIVE` 在 MSVC 下 **FATAL_ERROR 而非静默忽略**；
  `BitcaskSanitizers.cmake` 加 MSVC 分支（ASan → `/fsanitize=address` + 清 `/RTC1`
  + `/INCREMENTAL:NO`；**TSan/UBSan/LSan 报错退出**——「以为在跑 TSan、实际没插桩」
  比构建失败危险得多）
- `WIN32` 下 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 收进 `bin/`（Windows 无 RPATH，
  exe 与 bitcask.dll/tbb12.dll/zlib1.dll 须同目录）
- `bitcask_io` 改为**按平台选后端**（`posix_file.cpp` / `win32_file.cpp`）；
  后端文件不存在时告警并留空——库目标仍全部可编译，这正是本届量编译面的形态
- `cmake_minimum_required` 3.20 → 3.21（见下）

**静态库合并重写（设计稿 §1.3）——未按原方案改 OBJECT 库**：
原实现在**配置期生成 bash 脚本**跑 `ar x` 解包 13 个归档再重打包，Windows 无
bash/ar。设计稿建议改 OBJECT 库，但那会**改动整个链接图**（每个测试 exe 从
「按需抽取」变成「全量吞入」），而「Linux 侧零回归」是硬约束且本机无法验证。
改用 `add_library(bitcask_static STATIC $<TARGET_OBJECTS:...>)`——`$<TARGET_OBJECTS:>`
自 CMake 3.21 起对 STATIC 目标同样可用（已实测确认：符号进归档且不重复编译），
于是**链接图逐字不变**，只有「怎么产出这一个归档」变了。同样杜绝了同名 `.o`
互相覆盖的隐患（对象按目标各自成路径喂入，不再解包到同一临时目录）。

顺带修掉两个**现行缺陷**：
1. **`bitcask_simd` 原本不在合并列表里**（S37-3 新增该库时漏补）——即 v6.1.0
   发布的 `libbitcask.a` 缺 `cpu_features` / `crc32_sse42` 两个 TU，静态链接的
   下游会撞未定义符号。已补入并 `dumpbin /ARCHIVEMEMBERS` 核验（58 个成员，
   `bitcask::simd::*` 符号确为定义态）。
2. install 规则的合并静态库那条原是 `install(FILES <硬编码 .a 路径>)`，Windows
   上指向不存在的文件；改 `install(TARGETS)`。Windows 下产物改名
   `bitcask_static.lib`——`bitcask_shared` 的**导入库**已占用 `bitcask.lib`。

**三处被 `/usr/include` 长期掩盖的依赖缺失**（Windows 无全局头目录，一编即炸）：
| 目标 | 缺 | 经由 |
|---|---|---|
| `bitcask_simd` | `ZLIB::ZLIB` | 公开头 `hw_crc32.hpp` 直接 `#include <zlib.h>` |
| `bitcask_bm25` | TBB 需 PRIVATE→**PUBLIC** | 公开头 `inverted.hpp` 含 TBB 头，经 segment/text_plugin/search_cache 传播 |
| `bitcask_cask` | TBB 需 PRIVATE→**PUBLIC** | **公开头** `cask.hpp` 含 `thread_pool.hpp` → TBB |
| `bitcask_keydir` | TBB（PRIVATE 即可）| `keydir_registry.cpp` 含 `thread_pool.hpp` |

**⚠️ 本届最值得记住的一条：`#if defined(__has_feature) && __has_feature(...)`
在符合标准的预处理器下是语法错误，不是「MSVC 方言问题」**

全库 6 处 TSan 探测写成
`#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))`。
标准要求先对整个 `#if` 表达式做宏替换、把剩余标识符换成 `0`，于是右半边成为
`0(thread_sanitizer)`——不合法。GCC/Clang 对 `&&` 右侧宽容，`/Zc:preprocessor`
严格按标准来，报 **C1012「unmatched parenthesis」**，且错误位置指向 `#if` 行，
与真实原因（探测宏不存在）毫无关联，极难追。一处在 `thread_pool.hpp` 里，
波及 `bitcask_keydir`/`bitcask_cask`/`bitcask_hybrid` 等一大片。

修法：`detail/cpu_features.hpp` 新增 **`BITCASK_TSAN_ENABLED`** 单一出口，
用**嵌套** `#if`（外层先确认 `__has_feature` 存在，内层才调用）。仓库里
`oki_levelb/oki_locate/oki_range_test` 三处本来就写的是正确的嵌套形式——
即正确写法一直在库里，只是没被推广。6 处已全部切换。

**其余源码改动（全部是「Linux 上碰巧能过」的真问题）**：
- `seq_shard_table.hpp` 的 `__atomic_load_n(..., __ATOMIC_RELAXED)` → MSVC 侧
  `__iso_volatile_load64`。**GCC 分支逐字保留**：原注释里三条理由（TBAA 豁免、
  无 libcall、TSan 原生理解）中前两条在 GCC 上是正确性依赖，换 `std::atomic_ref`
  有重新引入陈旧读的风险且无法在本机验证。MSVC 侧选 `__iso_` 前缀那一族而非
  普通 `volatile`：x64 默认 `/volatile:ms` 会给 volatile 读加 acquire 语义，
  强于此处需要的 relaxed。
- `intersect_kernels_avx2.cpp` 的 `__m256i_u`（GCC 私有类型）→ `__m256i`；
  `_mm256_loadu_si256` 的非对齐语义由 intrinsic 自身保证，不靠指针类型。
- `intersect_kernels_avx512.cpp` 补 `<bit>`（`std::popcount`）、
  `keydir_test.cpp` 补 `<algorithm>`（`std::sort`）——MSVC STL 不做传递包含。
- **`fs::path::c_str()` 在 Windows 返回 `const wchar_t*`**（设计稿 C8）：
  `migrate.cpp` ×1 + 测试 ×7 处直接喂给窄 `std::fopen`，编译失败。改 `.string()`，
  与全库「路径以 `std::string` 流转」约定一致。⚠️ 这只解决可编译性——`.string()`
  走系统 ANSI 代码页，**非 ASCII 路径仍打不开**，属 S37-5 的 UTF-8→UTF-16 范畴。
- `c_api/internal.h` 的 `#pragma GCC visibility push/pop` **加编译器守卫而非
  按 S37-3.5 原计划删除**：`bitcask_shared` 是唯一不链 `bitcask_warnings` 的目标，
  拿不到 `-fvisibility=hidden`，这条 pragma 是那些助手符号在 Linux 上唯一的
  隐藏来源，删掉即改行为。
- `c_api_test.c` 用 `<stdatomic.h>`（parallel_scan 并发回调计数，非装饰）：
  MSVC 的 `.c` 默认 C89 且 C11 atomics 仍在开关后。该目标单独加
  `C_STANDARD 11` + `/experimental:c11atomics`，不动全局 `CMAKE_C_STANDARD`
  （那会把 utf8proc/zlib 的 C 方言从 gnu17 降到 gnu11，属无谓波及）。

**告警面（S37-7 开 `/WX` 前的账）——远好于设计稿预估**：
库目标全量 `/W4` 共 **94 条**：C4324 ×60（cacheline `alignas` 的填充提示，无害）、
C4244 ×32、C4100 ×1、C4456 ×1。**设计稿点名会刷屏的 C4267 实际为 0。**

**验收状态**：
| 项 | 状态 |
|---|---|
| 全部 188 个 TU 编译 | ✅ 0 错误 |
| 13 个第一方静态库 + 合并 `bitcask_static.lib` | ✅ 产出（121 MB，Debug） |
| `bitcask.dll` | ⛔ 缺 34 个 `bitcask::io` 符号（S37-5） |
| ctest | ⛔ 同上，40 个测试 exe 均卡在链接 |
| Linux 侧 | ⬜ **本届改动尚未在 Linux 上复验**（见下） |

> ⚠️ **本届记录未覆盖的一项**：所有改动都只在 Windows 上验证过。虽然每处都按
> 「GCC 分支逐字不变」的原则做（`__atomic_load_n`、visibility pragma、
> `find_package(TBB)` 路径均保持原样），`BITCASK_TSAN_ENABLED` 的替换与
> 4 处 `target_link_libraries` 的 PRIVATE→PUBLIC 仍须在 Linux 上过一遍
> **Debug 全量 + ASan + TSan**（TSan 尤其——`BITCASK_TSAN_ENABLED` 若因漏包含
> 头而恒 0，TSan 注解会**静默失效**，表现为 TSan 误报而非编译错误）。

**S37-5 的完整工作面（34 个未解析符号，链接器实测）**：

`File` 成员 ×9：`open` / `close_quiet` / `pread` / `pread_into` / `pwrite` /
`write` / `seek` / `seek_bof` / `sync` / `truncate_here`
（另 `truncate(len)` / `size()` / `identity()` / `release()` 在未解析表外——
未被 c_api 路径引用，实现时同样要补）
`MappedFile` ×4：`map_readonly` / `~MappedFile` / `operator=(&&)` / `reset`
句柄级 ×11：`open_handle` / `close_handle` / `sync_data` / `truncate_handle` /
`handle_size` / `handle_identity` / `path_identity` / `pread_all` / `pwrite_all` /
`pread_once` / `pwrite_once`
路径级 ×5：**`atomic_rename`** / `sync_directory` / `flush_and_sync_stream` /
`remove_file` / `prefetch_range`
进程/系统 ×4：`current_process_id` / **`process_alive`** / `max_open_files` / `page_size`

### S37-5 — Windows I/O 后端 🔴 HIGH

实现 `src/io/win32_file.cpp`，填 S37-1 留下的 seam：

1. `CreateFileW` + UTF-8→UTF-16 路径转换（**非 ASCII 目录名在 ANSI 代码页下直接打不开**）。
   编译期地雷：`std::filesystem::path::c_str()` 在 Windows 返回 `const wchar_t*`，
   任何直接喂给 `::open`/`std::fopen` 处编译失败（`file_util.hpp:96` 即一例）；
   `data_file.cpp:452` 的 `find_last_of('/')` 改 `fs::path::filename()`。
2. **`atomic_rename` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`**（C1）。
3. **定位读写（C3）**：Windows 无原生 pread；同步句柄上带 `OVERLAPPED.Offset` 的 `ReadFile`
   会更新文件指针且并发被内核串行化 —— 直接翻译会把并发点查悄悄变成串行，
   **测试全绿、只有 bench 掉**。定为**每线程句柄池**方案，并与 read-handle LRU 预算合并考虑。
   `io.hpp:6-12` 的线程模型注释需按平台重新表述。
4. **文件锁（C4）**：`CREATE_NEW` 对应 `O_CREAT|O_EXCL`；`release_quiet` 的
   「先 unlink 后 close」需带 `FILE_SHARE_DELETE`。
   ⚠️ **stale-lock 检测必须加进程创建时间**（`GetProcessTimes` 的 `ftCreationTime`）：
   Windows PID 复用远快于 Linux，仅凭 `OpenProcess` 判存活会误判「新进程复用了崩溃
   进程的 PID」→ 拒绝回收有效 stale lock → **库彻底打不开**。本移植中最易造成生产事故的单点。
5. `sync_data` → `FlushFileBuffers`（无 data/metadata 区分，checkpoint 路径需重新 bench）；
   `sync_directory` → no-op（并在 `doc/format-zh.md` 补：Windows 下 rename 的目录项
   持久性改由 `MOVEFILE_WRITE_THROUGH` 承担）。
6. `MappedFile` → `CreateFileMappingW` + `MapViewOfFile`；**析构要收两个句柄**
   （`UnmapViewOfFile` + `CloseHandle(section)`，`MappedFile` 需多存一个成员）；
   `MADV_RANDOM` → `FILE_FLAG_RANDOM_ACCESS`（**时机从 mmap 后前移到开文件时**）；
   `prefetch` → `PrefetchVirtualMemory`。
7. `getrlimit(RLIMIT_NOFILE)`（`cask.cpp:184`）→ Windows 句柄不受 fd 表约束，
   需重新标定 read-handle LRU 预算（原逻辑是 `nofile` 的比例）。
8. MAX_PATH 260 评估：`longPathAware` manifest 或 `\\?\` 前缀。

- **工作量**：2.5-3 周
- **验收**：ctest 全绿（除已知 Windows 语义差异项）；crash 注入用例通过

#### ✅ S37-5 落地记录（2026-08-07）

**结果：`bitcask.dll` 产出，49 个 exe 全部链接通过，ctest 733 个用例 724 通过
（99%）。剩余 9 个失败全部落在 S37-6 的映射/删除生命周期，无一例外。**
崩溃注入 6 个场景在 Windows 上全绿。

**`FileHandle` 在 Windows 上改 `void*`（HANDLE）**——S37-1 头注释写明的设计意图。
备选是用 CRT 的 `_open_osfhandle` 把 HANDLE 包成 `int` fd（调用站点零改动），
否决理由：CRT fd 表有 8192 硬上限、全局锁、且「CRT fd + HANDLE 双重所有权」
是经典 double-close 温床，而本库要把大量 sealed 段句柄挂在 read-handle LRU 上。
**哨兵值选 `nullptr` 而非 `INVALID_HANDLE_VALUE`**：后者需 `reinterpret_cast`，
进不了 constexpr（`kInvalidHandle` 与 `handle_valid` 都是 constexpr，65 处依赖）；
归一在 `win32_file.cpp` 边界完成，出了那个文件只有一种无效表示。
连带修掉 7 处把 `FileHandle` 当 `int` 的硬编码（`fd_ >= 0` / `int fd_ = -1`），
其中 `file_lock.cpp` 那处在 Linux 上一直是对的、只是不该那么写。

**两处会静默出错、必须逐站点核对才能发现的语义**：

1. **`File::write()` 恒为「原子追加到 EOF」，不走文件指针。**
   Windows 同步句柄上带 `OVERLAPPED` 的定位读**会移动文件指针**（POSIX 的
   pread 不会）。而 `HintFile` 恰恰把顺序 `write()`（hint_file.cpp:50）与定位
   `pread`（112/129/179/189/206/230）**交错**用在同一个 `File` 上——直译成普通
   `WriteFile` 的话，一次 pread 就让随后的追加变成**从上次读到的位置覆盖**。
   编译通过、写入「成功」，只有 hint 内容被悄悄写坏。改用
   `OVERLAPPED.Offset/OffsetHigh` 全 1 的原子追加惯用法，与 O_APPEND 逐条对应
   且完全不受指针状态影响。（核查过：`File::write()` 全库仅此一个调用点，
   `File::read()` 零调用点。）
2. **`truncate(len)` 用 `SetFileInformationByHandle(FileEndOfFileInfo)`**
   而非 `SetFilePointerEx + SetEndOfFile`——后者以文件指针为截断点，会把
   io.hpp 明写「不依赖 fd 当前 offset，故线程安全」的 `truncate` 变成有状态操作。
   `truncate_here()` 则**刻意**用 SetEndOfFile（语义就是「截到指针处」）。

**首轮 ctest 35 个失败 → 9 个，三处修复**（按贡献排序）：

| 修复 | 修好 | 性质 |
|---|---|---|
| `parse_data_tstamp` 的分隔符 | ~12 | **产品 bug** |
| crash_child 的 `CreateProcessW` | 6 | S37-2 留位补齐 |
| 测试里的 POSIX 假设 | ~5 | 测试可移植性 |

**最值得记住的：`parse_data_tstamp` 只认 `/`，导致 merge「成功但从不干活」**
它用 `find_last_of('/')` 剥目录前缀，而 `merger.cpp:292` 传的是**完整路径**。
Windows 上剥不掉 → base 成了 `C:\dir\42` → 数字校验失败 → 返回 nullopt →
**每一个输入文件都被判为「不是 data 文件」而跳过**。merge 于是返回成功、
不报错、不崩，只是 `records_kept` 恒为 0。12 个 merge 相关用例全挂在这一处。
修法是按平台取分隔符集合（Linux 仍只有 `/`——`\` 在 POSIX 是合法文件名字符，
不能一并当分隔符），等价于 `fs::path::filename()` 但保持 noexcept 且不分配。

**⚠️ PID 复用加固（风险 #2）——新增的单测当场揪出一个会静默损坏数据的 bug**

锁文件格式扩为**两行**：
```
<pid> <activefile>\n
<start_token>\n        ← 本届新增：进程实例令牌
```
加第二行而非扩第一行是**刻意**的：现有两个 parser（`parse_leading_pid` 只读
开头连续数字、`parse_active_file_id_from_lock` 取首个空格到首个 `\n`）对第二行
完全不可见，于是新旧锁文件双向兼容。Windows 令牌 = `GetProcessTimes` 的
`ftCreationTime`；**POSIX 恒返回 0**（令牌为 0 时 `process_alive(pid, 0)` 退化为
`process_alive(pid)`），因此 **Linux 行为逐字不变**——不引入 `/proc/<pid>/stat`
的 starttime，那是本届不涉及、也无法在 Windows 上验证的行为变更。

写完实现后加的 `ProcessToken` 三个用例立刻炸出：**`process_alive()` 对当前
存活进程返回 `false`**。根因是 `WaitForSingleObject` 需要 `SYNCHRONIZE` 权限，
而 `PROCESS_QUERY_LIMITED_INFORMATION` **不含**它 → 返回 `WAIT_FAILED` 而非
`WAIT_TIMEOUT`。后果是**每一把写锁都被判为 stale 并删掉 → 两个 writer 能同时
持锁写同一个库**。同时把「只有明确 signaled 才判死」定成不变量，让
`WAIT_FAILED` 一类意外落到保守判活那侧。

> **`grep kWriteLocked tests/` 全库零命中**——写锁竞争路径此前完全没有测试覆盖。
> 这个 bug 走完全套 733 个用例照样全绿。`ProcessToken` 那组用例的价值全在
> **否定断言**（令牌不符必须判死）：若实现退化成只查 pid，肯定断言依然通过。
> 建议 S37-7 补一个真正的「两进程争锁」用例。

**Windows 后端与 POSIX 的语义差异总表**（均在 `win32_file.cpp` 对应函数处展开）：
| # | 差异 | 处置 |
|---|---|---|
| 1 | `File::write()` 恒追加 | 见上；全部调用点本就以追加模式开档 |
| 2 | 定位读写会移动文件指针 | 见上；已逐站点核对 |
| 3 | `sync_directory` 是 no-op | 持久性改由 `MOVEFILE_WRITE_THROUGH` 承担 |
| 4 | `FileMode`(0600/0644) 被忽略 | Windows 靠 ACL 继承 |
| 5 | `kCloseOnExec` 是 no-op | Windows 句柄默认不被继承（W1 在此平台不成立）|
| 6 | `max_open_files` 返回 nullopt | 句柄不受 fd 表约束；调用方兜底 1024 |
| 7 | `advise_random` 被忽略 | 见下「刻意没做」 |
| 8 | `kOSync`/`kSyncAll` 合并 | 都是 `FILE_FLAG_WRITE_THROUGH`；S13-P2 的 dsync 优化在此平台不存在 |

**刻意没做的两项（附理由，避免被当成遗漏）**：
- **`FILE_FLAG_RANDOM_ACCESS`**：设计稿 C7 建议把 `MADV_RANDOM` 的时机前移到
  开文件时。实测 6 个 `map_readonly` 调用点里 **3 个传 `false`**（segment_v2 /
  diskann / ivf_rq 是顺序访问），在 `kReadOnly` 上一刀切会关掉顺序路径的预读。
  该提示只影响预读启发、不涉正确性。若 bench 显示有收益，应做成 `OpenFlag`
  位由调用方指定。
- **每线程句柄池（C3 / 风险 #3）**：同步句柄上的 I/O 被内核在文件对象上串行化，
  即多线程并发 pread 同一句柄不会真正并行。**这正是「测试全绿、只有 bench 掉」
  的那一项**，本次未做——它要与 read-handle LRU 预算合并考虑，属独立工作。

**已知缺口**：窄路径一律按 **UTF-8** 解读（`MB_ERR_INVALID_CHARS` 严格拒非法
序列，不退回 ANSI 猜测——「同一个 char* 可能是两种编码」会让「打开了错误的
文件」无法定位）。但库内多处经 `fs::path::string()` 产出窄路径，它在 Windows 上
走**系统 ANSI 代码页**。纯 ASCII 路径两者一致（现有全部测试与典型部署），
**非 ASCII 路径会被判为非法 UTF-8 而报 EINVAL**。彻底修复要把那些站点换成
`u8string()`，见下方遗留 W4。

### S37-6 — 删除/映射生命周期 🔴 HIGH（**唯一架构改动**）

Windows 下：① 所有 `CreateFile` 须带 `FILE_SHARE_DELETE`，否则删除报
`ERROR_SHARING_VIOLATION`；② **即使带了，被 section 映射持有的文件仍删不掉**
（映射对象持独立引用，与句柄共享模式无关）。

现有 `retire_files`/`drain_retired_files`（`cask.cpp:833-858`）重试队列是**必要但不充分**的
兜底：只要 sealed 段的 mmap 还在 read-handle LRU 里，重试永远失败，**队列无限增长**。

**新增不变量**：退休一个文件前，先把它从 read-handle LRU 逐出并 `reset()` 其 `MappedFile`。
流程改为 `evict_mappings(file_id) → close handles → delete`，且 evict 须与
`epoch_reclaim.hpp` 的并发回收协调 —— 不能在读者仍持 `span` 时 unmap。

需同步修订：`doc/read-handle-lru-design-zh.md`、`doc/sealed-mmap-read-design-zh.md`、
`io.hpp:135-151`（`MappedFile` 头注释明写依赖「POSIX unlink-while-mapped 语义」，
该注释在 Windows 上失效）。

- **工作量**：1-1.5 周
- **验收**：Windows 上 merge 后退休队列能排空（长跑测试）；Linux 侧行为零变化 + TSan 全绿

#### ✅ S37-6 落地记录（2026-08-07）—— **结论：不是架构改动**

**ctest 733/733 全绿。**

**开工第一件事是实测本项赖以立项的前提，结果推翻了它。**
设计稿 C2 断言「即使带 `FILE_SHARE_DELETE`，被 section 映射持有的文件仍删不掉」，
并据此规划「退休前先 `evict_mappings` 并与 `epoch_reclaim` 协调」这一架构改动。
写了个复刻本库 `MappedFile` 建法的探针逐条量（Windows 10.0.26200）：

| 设计稿断言 | 实测 |
|---|---|
| 被映射的文件删不掉 | **假**——删除成功，名字**立刻**从目录消失（Win10 1709+ 的 POSIX 语义删除），同名可立即重建 |
| 被映射的文件不能被 rename 覆盖 | **假**（前提是目标没有打开的文件句柄）|
| 覆盖/删除后旧视图读到什么 | **旧内容**，含之后才首次触碰的页——与 POSIX unlink-while-mapped **逐条等价** |

于是重试队列在 Windows 上本就能正常排空，`evict_mappings` 与 epoch 协调**都不必做**。
**风险排序第一位的那项，实际不存在。**

**真正的限制是另外两条，设计稿都没提到**：

1. **`MoveFileEx(REPLACE_EXISTING)` 覆盖一个尚有文件句柄打开的目标必然
   `ERROR_ACCESS_DENIED`**——与共享模式、与映射都无关，任何访问模式（连
   `GENERIC_READ`）都拦。命中点：`IvfSegment`/`DiskannSegment::open` 建好映射后
   仍把 `fd_` 留着，而**那个 fd 从头到尾没被读过**（段全部经 `base_` 访问，
   `fd_` 只在析构里被关）。段 rebase 正是「写 tmp → rename 覆盖旧段」的形态，
   于是 4 个 rebase 用例全挂。
   修法就是建好映射后立刻 `close_handle`——`io.hpp` 早写着「向量 payload 关 fd
   省预算」，这两个站点一直没照做。两平台同时受益（每个 sealed 段少一个 fd）。
   `fd_` 成员随之从两个公开头删除：改完之后它恒为 `kInvalidHandle`，
   `close()` 里那个分支成了死代码，而留一个永远无效的成员会让后来者以为
   「这儿有个 fd 可以用」。
2. **CRT stdio 是第二类阻塞源**。MSVC 的 `std::fopen` / `std::ifstream` 用
   `_SH_DENYNO`，共享位不含 `FILE_SHARE_DELETE`，**且 CRT 不提供任何开关**。
   任何被它们长期持有的文件都删不掉（`ERROR_SHARING_VIOLATION`）。
   新增 `io::open_stream(path, mode)`：POSIX 就是 `std::fopen`；Windows 走
   `CreateFileW`(带 SHARE_DELETE) → `_open_osfhandle` → `_fdopen`。
   全库长期持有的 `std::FILE*` **只有一处**（`field_schema.hpp:105` 的追加句柄），
   已切换；用完即关的 `FilePtr`/`AtomicFileWriter` 保持 `std::fopen` 不动。

**9 个失败的最终归因与修法**：

| 失败 | 真因 | 修法 |
|---|---|---|
| Diskann/Ivf Rebase ×4 | 段留着没用的 `fd_` 挡住 rename 覆盖 | 建映射后立刻关句柄 |
| FieldSchema | CRT `fopen` 无 SHARE_DELETE | `io::open_stream` |
| BackupHotCopy / MigrateHintOrd / SearcherFacade | 同上（目录里那个 `field.schema` 一个句柄卡住整个 `remove_all`）| 同上 |
| InvertedIndex.V5BlockMinDl | **测试**里 `std::ifstream` 未出作用域就 `remove` | 加一层作用域 |

> 注意最后一列：5 个「目录/文件删不掉」的失败里，**4 个的根因是同一个
> `field.schema` 句柄**。Windows 上一个漏网的 CRT 句柄就能让整个库目录删不掉，
> 排查时很容易误以为是 mmap。

**顺带修掉一个我自己引入的 flaky**：S37-5 给 W3 补的 `count_os_threads`
Windows 实现，单跑 5/5 全过、`ctest -j 8` 下偶发失败。两处原因叠加：
① `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 拍的是全系统线程表，繁忙时会以
`ERROR_BAD_LENGTH` 失败（微软文档明载需重试），首版失败即返回 0；
② 进程线程总数本就受被测对象之外的因素影响（ntdll 加载器工作线程、延迟加载 DLL）。
①加重试，②把整轮测量改为最多 5 次重试、任一轮满足不变量即通过。
**重试不削弱断言**：真回归是确定性的，每轮都会违反。
> flaky 测试比没有测试更糟——它会让人养成忽略这个用例的习惯。

**新增 3 个不变量测试**（`tests/posix_file_test.cpp` 的 `FileLifecycle`）：
把本届赖以成立的三条语义从「一次性探针验过」变成常驻守护——
`DeleteWhileMappedKeepsViewContent` / `AtomicRenameOverMappedFileKeepsViewContent`
（含「rename 前必须先关句柄」那条纪律）/ `OpenStreamAllowsDeleteWhileHeld`。
三条在 POSIX 上是常识、在 Windows 上是实测结论，任一条不成立的表现都是
「merge 后文件删不掉、退休队列增长」或「rebase 静默失败」，不会在别处炸出来。

**文档同步**：`io.hpp` 的 `MappedFile` 头注释、`doc/read-handle-lru-design-zh.md`、
`doc/sealed-mmap-read-design-zh.md` 均补「Windows 上同一条 pin 语义成立（实测）」；
`doc/windows-port-design-zh.md` 的 C2 加了醒目更正块并保留原文作对照——
留一个已知为假的前提在定稿设计里，下一个读它的人会照着做无谓的架构改动。

---

#### 🎯（历史）S37-6 开工前的工作面（S37-5 的 ctest 量出，2026-08-07）

| 失败用例 | 卡在哪 |
|---|---|
| `DiskannPlugin.RebaseDropsDeadPhysically` / `RebaseMinDocsWindow` | `IvfSegment/DiskannSegment::build` 写 tmp 后 `atomic_rename` 覆盖旧段文件，而**旧段文件正被 `MappedFile` 持有**——被 section 映射的文件在 Windows 上既删不掉也覆盖不了 |
| `IvfPlugin.RebaseDropsDeadPhysically` / `RebaseMinDocsWindow` | 同上 |
| `CaskDocValueTest.BackupHotCopyAndReopen` | `(*b)->close()` 后 `remove_all(dir)` 仍失败。已核实 `Cask::close()` **确实**清了 `read_files_` 与 `active_data_`，残留的是**搜索/向量插件持有的 mmap**（Cask 对象尚未析构）|
| `CaskDocValueTest.MigrateHintOrdV4EraDirOpensAndReads` | 同上 |
| `SearcherFacade.MatchesCaskFacade` | 同上 |
| `InvertedIndex.V5BlockMinDlTrackedAndPersisted` | `remove(*.snap)` 被占用 |
| `FieldSchema.InternDeterministicAndReverseLookup` | **第二类占用源**：`field_schema.hpp:105` 长期持有一个 CRT `std::fopen(path,"ab")` 句柄，而 **MSVC 的 fopen 不带 `FILE_SHARE_DELETE`**（CRT 用 `_SH_DENYNO` = SHARE_READ\|SHARE_WRITE），文件因此删不掉 |

**两条结论直接影响 S37-6 的设计**：

1. **占用源有两类，不只是 mmap。** 设计稿 C2 只点了 section 映射；实测还有
   **CRT stdio 句柄**——任何走 `std::fopen`/`ofstream` 且长期持有的文件都删不掉，
   因为 CRT 不给 `FILE_SHARE_DELETE`（也没有开关能要）。第一方长期持有的目前
   只有 `field_schema`，但 `AtomicFileWriter` / `segment.hpp:525` /
   `search_checkpoint.hpp:233` / `index_manifest.hpp:169` 同属该形态，需逐个确认
   生命周期。**建议给 io seam 补一个 `open_stream(path, mode)`**：POSIX 直接
   `std::fopen`，Windows 走 `CreateFileW`(带 SHARE_DELETE) + `_open_osfhandle` +
   `_fdopen`，把 `std::FILE*` 这条通路也纳入统一的共享策略。
2. **`remove_all(目录)` 与 POSIX 语义有本质差别。** 即使所有句柄都带
   `FILE_SHARE_DELETE`，被删的文件在最后一个句柄关闭前只是 *delete-pending*、
   **仍留在目录里**，于是父目录删不掉。POSIX 的 `rm -rf` 没有这个问题。
   即：Windows 上「关掉 Cask 才能删目录」是硬要求，`close()` 必须真正放掉
   **全部**映射与句柄（含插件侧），而不只是 KV 侧的 `read_files_`。

### S37-7 — CI + 收尾 🟡 MED

1. `.github/workflows/ci.yml` 加 `windows-2022` job（MSVC Release + 全量 ctest）。
2. 加 `BITCASK_SIMD_MAX` 矩阵（scalar/avx2/avx512），覆盖所有 ISA 档位对拍。
3. `/WX` 清理收口；bench 对拍（Linux vs Windows 同档）。
4. 文档：README 加 Windows 构建段、`doc/format-zh.md` 补持久性机制差异、CHANGELOG。

> ⚠️ **需明确承认的护栏损失**：Windows/MSVC **无 TSan**（ASan 有，UBSan 无）。
> 对一个重并发的库（epoch 回收、无锁 keydir 读、组提交、后台 merge）这是实打实的损失。
> 缓解：并发正确性继续以 Linux TSan 为准（并发逻辑本身平台无关）；但 S37-5.3
> （每线程句柄池）与 S37-6（映射逐出 × epoch 协调）是 **Windows 独有的新并发代码，
> TSan 覆盖不到** —— 这两处必须补压力测试，并考虑 Application Verifier / `/analyze` 补位。

- **工作量**：1-1.5 周

---

## 执行序

```
S37-1 (2周)   ───── 平台抽象层收编      ┐
S37-2 (1周)   ───── fork → exec-self    ├─ 纯 Linux，无需 Windows 环境，可并行
S37-3 (2周)   ───── SIMD 分 ISA TU      ┘   （合计约 5 周 ≈ 一半工作量）
                        ↓
S37-4 (5天)   ───── MSVC 构建适配（首次需要 Windows 环境）
S37-5 (3周)   ───── Windows I/O 后端    ← 依赖 S37-1 的 seam
S37-6 (1.5周) ───── 删除/映射生命周期   ← 依赖 S37-5
S37-7 (1.5周) ───── CI + 收尾
```

**合计约 10-12 周**至「Windows x64 MSVC 全套 ctest 通过 + SIMD 同档性能」。

---

## ⚠️ 风险排序（按「有多容易静默错」）

| # | 项 | 任务 | 为何危险 |
|---|---|---|---|
| 1 | 映射生命周期 | S37-6 | 架构改动 + 与并发回收耦合 + TSan 覆盖不到；失败形态是退休队列无限增长（磁盘不回收），不报错 |
| 2 | PID 复用误判 | S37-5.4 | 导致库彻底打不开；只在特定时序复现 |
| 3 | pread 并发退化 | S37-5.3 | **测试全绿、只有 bench 掉**，最易漏 |
| 4 | CPUID 漏检 XCR0 | S37-3.3 | 只在特定虚拟化环境 `#UD`；本地全绿、线上偶发 |
| 5 | `std::rename` 覆盖语义 | S37-1.7 / S37-5.2 | 危害大（9 个原子写站点全线挂）但**必然立刻暴露**，反而最安全 |

---

## ⏸ 遗留

| 项 | 内容 | 状态 |
|---|---|---|
| T8 | 搜索读屏障无界等待（`prepare_search` 饥饿）| ⏸ 4 项前置未满足（原文 62789cd）|
| T12 | HNSW ckpt 去重（~115 行）| ⏸ 默认不做（注释同步已替代）|
| **W1** | `File::open` 默认**不设 `O_CLOEXEC`** | 🔍 S37-1 期间发现：11 处裸 `::open` 全部显式带 `O_CLOEXEC`（收编后由 `kCloseOnExec` 原样保留），而走 `io::File` 的核心 KV 路径（data/hint/oki run/write.lock）**没有** —— fork+exec 场景下 data file fd 泄漏进子进程。现有 fork 测试只 fork 不 exec 故未暴露，**S37-2 改 exec-self 后会变成真实暴露面**。收编时未擅自统一（那是行为变化，超出「零行为变化」约束）。**待评估**：给核心路径补 `kCloseOnExec`。Windows 侧句柄默认不继承，无对应问题 |
| **W4** | 窄路径的编码约定 | 🔍 S37-5 引入：Windows 后端把窄路径**一律按 UTF-8** 解读（严格拒非法序列，不退回 ANSI 猜测）。但库内多处经 `fs::path::string()` 产出窄路径，它在 Windows 上走**系统 ANSI 代码页**。纯 ASCII 两者一致（现有全部测试与典型部署），**非 ASCII 路径会报 EINVAL**。修法是把这些站点换成 `u8string()` 并统一「窄路径 = UTF-8」这条库级约定；面积需先量（S37-4 期间已新增 8 处 `.string()`）。Linux 侧 `string()`/`u8string()` 等价，无影响 |
| **W2** | `bitcask_format` → `bitcask_io` 的 PUBLIC 依赖 | 🔍 S37-1 引入（见落地记录）。当前无环且必要，但「记录 codec 层依赖 I/O 层」是轻微的分层异味。若 S37-4 把 13 个 STATIC 改 OBJECT 库，可顺带复核是否有更干净的归置 |
| ~~W3~~ | `count_os_threads()` 读 `/proc/self/task` | ✅ S37-5 结清：补 `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 实现（按 owner pid 过滤），**没有**把用例标 Linux-only——AT5 守的「线程数与库数解耦」是平台无关的结构性保证，在 Windows 上同样值得守。原实现在非 Linux 上恒返回 0，而断言的是差值，0-0=0 直接失败 |

---

## 当前状态快照

| 项 | 状态 |
|---|---|
| 设计文档 | ✅ `doc/windows-port-design-zh.md`（含三项决策基线与风险排序）|
| 面积实测 | ✅ 裸 POSIX 调用面已重新量准（`::open` 13 处而非 51，见本届总纲）|
| S37-1 抽象层收编 | ✅ done（裸宿主原语归零；Debug/ASan 725/725 + 双树，见落地记录）|
| S37-2 fork → exec-self | ✅ done（6 个 fork 点全转；`tests/` POSIX 头归零；Debug/ASan 725/725，见落地记录）|
| S37-3.a cpu_features + 降档开关 | ✅ done（`__builtin_cpu_supports` 归零；四档 732/732 跨档对拍，见落地记录）|
| S37-3.b 内核分 ISA TU | ✅ done（24 个 target 函数搬完；ISA 泄漏 0；四档 732/732；bench ±2%，见落地记录）|
| S37-4 MSVC 构建适配 | ✅ done（188 个 TU 编译零错误；13 库 + 合并静态库产出；剩余全部为 io 后端符号。⚠️ Linux 侧未复验，见落地记录）|
| S37-5 Windows I/O 后端 | ✅ done（bitcask.dll 产出；ctest 733 → 724 通过 99%；余 9 个全部落在 S37-6，见落地记录）|
| S37-6 删除/映射生命周期 | ✅ done（**实测推翻立项前提，非架构改动**；ctest 733/733 全绿，见落地记录）|
| S37-7 CI + 收尾 | ⬜ 未开始 |

### 待确认项

已由 S37-4 实测结清（VS 18 / MSVC 14.51 / SDK 10.0.28000）：

- ~~cppjieba / limonp 在 MSVC 下的可编译性~~ → ✅ **零修改编过**（含 utf8proc /
  googletest / benchmark / unordered_dense / oneTBB，第三方一处没改）。
- ~~`std::expected` 等 C++23 设施的覆盖度~~ → ✅ 21 个依赖 TU 全部编过，
  `/std:c++latest` 下 `<expected>` / `<span>` / `<bit>` 齐备。
- ~~MSVC 侧的 `/arch:` 施加~~ → ✅ `bitcask_simd_tu` 的 MSVC 分支实测可用，
  12 个分 ISA TU 全部编过（含 `/arch:AVX512` 的 5 个）。

仍未结清：

- 设计稿 §2.2「MSVC intrinsic 无需 `/arch` 开关」——**只验证了能编，没验证
  生成的指令**。分 TU 是性能需要还是正确性需要，须等 S37-7 的反汇编泄漏检查
  （Linux 侧 S37-3.b 已做过：42 个非 SIMD TU 宽指令泄漏 0）。
- 目标最低 Windows 版本（决定 `PrefetchVirtualMemory` / `FILE_ID_INFO` /
  长路径 opt-in 可用性）—— S37-5 落地前须拍板。
