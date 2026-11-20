# `needs_merge` 看不见本次打开之后产生的死数据：文件 95% 是死的仍返回 false，重开库才认 —— 长期不重启的写者永远不合并

**报的人**：bosun（Erlang 任务管理，经 bitcask 10.0.0 撞到，11.0.0 `862d735` 复现；Linux x64；whitespace / jieba 分词均如此）。
**严重度**：🟠 该合并时不合并，盘上体积只增不减，不报错。`status` 与 `needs_merge` 对同一批文件给出相反的结论。

## 症状

bosun 给检索索引加定时 merge，按文档里的用法「`needs_merge` 说要并就并」。一个长期开着的写句柄上，
每个 key 覆盖写了 20 遍，`status` 清清楚楚报着死字节，`needs_merge` 一直是 false——
封口当前写文件、再写、`sync`、等 2 秒都不变，**关掉重开才变成 true**：

```text
                                    status = [{file, frag%, dead_bytes, total_bytes}]                needs_merge
reopened                            [{1,0,0,20332}]                                                   false
20 overwrites of every key          [{2,95,386858,407240},{1,100,20332,20332}]                        false
close_write_file                    [{2,95,386858,407240},{1,100,20332,20332}]                        false
one more put (new active file)      [{3,0,0,408},{2,95,386858,407240},{1,100,20332,20332}]            false
sync + 2s                           [{3,0,0,408},{2,95,386858,407240},{1,100,20332,20332}]            false
close + reopen                      [{3,0,0,408},{2,95,386858,407240},{1,100,20332,20332}]            ["1","2","3"]
```

文件 1 是 100% 死的、文件 2 是 95% 死的（缺省 `frag_merge_trigger` 60 / `frag_threshold` 40 早就过了），都已封口、都不是 active。

另一个角度的同一件事：一次 merge 做完后句柄继续写，把刚并出来的文件整个写死，`needs_merge` 也是 false，直到重开。
而**首次建库的那一轮**（空目录打开 → 写 → `close_write_file`）`needs_merge` 是 true——所以按「写一轮、封口、问一下」写的测试是绿的，
问题要到第二轮（重开过、或 merge 过之后）才出现。

## 位置

- `src/cask/cask.cpp:3190`（`Cask::needs_merge`，S43-7）：候选统计优先取 `authority_->state().manifest_fstats()`——
  注释写的是「BCOM v4 是 OKI / authority 共同提交点的**冻结**对拍结果」——只有它为空才退回实时的 `authority_->fstats()`。
  有了 v4 清单之后（任何重开过 / 提交过的目录），`needs_merge` 看的就是上一个提交点的快照；
  这之后的覆盖写 / 删除产生的死字节只进了实时 fstats。
- `status` 走的是实时 fstats，所以两者对同一批文件结论相反。
- 提交点什么时候推进，从下游看不出来也触发不了：`sync`、`close_write_file`、再写一批、等待都不行；实测只有 close + open（以及做一次 merge）会让它跟上。

## 怎么撞上的

```erlang
Doc = fun(I, V) -> #{text => iolist_to_binary(io_lib:format("task ~b rev~b ~s", [I, V, lists:duplicate(20, "lorem ipsum dolor ")]))} end,
W = fun(H, Revs) -> [ok = bitcask:put(H, <<"k", (integer_to_binary(I))/binary>>, Doc(I, V)) || V <- Revs, I <- lists:seq(1, 50)] end,
O = [read_write, {analyzer, whitespace}],
H = bitcask:open(Dir, O), W(H, [1]), ok = bitcask:close(H),
H2 = bitcask:open(Dir, O),
W(H2, lists:seq(2, 21)),                       %% 每个 key 覆盖 20 遍
ok = bitcask:close_write_file(H2),
bitcask:status(H2),                            %% 文件 2：95% 死；文件 1：100% 死
bitcask:needs_merge(H2),                       %% false  ← 该是 {true, ...}
ok = bitcask:close(H2),
H3 = bitcask:open(Dir, O),
bitcask:needs_merge(H3).                       %% {true, {[1, 2, 3], []}}
```

C++ 侧等价：`Cask::open` → 覆盖写 → `needs_merge()`，与 `status()` 的 `dead_bytes` 对照。

## 影响面

- 所有「定时问 `needs_merge`、true 才 `merge`」的长驻写者（这是文档给的标准用法，C API `bitcask_needs_merge` 同源）：
  进程不重启，死数据就一直攒着。重启一次才会并一次。
- merge_only 旁车（`bitcask:merge/1,2` 让 cask 自己挑文件）同样问的是 `needs_merge`：实测在上面的状态下 `merge(Dir, Opts)` 返回 ok 但什么都没并。
- 不报错；`status` 报着死字节，下游会以为「阈值没到」而不是「它没看见」。
- bosun 已绕开：不问 `needs_merge`，按 `status` 自己挑文件，显式传给 `merge/3`（显式文件表的合并是好的，回收量与 `status` 一致）。

## 建议（下游不替 libbitcask 拍板）

1. `needs_merge` 的触发判断用实时 fstats（至少把提交点之后的死字节增量叠上去）；清单快照只用来约束「哪些文件可以安全地并」。
2. 或者给一个下游能调的「推进提交点」入口，并在 `needs_merge` 的文档里写明它看的是提交点快照——现在 `sync` 不算，容易想当然。
3. 回归用例：重开一次库之后覆盖写，`needs_merge` 与 `status` 的结论要一致。

## 回没回

已修（**未发版，S57**，按建议 1）：`needs_merge` 候选统计改用实时 per-file 账本（与 `status()` 同源；实时账本 = 打开时采信的
v4 清单 + 之后每次写 / 删 / 合并的增量），实时账本为空才退回清单。不需要「推进提交点」入口。merge 的活性裁决本就走 OKI 读视图，
不依赖选文件的统计。回归 `NeedsMergeLive.SeesDeadBytesAfterReopen/{Kv,Search}`：重开后覆盖写 20 遍 ⇒ `needs_merge` 为真、候选含
`status` 过阈值的全部封口文件；合并后把输出写死同样看见（11.0.0 上两组都红）。bosun 按 `status` 自挑文件的绕法可以撤。
