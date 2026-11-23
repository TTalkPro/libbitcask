# 路径编码守门脚本在 `src/` 里已有两处命中：backup 的 `path::string()`（`7af29da` 起）

**报的人**：codex（Windows 兼容性验证 + 审计，12.0.3 `d995557`）。
**严重度**：🟡 CI 的 `path-encoding` job 按规则会红（或已红）；`backup` 本身功能未见异常。

## 症状

按 `scripts/check-path-encoding.sh` 的规则（扫 `src include c_api` 的 `.string()`，只放行 4 个转换层文件和注释行）
人工比对，命中两行：

```
src/cask/cask.cpp:3345:        const auto name = entry.path().filename().string();
src/cask/cask.cpp:3349:                "backup: copy failed: " + entry.path().string()));
```

`git blame`：两行都来自 `7af29da`（2026-11-15，backup 复制 `kv.oki.seg-*`）。

## 位置

- `src/cask/cask.cpp:3345`、`:3349`（`backup()` 里遍历 `base` 目录的那段，`:3343` 起）。
- `scripts/check-path-encoding.sh:28-29`：`scan()` 的扫描目录写死 `src include c_api`（`:65`、`:95` 两处后续检查同样不含 `tools`）；`:47` 是 `\.string\(\)` 规则。
- `src/io/win32_file.cpp:114-117`：注释「⚠️ 已知缺口：库内多处经 `std::filesystem::path::string()` 产出窄路径……见 TASK.md 的遗留项」。

## 怎么撞上的

脚本本机没跑成（本机 `bash` 解析到 WSL，起不来；没装 Git Bash），所以**没有脚本的原始终端输出**，
上面是用同一条正则、同一份放行清单在 PowerShell 里复刻的结果；CI 上 `path-encoding` job 的实际颜色未核实（本机没有 `gh`）。

## 影响面

- **守门失效**：这条 job 是 `build-test` 之前唯一挡「窄路径不是 UTF-8」的关卡；它一红，后面的同类回退就混在旧红里看不见。
- **`backup` 本身**：`:3345` 的 `name` 只用来 `starts_with("kv.oki.seg-")`，文件名是 ASCII，判定不受影响；
  `:3349` 只进错误报文，非 ASCII 目录下报文里会是 GBK 字节混在 UTF-8 里。没发现功能性后果。
- **注释过期**：`win32_file.cpp:114` 说「库内多处」，实际 `src/include/c_api` 只剩上面两处。

## 建议

- `:3345` 改 `bitcask::detail::to_utf8(entry.path().filename())`；`:3349` 改 `to_utf8(entry.path())`。
- 本地给 `cmake --build` 加一个不依赖 bash 的等价检查（CMake script 或 CTest 用例），让 Windows 开发者也能在提交前看到。
- 更新 `win32_file.cpp:114` 的「已知缺口」注释（剩余站点与工具侧那条见
  [`2026-11-23-vec-engine-migrate-non-ascii-dir-false-store-in-use.md`](2026-11-23-vec-engine-migrate-non-ascii-dir-false-store-in-use.md)）。

## 回没回

已修（未发版）。写本条时脚本没跑成，这次用 Git 自带的 `sh`（`scoop shims\sh.exe`；`bash` 解析到 WSL 起不来）跑了原脚本，结论有修正：

- **HEAD（`d995557`）上脚本是红的，且不止这两处**：除 `cask.cpp:3345` / `:3349` 外，还有
  `c_api/bitcask_kv.cpp:147` / `:150`（`9f940ed`，11-19）与 `src/text/jieba_analyzer.cpp:87`（`a30186d`，11-20）三处——
  后三处入参本就是 `fs::path`，只是变量名不合 `*_path` 约定（假阳性，改名 `dir_path` / `file_path` 即可）。
  按 `7af29da` 的日期，`path-encoding` job 从 11-15 起就红，11-19 起红 5 处；CI 上的实际颜色仍没核实（本机没有 `gh`）。
- **更大的问题在脚本自己**：隐式规则的正则只认 `(std::)?filesystem::x(`，而库内绝大多数调用点写的是 `fs::` 别名
  ⇒ `Cask::open` / `upgrade` / `backup` / 三个迁移入口的「窄串直接喂 `fs::`」全都漏过（详见工具侧那条的「回没回」）。
  本次：正则改 `((std::)?filesystem|fs)::`，新增 `fs::path x(窄)` 具名变量规则，扫描范围加 `tools`，
  受检函数加 `create_hard_link` / `create_symlink` / `directory_iterator` / `canonical` / `weakly_canonical` / `absolute` / `equivalent` / `last_write_time`。
- `:3345` 改 `to_utf8(entry.path().filename())`；`:3349` 只进报文、改为同一个 `name`。`win32_file.cpp:114` 的注释已改写成现状
  （库内站点已清，`fs::` 仍只收 `fs::path`，窄串一律经 `from_utf8`）。
- **没做**建议里的「CMake script 版等价检查」：规则是一组正则，移植成 CMake 要维护两份、迟早漂移；
  Windows 上装了 Git for Windows（或 scoop 的 `sh`）就能跑，「怎么撞上的」里缺的只是这个环境。
