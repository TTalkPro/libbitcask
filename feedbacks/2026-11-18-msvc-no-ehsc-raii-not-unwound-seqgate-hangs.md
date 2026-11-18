# MSVC 构建不开异常展开 ⇒ RAII 跨异常不析构，SeqGate 序号停在奇数、读者死循环

**报的人**：keel（keelson，Windows x64，pin 8.0.1 `46eac41`，VS 18 Community
MSVC 14.51，preset `msvc-release`，全量 894 测跑到最后一个卡死）。
**严重度**：🔴 生产路径会挂死（不报错、不超时）；测试同场红。

## 症状

```
tests\sync_primitives_test.cpp(130): error: Expected equality of these values:
  gate.completed_writes()
    Which is: 0
  1u
    Which is: 1
```

随后 `gate.read([] { return 1; })` **100% CPU 永不返回**（读者看到奇数序号，
无限 yield 重试）。全量 ctest 里该子测试占住槽位 25 分钟以上不结束；
单独跑该二进制（带全部用例）同病同现，`--gtest_filter=-SeqGate.WindowReleasedOnException`
则 6/6 绿，问题收敛在这一个用例，且与测试逻辑无关——测试正是为了验证
「守卫析构仍离开窗口」这条注释里的契约（`tests/sync_primitives_test.cpp:126-135`）。

## 位置

- `include/bitcask/detail/seq_gate.hpp:51-60` `read()` 的重取循环；
  `seq_gate.hpp:15-25` `WriteGuard` 构造/析构各 `fetch_add(1)`。
- 根因在构建配置：`CMakeLists.txt:75-76` MSVC 全局
  `add_compile_options(/utf-8 /bigobj /permissive- /Zc:preprocessor /Zc:__cplusplus)`
  ——**没有任何异常展开开关（/EHsc）**。后果链：
  1. 每个 TU 编译时 `warning C4530: C++ exception handler used, but unwind
     semantics are not enabled`（8.0.1 全量构建日志刷屏，此前被当纯噪音放过）；
  2. 无展开 ⇒ 异常传播**不析构**跨过的局部对象（MSVC 文档明示行为）；
  3. `SeqGate` 头文件注释明写的契约「异常/提前 return 同样离开」在 MSVC 下不成立；
  4. 附带效应：`_CPPUNWIND` 未定义 ⇒ oneTBB `TBB_USE_EXCEPTIONS 0`，
     这就是 S47 修的那个 finalize 编不过的同一根因。

## 怎么撞上的

- 测试：`cmake --preset msvc-release && cmake --build --preset msvc-release
  && ctest --test-dir build-msvc-release`，其余 893 个全绿，
  卡死的就是这个用例（两台角度都复现：ctest 槽位挂死 + 单独跑失败后挂死）。
- 生产：任何异常穿过 `SeqGate::write()` 窗口作用域（如 OKI 写路径的
  `WriteGuard`）——Windows 上序号永远停在奇数，该 gate 的**全部读者**从此
  100% CPU 死循环。不报错、不超时。

## 影响面

- Windows/MSVC 下凡是「RAII 守卫跨异常」的路径行为都和 Linux/Clang 不同
  （守卫不析构）。SeqGate 只是目前**被测试钉住**的一个；其余跨 throw 的
  RAII（锁、句柄、defer 式守卫）同病，只是没被测到。
- C4530 告警是病征不是噪音，建议在修好前把它当错误看。

## 建议（下游不替 libbitcask 拍板）

**给 MSVC 全局加 `/EHsc`**（`CMakeLists.txt:76` 那行加进去即可，目录作用域
位置已经是对的）。理由：

1. 库大量依赖 RAII 跨异常语义（`SeqGate` 契约、C API `try_call` 结构、
   `c_api/bitcask_kv.cpp:297` 起的 shutdown 路径都按「析构会跑」写的），
   不开展开等于整套异常安全设计在 Windows 上失效；
2. 开了 `/EHsc` 后 `_CPPUNWIND` 定义 ⇒ oneTBB `TBB_USE_EXCEPTIONS 1`，
   S47 改的 nothrow 双参 `finalize` 依然成立（该重载两种模式都在），
   与 8.0.1 零冲突；
3. 副作用小：全库 94 处 `std::min/std::max` 已有 NOMINMAX 封死，二进制体积
   增量有限；相比「库内改写 SeqGate 为无异常依赖」改造面小得多。

修复后 Windows 回归建议把 `SeqGate.WindowReleasedOnException` 设为必测
（它就是这条语义的哨兵）。

