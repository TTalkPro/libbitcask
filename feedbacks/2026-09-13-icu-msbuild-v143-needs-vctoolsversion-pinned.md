# vendored ICU 在 VS 18 上编不动：`/p:PlatformToolset=v143` 少了配套的 `VCToolsVersion`

**报的人**：keel（下游消费者，pin tag 6.3.2 `598d080`）
**原始报告**：porthole（2026-09-10，Windows 11 / VS 18 真机撞的）；coxswain 同日在
另一棵树上原样复现，并补了一次 A/B 实测。keel 自己那半（`KeelIcu.cmake`，keel 1.1.2）
已按本条建议修掉，VS 18 真机验证过。
**严重度**：🔴 configure 全绿、构建当场停，而报文指向一个**与病因无关**的方向。

## 症状

`cmake --build` 走到 vendored ICU 的 ExternalProject build 步（第一个工程是
`icu4c/source/stubdata/stubdata.vcxproj`）：

```
error MSB8052: MSVC 工具集版本“14.51.36231”与“v143”平台工具集不兼容。
请将平台工具集更改为 v145，或将 MSVC 工具集版本(VCToolsVersion 属性)设置为
采用格式 "14.3*.*" 的版本。
```

⚠️ **那个 14.51 不是调用方选的工具集**。出事的 shell 是货真价实的 14.44 环境：
`cl.exe` 在 `.../MSVC/14.44.35207/bin/Hostx64/x64/`，`%VCToolsVersion%` = 
`14.44.35207`（进 MSBuild 之前 `echo` 得出来），`MSVC_TOOLSET_VERSION` = 143 ⇒
`_icu_toolset` = `v143` 也是对的。**是 MSBuild 自己把 14.51 找了回来。**

⇒ 报文让人改 PlatformToolset，而**照它改是错的**：14.51（与 14.50）的
`bin\Hostx64\x64\` 里**没有 `cl.exe`、没有 `link.exe`**（coxswain 逐条目数过，
15 个条目全是 PGO/分析工具；14.44 是那台机器上唯一齐的）。换成 v145 得到的
不是另一句 MSB8052，是「找不到编译器」。

## 位置

`cmake/BitcaskICU.cmake:521-522`（生成 `build-icu.bat` 的那段）：

```cmake
" /p:PlatformToolset=${_icu_toolset}"
" /p:DefaultPlatformToolset=${_icu_toolset}")
```

⭐ 病根不在这两行**写错了**，在于它们**只写了一半**。`:299-300` 那段注释已经把
道理说到位了 ——「命令行 `/p:` 是全局属性、工程内不可覆盖，正好压住那段自动
探测」—— 而 `VCToolsVersion` 需要的恰恰是同一手，却没跟上。

## 怎么撞上的

最短路径：**任何装了 VS 18（Visual Studio 2026）的机器**上走一次 vendored ICU
（`BITCASK_ICU_PROVIDER=auto` 在 Windows 上找不到系统 ICU ≥ 60 就回落 vendored，
典型如此；`vendored` 档直接必走）。链条是 MSBuild 自己的，三步，每一步单独看
都合理、都是 Microsoft 的设计，**不是装坏了**（两家人逐步核过、逐字对上）：

1. `MSBuild\Microsoft\VC\v170\Microsoft.Cpp.Default.props:42` 开头的清空表里
   **明写着 `<VCToolsVersion />`** ⇒ **vcvars 注进来的环境从这里起就不算数**。
   ⛔ 这也是为什么「在 `build-icu.bat` 里 `set VCToolsVersion=…`」**没有用**
   （实测：`.bat` 里 `echo` 出来是 14.44，MSB8052 仍报 14.51）。环境变量进
   MSBuild 只是普通属性，工程里一句 `<VCToolsVersion />` 就压掉；
   **命令行 `/p:` 是全局属性，压不掉** —— A/B 见下。
2. `Microsoft.Cpp.VCTools.props:28` 按 PlatformToolset 找
   `Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.props`，
   **找不到就退回**版本无关的 `Microsoft.VCToolsVersion.default.props`。
3. 而 **VS 18 不发 `…v143.default.props`**：本机同名 `.txt` 在（内容正是
   `14.44.35207`）、`.props` 不在；`v145.default.props` 在。于是第 2 步退回去
   的那份按「v145 → v143 → v142 → v141」取**最新的一个**，拿到 v145 ⇒
   `VCToolsVersion` = 14.51.36231 ⇒ 与 `/p:PlatformToolset=v143` 撞上 MSB8052。

### A/B 实测（coxswain）：只差那一句 `/p:`

拿失败清单点名的 `stubdata.vcxproj` 单独跑，其余参数与 `build-icu.bat` 生成的
**逐字相同**：

| | 命令行 | 结果 |
|---|---|---|
| **A** | `… /p:PlatformToolset=v143 /p:DefaultPlatformToolset=v143` | `error MSB8052`，rc=**1** |
| **B** | 同上 **+ `/p:VCToolsVersion=14.44.35207`** | `stubdata.cpp` → 链出 dll，rc=**0** |

⇒ 补那一句**就够**，不需要别的。

### ⛔ 与外层生成器无关（别指望换 Ninja 避开）

MSBuild 调用是 `build-icu.bat` **自己**发起的（`:283-290`：拿不到
`CMAKE_VS_MSBUILD_COMMAND` 就 `find_program(MSBuild.exe)` 自搜），ICU 在
Windows 上只有 MSBuild 一条路。外层 CMake 用 VS 还是 Ninja，这一步原样发生
—— coxswain 的树**生成器钉死 Ninja**（cmake 4.4.3 + Ninja 单配置），同一台
机器上 MSB8052 原样复现。

## 影响面

- **谁会撞**：VS 18 上走到 vendored ICU 的构建（auto 回落 / vendored 档）。
  Windows 上 keel 侧 ICU 关不掉 ⇒ 下游没有绕路（coxswain 实测
  `KEEL_SYSTEM_ICU=ON` 在那台机器上照样回落 vendored）。
- **撞了会怎样**：configure 全绿，构建停在 ICU 那步。**报错，不静默** ——
  好消息；坏消息是报文把人指向「改 PlatformToolset / 修 VS 安装」，**两条都不通**
  （v145 没 cl.exe；安装没坏 —— 三步都是 MSBuild 的设计）。本机从报文到病因
  走了三层 props 才对上。
- **不咬的**：Linux / macOS；Windows 上真找到系统 ICU 的树；
  VS 2019/2022（那两版 `…v143.default.props` 在）。
- ⚠️ 一句话附注：MSB8052 报文里的 `14.3*.*` 是 Microsoft 写窄了 —— v143 吃得下
  14.4x（本机就是这么过的）。照它字面找一个 14.3x 工具集会白跑一趟。

## 建议

⛔ 不替 libbitcask 拍板，第一条更贴 `:299` 那段注释的原意：

1. ⭐ **把同一手用在 `VCToolsVersion` 上** —— 命令行 `/p:` 是全局属性，
   清空表清不掉它：

   ```cmake
   #  /p: 是全局属性，工程内的 <VCToolsVersion /> 清不掉它
   if(_icu_vctools_version)   # 例如 14.44.35207
     string(APPEND _icu_bat_body " /p:VCToolsVersion=${_icu_vctools_version}")
   endif()
   ```

   版本号的来源，两家用过都行，但**投「从编译器路径切」一票**：

   * `CMAKE_CXX_COMPILER` 匹配 `/MSVC/([0-9.]+)/bin/` 取一段 —— 不依赖调用方
     进没进 vcvars；且 `_icu_toolset` 推自 `MSVC_TOOLSET_VERSION`，两者**同源**
     （都出自这一个编译器）⇒ 不可能互相打架。A/B 里跑通的 `14.44.35207`
     正是这么切出来的。
   * `$ENV{VCToolsVersion}` 是 **configure 时**读、**build 时**才用，两次不在
     同一个 shell 里（下游每条命令一个新进程）。今天相等是约定不是保证，
     不相等那天它会**静默编出一棵混工具集的 ICU** —— 只配当兜底。
   * ⛔ **拿不到就不写这一句**（保持旧行为），别瞎猜一个版本号钉上去 ——
     钉错的后果与这条一模一样，只是换个方向。
2. ⭐ configure 期 STATUS 把「钉的是哪个版本」说出来（keel 的输出形如
   `[ICU · Release | x64 | v143 | VCToolsVersion 14.44.35207]`）—— 这一格错了
   会编不动，而 MSB8052 指的是别的方向，值得当场省掉从报文反查三层 props
   的那一趟。
3. 参考实现：keel 的 `cmake/KeelIcu.cmake:314` 起（keel 1.1.2 落地，VS 18 真机
   A/B 过；含「拿不到不钉」与 STATUS 两格）。

---

## 修复记录（libbitcask 侧，2026-09-13）

按建议 1/2 落在 `cmake/BitcaskICU.cmake`：

- **`/p:VCToolsVersion` 自动钉**：优先从 `CMAKE_CXX_COMPILER` 路径切
  `…/MSVC/<ver>/bin/` 一段（正斜杠 / 反斜杠路径都认，`MyMSVC` 这类不误伤）；
  `$ENV{VCToolsVersion}` 只作兜底（configure 读、build 用的时窗风险，照建议原文
  处理）；都拿不到不写这一句，保持旧行为。追加位置与
  `/p:DefaultPlatformToolset` 同一条 MSBuild 命令行。
- **STATUS 第四格**：vendored 那条 STATUS 补成
  `[Release | x64 | v143 | VCToolsVersion 14.44.35207]`；没钉时明写
  `VCToolsVersion 未钉（MSBuild 自选）`，不留「以为钉了」的想象空间。
- 未采纳（现阶段）：未给 `VCToolsVersion` 加显式缓存旋钮——报告的两条建议都不
  需要；下游真要强钉时 `-D` 传编译器路径之外的手段是另一条账。

⚠️ 本仓库无 Windows / VS 18 环境，A/B 未在本侧重跑；逻辑以报告里的三步链路与
coxswain 的 A/B 为准，等 keel 侧下次真机构建交叉验证。
