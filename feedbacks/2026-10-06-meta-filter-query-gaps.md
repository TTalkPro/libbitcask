# 带 meta filter 的 `search_text` 静默少返回（K=10 只给 6 条，满足条件的有 100 条）；同分命中的 top-K 选取和输出顺序不一致（offset 分页重复页）；迭代器解出 meta 后丢弃；filter 不能脱离全文查询使用

**报的人**：bitcask（下游 Erlang 封装，pin 6.6.0 `1a6d698`）。起因：在 BEAM 侧做结构化查询 DSL（`bitcask_query`，
「key 范围 + 全文条件 + meta 条件」组合查询，规划器挑一个索引驱动），按代码对账时撞到下面四条。
**严重度**：第 1、5 条 🔴（结果错，不报错）；第 2 条 🟠（多一倍 IO，且 text 与 meta 可能对不上版本）；第 3、4 条 🟡（能力缺口）。

## 1. 带 filter 的 `search_text` 返回数少于 k，但满足条件的文档还有很多，不报错

### 症状

1000 篇文档都含 `apple`，其中每 10 篇有 1 篇 `tag=keep`（满足 filter 的共 100 篇）。
`search_text("apple", K, [tag eq keep])`：

| K | 返回 | 应返回 |
|---|---|---|
| 10 | **6** | 10 |
| 50 | **20** | 50 |
| 100 | **40** | 100 |
| 1000 | 100 | 100 |

返回值里一个字都不提「后面还有」。调用方拿到 6 条，无法区分「只有 6 条满足」和「候选不够被截掉了」。

### 位置

- `src/search/text_plugin.cpp:682`：`k_req = filter ? max(k×4, 64) : k`，只多取一次固定数量的候选。
- `src/search/text_plugin.cpp:728` → `materialize_hits`（`:568` 起）：对这批候选做后过滤，再截断到 k。候选不够就直接少返回，没有补取。
- `src/search/hybrid_searcher.cpp:20/27`：hybrid 的文本那一路同样用 `max(k×4, 64)`，问题相同。向量那一路的 filter 折进了 live callback，见 `:33` 的注释，不受影响。

### 怎么撞上的

filter 的选择性低于 `k / max(k×4, 64)` 就会撞上。k=10 时，满足 filter 的文档不到 15% 就会少返回。复现脚本（Erlang，下游仓库）：
1000 篇文档，`put(#{text => <<"apple pie">>, meta => encode_meta(#{tag => keep|drop})})`，然后用 `search_text/4` 查。

### 影响面

所有带 filter 的文本检索和 hybrid 检索。filter 越严，排名靠前的结果越容易被漏掉。不报错。

### 建议（下游不替 libbitcask 拍板）

- 首选：在 C++ 侧循环补取。候选不够 k 时，把 k_req 翻倍重查，直到凑满 k 或者倒排里的候选取完。查询缓存的 key 已经包含 k_req，翻倍重查不会和缓存冲突。
- 退一步：至少在结果里带一个「候选已截断 / 已穷尽」的标志，让调用方自己决定要不要加大 K 重查。

## 2. fold / range 迭代器已经解出了 meta，又把它丢掉了

### 症状

索引模式下，`get` 返回 `{text, meta}`；fold 和 range 的 Entry 只有 `value`，也就是 text 段。
要在扫描中按 meta 判断，下游只能每行再 `get` 一次。

### 位置

- `src/cask/cask_iter.cpp:178-184`：`decode_doc_value` 已经把 `dv->meta` 解出来了，只 assign 了 `dv->text`。
- `src/cask/cask_range_iter.cpp:184`（串行路径）和 `:244`（prefetch 路径）：调的是 `get_owned`，`GetResult` 里已经有 `meta`，只 move 了 `g->value`。

### 影响面

- 按 meta 条件扫描时 IO 和解码都翻倍。
- range 本身是 per-key 弱一致，下游补的那次 `get` 和迭代器读到的 text 可能**不是同一个版本**：两次读之间有并发 put，就会拿到 A 版本的 text 配 B 版本的 meta。下游没办法检测到这种情况。

### 建议

给 Entry 加一个 `meta` 字段，内容是原样的 blob，不解码。可以用一个迭代选项 `want_meta` 控制，不需要的调用方不付拷贝成本。这里的数据都已经在手上了，改动很小。

## 3. 只有 text / vector / hybrid 三类检索能带 filter

`include/bitcask/cask.hpp`：`search_phrase`（:691）、`bool_search`（:695）、`search_fields`（:747）、
`search_near`（:752）、`search_fuzzy`（:757）、`search_wildcard` 都没有 `MetaFilter*` 参数。
下游只能自己多取一批再过滤，而且要逐条补 `get` 拿 meta，等于在下游把第 1 条的问题又实现了一遍。

建议：统一加上 `const meta::MetaFilter* filter = nullptr`，补取策略和第 1 条用同一套。

## 4. filter 不能脱离全文查询使用：没有「只按 meta 筛选」的扫描

只有 meta 条件（比如 `year >= 2024 AND cat IN [...]`）的查询，下游目前只能全表 fold，每行补 `get`、解码 meta、在 BEAM 里求值。
引擎里明明有 `MetaFilter::evaluate`，而且走的是 `meta_lookup` 二分、不需要完整解码，却只挂在搜索路径上用。

建议：加一个 `scan(filter, [lo, hi), want_meta)` 迭代器，在 C++ 侧逐条做 `evaluate`，只把命中的条目交出去。和第 2 条一起做，改动面可以共享。

## 5. 同分命中：top-K 的选取按 key 升序，输出却按 ord 降序排 ⇒ 大 K 的结果不是小 K 结果的延长，offset 分页会出重复页

### 症状

1000 篇文档的文本相同，BM25 分数全部一样，按 d0001..d1000 的顺序写入：

| 调用 | 返回 |
|---|---|
| `search_text("apple", 5)` | d0005, d0004, d0003, d0002, d0001 |
| `search_text("apple", 1000)` | d1000, d0999, …, **d0001 排最后** |

K=5 选出的是 key 最小的 5 篇，再按 ord 降序输出。K 变大以后，前面几名就换了人。

### 位置

`src/search/text_plugin.cpp:708-720`：`multi_segment_search` 在同分时按 key 升序截取前 k_req 个，紧接着又被重排成「同分 ord 降序」（S29-5 的注释写明是为了和单索引路径对齐）。**进入 top-K 的选取规则**和**输出顺序**用的是两套平局规则。

### 影响面

- **offset 分页（S13-D10）**：按机制推导，没有直接调 offset 验证。第 1 页（k=5）= top(5) = {d1..d5}，按 ord 降序输出为 d5..d1。第 2 页（k=5, offset=5）= top(10) 按 ord 降序输出为 d10..d1，丢掉前 5 条后剩 d5..d1，**和第 1 页完全一样**，d6..d10 永远翻不到。同分在短文本、模板化文本里很常见。
- 任何「K 不够就加大 K 重查」的调用方，如果假设结果是前缀关系，都会漏结果。bitcask_query 一开始就踩了这个坑：只处理新增的那段命中，导致 0 条命中。现在改成每轮从头组装结果。
- 不报错。

### 建议

截取和输出用同一个平局键，二选一都行：截取时也按 ord 降序；或者输出时也按 key 升序。只要统一，「大 K 结果 = 小 K 结果的延长」就成立，offset 分页也就对了。

## 附：不急的一条

`GetResult` 没有 `fields`（DocValue 的 fields 段只写不读，字段名也只存 id）。查询结果目前没法把命名字段的原文带回来，下游只能让业务把需要回显的内容重复存进 text 或 meta。先记一笔。

## bitcask 这一侧怎么处理

`bitcask_query`（下一版）先在 BEAM 侧兜底：

- 第 1 条：拿 `K' = max(K×4, 64)` 调用；如果返回数少于 K、且 K' 还没到上限，就把 K' 翻倍重查。这样做结果对，但每次重查都要重新算一遍 BM25。
- 第 2、4 条：逐条补 `get`，在文档里注明 text 和 meta 可能不是同一版本。

- 第 5 条：每轮从头组装，已经求值过的 key 缓存起来，不重复 get。

上游第 1、2、5 条修好后，下游会删掉这些兜底。
