# WAND / BlockMax-WAND:倒排索引 top-k 动态剪枝算法

> 前置阅读：`kway-blockmax-bmw-zh.md`(设计路线与动机)、`kway-blockmax-bmw-zh.md` §3(块级元数据)。
> 对应代码:`src/bm25/inverted.cpp` 的 `search_wand`(BlockMax-WAND 主循环)。
> 参考文献：
> - Broder et al., *"Efficient Query Evaluation using a Two-Level Retrieval Process"*, CIKM 2003(WAND)
> - Ding & Suel, *"Faster Top-k Document Retrieval Using Block-Max Indexes"*, SIGIR 2011(BlockMax-WAND)
> 状态：已实现(算法讲解 + 现有代码映射文档)。

本文解释 `search_wand` 实现的算法本身。设计动机/选型见 `kway-blockmax-bmw-zh.md`。

## 1. 要解决的问题:top-k 检索

查询有多个词(如 "vector database"),要返回 **BM25 分数最高的 k 篇文档**。

朴素做法(DAAT,document-at-a-time 全 OR):把**所有含任一查询词的文档**都算一遍分,再取 top-k。posting list 很长时,绝大部分文档分数很低、根本进不了 top-k —— **白算了**。工业引擎(Lucene/ES)靠 WAND 系算法在遍历阶段就跳过注定进不了 top-k 的文档,典型只触碰百分之几的 posting。

## 2. WAND(Weak AND,Broder 2003)

核心思想:**用阈值跳过不可能进 top-k 的文档**。

- 维护阈值 **θ = 当前已找到的第 k 高分**(min-heap 堆顶)。
- 每个词有个**上界 `list_upper_bound`**(它能贡献的最高分 = idf × 最大 tf 归一)。
- 各词游标按当前 docID 排序,从小到大累加上界,**第一个让累加和 ≥ θ 的词 = pivot**。
- **关键推论**:排在 pivot 之前的 docID,即使含上它们所有词、拿满上界,合计也 < θ → **不可能进 top-k → 直接跳过,不算分**,游标快进到 pivot 的 docID。

名字 "Weak AND" 指它是介于 AND 与 OR 之间、由阈值 θ 参数化的布尔算子:`WAND(x₁,w₁,…,xₙ,wₙ,θ)` 在 `Σwᵢxᵢ ≥ θ` 时为真,用来驱动跳跃。

## 3. BlockMax-WAND(Ding & Suel 2011)—— 本实现

WAND 的上界是**整条 posting list 的最大值**,太松:一条长列表里只要有一个高分文档,整条上界就被抬高,跳不动。

BlockMax 改进:**posting list 切成块,每块存块内最大分(block-max)**。剪枝时用**更紧的块级上界**:

```
本块上界 block_upper + 其余未耗尽词的列表上界之和 ≤ θ  ⟹  整块跳过
```

因为 block-max 通常远小于 list-max,剪枝**激进得多** —— 这是 BMW 比原始 WAND 快的根源。

## 4. 映射到代码(`search_wand`,`inverted.cpp`)

| 概念 | 代码 |
|---|---|
| 阈值 θ | `threshold = heap.top().first`(min-heap 存当前 top-k) |
| 词上界 | `tp.list_upper_bound` |
| 块上界 | `block_upper`(由 `block->max_tf` + `block->min_dl` 算) |
| 其余词上界和 | `total_ub - tp.list_upper_bound` |
| pivot 选取 | 累加 `acc_score` 到 ≥ threshold 的位置(`pivot_pos`) |
| 块跳跃判据 | `block_upper <= threshold - (total_ub - tp.list_upper_bound)` |
| 游标推进 | 跳到下个块边界(`block->start_idx + block->count`)/ pivot_ord |

主循环骨架:
1. **排序**:`order` 按各词当前 docID 升序(耗尽词排前,被跳过)。本实现用插入排序(优化②,`order` 迭代间持久近乎有序)。
2. **找 pivot**:累加 `list_upper_bound` 到 ≥ threshold。
3. **块跳跃**:对 pivot 及之前、当前 docID = pivot_ord 的词,块上界 + 其余词上界 ≤ θ 则整块跳过。
4. **打分**:无可跳则对 pivot_ord 实际算 BM25 分,够阈值则进 heap、更新 θ。
5. **推进**:所有 cursor ≤ pivot_ord 的词前进。

## 5. 本实现的工程增强

- **A1:块上界收紧** —— 块上界接 v5 impacts 的 `max_tf + min_dl`,替代松散的 `dl=1` 假设,与 bool MUST 路径同源收紧 ~25%/词。
- **`live` 过滤** —— 删除文档不计入 df(`fill_is_live` 批量取),idf 与上界按存活文档算,保证剪枝判据与最终结果一致。
- **admissible 块跳跃的 epsilon 敏感性** —— 判据用 `<=` 严格比较,**不能加绝对 epsilon**:idf 极小时(df≈N)分数量级 ~1e-4,任何容差都变成巨大相对误差,会把真 top-k 所在块误判为"平分"而错跳。这也是为何 `total_ub` 必须每轮重新求和、不做增量维护(增量 FP 和会漂移→可能过剩跳过→结果欠落;见 TASK.md ⑫ 跳过结论)。

## 6. 一句话

> **WAND**:维护 top-k 阈值,按"剩余词最高可能分之和 < 阈值"批量跳过没希望的文档。
> **BlockMax-WAND**:把"整条列表最大分"换成"块内最大分",上界更紧 → 跳得更狠。

## 7. 关键代码位置

- 主循环:`src/bm25/inverted.cpp` `search_wand`
- 上界计算:`tp.list_upper_bound = tp.fp.block_upper_bound(...)`
- 块上界 + 跳跃判据:块循环内 `block_upper <= threshold - (total_ub - tp.list_upper_bound)`
- live/dl 批量化:`fill_is_live` / `fill_doc_lens`
