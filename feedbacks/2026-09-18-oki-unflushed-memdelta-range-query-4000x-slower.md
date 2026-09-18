# OKI memdelta 未 flush 时 range 查询按 memdelta 体量线性变贵：5 万边规模实测 ~120ms/跳，flush 后 ~0.03ms（4200×）

**报的人**：bitcask（下游消费者，submodule pin `598d080` = 6.2.0-35-g598d080，经 Erlang
NIF 门面访问；每次查询 = 一个 `bitcask_range_iter`）
**严重度**：🔴 不报错的静默性能悬崖——装载后开始服务的负载必撞，撞了就是生产事故级查询延迟。

## 症状

**一个字都不报**：批量装载之后，range 扫描（`make_range_iter` / `bitcask_range_*`）
每次调用固定付出与「未 flush 写入量」成正比的时间；close→reopen（memdelta flush 成
sealed run）后同样的查询快 3~4 个数量级。下游图层一跳 = 一次 range，BFS 三跳从
可用直接劣化到分钟级。

实测（下游机器，固定随机种子，前缀 range 单跳 = `{"e"+vid+etype 前缀, succ}`，
300 次随机顶点取平均）：

| 规模 | memdelta 未 flush | close→reopen 后 | 比值 |
|---|---|---|---|
| 2k 顶点 / 8k 边 | ~11.9 ms/op | ~0.017 ms/op | ~700× |
| 10k 顶点 / 50k 边 | ~120.2 ms/op | ~0.028 ms/op | ~4236× |

装载耗时本身不贵（5 万边 0.63s，经 `put_batch_atomic`）——贵的是**装载完之后
的每一次读**。

## 位置

`src/cask/cask_range_iter.cpp:52`（seek 时 memdelta `lower_bound(lo)`）与
`:89-110`（归并步跨「全部 run 头 + memdelta 头」）。机制推测（请上游核实）：
`doc/ordered-key-index-design-zh.md` §5.1 写明 memdelta 是「append + 惰性排序」——
惰性排序的成本落在**每一次** range 的 seek/归并上，与查询次数相乘；flush 之后
memdelta 近空，同样的归并近似免费。

## 怎么撞上的

最短复现（下游侧，Erlang；任何装载→查询的序列都行）：

```erlang
R  = open_read_write(Dir),
%% 灌 5 万条边（put_batch_atomic，e+ei+et+deg+degi 五键/条）
[ok = put_edge(R, Src, 7, Dst) || _ <- lists:seq(1, 50000)],
%% 装载完立即查询：~120 ms/次
timer:tc(fun() -> [range_one_hop(R, V) || V <- Probe] end),
ok = close(R),
R2 = open_read_write(Dir),                    %% close 触发 memdelta flush
%% 同样的查询：~0.03 ms/次
timer:tc(fun() -> [range_one_hop(R2, V) || V <- Probe] end).
```

复现脚本（可调规模）已随下游工作区留存，需要可直接取。

## 影响面

- **谁会撞**：一切「批量装载 → 开始读」的负载（图装载、索引导入、批量导出后
  校验、测试夹具）。**不报任何错、不告警**——只是每次 range 悄悄变成 100ms+。
- **撞了会怎样**：查询延迟与「未 flush 写入量」同阶放大（我们的 BFS 三跳在
  8k 边规模就劣化到 4.5s，且每次 BFS 的每跳每顶点都要付一次）。临时缓解 =
  装载完 close→reopen 或等 memdelta 阈值自动 flush。
- 顺带一条**实测良好**的行为：迭代进行中的并发写（含 put 与 delete）不会让
  游标跳变、输出依旧 key 序完整——per-key 弱一致的实作比文档承诺的更稳，
  建议把这个性质固化成回归测试。

## 建议

下游不替上游拍板，列几个方向供评估：

1. **文档先行**：`doc/api-c*.md` 与 OKI 设计文档写明「range 查询成本含未 flush
   memdelta 的重整项，装载后建议 checkpoint/reopen」——现在完全查不到。
2. `status()` 暴露 memdelta 体量（条数/字节），让调用方至少能观测到「查询为什么慢」。
3. 结构性选项（成本自高到低）：memdelta 改为「有序小段追加」（flush 前查询
   只归并增量）；或装载期间自动放宽 flush 阈值、装载停止后异步 flush；或
   seek 时对 memdelta 只做一次排序并缓存脏标记。
