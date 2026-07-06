# WAND / BlockMax-WAND：倒排索引 top-k 动态剪枝算法

> 前置阅读：[`kway-blockmax-bmw-zh.md`](kway-blockmax-bmw-zh.md)（设计
> 路线与动机）、其 §3（块级元数据）与 §6.2（must-only 合取 BMW 的算法
> 步骤——代码符号一一对应表）。
>
> 对应代码：`InvertedIndex::search_wand`（`bm25/inverted.cpp`）—— DAAT
> 循环 + 小顶堆的 Block-Max WAND 主循环；共享上界计算在匿名命名空间
> 的 `upper_bound_from` 闭包；多词「块级 `(max_tf, min_dl)`」由
> `PostingList::note_appended` / `seal_full_blocks` / `finalize` 维护。
>
> 参考文献：
> - Broder et al., *«Efficient Query Evaluation using a Two-Level
>   Retrieval Process»*, CIKM 2003（WAND）。
> - Ding & Suel, *«Faster Top-k Document Retrieval Using Block-Max
>   Indexes»*, SIGIR 2011（BlockMax-WAND）。
>
> 状态：已落地（算法讲解 + 当前代码映射文档）。

本文解释 `search_wand` 实现的算法本身。设计动机/选型见
`kway-blockmax-bmw-zh.md`。

## 1. 要解决的问题：top-k 检索

查询有多个词（如 "vector database"），要返回 **BM25 分数最高的 k 篇文档**。

朴素做法（DAAT，document-at-a-time 全 OR）：把**所有含任一查询词的文档**
都算一遍分，再取 top-k。posting list 很长时，绝大部分文档分数很低、
根本进不了 top-k —— **白算了**。工业引擎（Lucene/ES）靠 WAND 系算法在
遍历阶段就跳过注定进不了 top-k 的文档，典型只触碰百分之几的 posting。

## 2. WAND（Weak AND，Broder 2003）

核心思想：**用阈值跳过不可能进 top-k 的文档**。

- 维护阈值 **θ = 当前已找到的第 k 高分**（小顶堆顶）。
- 每个词有个**上界 `list_upper_bound`**（它能贡献的最高分 = idf ×
  全局 max_tf 算出的 tf 归一化上限）。
- 各词游标按当前 docID 排序，从小到大累加上界，**第一个让累加和 ≥ θ
  的词 = pivot**。
- **关键推论**：排在 pivot 之前的 docID，即使含上它们所有词、拿满上界，
  合计也 < θ → **不可能进 top-k → 直接跳过，不算分**，游标快进到
  pivot 的 docID。

名字 "Weak AND" 指它是介于 AND 与 OR 之间、由阈值 θ 参数化的布尔
算子：`WAND(x₁,w₁,…,xₙ,wₙ,θ)` 在 `Σwᵢxᵢ ≥ θ` 时为真，用来驱动跳跃。

## 3. BlockMax-WAND（Ding & Suel 2011）—— 本实现

WAND 的上界是**整条 posting list 的最大值**，太松：一条长列表里只要有
一个高分文档，整条上界就被抬高，跳不动。

BlockMax 改进：**posting list 切成块，每块存块内
`(max_tf, min_dl)` —— v5 impacts 简化对**。剪枝时用**更紧的块级上界**：

```
本块上界 block_upper + 其余未耗尽词的列表上界之和 ≤ θ  ⟹  整块跳过
```

因为 block-max 通常远小于 list-max，剪枝**激进得多** —— 这是 BMW
比原始 WAND 快的根源。

## 4. 映射到代码（`search_wand`，`bm25/inverted.cpp`）

| 概念 | 代码 |
|---|---|
| 阈值 θ | `threshold = heap.top().first`（小顶堆 `std::priority_queue<Entry, std::greater<>`） |
| 词上界 | `tp.list_upper_bound = tp.fp.block_upper_bound(...)` |
| 块上界 | `tp.block_upper_bounds[b]`（`search_wand` 内 `TermPostings`，每块预计算） |
| 其余词上界和 | `total_ub`（主循环每轮重算，杜绝 FP 漂移误跳） |
| pivot 选取 | 累加 `acc_score += tp.list_upper_bound` 到 ≥ threshold |
| 块跳跃判据 | `block_upper ≤ threshold - (total_ub - tp.list_upper_bound)` |
| 游标推进 | 跳到 `block->start_idx + block->count` 或 pivot_ord |
| 排序 index | `order`（耗尽词排前）+ 插入排序（迭代间近乎有序 O(t+inversions)） |

主循环骨架：(1) 排序 —— 按各词当前 docID 升序；(2) 找 pivot —— 累加
`list_upper_bound` 到 ≥ threshold；(3) 块跳跃 —— 块上界 + 其余词上界
≤ θ 则游标推进到块尾；(4) 打分 —— `ensure_dls` 懒填充后实际 BM25
分数，够阈值进 heap、更新 θ；(5) 推进 —— 所有 `cursor ≤ pivot_ord` 的
词前进；任一耗尽则继续，全耗尽则退出。

## 5. 本实现的工程增强

- **A1：块上界收紧** —— 块上界接 v5 impacts 的 `max_tf + min_dl`，
  替代松散的 `dl = 1` 假设，与 bool MUST 路径同源收紧 ~25%/词。
- **`live` 过滤** —— 删除文档不计入 df（`fill_is_live` 批量取），
  idf 与上界按存活文档算，保证剪枝判据与最终结果一致。
- **admissible 块跳跃的 epsilon 敏感性** —— 判据用 `<=` 严格比较，
  **不能加绝对 epsilon**：idf 极小时（df ≈ N）分数量级 ~1e-4，
  任何容差都变成巨大相对误差，会把真 top-k 所在块误判为「平分」而
  错跳。这也是为何 `total_ub` 必须每轮重新求和、不做增量维护（增量
  FP 和会漂移 → 可能过剩跳过 → 结果欠落）。

## 6. 一句话

> **WAND**：维护 top-k 阈值，按「剩余词最高可能分之和 < 阈值」批量
> 跳过没希望的文档。
> **BlockMax-WAND**：把「整条列表最大分」换成「块内 `(max_tf, min_dl)`
> 算出的最大分」，上界更紧 → 跳得更狠。**合取 BMW** 形态（must-only
> + k-way leapfrog）见 `kway-blockmax-bmw-zh.md` §6.2。
