# HNSW `clone_live` 重活邻过滤后 0 层图断裂 —— merge 后向量检索返回错 key / 命中数不足

**报的人**：锦书（jinshu，clj，经 clj-bitcask，Windows x64，pin 8.1.0
`13322bd`，preset `msvc-release`）。
**严重度**：🔴 结果错，不报错。

## 症状

覆盖写几轮（活文档占少数）之后 `merge!`，向量检索开始返回不属于查询的
key，且 top-K 命中数不足（K=3 只回 1 条）。不报错；跨进程不稳定——
同一份数据重新开库后表现可能变化。BM25 检索不受影响。

## 位置

- `src/vector/hnsw.cpp:445` `HnswIndex::clone_live()`
- `src/vector/hnsw.cpp:563` pass 2：邻接重映射 + 死邻过滤（一跳路径收缩补边）

## 怎么撞上的

写四轮：同一批 key 每轮覆盖一遍（每轮 flush + 关写文件），
活文档 ≈ 1/4、死比 ≈ 3/4，然后 `merge!`，再做向量检索。

复现与定量（Windows 8.1.0 实测，60 条查询 × 6 轮压力）：

- 6/6 轮出现错配，每轮 6~16 条错 key，全部集中在 merge 后阶段；
- K=3 命中数不足（只回 1 条）同样只在 merge 后出现。

最小回归（红/绿均可稳定复现）：
`tests/hnsw_test.cpp` 建议新增 `HnswCloneConnectivity.RepairAfterHeavyDeadFilter`
：800 节点（200 key × 4 轮版本，`ord % 4 == 3` 活）clone 后
对全部 200 个查询做 top-3 全量暴力比对；在 pin 8.1.0 上红，见下文
「已核实病因与验证」后绿。

## 病因（已核实）

pass 2 的一跳路径收缩只在「一个节点的**全部**邻居都死」时触发补边。
死比高（≈3/4）时，大量节点仍有少量活邻居，不触发收缩，但活邻居
彼此不相邻 ⇒ 0 层图被撕裂成多个互不连通的 hub 分量。merge 后图
从入口出发的贪心下降 / `search_layer` 只能到达入口所在分量的 hub，
其它分量的正确结果完全搜不到 ⇒ 返回错 key、命中数不足。
入口落在哪个分量由建图序决定，所以跨进程不稳定。

## 影响面

覆盖写为主（rewrite-heavy）的工作负载 merge 之后必然暴露；不报错、
不超时，结果是静默错的。f32 与 int8（含 int8-only 模式）都受影响。

## 建议（下游只给方向，不拍板）

pass 2 死邻过滤之后加一遍 0 层连通性检查：从入口 BFS（新图未发布，
无需锁）；对不可达节点按标准插入路径重连（贪心下降 +
`search_layer(ef_construction)` + `select_neighbors` + 双向建边 +
shrink）。连通性正常时零开销。

已按此方向做过下游侧原型验证（仅验证，不改库）：

- 上面的最小回归红 → 绿；
- 60 条查询 × 6 轮压力 6/6 错配 → 0 错配；
- clj-bitcask 全量 152 测试绿（含新加的
  「写四轮 → merge → 向量 + BM25 命中形状不变」端到端回归）；
- 带原型修复的 Windows 全量 ctest 893/893 绿（1 skipped，2 disabled），
  未发现其它面回归。
