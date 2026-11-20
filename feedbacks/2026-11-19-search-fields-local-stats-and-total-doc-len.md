# `search_fields` 的分数与 `search_text` 对不上：新写的文档按分片本地统计打分，命名字段的 dl 又取了整篇长度 —— `title:w^3 labels:w^8` 压不过正文词频

**报的人**：bosun（Erlang 任务管理，经 bitcask 10.0.0 / libbitcask `46da576`，Linux x64，jieba / whitespace 分词均复现）。
**严重度**：🟠 排序静默变化，不报错。①是 7.0.3（S45-4）之后才有的回退——同一个查询的分数取决于文档还在不在 building 段里，
重开一次库排序就变；②是一直如此的设计后果，命名字段的 boost 实际起不到文档里写的作用。

## 症状

bosun 用 `w title:w^3 labels:w^8` 让「标签命中 > 标题命中 > 正文命中」。6.7.1 下成立，升到 10.0.0 后
正文里词频高的文档排到了标签命中的前面（bosun 的 `full_text_search` 用例转红）。

最小复现（40 篇文档，whitespace 分词；`body` 正文里 3 个 zebra，`title` 标题里 1 个，`label` 标签就是 zebra）：

```text
--- 刚写完（同一进程，文档还在 building 段）
search_text   zebra           body=4.0170 title=3.5580 label=3.3749
search_fields zebra           title=1.9907 body=1.8927 label=1.5874     ← 同一个默认字段查询，分数和顺序都不一样
search_fields title:zebra     title=0.5701
search_fields labels:zebra    label=0.3026
--- close + 重开
search_text   zebra           body=4.0170 title=3.5580 label=3.3749
search_fields zebra           body=4.0170 title=3.5580 label=3.3749     ← 现在和 search_text 一致了
search_fields title:zebra     title=1.3603                              ← 重开前的 2.4 倍
search_fields labels:zebra    label=0.6400                              ← 重开前的 2.1 倍
```

两件事：

1. **同一个查询重开前后分数不同、顺序不同**；重开前 `search_fields "zebra"` 与 `search_text "zebra"` 不一致。
2. **重开后（统计已归正）命名字段的分数仍然只有默认字段的 1/3～1/6**：`labels:zebra` 整个字段就这一个 token、
   全库只有这一篇有（df=1），按 BM25 应该是最高的那一档（idf≈3.3，tf 归一≈1），实际 0.64。
   `labels:zebra^8` = 5.1，只比正文 3 个 zebra 的 4.0 高一点；`title:zebra^3` = 4.1；
   正文再长一点或词频再高一点，boost 就压不住了。重开前（0.30×8 = 2.4、0.57×3 = 1.7）则直接输给正文。

## 位置

- ① `src/search/text_plugin.cpp:1110`（`search_fields_attributed`）：逐段逐字段
  `fv.inv->search(gterms, k_req, *sv.seg, params_override)`——**没有传跨段聚合的统计**，idf / avgdl 用的是该段该字段本地的
  N / df / sum_dl。`search_text` 走 `multi_segment_search`（G-on-the-fly 全局统计），所以两者对不上。
  S45-4 之后每个索引分片各有一个 building 段（`text_plugin.cpp:722` `shard_slots_`），新写的文档按 key 哈希散在
  `index_workers` 个段里：8 核机器上 40 篇文档每段约 5 篇，各算各的 idf。checkpoint / close 合并落盘（S45-7）后才归正。
  `include/bitcask/segment.hpp:776` 的 `multi_field_segment_search` 做的正是「每字段聚合全局 N / sum_dl / df 再逐段打分」，
  但除了 `tests/inverted_test.cpp:2116` 没有调用方——`TextPlugin` 没接上它。
- ② `include/bitcask/segment.hpp:773`（及 `:118`、`:140` 的注释）：字段倒排打分时 `doc_len` 取段级 `LiveChecker` 的
  **整篇总词数**（Σ 所有字段 dl），而 avgdl 是**该字段**的 `sum_dl / N`。短字段的 dl/avgdl 因此被放大到「整篇长度 ÷ 字段平均长度」：
  上例 `label` 那篇整篇 11 个词、labels 字段平均 1 个词 ⇒ dl/avgdl = 11 ⇒ tf 归一 = 2.2 / (1 + 1.2×(0.25 + 0.75×11)) = 0.196，
  × idf 3.31 = 0.65，与实测 0.6400 吻合。字段越短、文档越长，罚得越重——正好是 title / labels / tags 这类最想加权的字段。
  `segment_merge.hpp:157` 合并时 per-field dl 是带着的（posting 的 dls 列），打分时没用上。

## 怎么撞上的

```erlang
doc(Title, Body, Labels) ->
    #{text => <<Title/binary, "\n", Body/binary, "\n", Labels/binary>>,
      fields => #{<<"title">> => Title, <<"labels">> => Labels}}.

H = bitcask:open(Dir, [read_write, {analyzer, whitespace}]),
ok = bitcask:put(H, <<"body">>,  doc(<<"alpha beta">>, <<"zebra zebra zebra gamma delta">>, <<>>)),
ok = bitcask:put(H, <<"title">>, doc(<<"zebra crossing">>, <<"gamma delta epsilon zeta eta">>, <<>>)),
ok = bitcask:put(H, <<"label">>, doc(<<"alpha beta">>, <<"gamma delta epsilon zeta eta">>, <<"zebra">>)),
[ok = bitcask:put(H, <<"fill", (integer_to_binary(I))/binary>>,
                  doc(<<"alpha beta">>, <<"gamma delta epsilon zeta eta">>, <<"misc">>)) || I <- lists:seq(1, 37)],
bitcask:search_text(H, <<"zebra">>, 10),            %% 对照
bitcask:search_fields(H, <<"zebra">>, 10),          %% ① 与上一行不一致
bitcask:search_fields(H, <<"labels:zebra">>, 10),   %% ② 0.30
ok = bitcask:close(H),
H2 = bitcask:open(Dir, [read_write, {analyzer, whitespace}]),
bitcask:search_fields(H2, <<"zebra">>, 10),         %% ① 现在一致了
bitcask:search_fields(H2, <<"labels:zebra">>, 10).  %% ② 0.64，仍远小于默认字段的 3.4～4.0
```

C++ 侧等价：`put_doc` 带 `fields`，`search_text` 与 `search_fields` 各查一次，`close` 重开后再查一次。
①只在 `index_workers > 1`（多分片）时出现，单分片下 building 段只有一个；我们没有去构建 6.7.1 做对照，
「7.0.3 引入」是从 S45-4 的改动和 bosun 的用例在 6.7.1 下通过反推的。

## 影响面

- 所有用 `search_fields` / `search_fields_attributed`（含 C API `bitcask_search_fields*`、`bitcask_query` 的 `fields`）做字段加权的下游。
- ①：刚写入的文档与落盘后的文档打分口径不同——同一个库里新旧文档混排时，新文档的分数系统性偏离；
  小库 / 测试里最明显（每个分片段只有几篇，idf 接近常数），自动 checkpoint 之后又悄悄变回来。分页游标依赖分数序的也受影响。
- ②：boost 的实际效果随文档长度漂移。文档里说的 `field:term^boost` 是「跨字段加权合并」，下游据此挑的系数
  （bosun 的 ×3 / ×8）在长文档上不成立，而且没有任何提示。
- 都不报错。bosun 这边已经绕开：改用 `search_fields_attributed` 拿命中字段，在引擎分数外再乘一次系数，不依赖两类分数的相对量级。

## 建议（下游不替 libbitcask 拍板）

1. `search_fields_attributed` 走跨段聚合的每字段统计（现成的 `multi_field_segment_search` 思路），
   让 `search_fields "w"` 与 `search_text "w"` 在任何时刻逐位一致——这条可以直接当回归用例。
2. 字段打分的 dl 用该字段自己的长度（posting 里已有 per-field dl）。这会改变既有库的 `search_fields` 分数与排序，
   算不算破坏性、要不要给开关，由你们定；至少文档里该写明现在 dl 取的是整篇。

## 回没回

已修（**未发版，S56，定 11.0.0**，两条都修）：

1. `search_fields_attributed` 每字段跨段聚合 N / sum_dl / df（G-on-the-fly，同 `search_text`），各段同一 idf / avgdl。
   `search_fields "w"` 与 `search_text "w"` 在 building 段、重开、checkpoint 后重开三个时刻逐位一致（已作回归）。
2. 字段打分的 dl 改取该字段自身词数：每(段, 字段)一张字段长表（内存段写入时记，v1 段载入时由 posting dls 列重建，v2 段
   首次查询该字段时从 posting dls 列惰性推出——**盘上格式不变**）。默认字段同样按此口径：catch-all 开（缺省）时与整篇总长
   逐篇相等，`search_text` 分数不变；**catch-all 关时默认字段此前也被命名字段长度压分（你们没提，基线实测证实），一并归正**。
   不给旧口径开关；既有库的 `search_fields` 分数与排序会变，CHANGELOG `[Unreleased]` 已写明。

复现对照（新用例在 10.0.0 上红、修后绿）：`labels:zebra` = idf（log 26 ≈ 3.258，之前 0.30 / 0.64），
`zebra title:zebra^3 labels:zebra^8` 排序 label > title > body。bosun 的「引擎分数外再乘系数」绕法可以撤，也可以留。
回归 `tests/search_fields_scoring_test.cpp`（`SearchFieldsScoringTest.*` / `FieldDocLensSegment.MemoryAndMmapAgree`）。
