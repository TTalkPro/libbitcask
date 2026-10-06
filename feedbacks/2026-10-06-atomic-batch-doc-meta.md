# 原子批 / 事务提交写不了带 meta 的文档 —— 文档和它的二级索引项进不了同一批

**报的人**：bitcask（下游 Erlang 封装，pin 6.6.1 `33f2147`）。起因：在 BEAM 侧给 meta 字段做二级索引
（`bitcask_query` 的 `create_index`，对应 mnesia 的 `index_read`），需要「文档 + 它的索引项」原子写入。
**严重度**：🟡 能力缺口。不报错，但下游只能做成非原子的，再靠读时校验兜底。

## 症状

`put_batch_atomic` / `txn_commit` 的 `BatchOp` 只有 `key` 和 `value` 两段，value 一律被当成 text：
带 `meta`（以及 `vector`、`fields`、`expiry_at`）的文档放不进原子批。要写带 meta 的文档，只能单独调 `put_doc`。
所以「改文档的 meta」和「改对应的索引项」没办法同批落盘。

## 位置

- `include/bitcask/cask.hpp:674` `BatchOp { type; key; value; }`：没有 `DocInput` 的位置。
- `src/cask/cask.cpp:2147-2149`：`parts.text = op.value; encode_doc_value(...)`，只填了 text 段。

## 怎么撞上的

用 meta 维护二级索引的下游都会撞上：一篇文档的 meta 从 `c=red` 改成 `c=blue`，需要在同一批里完成三件事：
put 文档（新 meta）、put 索引项 `ix/c/blue/pk`、remove 索引项 `ix/c/red/pk`。第一件放不进批。

## 影响面

下游被迫拆成三步：加新索引项、`put_doc`、删旧索引项，代价有三处：

- 每次写入都要拿该 key 的写锁。否则两个并发写同一个 key 的人交错执行，会把对方刚加的索引项当成「旧项」删掉，导致漏项。
- 每次读都要回表校验：核对文档当前值是否就是索引项里的值。因为崩溃会留下过期项。
- 绕过这套流程直接写文档的调用方，会让新值缺索引项，只能整体重建。

不报错。

## 建议（下游不替 libbitcask 拍板）

- `BatchOp` 加一种 `kPutDoc`（或者加一个可选的 `const DocInput*`），在批内走和 `put_doc` 相同的编码与索引登记，
  meta / vector / fields / expiry_at 都能进批。C API 那边配一个带 `bitcask_doc_t` 的 op 结构体，按纯加法的方式加。
- 有了它，下游的二级索引就能做成「一批写完文档和索引项」：去掉 key 锁，读时也不用再回表校验。

## bitcask 这一侧怎么处理

`bitcask_query_index`（随 bitcask 6.7.1 发布）按上面说的三步写入，配合 key 锁和读时校验。
崩溃最坏留下过期项，不会漏项；读时校验保证不会出现错行或重复行。上游支持后，下游会把写入改成一批。
