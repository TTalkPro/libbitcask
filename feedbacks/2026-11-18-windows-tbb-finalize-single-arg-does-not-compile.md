# Windows (MSVC) 上 `tbb::finalize(*handle_)` 单参重载编不过 —— 整个库链接不起来

**报的人**：keel（keelson，Windows x64，pin 8.0.0 `eade59e`，VS 18 Community
MSVC 14.51，preset `msvc-release`）。
**严重度**：🔴 构建当场停。

## 症状

```
include\bitcask\thread_pool.hpp(633): error C2660:
    'tbb::detail::d1::finalize': function does not take 1 arguments
```

## 位置

- `include/bitcask/thread_pool.hpp:633` `TbbLifetime::release()`：
  `tbb::finalize(*handle_);`
- 对照：仓库自己已经在用 nothrow 版的地方——`c_api/bitcask_kv.cpp:301`
  `oneapi::tbb::finalize(handle, std::nothrow)`。

## 怎么撞上的

任何包含 `thread_pool.hpp` 的编译单元在 Windows/MSVC 下都过不了。复现最短路径：

```
cmake --preset msvc-release && cmake --build --preset msvc-release
```

病因链（已逐步核实）：

1. libbitcask 的 CMake 全程没有给 MSVC 加 `/EHsc`（全仓库搜不到 `EHsc` /
   `CPPUNWIND` / `EXCEPTIONS`），所以编译开关不带异常展开，`_CPPUNWIND` 未定义
   （每个 TU 都有 `warning C4530: ... unwind semantics are not enabled` 佐证）。
2. oneTBB `detail/_config.h`：无 `_CPPUNWIND` ⇒ `TBB_USE_EXCEPTIONS 0`。
3. oneTBB `global_control.h`：单参 `finalize(task_scheduler_handle&)` 整段包在
   `#if TBB_USE_EXCEPTIONS` 里；`TBB_USE_EXCEPTIONS=0` 时只剩
   `finalize(handle, std::nothrow)` 一个重载。
4. Linux/Clang 下 `__EXCEPTIONS` 恒为 1，所以单参重载一直在，CI 全绿，
   问题只在 Windows 暴露。

## 影响面

- Windows 上 8.0.0（以及所有含 `TbbLifetime` 的版本）**编译不过**，无绕法
  （头文件级错误，任何包含者都中招）。
- 报错信息指的方向有误导性（"function does not take 1 arguments" 不会让人
  想到是异常开关），按报文改容易改错。

## 建议（下游不替 libbitcask 拍板）

二选一即可，下游倾向 1（与 `c_api/bitcask_kv.cpp:301` 现有写法对齐，跨平台
行为一致——nothrow 版在 `TBB_USE_EXCEPTIONS=1` 时同样存在）：

1. `thread_pool.hpp:633` 改用 nothrow 重载：
   `tbb::finalize(*handle_, std::nothrow);`（需 `<new>`）。
2. 或给 MSVC 全局加 `/EHsc`（改动面大，且会改变全库的异常语义，不建议顺手做）。

