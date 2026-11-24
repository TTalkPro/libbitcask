# 文本编码与 Unicode 版本

> 从 README 拆出。[← 返回 README](../README.md)


## 文本编码：内部只吃 UTF-8

库内部全程只认 UTF-8——分词、NFKC_Casefold 归一化、highlight 的 byte offset
都以 UTF-8 码点边界为准。`put_doc` 的 `text` / `fields` 必须是 UTF-8。

**要不要转码是调用方的决定**，库只提供机制（`bitcask/text_encoding.hpp`），
不猜编码、不自动转换：

```cpp
#include "bitcask/text_encoding.hpp"
using namespace bitcask::text;

std::string utf8;
if (transcode_to_utf8(gb18030_bytes, "GB18030", utf8) != TranscodeStatus::kOk) {
    // 编码名不认识 / 输入是畸形序列 —— 由你决定是拒绝还是走 lenient
}
```

编码名走 ICU 的转换器名与别名（`GB18030`、`GBK`、`Big5`、`Shift_JIS`、
`EUC-KR`、`windows-1252` …），大小写与连字符不敏感；`encoding_supported()`
可先探测。默认**严格**：遇畸形字节返回 `kMalformedInput` 而非静默替换成
U+FFFD（ICU 转换器的出厂回调是 SUBSTITUTE，本层显式改成了 STOP）。
`transcode_from_utf8()` 是反向。

不转码直接喂非 UTF-8 字节不会崩，但那段文本的索引结果没有意义：非法序列被
替换成 U+FFFD，整段退化成一个不可检索的 token——**存得进去、搜不出来**。
拿不准就先过 `bitcask::text::detail::validate_utf8()`。

注意 `key` / `value`（`get`/`put` 那条路）**不受**此约束：它们是不透明字节，
库不做任何编码解释，GB18030 存进去原样取出来。

## 索引与 Unicode 版本的绑定

分词结果 = NFKC_Casefold 表 = Unicode 版本 = **ICU 版本**。换一个 ICU 版本打开
既有索引，新写入的文档可能被切成与老文档不同的 term，两边互相搜不到——而这
**没有任何显式症状**，只是召回悄悄少一块。

所以建索引时会把当时的 ICU / Unicode 主版本记进 `bitcask.meta`（`[12]`/`[13]`
两字节，在 CRC 覆盖区内），重开时比对，不一致就经 `CaskOptions::log_fn` 报一条
`kWarn`：

```
Unicode data version differs from when this index was built: index built with
ICU 76 (Unicode 16), current runtime is ICU 74 (Unicode 15). NFKC_Casefold
tables change between Unicode versions, so newly indexed documents may be
tokenized differently from existing ones and the two may not match each other.
Rebuild the index, or use the original ICU version, if recall consistency
matters for your corpus.
```

**只告警，不拒开。** 跨版本索引仍然可读可写，绝大多数码点的切分也没变；做成
硬门禁会让「升级发行版的 libicu」变成「所有库打不开」，代价远大于风险。是否
重建由你按自己的语料决定。

几点语义：

- **0 = 未记录**，不是「版本 0」。S38 之前建的目录、以及 KV 模式的目录（无
  文本分析）都是 0，比对时跳过——不会把「没记录」误报成「不匹配」。
- 记录只在**建索引时**写入（新建索引目录，或 `open_as_index` 把 KV 目录转成
  索引模式）。`v5→v6` 懒升级一类的 meta 重写**不会**覆盖它，否则记录会被刷成
  当次运行的版本、告警永不触发。
- 只记主版本：ICU 主版本与 Unicode 版本一一对应（ICU 74 = Unicode 15.1，
  76 = 16.0），主版本一致即数据表一致。
- 需要跨机器可复现的索引，用 `BITCASK_ICU_PROVIDER=vendored` 把版本钉死——
  这正是 vendored 模式最实际的用途。

三个取值的含义、怎么显式拉那个子模块、Windows 上的旋钮，见 [`build-zh.md`](build-zh.md)「ICU 的来源」。

**6.6.0 起同一思路覆盖 analyzer 配置本身**：建索引时把影响切词的 analyzer
配置（分词器类型、n-gram 范围、停用词、token 长度上下限、词干化）的指纹写进
`bitcask.meta` **可选尾段**（自带 CRC，旧二进制读写透明），之后以不同配置重开
同样只经 `CaskOptions::log_fn` 报一条 `kWarn`、不拒开。6.6.0 之前建的目录
未记录指纹：不告警，也不补记。尾段布局见
[`doc/format-zh.md`](format-zh.md) §3.1b。
