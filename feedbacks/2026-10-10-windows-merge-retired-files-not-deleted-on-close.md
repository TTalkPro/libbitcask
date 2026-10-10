# Windows 上 merge 之后 `bitcask_close` 不删退休文件，盘上体积不降（close 的排水排在释放读句柄之前）

**报的人**：锦书（C# 下游，经 Keelson.Bitcask 1.0.6 调 libbitcask，Windows x64）。起因：锦书的 `Jinshu.Rag` 有一条压缩判据
`RagIndexTests.Compaction`（merge 之后要求「盘上变小了」、并之后旧 data 文件不再留在目录里），Linux 上绿、Windows 上红。
**严重度**：🟠 结果错，不报错（盘空间不回收，直到下次打开才被当普通 data 文件收编；调用方拿到的 merge 统计是「并了」）。

## 症状

Windows 上：`merge` 成功（`NeedsMerge` 报需要、`Merge()` 返回，3/4 条记录已死），随后 `bitcask_close`（下游 Dispose）。
关闭之后数据目录里旧的 `*.bitcask.data` 仍在，`BytesAfter == BytesBefore`，不报错。

## 位置

- `src/cask/cask.cpp:775`：`Cask::close()` 一开头的 `drain_retired_files();  // B4：close 兜底排水（退休文件不过夜）`。
- `src/cask/cask.cpp:786–792`：排水之后才 `read_files_.clear()`（释放读句柄缓存，也就是 sealed data 文件的映射）。
  **排水在前、释放映射在后**，所以排水时要删的退休文件仍被映射占着。
- `src/cask/cask.cpp:946–962`：`drain_retired_files()`。删除失败的路径被放回 `retired_files_`，然后就没有下一次排水了
  （close 之后析构，队列直接丢弃）。
- `src/io/win32_file.cpp:694`：Windows 的 `remove_file` 就是 `::DeleteFileW`。`:692` 的注释写明「被 section 映射持有的文件
  即便如此也删不掉（设计稿 C2）…把映射逐出后再删是 S37-6 要新增的不变量」——这条不变量至今没实现。

## 怎么撞上的

1. 开一本库，写入同一批键 4 轮（每轮覆盖上一轮），中间做过一次搜索（搜索让 data 文件进读句柄缓存、被映射）。
2. `bitcask_merge`（或 `NeedsMerge` + `Merge`）：输入文件进退休队列，`IndexBytes` 之前仍在盘上。
3. `bitcask_close`：`drain_retired_files()` 对仍被映射的文件 `DeleteFileW` 失败 → 放回队列 → `read_files_.clear()` 释放映射
   → 析构时队列丢弃。
4. 目录里旧文件仍在，`*.bitcask.data` 总大小没有降。

Linux 上 unlink 映射中的文件是成功的，所以 Linux 不暴露。

> 病因是从代码读出来的（排水与映射释放的先后顺序 + `DeleteFileW` 对映射文件失败的注释），还没有用 C API 单独复现。
> 能确认的是：在 Windows 上这条判据稳定失败，且失败形态与上面一致（并了，但盘上没变小）。

## 影响面

- 所有在 Windows 上做 merge 然后关库的下游都会撞：空间不回收，直到下次 open（崩溃残留的退休文件会在下次 merge 时自愈收编，
  所以数据不丢）。
- 不报错。`merge` 的返回值与 `needs_merge` 都正常。

## 建议

1. **最小改动**：把 `close()` 里的 `drain_retired_files()` 挪到 `read_files_.clear()` 之后（以及 `active_data_.reset()` 之后）。
   释放映射之后删除就不再被占用挡住。
2. **更稳**（也是 S37-6 那条未实现的不变量）：Windows 上删除退休文件之前先把它们的映射逐出（从读句柄缓存摘掉、释放视图），
   再 `DeleteFileW`。
3. 不论哪种，**close 结束时仍留在 `retired_files_` 里的路径不应静默丢弃**：至少记一条日志（路径 + 原因），让下游看得见
   「还有 N 个退休文件没删掉」。
4. 其它排水点（`checkpoint` 入口，`cask.cpp:3056` 附近）也要检查同样的先后顺序。

## 验收

- Windows：merge 之后 `close`，退休文件应从目录里消失，`*.bitcask.data` 总大小变小。
- 锦书侧的回归用例：`RagIndexTests.Compaction`（`tests/Jinshu.Rag.Tests/RagIndexTests.cs:226`）。
