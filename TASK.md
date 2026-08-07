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
| **W2** | `bitcask_format` → `bitcask_io` 的 PUBLIC 依赖 | 🔍 S37-1 引入（见落地记录）。当前无环且必要，但「记录 codec 层依赖 I/O 层」是轻微的分层异味。若 S37-4 把 13 个 STATIC 改 OBJECT 库，可顺带复核是否有更干净的归置 |
| **W3** | `count_os_threads()` 读 `/proc/self/task` | 🔍 S37-2 期间确认：`thread_pool_test` 的 AT5 用例（「线程数与库数解耦」）靠它计数，**`/proc` 无 Windows 对应物**。已改用 `std::filesystem` 去掉 `<dirent.h>`，且目录不存在时返回 0。S37-5 需二选一：换 `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` 按 owner pid 过滤，或把依赖它的用例整体标 Linux-only |

---

## 当前状态快照

| 项 | 状态 |
|---|---|
| 设计文档 | ✅ `doc/windows-port-design-zh.md`（含三项决策基线与风险排序）|
| 面积实测 | ✅ 裸 POSIX 调用面已重新量准（`::open` 13 处而非 51，见本届总纲）|
| S37-1 抽象层收编 | ✅ done（裸宿主原语归零；Debug/ASan 725/725 + 双树，见落地记录）|
| S37-2 fork → exec-self | ✅ done（6 个 fork 点全转；`tests/` POSIX 头归零；Debug/ASan 725/725，见落地记录）|
| S37-3.a cpu_features + 降档开关 | ✅ done（`__builtin_cpu_supports` 归零；四档 732/732 跨档对拍，见落地记录）|
| S37-3.b 内核分 ISA TU | ⬜ 未开始 |
| S37-4 MSVC 构建适配 | ⬜ 未开始（需 Windows 环境）|
| S37-5 Windows I/O 后端 | ⬜ 未开始 |
| S37-6 删除/映射生命周期 | ⬜ 未开始 |
| S37-7 CI + 收尾 | ⬜ 未开始 |

### 待确认项（不阻塞 S37-1/2/3）

- 设计稿 §2.2「MSVC intrinsic 无需 `/arch` 开关」前提须在目标 VS 版本实测。
  结论决定 S37-3 的分 TU 是**性能需要**还是**正确性需要** —— 若为后者，改动面更硬。
- cppjieba / limonp 在 MSVC 下的实际可编译性（header-only，但含类 POSIX 习惯）。
- 目标最低 Windows 版本（决定 `PrefetchVirtualMemory` / `FILE_ID_INFO` / 长路径 opt-in 可用性）。
- `std::expected` 等 C++23 库设施在目标 VS 版本的覆盖度（21 个 TU 依赖）。
