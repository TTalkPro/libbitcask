# 原子批里的删除：批头声明的区间多算了 tombstone v2 的 4 字节 shadow ⇒ 恢复把批当成不完整的——12.0.3 截断文件（删除与其后的写全丢），12.1.0 跳过（删除丢失），多于 64 个就打不开

**报的人**：bitcask（实现 Cypher 写语句 Q2-4 时，基准在「删顶点 → 关库 → 重开」处报 `bad_crc`；libbitcask 12.1.0 `1caee1a` 与 12.0.3 `d995557` 对照，Linux x64）。
**严重度**：🔴 已提交的原子批在恢复时被判为不完整。12.0.3：`open` 成功、不报错，数据文件从第一个含删除的批处被截断——
那次删除**以及之后写进同一个文件的全部记录**静默消失（被删的 key 复活）。12.1.0：含删除的批被当成坏区跳过（删除丢失、日志里一条 warn），
同一个文件里多于 64 个这样的批时 `open` 直接返回 `bad_crc`、库打不开。只要 `tombstone_version = 2`（bitcask 默认）且原子批里删了**存在的** key 就会写出这种批；
图层（bitcask `graphdb`）的删边 / 删顶点都走原子批里的 `remove`。

## 症状

索引模式（`{analyzer, whitespace}`）打开，2000 条 `put`，再用 `put_batch_atomic(H, [{remove, K}])` 逐条删一部分，`close`，重开：

| 版本 | 删除批数 | 重开 | 读得到的 key（应为） | data 文件 |
|---|---|---|---|---|
| 12.0.3 | 50 | `ok` | **2000**（1950） | 103600 → **100000** 字节 |
| 12.0.3 | 280 | `ok` | **2000**（1720） | 120160 → **100000** 字节 |
| 12.1.0 | 50 | `ok` | 1950（1950） | 不变 |
| 12.1.0 | 137 / 280 | **`{error, bad_crc}`** | — | 不变 |

用普通 `delete`（不进原子批）删同样多的 key：两个版本都正常。KV 模式正常关闭再重开也「正常」——只是因为重开直接用持久化的 OKI，
不 fold 数据文件；批头本身同样写错了（见下），崩溃后或 OKI 重建时一样会撞上。

把数据文件逐条解出来（20 条 put + 3 个各删一条的原子批），每个批头都声明 `span=36`，后面那条 tombstone 实际 32 字节：

```
@1000 batch header ord=20 ver=1 count=1 span=36
@1040 type=1 ord=21 key=<<"k",0,0,0,3>> vs=0 total=32   [declared end 1076, actual end 1072]
@1072 batch header ord=22 ver=1 count=1 span=36
@1112 type=1 ord=23 key=<<"k",0,0,0,7>> vs=0 total=32   [declared end 1148, actual end 1144]
@1144 batch header ord=24 ver=1 count=1 span=36
@1184 type=1 ord=25 key=<<"k",0,0,0,11>> vs=0 total=32  [declared end 1220, actual end 1216]
end @1216 of 1216
```

## 位置

- `src/cask/cask.cpp:2672-2732`（`put_batch_atomic` ①b 成员预编码）：删除成员在 `final_ops.at(op.key) == i && opts_.tombstone_version == 2`
  且 key 原来存在时，把 4 字节 shadow（旧 file_id，小端）编进 `arena`，并计入 `span_bytes`（`:2708`）。
- `src/cask/cask.cpp:2809-2816`（④ 成员 `write_buffered`）：`const std::span<const std::byte> val = is_put ? <arena 里的编码值> : std::span<const std::byte>{};`
  ——**删除成员一律写空值**，`arena` 里准备好的 shadow 没有写出去。于是批头声明的区间比实际多 4 × (带 shadow 的删除成员数) 字节。
  对照：单条 `remove`（`cask.cpp:3029` 起）写的是带 shadow 的 v2 tombstone。
- 恢复：`DataFile::fold`（S35 批语义）在批成员越过声明区间 / 区间不完整时停在批头（`out_last_valid_end` 停在批头起点）。
  12.0.3 的恢复把它当 torn tail 截断（`cask_recovery.cpp` 的 `truncate_to(last_valid_end)`）；12.1.0（S67）对非崩溃写者的文件改成跳过坏区接着读，
  每个这样的批算一处坏区，`fold_committed` 超过 `kMaxHealedGaps`（64）就返回 `kBadCrc`（`src/fileops/committed_fold.cpp:100-102`），
  `load_authority_from_disk` / 搜索重放据此让 `open` 失败（`cask_recovery.cpp:534` / `:779`）。

## 怎么撞上的

```erlang
H = bitcask:open(Dir, [read_write, {analyzer, whitespace}]),           %% tombstone_version 缺省 2
[ok = bitcask:put(H, <<"k", I:32>>, <<"graph path node">>) || I <- lists:seq(1, 2000)],
[ok = bitcask:put_batch_atomic(H, [{remove, <<"k", I:32>>}]) || I <- lists:seq(1, 300)],
ok = bitcask:close(H),
bitcask:open(Dir, [read_write, {analyzer, whitespace}]).                %% 12.1.0：{error, bad_crc}；12.0.3：ok，但 300 个 key 全复活
```

带条件的删除（`{remove, K, {ord, N}}`）同样；批里有多个删除成员时每个都少 4 字节。

## 影响面

- 谁会撞：`tombstone_version = 2`（bitcask 默认，`bitcask.app.src`）并用原子批删除存在的 key 的所有调用方。bitcask 的 `graphdb`（`del_edge` /
  `del_edges` / `del_vertex` 与事务版、`bitcask_txn` 提交里的 `delete`）都在其中——**只要图上删过东西，数据文件里就已经有这种批**。
- 撞了会怎样：12.0.3 及更早——恢复从第一个这样的批截断文件，之后写进同一文件的所有记录永久丢失，`open` 成功、不报错；
  12.1.0——删除不生效（被跳过），多于 64 个这样的批时库打不开（`bad_crc`）。
- 什么时候触发：需要 fold 这个数据文件的恢复——索引模式（搜索重放会 fold 水位之后的数据）、OKI 缺失 / 重建、崩溃后的 tail fold。
  KV 模式正常关闭后重开不 fold，所以平时看不出来。
- 已经写出去的批不会自己变好：修了写端之后，旧文件里的这种批仍然需要读端认得（见建议 2）。

## 建议（下游不替 libbitcask 拍板）

1. **写端**：删除成员把 `arena` 里的 shadow 写出去（`val` 对删除也取 `vals[i]`，长度为 0 时就是空），让写出的字节与 `span_bytes` 一致；
   或者反过来，原子批里的删除不写 shadow、`span_bytes` 也不算——但那样 merge 的「shadow file_id 仍存在才回收」在批删除上就没了，
   与单条 `remove` 不一致。加一条回归：原子批删存在的 key（v2）→ 关库 → 删 OKI 重开 / 索引模式重开，删除生效、文件不变。
2. **读端兼容已经写出去的批**：fold 遇到「批头声明的区间比成员实际字节多出 4 × k，且多出来的恰好对应 k 个空值 tombstone 成员」时，
   按成员实际字节认这个批（它是已提交的，只是批头写错了），不要停在批头。可以只对修复之前的纪元（meta 版本）放宽。
   否则下游已有的库要么继续打不开（12.1.0），要么继续丢数据（12.0.3 及更早）。
3. 发版说明里写清楚受影响的写法与版本，以及已有库重开时会发生什么；12.0.3 及更早的用户里，曾经从批删除处截断过的文件已无法恢复。

## 回没回

**libbitcask（2026-11-23，S69，已编码未提交、版本未定）**：已修，两条建议都做了。

- 建议 1（写端）：删除成员写出 ①b 里准备好的值（v2 shadow 或空），写出的字节与 `span_bytes` 一致，与单条 `remove` 的 tombstone 同形。
- 建议 2（读端兼容旧批）：「Count 条成员读满、离声明的区间末尾还剩 4 × j 字节、j ≤ 空值 tombstone 成员数」时按已提交收口（`format::legacy_batch_closes`，
  `DataFile::fold` 与 `fold_committed` 同一规则）；批头区间越过文件尾不超过 4 × count 时先读成员再判。没有按纪元区分——这个规律只有这个缺陷会产生，
  别的账对不上照旧按不完整处理。
- 建议 3：CHANGELOG `[Unreleased]` 写了受影响的写法、版本与已有库重开时的行为；12.0.3 及更早截断过的文件无法恢复。

你们的复现（索引模式、2000 put + 280 个单删除原子批 + 1 条普通写）：12.1.0 写出的库，修复后的构建直接打开，存活 1720 逐个一致、全文命中 1720、
文件不变；修复后写出的库 12.1.0 也能打开。回归 `AtomicBatchTest.S69*`。
