# `vec_engine_migrate` 在非 ASCII 目录下报「store in use by another process?」，其实没人占着

**报的人**：codex（Windows 兼容性验证 + 审计，12.0.3 `d995557`，Windows x64 / MSVC 14.51 / 系统 ACP 936）。
**严重度**：🟠 工具在非 ASCII 目录下用不了，报文指向错误方向；不碰数据。

## 症状

同一个空目录，只换目录名：

```
> vec_engine_migrate.exe --dir C:\...\Temp\bc-pathcheck-c654e3-中文库 --to ivfrq
cannot acquire C:\...\Temp\bc-pathcheck-c654e3-<乱码>\write.lock (store in use by another process?)
exit=1

> vec_engine_migrate.exe --dir C:\...\Temp\bc-pathcheck-db42c5-ascii --to ivfrq
read meta failed: read meta file truncated
exit=1
```

ASCII 那边越过了锁、走到了读 meta；中文目录连锁都没拿到，却报成「别的进程在用」。目录里根本没有 `write.lock`。

## 位置

- `tools/vec_engine_migrate.cpp:108`：`const std::string lock_path = (fs::path(dir) / "write.lock").string();`
- `tools/vec_engine_migrate.cpp:69`：`int main(int argc, char** argv)`，`argv` 原样进 `dir`（`:77`），没有编码转换。
- 同文件 `:59`：`ent.path().filename().string()`，同一类写法，在 `engine_files()`（`:41`）里；拿到锁（`:108`）之后由 `:146` 调用。
- `src/io/win32_file.cpp:119-133`：`widen()` 用 `CP_UTF8 + MB_ERR_INVALID_CHARS`，非 UTF-8 字节直接 `nullopt` ⇒ `open_handle` 回 `EINVAL`（`:309`）。

## 怎么撞上的

Windows 上 `main` 的窄 `argv` 是 CRT 按**系统 ACP** 转出来的（本机 936 = GBK），而库的约定是「窄路径一律 UTF-8」。
`:108` 把 GBK 字节经 `fs::path / .string()` 原样拼成 `lock_path`，交给 `FileLock::acquire`，后者走 io 层的 UTF-8 seam：
GBK 的「中文库」不是合法 UTF-8 ⇒ 拿不到句柄。`FileLock::acquire` 的失败被工具统一打印成「store in use」。

两个因素的实测对照：

- 只改目录名（ASCII → 中文），结果从「读 meta」变成「拿不到锁」。
- `bitcask_migrate`（`tools/bitcask_migrate.cpp`）对同一个中文目录的 `detect` / `hintord` / `be2le` 与 ASCII 目录行为一致
  （都报 `meta too short`）——它全程待在「窄串 = ACP」的世界里（`std::fopen`、`fs::path(窄)`），两头都是 ACP，抵消掉了。
  所以问题只在**跨到 UTF-8 seam 的工具**上现形，`vec_engine_migrate` 是其一。

## 影响面

- 中文 Windows 上用户目录常含中文（`C:\Users\张三\...`），库放在这种路径下时 `vec_engine_migrate` 必撞。
- 报错说「被别的进程占着」，运维会去找占锁的进程，找不到；报文里的路径还是乱码。
- 不报 `EINVAL` / 不提示编码问题，看不出是路径编码。
- 同类风险：任何从 `argv` 取路径再调库的 first-party 工具。`tools/` 目前没有 `from_utf8` / `wmain` 的使用。

## 建议

- 治本：工具入口把 `argv` 转成 UTF-8（`wmain`，或按 `CP_ACP` 转一次的公共 helper），之后所有路径都按库约定走。
- `:108` 改成 `bitcask::detail::to_utf8(bitcask::detail::from_utf8(dir) / "write.lock")`，与 `cask.cpp:172` 的 `acquire_writer_lock` 同写法；`:59` 用 `to_utf8(ent.path().filename())`。
- `FileLock::acquire` 的失败按 `errnum` 分流再出报文：`EEXIST` 才说「被占用」，`EINVAL` 说「路径不是合法 UTF-8」。
- 回归：非 ASCII 目录下 `vec_engine_migrate --dry-run` 能越过锁、到达读 meta 的那一步（ASCII / 非 ASCII 两目录报文一致）。
- `scripts/check-path-encoding.sh` 的扫描范围加上 `tools/`（现在只扫 `src include c_api`，所以 `:108` 的 `fs::path(窄)` / `.string()` 没被拦）。
