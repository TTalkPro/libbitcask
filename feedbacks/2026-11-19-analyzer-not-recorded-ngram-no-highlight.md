# 分词器配置没进 meta、ngram 不出高亮片段 —— 只读查看工具用不上冷文档高亮，也没法按建库时的分词器打开

**报的人**：keel（keelson clj-bitcask 1.5.0，转 bitcask-viewer V-18，pin 9.1.0 `baad538`，Linux x64）。
**严重度**：🟠 高亮结果缺失 / 检索可能用错分词器，均不报错（②③）；🟡 能力缺口（①）。

## 症状

bitcask-viewer 想把词袋检索的片段改用 S53 的 `bitcask_search_text_highlight_ex(..., BITCASK_HIGHLIGHT_LOAD_COLD, ...)`，
实测**一条片段都拿不到**：

```clojure
;; ① whitespace 建的库（目标文档 + 1500 篇填充，冷），重开后 :cold :load 检索 "fox"
{:read-only? true}                         => ([])            ; 不知道建库分词器，只能按缺省（ngram）开
{:read-only? true :analyzer :whitespace}   => (["<em>fox</em>"])
;; ② ngram（缺省分词器）库：同一进程刚写的小库（热，在原文 LRU 里）
search-highlighted "fox" / "着色器"          => ([]) / ([])  ; whitespace / jieba 同样步骤都有片段
;; ③ jieba 建的库按 whitespace 开：不报错，只在 log_fn 记一条 warn，检索结果静默变化
{:analyzer :jieba}       {日志结构 [b]}
{:analyzer :whitespace}  {日志结构 []}
```

## 位置

- ② `src/text/analyzer.cpp:458` `NgramAnalyzer::analyze_with_offsets`：每个 token 记 `TokenInfo{p, 0, 0}`（基类缺省同样，`:146`）
  ⇒ `src/search/highlighter.cpp:22` `collect_query_ranges` 丢掉 `end_byte <= start_byte` 的区间 ⇒ 片段恒空。
  缺省分词器就是 ngram，大多数库都是它。
- 同处还有一个顺序问题：`highlighter.cpp:127` 按 `unordered_map` 的遍历序逐词插标签，只收 `start_byte >= pos` 的出现——
  多个查询词时，**位置在前、遍历在后**的词不被标出；ngram 的 token 互相重叠（「着色」「色器」「着色器」）时更甚。
- ① `include/bitcask/meta_file.hpp`：meta 只记分析器配置的**指纹**（C1 尾段，`analyzer_fp`），不记配置本身 ⇒ 打开方
  无从得知建库时用的分词器；`bitcask_options_t::analyzer_type` 也没有「按库里记的」这一档。
- ③ `src/cask/cask.cpp:511`：配置指纹不符只 `log_warn`（C1 有意如此）。

## 怎么撞上的

1. 用 whitespace（或 jieba）分词建一个检索库、写够文档后关闭；
2. 另一个进程只读打开——它只能按缺省分词器（ngram）开，因为没有任何入口告诉它建库时用的是什么；
3. `bitcask_search_text_highlight_ex(..., LOAD_COLD)` ⇒ 片段全空（冷读回的正文按 ngram 切，而 ngram 没有偏移）。

ngram 库则不论冷热、不论怎么开，`highlights_count` 恒为 0。

## 影响面

- 只读查看 / 诊断工具（bitcask-viewer）：S53 的冷文档高亮在它那里等于没有；检索片段只能继续自己截（分词与引擎对不上）。
- 所有用缺省 ngram 分词的下游：高亮功能整个不可用，且不报错——下游分不出「没命中可高亮的」与「分词器不支持」。
- 打开方猜错分词器：查询切词与索引不一致，召回静默变化（只有 `log_fn` 的一条 warn；多数下游没接 `log_fn`）。

## 建议（下游不替 libbitcask 拍板）

1. meta 记下分析器配置本身（类型、n、停用词、token 长度上下限、词干），不只是指纹；
2. 打开选项加「按库里记的分词器」一档，查看工具不用猜；
3. 给一个不打开库就能读 meta 的 C 入口（现在 keelson 的 `probe` 是自己按字节解析 `bitcask.meta`，又多了一份内部格式依赖）；
4. ngram 的 `analyze_with_offsets` 给真实字节偏移；高亮器按位置排序、合并重叠区间后再插标签。

指纹不符要不要拒开，是 C1 的既定取舍，我们不要求改；有了 3，下游可以自己比对后决定。

## 回没回

**已修（未发版，S55）。** 四条建议都做了；「指纹不符拒开」按 C1 既定取舍未改。

### 怎么修的

- **meta 记配置**：C1 尾段后追加 C2 尾段（类型、n、停用词开关与自定义表、token 长度上下限、词干；`dict_path` 不记），
  自带 CRC，读端再用 C1 指纹自校验。建库、KV→检索库升级时记录；S55 之前的库**可写**打开且配置与指纹相符时补记。
  C++ `meta::MetaConfig::analyzer`。
- **按记录打开**：C `BITCASK_ANALYZER_STORED` / C++ `SearchLayerConfig::analyzer_from_meta`。`jieba_dict_path` 仍取选项。
  没有 C2 的旧库：C1 指纹是某个缺省配置（ngram / whitespace / jieba × 停用词 × 词干）就认出来按它开，否则 `INVALID_OPTION`。
- **不开库读 meta**：`bitcask_meta_read` / `bitcask_meta_info_free`（`bitcask_meta_info_t` 含向量配置与分析器配置）；
  失败分 `NOT_FOUND` / `BAD_CRC` / 新错误码 `BITCASK_ERR_UNSUPPORTED_FORMAT`（18）/ `IO`。
- **ngram 偏移**：`NgramAnalyzer::analyze_with_offsets` 给真实字节偏移（归一化文本坐标），与 positions / tf 两版共用收集与过滤。
- **高亮器**：片段内命中区间先排序、合并重叠再插标签——多个查询词不再漏标，ngram 重叠 token 标成一整段。

### 下游要知道的

- Keelson 的 `probe` 可以改调 `bitcask_meta_read`，不用再自己按字节解析 `bitcask.meta`；`:analyzer` 缺省时传
  `BITCASK_ANALYZER_STORED` 即「按库里记的开」（记录是 jieba 时照常给 `jieba_dict_path`）。
- 已有的库：9.2.0 之前建的只有指纹。用缺省配置建的，`STORED` 能直接认出；非缺省配置的，要用原配置**可写**打开一次补记，
  或者调用方显式给配置。
- 「指纹不符拒开」没有做（C1 既定：只告警）。要严格的下游先 `bitcask_meta_read`，与自己要用的配置比对后决定。
- 开发中顺带发现：S53 的 `HighlightColdTest` 在高并发下偶发失败（「写够填充 ⇒ 重开即冷」的前提不稳），9.1.0 基线同样，
  与本修复无关；记在 CHANGELOG「已知」。
