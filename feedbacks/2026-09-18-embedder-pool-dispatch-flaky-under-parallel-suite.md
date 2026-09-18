# `pool_dispatch_avoids_busy_worker` 在全量套件并行负载下偶发假失败：guard 与断言之间邮箱可能已排空

**报的人**：bitcask（下游消费者，submodule pin `598d080`；本条与图功能无关，是跑全量
回归时撞到的）
**严重度**：🟡 CI 稳定性——间歇、与负载相关、隔离复跑不现形。

## 症状

全量 `rebar3 eunit`（~200 个测试）偶发一例：

```
bitcask_embedder_server_tests: -pool_dispatch_avoids_busy_worker_test_/0-fun-7-...
**error:{assertEqual,...}
```

失败点为 `test/bitcask_embedder_server_tests.erl:445` 的
`?assertEqual(W1, pick_probe([W0, W1]))`——pick 返回了 **W0**（被灌忙的 worker）。
**模块隔离跑不复现**：`rebar3 eunit --module=bitcask_embedder_server_tests`
连续 6 次全绿（38/38）；同日全量套件 4~5 次里撞到 1 次。

## 位置

`test/bitcask_embedder_server_tests.erl:444-447`：

```erlang
case qlen_probe(W0) of
    {0, L} when L > 0 -> ?assertEqual(W1, pick_probe([W0, W1]));
    _ -> ok      %% mock 太快没堆起来，这条就不作数
end,
```

⭐ `:446` 自己的注释（「mock 太快没堆起来」）已经承认了竞态窗口，但 guard 只挡
「**从头到尾没堆起来**」这一种——挡不住另一种：**`qlen_probe(W0)` 在 `:444` 采样到
`L > 0`，到 `:445` 的 `pick_probe` 再采样时，200 个瞬时 mock 已经排空**。此时
`pick_probe`（`:507-508`，`lists:min([{qlen_probe(X), X} || ...])`）看到两个 worker
队列同为 0，`lists:min` 平局取首元素 → 返回 **W0** → 断言失败。全量套件的并行
编译/调度负载放大的正是这个窗口。

## 怎么撞上的

全量 `rebar3 eunit`，多跑几次。本侧观测：全量 ~4-5 次 1 次失败；模块隔离 6 次 0 失败。

## 影响面

- 只影响 CI 可信度（假红），不影响库本身行为——真队列在时 least-queue 派发
  工作正常（隔离跑与生产路径都没见过错派发）。
- 随机器快慢波动：越快的机器 mock 排空越快，窗口越窄但随机性越强。

## 建议

下游不替上游拍板，两个低成本方向：

1. **采样合一**：把「读 qlen」与「断言 pick」改成对同一份瞬时快照判断——例如
   `pick_probe` 返回 `{Worker, Qlens}`，断言处先校验 `Qlens` 里 W0 确实 > 0
   再断言选了 W1（排空即自动跳过，与 `:446` 的意图一致）；
2. **让 mock 慢到排不空**：给这条用例的 mock provider 加可配置的执行时延
   （如 5ms），把窗口做宽到调度噪声进不来。
