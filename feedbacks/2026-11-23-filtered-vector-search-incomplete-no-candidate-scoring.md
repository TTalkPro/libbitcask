# 带过滤的向量检索在过滤很严时回不满 k，也没有「只在给定候选里排序」的入口

**报的人**：bitcask（对照 LatticeDB 的带过滤向量检索核对 libbitcask 现状；为图查询「先按图模式得到候选，再按向量排序」评估引擎能力。
libbitcask 12.0.3 `d995557`）。
**严重度**：🟡 能力缺口，不报错。结果「少给」是文档写明的取舍；本条没有做新的复现，依据是代码里的契约说明。

## 症状

不报错。两件相关的事：

1. `search_vector(query, k, ef, filter)` 在 filter 通过率很低时返回不足 k 条，哪怕库里满足 filter 的文档远多于 k。
   调用方无法区分「真的只有这么多」和「预算用完了」。下游 `bitcask_query` 的文档因此只能写「向量检索带 filter 时引擎本身可能少给，
   返回不足 K 也只能当作穷尽」。
2. 没有办法让引擎「只在这 N 个 key 里按与查询向量的距离排序」。调用方已经知道候选集（图上某个顶点的邻居、一次全文检索的命中、
   业务侧筛出的一批 id）时，只能把候选条件硬翻译成 meta filter 再走图遍历，或者把每个候选的向量读回宿主语言自己算距离。

## 位置

- `include/bitcask/hnsw.hpp:113-115`：带过滤时 0 层距离计算上限 `ef × kFilterBudgetMul`（256），
  「耗尽即返回已找到的（可能 < k）——通过率极低时宁可少返回，不把全图扫一遍」。S51 把结果侧过滤改成了遍历期过滤，
  通过率中等时已经能回满；这里说的是通过率再低一两个数量级的情形。
- `src/search/vector_plugin.cpp:165`：`hnsw->search_filtered(q, k, ef, live)`，过滤器是「对 ord 求值的谓词」
  （`DocTable::fill_eval_meta`）。引擎拿不到「通过者一共有多少」，所以没法判断走图还是直接算。
- 公开 API（`Cask::search_vector` / `search_vector_batch`，C API 同）只接受 meta filter，没有候选 key 列表的形态。

## 怎么撞上的

（未复现，按契约推演。）100 万篇文档，其中 200 篇带 `tenant = "x"`；`search_vector(q, 10, 64, tenant == "x")`：
预算 64 × 256 = 16384 次距离计算，通过率 0.02% ⇒ 期望遇到约 3 个通过者就耗尽，返回 ~3 条；而对那 200 篇直接算距离只要 200 次。

图查询的情形：`MATCH (a)-[:CITES]->(c) WHERE c.embedding <=> $q < 0.3`——候选是 a 的出边邻居（几十到几千个顶点），
今天只能逐个 `get` 把向量读回来在宿主语言里算。

## 影响面

- 多租户 / 按权限 / 按时间窗过滤的向量检索：过滤越严越容易少给，且不可察觉。
- 所有「候选集先由别的索引给出」的组合查询：图 + 向量、全文召回后向量重排、二级索引命中后向量排序。
  LatticeDB 的查询层正是这样用的（MATCH 产出的行作为候选集交给向量算子）。
- 不报错。

## 建议（下游不替 libbitcask 拍板）

LatticeDB 的做法（`database.zig` 的 `exactScanPays`）：候选集大小已知时，若 `候选数² ≤ ef × m_max0 × 总数` 就对候选逐个精确计算，
否则走带过滤的图遍历——阈值是它在 1 万 / 10 万条 128 维向量上实测定的。libbitcask 可以分两步吸收：

1. **新增「在给定候选里按向量排序」的入口**：输入查询向量 + 一组 key（或 ord）+ k，输出这组里最近的 k 个及分数；
   内部直接取向量算距离（量化库用它现有的精排路径），不走图。C API 同步。这是纯新增，下游的组合查询马上能用。
2. **带 meta filter 的检索在预算耗尽时补全**：预算耗尽且结果不足 k 时，可选地退到「扫一遍 docmap 求值 filter，对通过者精确计算」
   （代价 O(文档数) 次 filter 求值——是内存里的 blob 比较，百万级是几十毫秒；由调用方用选项决定要不要付这个代价）。
   至少把「是否因预算耗尽而提前返回」带回给调用方，让它能区分穷尽与截断。
3. 若以后有 meta 二级索引 / 位图（`doc/roaring-meta-bitmap-design-zh.md` 的预研），候选数可事先估出，就能像 LatticeDB 那样
   在检索开始前直接选路径。
4. 回归：100 万篇里 200 篇通过 filter，断言回满 k 且与暴力结果一致；候选入口对照暴力排序。

## 回没回

**libbitcask（2026-11-23，S67，已编码未提交、版本未定）**：两件都做了。

- 回不满：带过滤的 HNSW 检索在预算耗尽且不足 k 时退到精确扫描（全部节点过一遍过滤器，通过者逐个算距离取前 k）。节点数
  ≤ 2^20（`HnswIndex::kExactFallbackMaxNodes`）时，返回不足 k 条就是「真的只有这么多」；更大的图维持原取舍，仍可能少给，
  调大 `ef` 可放大预算。没有加「是否被截断」的返回标志——2^20 以内不需要，以上的图这条仍是缺口。
  IVF-RQ / DiskANN 两种引擎没有动。
- 候选集内排序：`Cask::rank_by_vector(query, keys, k)` / C API `bitcask_vector_rank(cask, query, query_len, keys, key_lens, n_keys, k, &out, &fault)`。
  精确、不经向量索引、不需要先 flush；分数口径同 `search_vector`（cosine / dot：内积；L2：负的平方距离），降序，同分按候选先后。
  不存在 / 已删 / 没有向量的 key 跳过，重复的只算一次。每个候选一次点读——候选上万时这是主要开销。量化落盘的库用还原后的向量算。

回归 `HnswFilteredSearch.SelectiveFilterFallsBackToExactScan`（2 万节点、通过者 30 个、k=10、ef=16：回满且与暴力结果逐条相同）、
`CaskDocValueTest.S67RankByVectorAmongCandidates`、`c_api_test`。
