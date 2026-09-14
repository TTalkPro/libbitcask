# C API 够不到 merge 策略、显式文件表与 checkpoint —— 下游只能吃缺省、只能等 `needs_merge` 点头

**报的人**：keel（下游消费者，pin 6.3.3 `780755a`），转的是锦书（keel 的宿主，
一本书一个 bitcask）2026-09-13 的账：keel `feedbacks/2026-09-13-jinshu-bitcask-merge-policy.md`。
**严重度**：🟡 不是 bug，是**调不到的旋钮**——行为全按缺省走，一个字都不报；
下游今天靠 `close_write_file` + `close` 重开绕过去了，绕法多一次重开（百毫秒）、
且压缩阈值收不下来。

## 症状

锦书一本书一个索引，几 MB 到几十 MB。要压缩，四步：`close_write_file`（不封的话
`needs_merge` 永远 `needs=0`——2 GiB 缺省下它一辈子住在一个 active 文件里）→
`needs_merge`（60% 碎片才触发 ⇒ 盘上占用上界 ≈ 2.5 × 活数据）→ `merge` →
`close` 再开（退休文件要到 close / 下次 merge / checkpoint 才真删）。
探针：500 条文档写 4 轮 2.7 MB → 封之前 `needs=0`；merge 后新文件 685 KB，
**旧 2.7 MB 还在**；close 重开之后才没。

## 位置

- `c_api/bitcask_kv.h`：`bitcask_options_t` 没有 `merge::PolicyOptions` 那九格；
- `c_api/bitcask_kv.h:829`：`bitcask_merge(cask, fault)` 只有空表那一支，
  注释却写着「files 为 NULL 时自动」——签名里根本没有 files；
- `Cask::checkpoint()` 没接进 C API。
- （`max_file_size` C API 本来就有，是 keel 那一侧没接，不算这里的账。）

## 怎么撞上的

任何走 C API、库小而写多删多的宿主：桌面写作软件、一人一库那种。

## 影响面

- 只能吃 60/40 那组缺省，收不紧；
- `needs_merge` 说不需要就什么都劝不动——宿主算得出碎片率也没地方递；
- 退休文件回收只能靠 close 重开。
- 报不报错：**不报**——它是「没有那个口子」。

## 建议（keel 提的三条 + 一条附带）

1. `bitcask_options_t` 里开 merge 策略，缺省逐字等于上游缺省；
2. `bitcask_merge` 收显式文件表（上游 `merge(std::vector<std::string>)` 就在那儿）；
3. 暴露 `checkpoint()`。

## 回

✅ **6.4.0 全收**，一处与建议不同：策略**没**放进 `bitcask_options_t`
（改布局 = ABI 破坏 = major），走独立的 `bitcask_merge_policy_t` +
`bitcask_open_ex`，`bitcask_open` 逐字节等价。另两条照建议：`bitcask_merge_files`
（⚠️ 空表 = 自动，不是并零个）、`bitcask_checkpoint`。
⭐ 顺带 `Cask::merge(files)` 对显式表多两道闸（名字不对 / active 文件），
当场抓到两条自家判据在并 active 文件。C API 文档 §8.1b / §12.5b / §12.5c。
