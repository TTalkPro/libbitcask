# `bitcask_range_iter_start` 的头注释说「只读打开且没建过 OKI ⇒ NO_INDEX」，与实现不符

**报的人**：keel（keelson clj-bitcask，转 bitcask-viewer V-02，pin 9.0.0 `d164744`，Linux x64）。
**严重度**：🟢 文档勘误，行为本身是对的。

## 症状

clj-bitcask 照 C API 头写了 docstring「只读打开且目录没建过有序索引 ⇒ :no-index」；bitcask-viewer 实测：只读打开、删掉
`kv.oki.*` 之后 range 迭代照常工作，从没见到 `BITCASK_ERR_NO_INDEX`。Keelson 复核（9.0.0）：删 OKI 后只读遍历 50 条正常、
不写回 OKI 文件；空目录只读打开也正常回 0 条。

## 位置

- `c_api/bitcask_kv.h:985-986`：`BITCASK_ERR_NO_INDEX — 只读/merge-only 打开且目录未建过 OKI（本句柄本就不建索引）`。
- 实际行为见 `include/bitcask/cask.hpp:1097-1099`（「merge_only 不建读视图 → kNoIndex …… RO 打开可从 data 建内存读视图，不写 run/manifest」）
  与 `src/cask/cask_range_iter.cpp:56` 的报文（`read-only or merge-only open of a dir without oki`，同样把只读写进去了）。

## 怎么撞上的

KV 库读写写入若干条、关闭；删 `kv.oki.*`；`read_write = 0` 打开；`bitcask_range_iter_start` ⇒ `BITCASK_OK`，遍历完整。

## 影响面

下游按注释给用户写「只读要先读写开一次建索引」之类的说明 / 分支，是错的；不影响数据。

## 建议

`bitcask_kv.h:985` 改成「merge-only 打开（不建读视图）⇒ NO_INDEX；只读打开会从数据建内存读视图（不写盘）」，
`cask_range_iter.cpp:56` 的报文去掉 read-only。Keelson 这边 clj-bitcask 的 docstring 已按实际行为改了。

## 回没回

**已改（9.1.0 已发版，S54）。** 行为本来就是对的，改的是说明。核对过：只读打开时 recovery 走 `rebuild_memory_from_data`
（内存读视图、不写盘），失败才置 `oki_rebuild_failed_` ⇒ `INDEX_REBUILD_FAILED`；`NO_INDEX` 只出在 merge-only 句柄。

改动面：`bitcask_kv.h`（range 注释与 `INDEX_REBUILD_FAILED` 枚举注释）、`cask.hpp`（`kNoIndex` / `kIndexRebuildFailed`
两处注释）、`cask_range_iter.cpp` 报文（原文把 read-only 写进了 NO_INDEX 的成因，现为
`oki unavailable on this handle (merge-only open builds no read view; open read-only or read-write to iterate)`）、
`doc/api-c.md`、`doc/api-cpp.md`。回归 `OkiRangeTest.ReadOnlyWithoutOkiIteratesMergeOnlyHasNoIndex`：删掉 OKI 后
只读遍历与影子一致、不写回 OKI 文件；merge-only ⇒ `kNoIndex`。

