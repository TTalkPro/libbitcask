# 7.0.2 读旧格式 OKI 索引时检索分数漂移(0.006~0.066),键/正文/向量方向仍一致

**报的人**:jinshu(下游消费者;jinshu rag 的长期格式判据测试 `golden-index-reads-the-same`)
**严重度**:🔴 数据正确性疑点——旧格式索引读出的分数和写入时不一致;新格式(fresh 重建)读回正确。当前未见键错位,但分数漂移意味着旧库上的排序/阈值行为不可信。

## 症状

jinshu rag 的黄金夹具测试(`modules/jinshu-rag/test/jinshu/rag/bitcask_interop_test.clj`
的 `golden-index-reads-the-same`)在 Windows + bitcask 7.0.2 下失败,共 10 条命中分数漂移:

| 查询 | 漂移条数 | 分数偏差范围 |
|---|---|---|
| text-wujiu | 6 | 0.0066 ~ 0.0088 |
| text-xue | 2 | 0.0533 ~ 0.0661 |
| text-beijing-ch2 | 2 | 0.0245 ~ 0.0250 |

同一测试里其余判据**全部通过**:

- `count` / `entries` / `entries_graph` / `text_of_first` 全等;
- 每个查询的命中键与顺序、正文逐条一致;
- 命中带回的归一化向量方向与黄金值逐分量一致(容差 1e-5);
- 只有 `score` 偏差超过容差 1e-4。

样例(容差 1e-4):

```text
text-wujiu  graph1000g_n1_n3          |Δ| = 0.00883
text-wujiu  codex_section0000cs_1_a_b |Δ| = 0.00840
text-xue    summary11001_1_summary    |Δ| = 0.06606
text-xue    content11001_1_0          |Δ| = 0.05325
text-beijing-ch2 content211212_1_1    |Δ| = 0.02496
```

完整 10 条见 jinshu 仓库 `target/test-fix.log`(2026-11-16 全量回归,1213 tests / 10 failures,
全部为本条)。

## 复现

1. jinshu 仓库,Windows(当前环境 D:\workspace\jinshu),bitcask 7.0.2(本地构建安装,
   `D:\workspace\libbitcask` @ `76aa98d`)。
2. 黄金夹具:jinshu `modules/jinshu-rag/test/resources/golden/k26-data`(旧格式索引目录,
   原样拷贝后打开)+ `golden/k26-bitcask.json` 黄金期望值。
3. 运行 `clojure -M:test`(rag 模块):`golden-index-reads-the-same` 失败;
   同文件的 `fresh-index-gives-the-same-results`(**同一组文档新建索引再查**)全绿,
   与黄金值完全一致。

## 已排除 / 观察

- 键、正文、向量方向都没错,说明文档载荷和键编码没坏;坏的只是分数计算,
  且只在**读旧格式索引**时发生。
- fresh 重建路径与黄金值一致,说明"写入→查询"的新格式链路没问题。
- 7.0.1→7.0.2 的差异 commit `3ef099e` 改了 `src/fileops/oki_merge_cursor.cpp`:
  `load_be8` 从 `__builtin_bswap64` 换成 `std::byteswap`(另含 `<bit>` 头)。
  两者语义上都应是 64 位字节序反转,理论上等价;但这是 Windows 兼容性提交,
  金标读路径恰好经过这里,值得优先排查:MSVC 下这条路径的行为是否与
  GCC/Clang 构建一致(例如 `std::byteswap` 的实例化、`load_be8` 调用点是否
  在不同构建下走进了不同分支)。
- 也可能漂移不是 `3ef099e` 引入,而是 7.0.0 → 7.0.1 之间(S44 OKI 分级归并落地
  前后的读路径)对**旧格式索引**的分数计算就有差异,只是当时没有跨版本
  黄金判据。请以"读旧格式索引的分数计算"为单位核对 7.0.0 / 7.0.1 / 7.0.2
  三个版本的输出。

## 期望

- 读旧格式 OKI 索引的分数与写入时(黄金值)一致;
- 若这是有意识的分数口径变化,请给出明确说明与迁移方案(旧库需要重建索引),
  jinshu 侧再同步更新黄金夹具——但当前测试用的是同一查询向量,同一文档集,
  没有版本声明的情况下应当视为回归。

## 状态

已修（**7.0.2**，同日后续修复；未改版本号）。

## 病因

不是 `3ef099e` 的字节序替换，也不是 OKI 游标读坏数据。段文件里的键、正文、
向量与倒排都正确；漂移只来自 TextPlugin 查询视图的全局 BM25 聚合。

恢复全量 data fold 重放时，同 key / 同 ord 的逐篇重放会把旧 SegmentSet 封口段
的全部文档标记为 dead，同时在新的 building 段重建这些 live 文档。旧段
`mark_dead()` 只翻转 live 位，不会也不应改写其封口时持久化的 BM25 统计快照
（`N` / `sum_dl` / `df`）。此前查询视图仍会把 `live_doc_count() == 0` 的旧段
纳入聚合，于是 stale 全局统计和 building 段统计混算，idf 口径错，同一批
文档的分数呈现恒比例漂移。

## 修复

- 默认字段 segment views 跳过全死段（`live_doc_count() == 0`）。
- 多字段 segment views 同样跳过全死段。
- building / 各 builder building 段的参与条件从 `doc_count() > 0` 改为
  `live_doc_count() > 0`。
- `SegmentSet::search()` 也跳过全死段。

恢复重放行为本身未改；该修法只隔离全死段对查询分数的副作用。C API、盘上
格式与既有索引文件零改动。

## 验证

- 新增回归
  `TextPlugin.RecoveryReplayFullyDeadSegmentKeepsScoresStable`：写 5 篇文档、
  flush、重开建立 baseline，再按同 key / 同 ord 全量重放；断言旧段
  `live == 0`、building `live == 5`，且重放后的 keys / ords / scores 与
  baseline 完全一致（`EXPECT_DOUBLE_EQ`）。
- libbitcask Windows Release 全量 ctest：**854 passed / 0 failed**
  （1 skipped / 2 disabled）。
- jinshu rag：`golden-index-reads-the-same` 与
  `fresh-index-gives-the-same-results` 均 0 failures；rag interop 定向
  **2 tests / 312 assertions** 全绿。
- jinshu 全量 `clojure -M:test`：**1213 tests / 12446 assertions**，
  0 failures。

