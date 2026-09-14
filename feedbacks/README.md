# feedbacks —— 下游消费方报上来的账

一条一个文件，`<日期>-<slug>.md`。**只记「libbitcask 这一侧该改」的**：
下游自己绕过去了但绕法很蠢、或者根本绕不过去的那些。下游自己的 bug 不进这里。
格式与判据照下游 keel 的 `feedbacks/` 约定（症状先行，病因其次，建议最后；
报文让人改错方向的，与不报错同罪）。

| 段 | 写什么 |
|---|---|
| **症状** | 下游看到的**那一行**（报错原文 / 或者「一个字都不报」） |
| **位置** | `文件:行`，libbitcask 这一侧的 |
| **怎么撞上的** | 最短复现路径 |
| **影响面** | 谁会撞、撞了会怎样、**报不报错** |
| **建议** | 可选，下游不替 libbitcask 拍板 |

## 现有

| # | 标题 | 报的人 | 严重度 | 回没回 |
|---|---|---|---|---|
| [2026-09-14](2026-09-14-c-api-merge-policy-files-checkpoint-not-reachable.md) | C API 够不到 merge 策略 / 显式文件表 / checkpoint：宿主只能吃 60% 缺省、只能等 `needs_merge` 点头、退休文件只能靠 close 重开回收 | keel（转锦书 2026-09-13） | 🟡 调不到的旋钮，不报错 | ✅ 已修（**6.4.0**：`bitcask_merge_policy_t` + `bitcask_open_ex` / `bitcask_merge_files` / `bitcask_checkpoint`；策略走独立结构体而非 `bitcask_options_t`，ABI 不破） |
| [2026-09-13](2026-09-13-icu-msbuild-v143-needs-vctoolsversion-pinned.md) | vendored ICU 在 VS 18 上编不动：`/p:PlatformToolset=v143` 少了配套的 `VCToolsVersion` ⇒ MSB8052，报文指的两条改法都不通 | keel（porthole 报 · coxswain 复现 + A/B） | 🔴 构建当场停，报文指向与病因无关的方向 | ✅ 已修（`/p:VCToolsVersion` 自动钉 + STATUS 第四格；VS 18 真机待复验） |
