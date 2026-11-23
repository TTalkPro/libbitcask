# 行落盘之后（重开 / checkpoint 之后）点查命中慢 5～13 倍：run 的点查在 4 KiB 块内从头线性解码，每次还重算一遍块 CRC

**报的人**：bitcask（Erlang 封装；升到 12.0.3 后量图层，发现「重开后」的读没有跟着变快。libbitcask 12.0.3 `d995557`，Linux x64，GCC Release）。
**严重度**：🟠 性能，不报错，结果正确。影响面大——库只要重开过或 checkpoint 过，点查就都走这条路；对长驻的库这是常态。

## 症状

不报错。同一批 key（128 字节的值，页缓存全热），唯一的区别是行在 memdelta 里还是已经落进 run：

| 库里的 key 数 | 行在 memdelta（12.0.3，S65 之后） | 行在 run（建库 → close → 重开） |
|---|---|---|
| 1 024 | 0.26 µs | **3.38 µs** |
| 100 000 | 0.62 µs | **3.97 µs** |
| 400 000 | 0.73 µs | **4.08 µs** |

每格是经 NIF 命中一次的平均（300 万次）。落盘之后与 key 数几乎无关，稳定在 3.4～4.1 µs。

下游图层同一个库（2 万顶点 / 10 万条边）重开前后：一跳读 1.0 → 3.2 µs，度数 1.2 → 3.2 µs，点查一条边 1.2 → 3.2 µs，
只含一条 check 的原子批 0.9 → 2.9 µs。

`perf record`（10 万 key，重开后，300 万次命中）：

| 占比 | 符号 |
|---|---|
| 27.4% | `__memcmp_avx2_movbe` |
| 20.3% | `OkiRunReader::Cursor::next` |
| 约 10.3% | `std::string::_M_assign` / `append` / `resize`（逐条重建 key） |
| 6.6% | `hw::detail::crc32_pclmul` |
| 4.9% | `codec::decode_data_record` |
| 3.8% | `OkiRunReader::seek_impl` |
| 2.3% | `OkiState::locate` |

（另有 5% 是 BEAM 空闲调度线程。）取 data 记录本身（`decode_data_record`、sealed 映射读）只占一小部分，时间在 run 里找这一行上。

## 位置

- `src/fileops/oki_run.cpp:712` `OkiRunReader::find` → `:682` `seek_impl`：先在块首 key 表上二分出块（`:690`，这步没问题），
  然后「块内线性推进到首个 key ≥ lo」（`:699` 起的循环）——每一步调 `Cursor::next`。
- `:592` `Cursor::next`：块内条目是前缀压缩的（`shared_len` + `suffix`），没有重启点。每走一条要：读 2～6 个 vbyte、
  `prev_key_.resize` + `append` 重建完整 key（`:617`）、解位置字段、**`out.key.assign(prev_key_)` 再拷一份**（`:656`）；
  回到 `seek_impl` 再做一次 `std::string_view(e.key) >= lov` 比较（`:704`）。
  块是 4 KiB（`kDefaultBlockBytes`，`oki_run.hpp:74`），短 key 的行每条十几个字节，一块两三百行；命中平均要这样走过半块。
- `:574` `Cursor::load_block`：块缓存**命中**之后仍然对整块 `verify_crc`（v3 run）。块进缓存时已经读进内存，之后每次点查重算一遍
  4 KiB 的 CRC（perf 里那 6.6% 的 `crc32_pclmul`）。

## 怎么撞上的

```erlang
R = bitcask:open(Dir, [read_write]),
V = binary:copy(<<"v">>, 128),
[ok = bitcask:put(R, <<"k", I:64>>, V) || I <- lists:seq(1, 100000)],
ok = bitcask:close(R),
R2 = bitcask:open(Dir, [read_write]),            %% 行都在 kv.oki.seg-* 里了
%% 命中：约 4.0 µs / 次；不 close 重开（行还在 memdelta）是 0.62 µs
[bitcask:get_with_ord(R2, <<"k", (1 + (I * 7919) rem 100000):64>>) || I <- lists:seq(1, 100000)].
```

脚本在开发机 `/tmp/gprof/perfget2.escript`（`escript perfget2.escript 100000 build` 再 `… read`），图层对比在 `/tmp/gprof/r6.escript`，
都在 bitcask 仓库根目录跑。

## 影响面

- 所有落盘之后的点查：`get`、原子批的条件判定（`kAbsent` / `kOrdEquals` 都要先定位）、写路径的 `probe_for_write`、
  range 迭代器回查取值。不只是重开——长驻写者每次 checkpoint / memdelta 到阈值 flush 之后，被冻结的行也都转到这条路上。
- S65 把「行在 memdelta」的命中压到 0.3～0.7 µs 之后，这条路成了点查的主要成本，并且两种状态差 5～13 倍：
  同一个库「刚写完」和「重开之后」的读延迟不是一个量级，下游做基准很容易只量到前一种。
- 未命中靠 bloom 挡掉，不走块内扫描，这条只影响命中（以及 bloom 假阳性）。
- 不报错，结果正确。

## 建议（下游不替 libbitcask 拍板）

1. **块内别从头线性解码。** 几种做法，按改动面从小到大：
   - 点查专用的块内扫描：只为比较而解——不重建完整 key（用 `shared_len` 与目标 key 的公共前缀长度做增量比较，
     这是前缀压缩块上的标准做法）、不解位置字段、不 `out.key.assign`；找到目标行再完整解那一行。不动盘上格式。
   - 块内加重启点（每 16 行一个完整 key + 块尾偏移表），点查先在重启点上二分再线性走 ≤ 16 行。要升 run 版本（v4），
     旧 run 照旧可读。
   - 缩小块或给块缓存里的块配一份解好的行索引（内存换时间）。
2. **块缓存命中不重算 CRC**：校验放在 loader 里（块进缓存那一次），`:574` 只对刚读进来的块做。
3. 基准：`BM_Cask_Get_Hot` 加「建库后 checkpoint / 重开」的变体，和 S65 的那几档并排——现在的基准只量得到 memdelta 这一种状态。

## 回没回

**libbitcask（2026-11-23，S66，12.1.0 已发版）**：已修，三条建议都做了，盘上格式没动。

- 建议 1：`OkiRunReader::find` 块内改成前缀压缩上的增量比较，不重建 key；块缓存开着时再给被点查过的块配一份采样索引（每 16 行一个完整 key + 偏移 + 差分累加值，同样放在块缓存里），先二分、再走至多 16 行。盘上加重启点（升 run 版本）没做——内存里的采样索引已拿到大部分收益。
- 建议 2：块 CRC 挪进块缓存的 loader，只在块读进缓存那一次校验，坏块不进缓存。
- 建议 3：新增 `BM_Cask_Get_Hot_View_Reopened`。

实测命中一次（建库后重开）：1 千 / 10 万 / 40 万个 key 3023 / 3392 / 3562 ns → **365 / 703 / 1008 ns**；同档行在 memdelta 时是 130 / 502 / 646 ns，两种状态的差距由 5～23 倍收到 1.4～2.8 倍。C API、盘上格式、C++ ABI 不变。采样索引占块缓存容量的一部分（约为块大小的 15%～20%）。范围迭代器的定位没改（每次迭代只定位一次）。回归 `OkiRunFind.*`。
