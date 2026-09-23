# C1 设计：analyzer 配置指纹（bitcask.meta 可选尾段）

> 状态：**已实现，随 6.6.0 发布**（2026-09-23；来源 `TODO.md` C1）。
> 盘上格式：`bitcask.meta` **向后兼容的追加**，不 bump meta 版本，不需要迁移。
> 布局的权威描述在 `doc/format-zh.md` §3.1b；本文记录设计取舍。

---

## 1. 问题

分词结果由 analyzer 配置决定：分词器类型（Ngram / Whitespace / Jieba）、
n-gram 范围、停用词、token 长度上下限、词干化。**配置只存在于每次开库传入的
`SearchLayerConfig` 里，盘上没有记录。**

所以用 Ngram(2,3) 建的索引，换成 Ngram(1,2) 或 Jieba 重新打开：

- 新写入的文档按新配置切词，老文档还是老 term；
- 查询也按新配置切词，搜不到老文档里的 term；
- **没有任何报错**，只是召回悄悄少了一块。

这与 S38（ICU / Unicode 版本变化导致 NFKC 表不同）是同一种失败形态。S38 已在
meta [12..13] 记录版本并在重开时告警；analyzer 配置此前没有同等守门
（`meta_file.cpp` 无 analyzer 字段）。

## 2. 约束

| 约束 | 来源 |
|---|---|
| meta 定长头 18 字节，**已无空闲字节**：[12..13] 被 S38 的 ICU / Unicode 主版本用掉 | `meta_file.cpp:28-31` 注释「再加字段就必须扩文件长度并 bump 版本了」 |
| meta 是**唯一纪元门禁**，改版本号等于整库格式变更 | `meta_file.hpp`、tstamp64 / S33 flag-day 先例 |
| 这是**诊断信息**，不是正确性门禁：不一致只该告警，不该让库打不开 | 与 S38 同策略（升级发行版 libicu 不能让所有库打不开） |
| 旧二进制（6.5.x 及以前）必须仍能打开新建的库 | 下游 pin 旧 tag 的常态 |

## 3. 方案比较

| 方案 | 做法 | 否决 / 采纳理由 |
|---|---|---|
| A. 扩 meta 版本（v7，定长 22+ 字节） | 新增字段进 CRC 覆盖区 | ❌ 旧二进制见 v7 直接拒开（纪元门禁语义），为一个告警付出整库不兼容，代价远大于收益 |
| B. 独立 sidecar 文件（如 `analyzer.fp`） | 新文件，旧二进制忽略 | ⚠️ 可行，但多一个需要随目录复制 / 备份 / 迁移的文件；`link_or_copy` 备份、迁移工具等处都要记得带上 |
| **C. meta 可选尾段（采纳）** | 定长头之后追加 9 字节，自带 CRC | ✅ 旧读端只读前 18 字节、**不检查文件长度**（`meta_file.cpp` read_meta），对尾段透明；不增文件；备份 / 复制天然随 meta 走 |

TODO 原文写的是「写进 meta（CRC 覆盖区内）」。定长头已满，覆盖区内无处可写，
方案 C 用尾段自己的 CRC 达到同等保护。

## 4. 布局

```
偏移 字节  字段           编码         含义
 0  18    定长头          —           不变（magic / version / mode / 向量 / ICU / CRC）
18   1    TailVersion     u8          = 1（kMetaTailVersion）
19   4    AnalyzerFp      u32 LE      text::analyzer_fingerprint(建索引时的 AnalyzerConfig)
23   4    TailCrc32       u32 LE      覆盖 [18, 23)
─────────────────────────────────────────
     27 字节合计
```

- **只在 `analyzer_fp != 0` 时写尾段**。未记录的目录（KV 模式、建库时无
  `search_config`、C1 之前建的目录）保持 18 字节，与旧版逐字节相同。
- `TailVersion` 为以后在尾段追加字段留余地：新版本号的尾段，旧读端按「未知版本
  = 未记录」处理。

实现：`src/cask/meta_file.cpp`（`kMetaTail*` 常量、read_meta 尾段解析、
write_meta 追加）；`MetaConfig::analyzer_fp`（`include/bitcask/meta_file.hpp`）。

## 5. 指纹

`text::analyzer_fingerprint(const AnalyzerConfig&)`（`src/text/analyzer.cpp`）：

- **FNV-1a 32**，纯函数、跨平台稳定——值要落盘，不能用 `std::hash`（实现相关）。
- 首字节混入**方案版本号 1**：以后改归一规则时递增，旧记录自然不等（会告警一次，
  用户确认后重建或忽略）。
- 结果恒非零（0 映射为 1），因为 0 在 meta 里表示「未记录」。

### 5.1 覆盖哪些字段

原则：**只覆盖影响切词结果的字段，并按分词器类型归一**。无关差异一旦触发告警，
用户很快会习惯性忽略所有告警。

| 字段 | Ngram | Jieba | Whitespace | 说明 |
|---|---|---|---|---|
| `type` | ✅ | ✅ | ✅ | |
| `min_token_length` / `max_token_bytes` | ✅ | ✅ | ✅ | 三种分词器都读（`analyzer.cpp` 注册处） |
| `enable_stemming` | ✅ | ✅ | ✅ | 工厂外包一层 `StemmingAnalyzer` |
| `min_n` / `max_n` | ✅ | ✅ | ❌ | Whitespace 不读 |
| `enable_stop_words` | ✅ | ✅ | ❌ | 同上 |
| `stop_words` | 仅启用时 | 仅启用时 | ❌ | 按**集合**语义：排序 + 去重后计入，顺序与重复不影响 |
| `dict_path` | ❌ | ❌ | — | 机器相关的路径；换机器路径不同但词典相同是常态。词典**内容**变化无法廉价探测，不在本设计范围 |

**同义词不计入**：同义词在查询时展开（`text_plugin.cpp` `synonym_map_->expand_terms`），
不影响索引里的 term，换同义词表不会让新旧文档分叉。

## 6. 写入与比对

| 时机 | 行为 | 位置 |
|---|---|---|
| 新建索引目录，带 `search_config` | 记录指纹 | `cask.cpp` check_or_create_meta |
| KV → 索引模式升级（`Cask::upgrade`） | 记录指纹（此刻起按该配置分词） | `cask_recovery.cpp` |
| 其他任何 `write_meta`（v5→v6 懒升级、`vec_engine_migrate` 等） | **原样透传读回的值**，绝不取当前配置 | 与 S38 同一不变式 |
| 重开：索引模式 + 记录非 0 + 本次带 `search_config` | 重算指纹，不等则 `kWarn`，**仍然开库** | `cask.cpp` 紧跟 S38 告警 |

**不补记**：记录为 0 的旧目录，重开时不把当前配置写进去。原因同 S38：我们无从知道
这个库当初是用什么配置建的，把当前配置记成「原始配置」会让之后的比对失去意义。

## 7. 兼容矩阵

| 场景 | 结果 |
|---|---|
| 新二进制读旧 meta（18 字节） | 无尾段 → `analyzer_fp = 0` → 不比对，静默 |
| 旧二进制读新 meta（27 字节） | 只读前 18 字节，正常开库，不知道尾段存在 |
| 旧二进制重写新 meta（懒升级 v6 等） | 只写 18 字节，**尾段丢失** → 之后新二进制视为未记录，不再告警。退化但不出错 |
| 尾段截断 / 版本未知 / CRC 不符 | 视为未记录，**不拒开**（诊断信息，不 fail-fast） |
| 迁移工具（`migrate.cpp` be2le / tstamp64 / hintord） | 只处理旧纪元 meta（它们本就没有尾段），固定写 18 字节；不受影响 |

## 8. 测试

`tests/meta_unicode_version_test.cpp`，`MetaAnalyzerFpTest` 8 项：

| 用例 | 钉住的点 |
|---|---|
| `RecordsFingerprintOnCreate` | 带 search_config 建库记录非零指纹，文件 27 字节 |
| `NoAnalyzerLeavesMetaAt18Bytes` | 不分词时不写尾段，与旧版同形 |
| `SameConfigReopenIsSilent` | 同配置重开不告警 |
| `DifferentConfigWarnsButStillOpens` | 换 n 范围 / 换分词器告警，且仍能开库 |
| `IrrelevantDifferencesDoNotChangeFingerprint` | Whitespace 改 n、停用词换序去重、改 dict_path、未启用时的停用词都不改指纹；真正影响切词的四类字段各自改指纹 |
| `TruncatedTailDegradesToUnrecorded` | 模拟旧二进制重写（截到 18 字节）→ 未记录、静默 |
| `CorruptTailIsIgnoredNotFatal` | 尾段位翻转 → 仍能开库，视为未记录 |
| `RewritingMetaPreservesFingerprint` | 懒升级重写 meta 保留原始指纹，之后仍能告警 |

## 9. 已知局限

- **旧二进制重写会丢记录**：混用新旧二进制、且旧二进制触发了 meta 重写（首次
  `put_batch_atomic` 懒升 v6）后，该库不再有指纹告警。属可接受的退化。
- **词典内容变化探测不到**：Jieba 换了同路径下的词典文件，指纹不变。若以后需要，
  可在 TailVersion 2 里追加词典文件的内容摘要，但开库要多读一遍词典。
- **只告警，不阻止**：与 S38 一致。是否重建由用户按自己的语料判断。
