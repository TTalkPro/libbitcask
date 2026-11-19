# 高亮检索对「冷」文档永远不出片段 —— 只读快照 / 新开进程的查看工具拿不到任何高亮

**报的人**：keel（keelson clj-bitcask，转 bitcask-viewer V-10，pin 9.0.0 `d164744`，Linux x64）。
**严重度**：🟡 能力缺口，不报错。

## 症状

新打开一个检索库（进程里没写过这些文档）做 `bitcask_search_text_highlight`：命中照常返回，**每条都 `highlights_count == 0`**。
bitcask-viewer 实测（clj-bitcask 1.2.8，演示库 60 篇）：`search-highlighted db "shaders" {:k 3}` ⇒ 3 条命中、片段全空。
下游分不出「这条没片段是因为冷」还是「原文里确实没有可高亮的内容」。

## 位置

- `src/search/text_plugin.cpp:1214` `TextPlugin::search_text_highlight`：`doc_texts_.get(r.ord)` 未命中 ⇒ 片段留空（S9.3 的降级）。
- 原文 LRU 只在写入 / 建索引路径填充（`text_plugin.cpp:162`、`:336`、`:537`），读文档（`get_doc`）不填 ⇒ 下游也没法自己把它「热」起来。
- 语义写在 `c_api/bitcask_kv.h:877-884` 的注释里（「LRU 未命中的冷文档降级为 highlights_count == 0 的 hit」）。

## 怎么撞上的

1. 进程 A 建库、写 N 篇文档、关闭；
2. 进程 B 打开（读写或只读都一样）⇒ `bitcask_search_text_highlight` ⇒ 全部命中无片段。
重启过的长驻服务同理：重启后命中过的文档要等被重写才有片段。

## 影响面

- 查看 / 诊断工具（每次新开库、只读快照）：高亮永远不可用。
- 长驻服务：重启后、或文档数超过 `doc_text_cache_max`（缺省 1024）后，高亮随缓存命中率时有时无。
- 下游的绕法只能是 `get_doc` 读回正文自己切片段、标命中词 —— **分词与引擎不一致**（引擎按分析器 + NFKC 归一化切，
  下游只能按空白 / 整串粗配，中文 ngram / jieba 尤其对不上）。Keelson 评估过替下游在 clj-bitcask 里补，同样做不对，没做。

## 建议（下游不替 libbitcask 拍板）

- 高亮选项加「冷文档从存储读原文」开关：LRU 未命中时走 `get_doc` 同一条读路径取正文，再按现有
  `analyze_with_offsets` + `highlight` 出片段（每条冷命中一次读盘，调用方按需开）。C 侧为了不动 `bitcask_highlight_options_t`
  的布局，可以是新入口（如 `bitcask_search_text_highlight_ex` + 带 flags 的选项）；读来的原文要不要顺手回填 LRU 也请定。
- 退一步：`bitcask_search_hit_ex_t` 能标出「因冷而无片段」，下游至少能决定要不要自己补。
