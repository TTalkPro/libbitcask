# 大端 → 小端目录迁移（`migrate_le`）

把**旧的大端（big-endian）bitcask 目录**离线迁移成**新的小端（little-endian）
格式**的命令行工具。底层 API 是 `bitcask::migrate::migrate_be_to_le`（声明于 [`include/bitcask/migrate.hpp`](../include/bitcask/migrate.hpp)，实现于 [`src/fileops/migrate.cpp`](../src/fileops/migrate.cpp)）；本工具是这个 API 的 CLI 包装。

## 1. 背景：为什么需要迁移

bitcask 做过一次字节序 **flag-day**：盘上所有多字节整数从大端切到小端（LE-only 主机原生零转换 + mmap 零拷贝友好；详见 [`format-zh.md`](format-zh.md) 字节序说明）。切换后：

- `bitcask.meta` 的 version 从 `1`（大端纪元）迁到 `3`（小端 + CRC32）。
- 新代码打开旧大端目录时**当场拒绝**（不会静默把大端读成小端而损坏）。

要把旧目录用起来，二选一：**① 从源头重灌数据（rebuild）**，或 **② 用本工具离线迁移**（无源头数据 / 想保留现有库时）。

## 2. 构建

随 CMake 构建产出可执行 `migrate_le`：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j --target migrate_le
# 产物：build/migrate_le
```

`migrate_le` 链接 `bitcask_fileops` + `bitcask_format` + `bitcask_io`（见根 `CMakeLists.txt` `add_executable(migrate_le ...)`）。

## 3. CLI 参数

`tools/migrate_le.cpp` 的 argv 解析逻辑（`argc` 严格等于 3，否则打印 usage 并退出码 2）：

```
migrate_le <src_dir> <dst_dir>
```

| 位置 | 参数 | 必填 | 含义 |
|------|------|------|------|
| 1 | `<src_dir>` | 是 | 源目录（v1 大端格式）。工具以**只读**方式打开 —— 任何时候都不会修改它。源目录必须存在且为合法 bitcask 目录（必须有 `bitcask.meta` 且 magic=`BCME`、version=1）。 |
| 2 | `<dst_dir>` | 是 | 目标目录（v2 小端格式）。工具在执行开始时 `fs::create_directories(dst)` 创建；若已存在则会向其中写入迁移产物。**不会自动清理已存在文件**，因此建议指向全新空目录。 |

> `migrate_le` 不接受任何 flag（无 `-h`、无 `--help`、无 `--dry-run`、无 `--verbose`）。参数语义唯一由源码 `if (argc != 3) { fprintf(stderr, "usage: ..."); return 2; }` 决定；任何额外参数都会触发 usage 提示并以退出码 2 退出。

## 4. 退出码

| 退出码 | 触发条件 |
|--------|----------|
| `0` | 迁移成功。stdout 打印 5 行统计摘要（见 §6），可能含 stderr 上的 `WARNING: N records failed CRC and were skipped`（仅在源数据本身有损坏记录时打印）。 |
| `1` | 迁移中途失败。stderr 打印 `migrate failed: <reason>`，`<reason>` 来自 `bitcask::migrate::migrate_be_to_le` 返回的错误字符串。常见原因：<br>· `src dir does not exist`<br>· `cannot create dst dir: <errno msg>`<br>· `no bitcask.meta in src (not a bitcask dir)`<br>· `meta too short`<br>· `bad meta magic`<br>· `src meta already v2 (little-endian); nothing to migrate`<br>· `unknown meta version`<br>· `cannot open <data file>` / `cannot create <dst file>` / `short read <path>` / `write failed <path>`<br>· `create dst data <path>` / `create dst hint <path>` / `write record to <path>` / `write hint to <path>` / `finalize hint <path>`（hint finalize 失败）|
| `2` | 参数错误（argc ≠ 3）。stderr 打印 usage：<br>`usage: <argv[0]> <src_dir> <dst_dir>`<br>`  migrate a v1 big-endian bitcask dir to v2 little-endian (non-destructive: src read-only)` |

## 5. 输入 / 输出约定

- **非破坏性**：工具以 `std::fopen(..., "rb")` 读 src；src 目录所有文件只读。dst 目录只通过 `std::fopen(..., "wb")` 写。
- **失败隔离**：迁移按 (meta → field.schema → 逐 data 文件) 顺序执行；任一步失败立即整体中止、保留 dst 中已写入的产物（**不删**，由 caller 决定是否清理）。
- **torn-tail 容忍**：迁移器在扫 data 文件时若遇到尾部截断（record 中途 EOF）会**直接停止**当前文件的处理，并继续写 hint trailer（trailer 自带整文件 running CRC，新库 open 时若 CRC 不过会回退到 fold(data)）。详见 `src/fileops/migrate.cpp` 的 `migrate_data_file`。
- **同名 hint 重建**：`<id>.bitcask.hint` **从不**从源目录复制 —— 总是从迁移后的 `<id>.bitcask.data` 用新的小端 `fileops::HintFile` 重新生成（trailer CRC 与新格式一致）。文件名前缀取自 data 文件名（经 `fileops::mk_hint_filename`）。

## 6. 输出示例

成功时，stdout 打印 5 行统计（每个值来自 `bitcask::migrate::MigrateStats`）：

```bash
$ build/migrate_le ~/db/wiki ~/db/wiki.le
migrated /home/me/db/wiki -> /home/me/db/wiki.le
  data files     : 2
  records        : 12000 (tombstones 134)
  skipped badcrc : 0
  meta migrated  : yes
  field.schema   : yes
```

字段含义：

| 字段 | 类型 | 含义 |
|------|------|------|
| `data files` | uint64 | 成功迁移的 `<id>.bitcask.data` 文件数（每写完一个 data+对应的 hint trailer 就 +1）。 |
| `records` | uint64 | 写入 dst 的 record 总数（含墓碑）。 |
| `tombstones` | uint64 | 其中墓碑（`type=kTombstone`）数。 |
| `skipped badcrc` | uint64 | 因源 CRC 校验失败被跳过（不写入 dst）的 record 数。**仅当此值 > 0 时**工具会额外向 stderr 打印一行 `WARNING: <N> records failed CRC and were skipped (source corruption?)`。 |
| `meta migrated` | `yes` / `no` | `bitcask.meta` 是否被迁移。源目录无 `bitcask.meta` 时整次迁移会在更早阶段以非零退出码失败，因此 `no` 实际不会在成功路径里出现。 |
| `field.schema` | `yes` / `(none)` | `field.schema` 是否被迁移。源目录无 `field.schema` 时（纯 KV 目录）打印 `(none)`；存在则打印 `yes`。 |

## 7. 迁移了什么 / 跳过了什么

| 文件 | 处理 | 字段级规则 |
|---|---|---|
| `<id>.bitcask.data` | **逐 record 迁移**：用 BE 解码头 → 用 LE codec 重编码 → CRC 重新计算。 | `KeySz`/`ValueSz`/`Tstamp`/`Ord` 大端 → 小端；`Type` 单字节照搬；key/value 字节流原样保留（UTF-8 文本/二进制 meta blob 均为字节安全）。CRC 覆盖 `[Type..Value]`（即 `[4..end]`），由 `codec::crc32` 重算。 |
| `<id>.bitcask.hint` | **重生成**：从迁移后的 dst data 用新的小端 `fileops::HintFile::open(kCreate)` 写；不读源 hint。 | trailer CRC 一致（v3 magic + 8B trailer + running_crc 由 HintFile 自己维护）。 |
| `bitcask.meta` | **迁移**：version 1 → 3（LE + CRC），VecDim `u16` 大端→小端，其余字段照搬。 | 布局：`"BCME"(4)` + version=`3`(1) + mode(1) + vec_metric(1) + VecDim(2 LE) + VecQuant(1) + VecInmemInt8(1) + reserved(3) + CRC32(4, 覆盖前 14 字节)。 |
| `field.schema` | **迁移**：写入新格式（8B 文件头 `[magic:"FSCH" u32 LE][version:1 u32 LE]` + 每条 `[NameLen:u16 LE][name][CRC32:u32 LE]`）。 | 旧文件如果是新格式就 round-trip；如果是 legacy 无头格式（flag-day 后的小端无头格式）就升级到带头格式；存在则迁移，不存在则跳过。 |
| 墓碑 v2 shadow file_id | **字段级翻转**：当 record 是墓碑且 value_sz == 4 时，把 4 字节大端值重排为 4 字节小端值。 | 这是墓碑 v2 引入的 shadow file_id 字段（v0/v1 墓碑 value 为空）。 |
| `kv.keydir.ckpt` / `search.ckpt`（旧）/ `docmap.ckpt` / `bm25.ckpt` / `vec.ckpt`（新） | **不迁移** —— 由 fold / 重建恢复。新库首开时各 ckpt 缺失 → 自动从迁移后的 data 文件 fold 重建。 | 这些是派生缓存，正确性不依赖它们；迁移器一律跳过，节省成本并避免对内部格式产生耦合。 |
| `<dir>.vec` / `<dir>.qc8` / `index.manifest` | **不迁移** —— 同上。 | HNSW payload 与 manifest commit point 都是 HNSW 派生结构，新库 open 时从 data 文件 fold 重放即可。 |
| `bitcask.write.lock` / `bitcask.merge.lock` | **不迁移** —— 运行时锁文件，不应跨目录迁移。 | 调用方需保证迁移时源目录无活跃 writer/merger；新库 dst 目录由首次 `open(read_write=true)` 创建新的 write.lock。 |

> 可重建的 checkpoint / 索引文件**故意不迁移**：它们是派生缓存，新库第一次 open 时从迁移后的 data 文件 fold 重建即可（见 [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)）。

## 8. dry-run / 试运行

**`migrate_le` 不提供 `--dry-run` 模式**。要做试运行验证，建议如下：

1. 先跑 `migrate_le <src> <dst1>`（产物落到 dst1）。
2. 用新代码以 `read_write=true` 打开 `dst1`：能正常 open 且读到数据即成功（meta 已是 v3）。
3. 若对结果满意，手动 `rm -rf <dst1>` 后再正式跑一次 `migrate_le <src> <dst>`（正式目录）。
4. 确认无误后删除旧 src。

若中途失败，dst 目录可能包含部分产物；caller 自行决定是否清理（工具不会自动回滚）。

## 9. 验证迁移结果

```c
#include "bitcask_c.h"

bitcask_options_t opts;
bitcask_options_init(&opts);
opts.read_write = 1;  /* 或 0 只读 */

bitcask_t* cask = NULL;
bitcask_fault_t fault;
if (bitcask_open("/home/me/db/wiki.le", &opts, &cask, &fault) != BITCASK_OK) {
    fprintf(stderr, "open: %s\n", fault.detail);
    return 1;
}

bitcask_slice_t key = {"some-key", 8};
bitcask_get_result_t* res = NULL;
if (bitcask_get(cask, key, &res, NULL) == BITCASK_OK) {
    printf("value: %.*s\n", (int)res->value.size, (char*)res->value.data);
    bitcask_get_result_free(res);
}

bitcask_close(cask);
```

`migrate_le` 已内置 round-trip 测试覆盖：构造大端固件 → 迁移 → 小端读路径逐 record / hint / meta / shadow 校验（`tests/` 目录内对应测试二进制）。

## 10. 注意事项

- **仅支持小端主机**（x86-64 / ARM64），与引擎本体一致。LE 主机上的 `le_store_u32` / `le_load_u32` 经 `byte_order.hpp` 的位移实现，与主机字节序无关；BE 主机无原生 LE 指令，需额外的字节交换层，本工具不提供。
- `migrate_le` 是引擎内**唯一仍读大端**的地方：它自带大端解码器（`be_u16` / `be_u32` / `be_u64` 在 `src/fileops/migrate.cpp` 的匿名 namespace），写侧复用小端 `fileops::DataFile` / `fileops::HintFile`，保证产物与新写入字节结构一致。
- 已是 v2 或 v3（小端）的目录再迁移会**干净报错**（`src meta already v2 (little-endian); nothing to migrate`），不会重复迁移或破坏已有数据。
- 实现：[`include/bitcask/migrate.hpp`](../include/bitcask/migrate.hpp) (`bitcask::migrate::migrate_be_to_le`) + [`src/fileops/migrate.cpp`](../src/fileops/migrate.cpp) + CLI [`tools/migrate_le.cpp`](../tools/migrate_le.cpp)。