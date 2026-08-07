# Windows 移植设计（MSVC 原生 · x64 · 保 SIMD）

> 目标平台：**Windows x64 / MSVC 原生 ABI**（VS2022 17.6+）。
> 明确排除：MinGW-w64、ARM64 Windows、32 位 x86。
> 目标：`ctest` 全绿（44 个测试），SIMD 内核在 Windows 上与 Linux 同档性能。

---

## 0. 现状评估

第一方代码 46.6k 行（`src/` `include/` `c_api/` `tools/`），测试 + bench 33.4k 行。
当前**纯 Linux-only**，无任何 Windows 构建痕迹。

### 0.1 已经就位的有利条件

| 项 | 位置 | 说明 |
|---|---|---|
| 盘上格式可移植 | `include/bitcask/byte_order.hpp` | 已 flag-day 全切 LE-only，x64 Windows 零改动可读 |
| mmap 已收敛 | `src/io/posix_file.cpp:185` | S36-B3 归并后全库只剩 **1 处** `::mmap`，`io::MappedFile` 是唯一出入口 |
| C API 导出宏 | `c_api/bitcask_kv.h:20-31` | 已有 `_WIN32` / `__declspec(dllexport|dllimport)` 分支 |
| 延迟删除已有骨架 | `src/cask/cask.cpp:833-858` | `retire_files`/`drain_retired_files` 注释已写明「非 POSIX 删除语义下失败 → 放回队列重试」 |
| 原子写已收敛 | `include/bitcask/detail/file_util.hpp` | T21 把 9 个站点归并到 `atomic_write_bytes` / `AtomicFileWriter` 两条路径 |
| 依赖链干净 | — | zlib / oneTBB / googletest / benchmark / utf8proc / unordered_dense 均官方支持 Windows |

**归并的历史投资在这里直接兑现**：mmap 从 7 处收到 1 处、原子写从 9 处收到 2 处，
使 C 段（系统调用移植）的攻击面比未归并前小一个数量级。

### 0.2 裸系统调用分布（第一方，20 个文件）

```
::close 59   ::open 51   ::read 20   ::write 13   ::rename 8
::fstat 8    ::fdatasync 8  ::pread 5  ::pwrite 4  ::ftruncate 4
::lseek 3    ::getpid 3   ::stat 2   ::unlink 2   ::madvise 2
::mmap 1     ::munmap 1   ::kill 1   ::getrlimit 1  ::sysconf 1  ::clock_gettime 1
```

关键观察：`io::PosixFile`（`include/bitcask/io.hpp`）虽然存在，但 51 处 `::open`
里只有 2 处在 `posix_file.cpp` 内 —— **绝大多数站点绕过抽象直接裸调 POSIX**。
移植的第一步不是写 Windows 后端，而是先把这些收编。

---

## 1. A 段 · 构建系统

### 1.1 编译第一天必炸的三项

| 问题 | 证据 | 解 |
|---|---|---|
| **源码编码** | 187 个源文件中 **181 个含非 ASCII**（中文注释与字符串） | 全局 `/utf-8`（等价 `/source-charset:utf-8 /execution-charset:utf-8`）。**不给此项，MSVC 按系统 ANSI 代码页解析源码，中文注释里的字节会吃掉后续代码，报出无从溯源的 C2001/C4819** |
| **`min`/`max` 宏** | `windows.h` 定义之，全库 94 处 `std::min`/`std::max` | 全局 `NOMINMAX` + `WIN32_LEAN_AND_MEAN`，且**只在移植层 TU 内 include `windows.h`**，绝不进公开头 |
| **大 TU** | `segment.hpp`(51 处 mmap 相关)、C++23 重模板 | `/bigobj` |

### 1.2 编译选项分流

`CMakeLists.txt` 现有选项全部无条件下发 GCC/Clang 语法：

- `:30-42` `-Wall -Wextra -Wpedantic -Werror -fvisibility=hidden -fvisibility-inlines-hidden`
- `:75` `-falign-functions=64`、`:86` `-fstack-protector-strong`、`:112` `-march=native`
- `cmake/BitcaskSanitizers.cmake` 的 `-fsanitize=*`

MSVC 等价映射：

| GCC/Clang | MSVC |
|---|---|
| `-Wall -Wextra -Wpedantic` | `/W4`（`/Wall` 噪音过大不可用） |
| `-Werror` | `/WX` |
| `-fvisibility=hidden` | 无等价物；Windows 默认隐藏，靠 `BITCASK_API` 显式导出（已就位） |
| `-fstack-protector-strong` | `/GS`（默认开） |
| `-falign-functions=64` | 无直接等价；可忽略 |
| `-march=native` | 无等价物；`BITCASK_NATIVE` 选项在 MSVC 下应报错退出而非静默忽略 |
| `-fsanitize=address` | `/fsanitize=address`（支持） |
| `-fsanitize=thread` | **不支持** —— 见 §5.3 |
| — | 追加 `/permissive- /Zc:preprocessor /Zc:__cplusplus /EHsc /utf-8 /bigobj` |

`/W4 /WX` 首轮必然刷出大量 MSVC 独有告警（C4267 size_t 窄化、C4244、C4100
未引用形参）。**策略：首轮以 `/W4` 无 `/WX` 落地，逐条清理后再开 `/WX`**，
避免 `/WX` 阻塞整个移植进度。

### 1.3 静态库合并必须重写

`CMakeLists.txt:458-471` **在配置期生成 bash 脚本**跑 `ar x` + `ar rcs` 合并 13 个
静态库。Windows 无 bash、无 `ar`。

改为 CMake 原生方案：把 13 个 `add_library(... STATIC)` 改为 `OBJECT` 库，
聚合 target 用 `$<TARGET_OBJECTS:...>`。

> **顺带修掉一个既有隐患**：`ar x` 把多个归档解到同一临时目录，不同库中的
> **同名 `.o` 会静默相互覆盖**（例如两个库各有 `codec.cpp.o`）。当前是否已
> 发生取决于文件名巧合，属于定时炸弹。OBJECT 库方案结构上杜绝此问题
> —— 这一步在 Linux 侧即是净收益。

### 1.4 依赖获取

建议 vcpkg manifest（`vcpkg.json`）承载 zlib + TBB，其余保持 submodule。
cppjieba / limonp 为 header-only，但用了若干类 POSIX 习惯（需实测；预计小修）。

### 1.5 install 规则

`CMakeLists.txt:497-511` 缺 Windows 的 import library 布局：
`RUNTIME`(.dll) → `bin/`、`ARCHIVE`(.lib) → `lib/`，当前两者都指向 `LIBDIR`。

---

## 2. B 段 · SIMD 派发层（MSVC 原生的主要重构）

### 2.1 当前架构与 MSVC 的冲突

现状：**MSVC 下 SIMD 全部编译掉，静默退化为 scalar。**
所有内核被 25 处形如以下的守卫排除：

```cpp
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
```

MSVC 既不定义 `__x86_64__`（用 `_M_X64`），也不定义 `__GNUC__`。

不兼容清单：

| 扩展 | 处数 | 分布 |
|---|---|---|
| `__attribute__((target("avx2,fma"/"avx512f"/"avx2")))` / `[[gnu::target]]` | 24 | `hnsw.cpp`×6, `hnsw_kernels.hpp`×4, `intersect.cpp`×4, `int8_kernels.hpp`×4, `vector_plugin.cpp`×4, `index.cpp`×3, `bm25_kernels.hpp`×2, `hw_crc32.hpp`×1 |
| `__builtin_cpu_supports` / `__builtin_cpu_init` | 18 + 3 | 同上 |
| `__builtin_ia32_pause` | 2 | `cask.cpp:1253`, `hnsw.cpp` |
| `__builtin_popcountll` / `__builtin_popcount` | 2 | `vec_disk_internal.hpp` |
| `__builtin_memcpy` | 3 | `seq_shard_table.hpp` |
| `__attribute__((no_sanitize))` | 3 | `seq_shard_table.hpp` |
| `__attribute__((noinline))` | 1 | `index.cpp` |
| `#pragma GCC visibility` | 2 | `c_api/internal.h` |

### 2.2 MSVC 的 intrinsic 模型差异（决定重构形态）

MSVC 与 GCC/Clang 的根本差异：**MSVC 下 intrinsic 无需命令行开关即可使用**
——在未加 `/arch:AVX2` 的 TU 里写 `_mm256_fmadd_ps` 可以编译通过。
`/arch:` 只影响**编译器自动生成的代码**（自动向量化、VEX 编码选择）。

> ⚠️ 此前提须在目标 VS 版本上实测确认后再据以定案（尤其 AVX-512 与
> AVX-VNNI intrinsic 的最低 VS 版本）。它决定了 §2.3 的分 TU 是「性能需要」
> 还是「正确性需要」——若实测为后者，分 TU 从建议升级为强制。

由此推论：

- **正确性**上，MSVC 不强制分 TU（不同于 GCC/Clang 必须靠 `target` 属性）。
- **性能**上仍必须分 TU：不给 `/arch:AVX2` 的 TU 里，编译器对**非 intrinsic
  的周边胶水代码**仍用 legacy SSE 编码，与 intrinsic 的 VEX 编码混用会触发
  **SSE↔AVX 状态切换惩罚**（每次数十周期），足以吃掉 SIMD 收益。

### 2.3 目标架构：内核按 ISA 分 TU

```
src/simd/
  cpu_features.hpp / .cpp   # 统一 CPUID 探测（替代 __builtin_cpu_supports）
  kernels_sse42.cpp         # 无 /arch（x64 基线含 SSE2；SSE4.2/PCLMUL 用 intrinsic）
  kernels_avx2.cpp          # /arch:AVX2      —— GCC/Clang 侧仍带 target 属性
  kernels_avx512.cpp        # /arch:AVX512
  kernels_vnni.cpp          # AVX-VNNI / AVX512-VNNI
  dispatch.cpp              # 函数指针表，一次性初始化
```

CMake 侧按 TU 施加 `/arch:`（GCC/Clang 侧则施加 `-mavx2` 等或维持 target 属性）：

```cmake
if(MSVC)
    set_source_files_properties(src/simd/kernels_avx2.cpp   PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    set_source_files_properties(src/simd/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
endif()
```

**结构性后果：3 个公开头 + 1 个内部头必须交出内联内核。**

| 头文件 | 现状 | 迁移后 |
|---|---|---|
| `include/bitcask/bm25_kernels.hpp` | AVX2 + AVX512 内核 inline 在头里 | 仅留 scalar 参考实现 + 派发声明 |
| `include/bitcask/detail/int8_kernels.hpp` | 4 个内核 + `pick_int8_dot_kernel()` | 仅留 `Int8DotFn` 类型与 `pick_*` 声明 |
| `include/bitcask/hw_crc32.hpp` | `[[gnu::target("sse4.2,pclmul")]]` 折叠内核 | 内核移入 `kernels_sse42.cpp` |
| `src/vector/hnsw_kernels.hpp`（内部） | 4 个内核声明 | 并入 `src/simd/` |

这是**跨 TU 边界的迁移**，内联被切断处需实测确认无性能回退
（`bm25_kernels` 在 BOW 热路径上，最需盯）。

### 2.4 `cpu_features`：替代 `__builtin_cpu_supports`

需自行实现的探测（MSVC 用 `__cpuid` / `__cpuidex` / `_xgetbv`）：

| 当前调用 | 处数 | CPUID 来源 |
|---|---|---|
| `"sse4.2"` | 1 | leaf 1, ECX bit 20 |
| `"pclmul"` | 1 | leaf 1, ECX bit 1 |
| `"fma"` | 4 | leaf 1, ECX bit 12 |
| `"avx2"` | 8 | leaf 7 sub 0, EBX bit 5 |
| `"avx512f"` | 5 | leaf 7 sub 0, EBX bit 16 |
| `"avx512vnni"` | 1 | leaf 7 sub 0, ECX bit 11 |
| `"avxvnni"` | 1 | leaf 7 **sub 1**, EAX bit 4 |

> **最易出错处 —— OS 状态支持位**：`__builtin_cpu_supports` 会**替你检查
> `XCR0`**（AVX 需 OSXSAVE + XCR0[2:1]，AVX-512 还需 XCR0[7:5]）。手写
> CPUID 时漏掉这步，会在「CPU 支持但 OS/hypervisor 未启用 YMM/ZMM 保存」
> 的机器上直接 `#UD` 崩溃 —— 且只在特定虚拟化环境复现，是典型的
> 「本地全绿、线上偶发」bug。此处必须写单测。

### 2.5 `/arch:AVX512` 的隐含 ISA 集扩大 —— 需收紧运行时门

MSVC 的 `/arch:AVX512` 隐含 **AVX512F + CD + BW + DQ + VL**，比代码当前
`target("avx512f")` 所要求的窄集**更宽**。后果：编译器可能在该 TU 的胶水代码里
自动生成 BW/DQ/VL 指令，而运行时门只检查了 `avx512f` → 在仅有 F+CD 的
CPU（Xeon Phi KNL/KNM 一类）上 `#UD`。

**对策**：Windows 侧的 AVX-512 运行时门必须同步收紧为
`F && CD && BW && DQ && VL`。现代 Xeon（Skylake-X 起）均满足，实际不损失覆盖，
但门必须与编译期契约一致。

### 2.6 逐项替换表（低风险机械项）

| 现状 | 替换 | 备注 |
|---|---|---|
| `__builtin_ia32_pause()` | `_mm_pause()` | 两侧通用，直接全替 |
| `__builtin_popcountll/popcount` | `std::popcount`（`<bit>`） | C++20 标准，比 intrinsic 更干净 |
| `__builtin_memcpy` | `std::memcpy` | — |
| `__attribute__((noinline))` | `BITCASK_NOINLINE` 宏 → `__declspec(noinline)` | — |
| `__attribute__((no_sanitize(...)))` | 宏包一层，MSVC 展开为空 | 仅影响 sanitizer 构建 |
| `#pragma GCC visibility`（`c_api/internal.h`） | 删除，靠 `BITCASK_API` | 已有导出宏 |
| `#if defined(__x86_64__)` ×25 | `BITCASK_X86_64` 宏（`__x86_64__ \|\| _M_X64`） | 注意 `cask.cpp:1253` 未带编译器条件，单独处理 |

### 2.7 SIMD 正确性守门

仓库既有纪律「**改评分算法必须过三方穷举对拍**」（见 top-k MaxScore 教训）
在此完全适用：本次重构把内核搬了 TU、换了派发实现，属于高风险改动。

- `int8_kernels.hpp:472+` 已有 `self_test`（int8 量化 + VNNI vs f32 参考）——
  必须在 Windows CI 上跑。
- BM25 / intersect / crc32 的 scalar-vs-SIMD 对拍需在 Windows 上全量执行。
- **建议加一个环境变量强制降档开关**（`BITCASK_SIMD_MAX=scalar|sse42|avx2|avx512`），
  让 CI 能在同一台机器上把所有 ISA 档位都跑一遍对拍，而不是听天由命看
  runner 的 CPU 型号。这对 Linux 侧同样有价值。

---

## 3. C 段 · 系统调用移植层

### 3.1 第一步：收编，而非直接写后端

把散在 20 个文件的裸 POSIX 调用收进 `io::File`（`PosixFile` 保留为别名以免
一次性改动过大）。**此步全程在 Linux 上完成，由现有 44 个 ctest 守门，
零平台风险**，且本身即是架构收益。

收编完成后，Windows 后端 = 实现一个 `io/win32_file.cpp`，而非改 20 个文件。

### 3.2 语义坑（按危险度排序）

#### C1 — `std::rename` 覆盖语义 · **最高危**

`detail/file_util.hpp:128`（`atomic_write_bytes`）与 `:200`
（`AtomicFileWriter::commit`）使用 `std::rename`。
**Windows CRT 的 `rename` 在目标已存在时失败**（POSIX 下是原子覆盖）。

全库 9 个原子写站点（keydir snapshot / index manifest / field.schema /
hnsw ×3 / docmap ckpt / search ckpt / oki run）都走这两条路径
→ **在 Windows 上第二次写入即全线失败**。

解：`MoveFileExW(tmp, final, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`。

> 注意：`std::filesystem::rename` 在 Windows 上**确实**是覆盖语义（内部走
> `MoveFileEx`），但 `file_util.hpp` 用的是 `<cstdio>` 的 `std::rename`。
> 两者极易混淆，改动时须逐处确认。

#### C2 — 删除/重命名正在打开的文件 · **唯一需要改架构处**

站点：merge 收尾删旧 data file（`cask.cpp:849-851`）、`oki_state.cpp` 7 处删旧
run、`cask_recovery.cpp` 删旧 ckpt、`oki_run.cpp` 2 处。

Windows 语义：

1. 所有 `CreateFile` 必须带 `FILE_SHARE_DELETE`，否则删除时
   `ERROR_SHARING_VIOLATION`；
2. **即使带了 `FILE_SHARE_DELETE`，被 mmap（section）持有的文件仍删不掉**
   —— 映射对象持有独立引用，与文件句柄的共享模式无关。

现有 `retire_files` / `drain_retired_files` 重试队列是**必要但不充分**的兜底：
只要 sealed 段的 mmap 还在 read-handle LRU 里，重试会永远失败，队列无限增长。

**必须新增的不变量**：merge/退休一个文件之前，先把它从 read-handle LRU 中
逐出并 `reset()` 掉其 `MappedFile`。这要跟以下两篇的 pin 语义一起重新设计：

- `doc/read-handle-lru-design-zh.md`
- `doc/sealed-mmap-read-design-zh.md`
- `include/bitcask/io.hpp:135-151`（`MappedFile` 头注释明确写着依赖
  「POSIX unlink-while-mapped 语义」，这条注释在 Windows 上失效，需改写）

设计要点：退休流程改为 `evict_mappings(file_id) → close handles → delete`，
且 evict 需与并发读者的 epoch 回收（`epoch_reclaim.hpp`）协调 —— 不能在
读者仍持有 `span` 时 `munmap`/`UnmapViewOfFile`。

#### C3 — `pread`/`pwrite` 的线程模型 · 易被漏掉

`io.hpp:6-12` 明确承诺「多线程可并发 pread 同一个 `PosixFile` 的同一 fd」，
这是 get/fold 热路径的基础。

Windows 无原生 `pread`。同步句柄上带 `OVERLAPPED.Offset` 的 `ReadFile` 虽可
指定偏移，但**会更新文件指针，且同一句柄上的并发调用被内核串行化** ——
直接翻译会把并发点查悄悄变成串行，且**测试全绿、只有 bench 掉**。

三个选项：

| 方案 | 代价 |
|---|---|
| 异步句柄 `FILE_FLAG_OVERLAPPED` + 每次带事件 | 正确且并发，但所有读路径要改成等待事件；改动面大 |
| 每线程一份句柄（thread-local handle pool） | 改动小、并发好；句柄数 × 线程数，需与 read-handle LRU 预算合并考虑 |
| 只读 sealed 文件走 mmap，绕开 pread | 已是现状的一半（S30）；但 pread 回退路径仍需正确 |

**建议：每线程句柄池**，并把 `io.hpp` 的线程模型注释按平台重新表述。

#### C4 — 文件锁

`src/lock/file_lock.cpp`：

- `acquire`: `O_CREAT|O_EXCL` → `CreateFileW(..., CREATE_NEW, ...)`，1:1 对应；
  `O_SYNC` → `FILE_FLAG_WRITE_THROUGH`。
- `release_quiet`（`:29-38`）：**先 `unlink` 后 `close`** ——注释说明这是照搬
  legacy 的既定顺序，为让持 fd 的 reader 仍能读到一致内容。
  Windows 下删除自己正打开的文件会失败。需带 `FILE_SHARE_DELETE` 打开，
  并接受「删除标记延迟到最后一个句柄关闭时生效」的语义差异
  —— 这恰好与原注释想要的效果一致，但要在注释里写清两边机制不同。

**stale-lock 检测**（`cask.cpp:39`）：`::kill(pid, 0)` →
`OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION, ...)` +
`GetExitCodeProcess`。

> ⚠️ **Windows PID 复用远快于 Linux**（内核主动复用小号 PID）。仅凭 PID 存活
> 判断会误判「新进程恰好复用了崩溃进程的 PID」→ 拒绝回收有效的 stale lock，
> 库彻底打不开。**必须在锁文件里同时记录进程创建时间**
> （`GetProcessTimes` 的 `ftCreationTime`），两者都匹配才判定「仍存活」。
> 这是本移植中最容易造成生产事故的单点。

#### C5 — fsync 家族

- `::fdatasync`（8 处）→ Windows 只有 `FlushFileBuffers`（**无 data/metadata
  区分**，等价于 `fsync`）。语义安全（更强），但性能会退 —— checkpoint 路径
  需重新 bench。
- `fsync_parent_dir`（`file_util.hpp:93-101`，用 `O_DIRECTORY` 打开父目录再
  fsync）**Windows 无对应物** → 降为 no-op。
  必须在 `doc/format-zh.md` 补一段：Windows 下 rename 的目录项持久性改由
  `MOVEFILE_WRITE_THROUGH` 承担，持久性契约不变但机制不同。

#### C6 — open flag 映射

| POSIX | 处数 | Win32 |
|---|---|---|
| `O_SYNC` / `O_DSYNC` | 13 | `FILE_FLAG_WRITE_THROUGH` |
| `O_CREAT\|O_EXCL` | 13 | `CREATE_NEW` |
| `O_CREAT`（不含 EXCL） | 11 | `OPEN_ALWAYS` |
| `O_TRUNC` | 2 | `CREATE_ALWAYS` |
| `O_CLOEXEC` | 9 | 默认不继承（`bInheritHandle=FALSE`），无需动作 |
| `O_APPEND` | 9 | `FILE_APPEND_DATA`；但与 C3 的定位写有交互，需逐处确认 |
| `O_DIRECTORY` | 2 | 见 C5，取消 |

#### C7 — mmap 细节（面积小，只有 `posix_file.cpp` 一处 + 2 处 madvise）

- `mmap(PROT_READ, MAP_SHARED)` → `CreateFileMappingW(PAGE_READONLY)` +
  `MapViewOfFile(FILE_MAP_READ)`；`munmap` → `UnmapViewOfFile` +
  `CloseHandle(section)`（**两个句柄都要收，`MappedFile` 需多存一个成员**）。
- `MADV_RANDOM`（`posix_file.cpp:188`）→ 移到 `CreateFile` 的
  `FILE_FLAG_RANDOM_ACCESS`（时机不同：mmap 后 vs 开文件时，需调整调用顺序）。
- `MADV_WILLNEED`（`hnsw.cpp:1406`）→ `PrefetchVirtualMemory`。
- `::sysconf(_SC_PAGESIZE)`（`hnsw.cpp:1380`）→ `GetSystemInfo`。
  ⚠️ **Windows 的 `dwAllocationGranularity`（64 KiB）≠ `dwPageSize`（4 KiB）**，
  映射视图的偏移必须按 64 KiB 对齐。当前是整文件映射（offset=0）故暂时安全，
  但 `hnsw.cpp` 的预取按页大小算，须区分用哪一个。

#### C8 — 路径与编码 · 面积广但机械

全库路径用 `std::string`（隐含 UTF-8）。
Windows 上 `fopen` / `_open` 走**系统 ANSI 代码页** → **非 ASCII 目录名直接
打不开**。移植层须统一 UTF-8 → UTF-16（`MultiByteToWideChar(CP_UTF8, ...)`）
并全走 `*W` API。

两个具体编译期地雷：

- `std::filesystem::path::c_str()` 在 Windows 返回 `const wchar_t*`。任何直接
  喂给 `::open` / `std::fopen` 的地方**编译失败** —— `file_util.hpp:96` 的
  `parent.c_str()` 即是一例。
- `src/fileops/data_file.cpp:452` 用 `filename.find_last_of('/')` 解析路径。
  Windows 分隔符为 `\`（虽然 API 接受 `/`，但 `fs::path` 产出的是 `\`）。
  改用 `fs::path::filename()`。

另需评估 **MAX_PATH 260 限制**：长路径需在 manifest 里 opt-in
`longPathAware`，或全路径加 `\\?\` 前缀（后者更可靠，但要求路径已规范化）。

#### C9 — 杂项

- `getrlimit(RLIMIT_NOFILE)`（`cask.cpp:184`，决定 read-handle LRU 默认上限）
  → Windows HANDLE 不受 fd 表约束（上限约 1600 万）。给一个保守常量即可，
  但需重新标定 LRU 预算的合理值（原逻辑是 `nofile` 的一个比例）。
- `dev_t` / `ino_t` 文件身份（`hnsw.hpp:44,480-481`，`.vec` 追加目标校验）→
  `GetFileInformationByHandle` 的 `dwVolumeSerialNumber` + `nFileIndexHigh/Low`
  （或 `FILE_ID_INFO`，128 位，ReFS 需要）。
  **顺带**：`<sys/types.h>` 目前出现在**公开头** `hnsw.hpp` 里，污染所有下游
  用户，应借机拆掉，换成库自有的 `FileIdentity` 结构。
- `std::filesystem::create_hard_link`（`cask.cpp:2494`，backup 用）NTFS 支持；
  跨卷/FAT 已有 `copy_file` 兜底分支，无需改动。
- `::clock_gettime` 1 处 → `std::chrono::steady_clock`（顺手标准化）。

---

## 4. D 段 · 测试移植

### 4.1 `fork()` 崩溃恢复测试 —— 工作量最易被低估处

4 个测试文件用 `fork()` + `_exit()` 模拟「进程崩溃后恢复」，这恰是全套测试中
**最有价值**的一批（验证 WAL、torn tail、墓碑复活门、原子批区间提交）：

| 文件 | fork 点 |
|---|---|
| `tests/crash_recovery_test.cpp` | 1 |
| `tests/oki_levelb_test.cpp` | 3（`:476` `:565` `:787`） |
| `tests/oki_recovery_test.cpp` | 1 |
| `tests/txn_test.cpp` | 1（`:182`） |

Windows 无 `fork`。改造方案：**子进程 = 用 `CreateProcess` 重新 exec 测试
二进制自身 + 专用 argv 开关分发场景**。

```
crash_recovery_test --bitcask-child-scenario=torn_tail --dir=<path>
```

父进程 `WaitForSingleObject` + `GetExitCodeProcess` 取代 `waitpid`。

**关键**：这一改造**完全可以在 Linux 上先做完并验证**（用 `posix_spawn` 或
同样的 exec-self 模式），不依赖任何 Windows 环境。应尽早排入，与 C 段并行。

> 注意：exec-self 模式下子进程不再继承父进程的内存状态。现有 fork 测试若
> 依赖「fork 时刻的内存快照」（例如已建好的 keydir），需改为子进程自己
> 重建 —— 逐个 case 确认，这是改造中的实质工作量所在。

### 4.2 零散 POSIX 依赖

- `::getpid()` 造临时目录名：`scanner_test.cpp:26`、`data_file_test.cpp:38`、
  `posix_file_test.cpp:30`、`oki_run_v2_test.cpp:42` 等 →
  `std::filesystem::temp_directory_path()` + `GetCurrentProcessId()`，
  或直接用 gtest 的临时目录设施统一。
- 硬编码 `/tmp` 路径：`scanner_test.cpp:53`。
- `<dirent.h>`：`thread_pool_test.cpp` → `std::filesystem::directory_iterator`。
- 直接 `stat` 断言 inode 不变：`cask_docvalue_test.cpp:38`、`hnsw_test.cpp:38`
  （`.vec` 追加语义）→ 换成 C9 的 `FileIdentity` 抽象。

---

## 5. CI

### 5.1 现状

`.github/workflows/ci.yml` 仅 `ubuntu-24.04`，两个 job（GCC 13 Release +
Clang Debug）。Clang job 的定位注释写着「作可移植性护栏，抓 gcc-ism」——
Windows 移植后这条护栏的价值进一步上升。

### 5.2 新增

`windows-2022` job：MSVC Release + 全量 ctest。
建议同时加一个 **`BITCASK_SIMD_MAX` 矩阵**（scalar / avx2 / avx512），
在同一 runner 上覆盖所有 ISA 档位的对拍（见 §2.7）。

### 5.3 Sanitizer 覆盖损失 · 需明确承认的风险

| Sanitizer | Linux | Windows/MSVC |
|---|---|---|
| ASan | ✅ | ✅ `/fsanitize=address` |
| UBSan | ✅ | ❌ |
| **TSan** | ✅（含插桩版 oneTBB，`CMakeLists.txt:50-66`） | ❌ **无** |

对一个重并发的库（epoch 回收、无锁 keydir 读、组提交、后台 merge），
**Windows 上失去 TSan 是实打实的护栏损失**。

缓解：
1. Windows 上的并发正确性**继续以 Linux TSan 为准**（同一份代码，
   并发逻辑本身平台无关）；
2. 但 §3.2 C3（每线程句柄池）与 C2（映射逐出与 epoch 协调）是
   **Windows 独有的并发新代码**，TSan 覆盖不到 —— 这两处必须补足够的
   压力测试，并考虑用 Application Verifier / `/analyze` 补位。

---

## 6. 分期与估时（单人）

| 阶段 | 内容 | 可在 Linux 验证 | 估时 |
|---|---|---|---|
| 0 | CMake MSVC 分流（`/utf-8` `NOMINMAX` `/bigobj` `/W4`）+ 静态库合并改 OBJECT 库 + vcpkg | 部分 | 4-6 天 |
| 1 | **平台抽象层收编**：51 处裸 `::open` 等收进 `io::File`，Linux 行为零变化 | ✅ 全部 | 2 周 |
| 2 | **测试改造**：fork → exec-self helper；去 `/tmp`/dirent/inode 假设 | ✅ 全部 | 1 周 |
| 3 | **SIMD 派发层重构**：内核出头文件、分 ISA TU、`cpu_features` CPUID | ✅ 大部分 | 2-2.5 周 |
| 4 | Windows I/O 后端：句柄、定位读写、`MoveFileEx`、UTF-16 路径、锁、Section 映射 | ❌ | 2.5-3 周 |
| 5 | **C2 删除/映射生命周期**（唯一架构改动）：evict-before-delete + epoch 协调 | 设计可 | 1-1.5 周 |
| 6 | CI（Windows job + SIMD 矩阵）、`/WX` 清理、bench 对拍、文档 | ❌ | 1-1.5 周 |

**合计约 10-12 周**至「Windows x64 MSVC 全套 ctest 通过 + SIMD 同档性能」。

### 关键排期建议

**阶段 1 + 2 + 3 共约 5 周，全部可以在 Linux 上完成**，且每一步对现有代码库
都是净收益（抽象层收敛、测试解耦、SIMD 分层）。应先行开工，**在没有任何
Windows 环境的情况下就能吃掉近一半工作量**，同时把风险最高的阶段 4/5
的攻击面压到最小。

### 风险排序

1. **C2 映射生命周期**（架构改动，且与并发回收耦合，TSan 覆盖不到）
2. **C4 PID 复用误判**（会导致库彻底打不开，且只在特定时序复现）
3. **C3 pread 并发退化**（测试全绿、只有 bench 掉，最易漏）
4. **§2.4 XCR0 漏检**（只在特定虚拟化环境 `#UD`）
5. **C1 rename 覆盖**（危害大但必然立刻暴露，反而安全）

---

## 7. 待确认项

- §2.2 的「MSVC intrinsic 无需 `/arch` 开关」前提，须在目标 VS 版本上实测
  （尤其 AVX-512 与 AVX-VNNI 的最低版本要求）。结论会决定 §2.3 分 TU 是
  性能需要还是正确性需要。
- cppjieba / limonp 在 MSVC 下的实际可编译性（header-only，但含类 POSIX 习惯）。
- 目标最低 Windows 版本（决定 `PrefetchVirtualMemory`、`FILE_ID_INFO`、
  长路径 opt-in 的可用性）。
- `std::expected` 等 C++23 库设施在目标 VS 版本的覆盖度（21 个 TU 依赖）。
