# libbitcask 磁盘字节级格式

本文档是 `libbitcask` 所有持久化文件结构的字节级真源（byte-level contract）。
所有多字节整数均为小端（LE-only 主机：x86/ARM64，原生零转换 + mmap 零拷贝友好）。

权威源：

- 通用常量与 record / hint / DocValue 布局：`include/bitcask/format.hpp`
- 编解码：`src/fileops/codec.cpp`
- 文件抽象：`include/bitcask/data_file.hpp` + `src/fileops/data_file.cpp`、
  `include/bitcask/hint_file.hpp` + `src/fileops/hint_file.cpp`
- Meta 文件：`include/bitcask/meta_file.hpp` + `src/cask/meta_file.cpp`
- 字段名注册表：`include/bitcask/field_schema.hpp`
- DocMap / Index：`include/bitcask/index.hpp` + `src/keydir/index.cpp`
- 分段 checkpoint 容器：`include/bitcask/search_checkpoint.hpp`
- 组件共用类型：`include/bitcask/component_ckpt.hpp`
- 检查点链走读：`include/bitcask/ckpt_chain.hpp`
- DocValue meta 段编码：`include/bitcask/meta_codec.hpp`
- 文件锁：`include/bitcask/file_lock.hpp`

CRC 多项式统一为 zlib / IEEE 802.3，与 `erlang:crc32/1` bit-identical，由
`bitcask::hw::crc32_update`（PCLMULQDQ 硬件加速 + zlib 兜底）实现。

## 一、目录与文件清单

`open(dirname, opts, registry)` 之后，`dirname` 下会出现一组文件（按角色分）：

- **数据 / 元数据**（不可丢）：`bitcask.meta`、`<tstamp>.bitcask.data`、
  `bitcask.write.lock`、`bitcask.merge.lock`、`field.schema`
- **派生缓存**（可 fold 重建）：`<tstamp>.bitcask.hint`、各 `*.ckpt` /
  `*.prev` / `*.d<seq>`、`index.manifest`、`kv.keydir.ckpt`、`search.vec`、
  `search.qc8`

派生缓存的校验失败一律丢弃 → 退全量 fold，不会影响正确性。

### 1.1 数据 / hint 文件

| 角色 | 文件名 | 创建方 | 模式 |
|------|--------|--------|------|
| 数据 record 流 | `<tstamp>.bitcask.data` | writer | append-only |
| 数据并行索引 | `<tstamp>.bitcask.hint` | writer | append-only |

`tstamp` 是单调递增的全局 counter（不是 wall clock），由 `KeyDirRegistry` 跨
open/close 持久化分配。解析与拼装见 `fileops::mk_data_filename` /
`mk_hint_filename` / `parse_data_tstamp`（位于 `src/fileops/data_file.cpp`）：

- `mk_data_filename(dir, tstamp)` → `<dir>/<tstamp>.bitcask.data`
- `mk_hint_filename(data_path)` → 把 `.bitcask.data` 后缀替换为 `.bitcask.hint`
- `parse_data_tstamp(filename)` → 从 `<tstamp>.bitcask.data` 末尾抠出 tstamp

数据文件生命周期：

1. **创建**：`Cask::open` 首次以 `Mode::kCreate` 打开；对应 `DataFile::open`。
2. **追加**：`write()` 推进 `current_offset_`；writer 单线程串行（接口在头
   `bitcask/data_file.hpp` 注释中明确「写不可并发」）。
3. **封口**：merge 把 sealed 文件 rename 为 `<tstamp>.bitcask.merged`，再
   atomic rename 覆盖回 `<tstamp>.bitcask.data`。
4. **删除**：merge 完成或失败回滚后由后台清理。

Hint 文件与数据文件一一对应；hint 是可重建的派生索引，崩溃丢失只会让恢复
回退到 `fold(data)` 重建 keydir。

### 1.2 目录级元数据与锁

| 文件 | 用途 |
|------|------|
| `bitcask.meta` | 库配置 v4：mode、向量 metric/dim、CRC32 校验（v4 = u64 tstamp 纪元门禁） |
| `bitcask.write.lock` | 单写者互斥（cask open 拿；merger 不抢） |
| `bitcask.merge.lock` | merger 互斥（与 writer 独立） |
| `field.schema` | 字段名 ↔ field id 注册表（schema interning） |

`bitcask.write.lock` 与 `bitcask.merge.lock` 由 `lock::FileLock` 提供进程间
互斥语义（头 `bitcask/file_lock.hpp`），通过 fcntl(F_SETLK) 实现。锁本身含
stale 检测（写入者 PID）+ PID 行 + 持锁文件 fd 三要素。路径字面量在
`cask.cpp::acquire_writer_lock` / `acquire_merge_lock` 处拼装。

`bitcask.meta` 由 `meta::read_meta` / `write_meta` 在 open / 首次配置时创建。
`field.schema` 在 `cask_recovery.cpp` 与 cask 构造路径分别调
`FieldSchema::open` 加载。

### 1.3 索引 checkpoint 文件族

恢复阶段的 per-component checkpoint（设计见
`doc/recovery-unified-checkpoint-design-zh.md`）：

| 文件 | 用途 |
|------|------|
| `bm25.ckpt` / `.prev` / `.d<seq>` | 倒排索引组件 |
| `vec.ckpt` / `.prev` / `.d<seq>` | HNSW 向量组件 |
| `docmap.ckpt` / `.prev` / `.d<seq>` | 文档身份表组件（kDocmapDeltaV3 段） |
| `search.ckpt` / `.prev` / `.d<seq>` | legacy 单文件 ckpt（S17-5 起迁移路径） |
| `index.manifest` | 三组件 ckpt 的提交点 + 链长 |
| `kv.keydir.ckpt` | keydir 快照（BCKS v3：tstamp 定宽 8B） |
| `search.vec` | HNSW f32 payload（BCVP） |
| `search.qc8` | HNSW int8 量化码字 payload（BCQ8） |

`*.ckpt` 走 `SearchCheckpoint` 容器（见 §九）；`*.d<seq>` 是组件 delta 文件，
链校验三元组（base_gen / prev_wm / seq）由 `CkptSectionType::kDeltaInfo` 段承
载。`*.prev` 是上一次成功的 base，回退目标。`index.manifest` 是三组件 ckpt
的统一提交点。

组件链的细节由头 `bitcask/ckpt_chain.hpp` 的 `walk_chain` /
`remove_chain_files` 统一管理，状态结构在 `bitcask/component_ckpt.hpp` 的
`ChainState` / `LoadResult` 中集中定义。

### 1.4 tstamp 与文件 ID

扫描器按 tstamp 升序排列 sealed 数据文件（`src/fileops/scanner.cpp`）；当前
active 文件另由 cask 独占持有（`Cask::active_data_`），不参与扫描。

`DocSlot::loc.file_id` 取的就是 tstamp 的低 32 位（`std::uint32_t`），恰好填
满 hint v2 record 的 32-bit offset 字段旁路——`file_id` 与 data file 的 offset
一起构成 `DocLoc`。

文件 rename 顺序（merge 收尾）：

1. sealed data `<tstamp>.bitcask.data` → `<tstamp>.bitcask.merged`
2. merger 产出 `<new_tstamp>.bitcask.data` 与 `<new_tstamp>.bitcask.hint`
3. atomic rename `merged` → 删，new → 占位
4. 清旧 hint 文件（unlink）

任何中间崩溃保留 sealed 旧文件 + 新文件，下次 open 走恢复路径（fold 重建）。

## 二、公共常量与字节序

字节序小端（LE-only flag-day）。`format.hpp` 顶层注释给出契约：

> 全部多字节整数小端（LE 主机原生零转换 + mmap 零拷贝）；旧大端文件不可读（需
> 重建），迁移工具见 `tools/migrate_le`。

这一节列出的常量是磁盘契约的根：改一个数就是 binary-incompatible 变更，必须
同步更新黄金测试（`tests/codec_test.cpp`、`data_file_test.cpp` 等）。

### 2.1 关键常量表

所有以下常量定义在 `include/bitcask/format.hpp` 的
`namespace bitcask::format` 下，定义形式均为 `inline constexpr`。本文档不再
重复字面值，请按符号查头。

| 符号 | 含义 |
|------|------|
| `kHeaderSize` | 数据 record header 字节数 |
| `kCrcOffset` / `kTypeOffset` / `kTstampOffset` / `kOrdOffset` / `kKeySzOffset` / `kValueSzOffset` | header 内各字段的字节偏移 |
| `RecordType::kDoc` / `kTombstone` | record type u8 取值 |
| `kMaxKeySize` / `kMaxValueSize` | Key / Value 字段上限 |
| `kHintRecordSize` | hint v2 单条 record 固定字节数 |
| `kHintHeaderV4` / `kHintTrailerV4` | hint v4 文件头/trailer 字节数 |
| `kHintMagicV4` / `kHintTrailerMagicV4` | hint v4 头部/trailer magic |
| `kMaxOffsetV2` / `kTombMaskV2` | hint offset u64 的最高位墓碑标记 |
| `kDocValueVersion` | DocValue 二进制格式版本（当前 = 4：ExpiryAt u64） |
| `kDocValueHeaderSize` | DocValue 头字节数（Ver + Flags = 2） |
| `kFlagHasVector` / `kFlagHasText` / `kFlagHasMeta` / `kFlagVecQuantized` / `kFlagHasFields` / `kFlagHasExpiry` | DocValue Flags 位 |
| `kQuantizedVersion` | 量化向量 scheme 版本（对称 int8 = 1） |
| `kChunkSize` / `kMinChunkSize` / `kMaxChunkSize` | hint CRC 分块边界 |

Hint CRC32 与 data CRC32 用同一多项式（zlib/IEEE 802.3），由 `bitcask::hw::crc32`
计算，bit-identical 兼容 zlib。

### 2.2 各文件族使用的常量

| 文件族 | 用到的 format 常量 |
|--------|-------------------|
| `<tstamp>.bitcask.data` | `kHeaderSize`、`kMaxKeySize`、`kMaxValueSize`、`RecordType::*` |
| `<tstamp>.bitcask.hint` | `kHintRecordSize` (v2) / `kHintHeaderV4` + `kHintTrailerV4` (v4) / `kMaxOffsetV2`、`kTombMaskV2` |
| DocValue（嵌在 data record value 段） | `kDocValueVersion`、`kDocValueHeaderSize`、`kFlag*`、`kQuantizedVersion` |
| `bitcask.meta` | 用自己的 `kMetaMagicSize` 等（见 §三） |
| `field.schema` | 用自己的 `kMagic` / `kVersion` / `kHeaderSize`（见 §八） |
| `*.ckpt` / `index.manifest` | 用 `search_checkpoint.hpp` 的 `kCkptMagic` / `kCkptVersion` |

### 2.3 端序与 CRC 多项式

| 文件 | 字节序 | 整数编码 | CRC 多项式 |
|------|--------|----------|-----------|
| `<tstamp>.bitcask.data` | 小端 | u8 / u16 / u32 / u64 LE | zlib CRC32（IEEE 802.3） |
| `<tstamp>.bitcask.hint` | 小端 | u32 / u64 LE | zlib CRC32 |
| DocValue | 小端 + VByte | LE 整数 + varint 长度 | —（DocValue 自身不带 CRC，由 record CRC 兜底） |
| `bitcask.meta` | 小端 | u8 / u16 / u32 LE | zlib CRC32（前 14 字节覆盖） |
| `field.schema` | 小端 | u16 / u32 LE | zlib CRC32（每条 entry 单独） |
| `*.ckpt` 页脚 | 小端 | u16 / u32 / u64 LE | zlib CRC32（每段独立 + 整体 footer） |
| DocMap sidecar（`BCIS`） | 小端 | u32 / u64 LE + VByte | zlib CRC32 |

`bitcask::hw::crc32` 在 x86_64 上用 PCLMULQDQ 加速，缺该特性时回退到 zlib
查表实现——两者输出 bit-identical，保证现有 data file / hint file 的 on-disk
CRC 与历史数据兼容。

## 三、`bitcask.meta` v3

目录级配置文件，定位库运行模式与向量配置。本节对应头 `bitcask/meta_file.hpp`
与 `src/cask/meta_file.cpp` 的 `kMetaFileSize = 18`。

### 3.1 18 字节 header 布局

```
偏移 字节  字段           编码         含义
 0   4    magic           ASCII       4 字节 "BCME"（无 null 终止符）
 4   1    Version         u8          = 3（kMetaVersion）
 5   1    Mode            u8          0 = kKV / 1 = kIndex
 6   1    VecMetric       u8          见下表
 7   2    VecDim          u16 LE      向量维度，0 = 无向量
 9   1    VecQuant        u8          0/1，向量落盘 int8 量化
10   1    VecInmemInt8    u8          0/1，HNSW int8-only 内存
11   3    Reserved        全 0        保留位
14   4    CRC32           u32 LE      覆盖前 14 字节（不含 CRC 自身）
─────────────────────────────────────────
     18 字节合计（kMetaFileSize）
```

各字段对应源 `src/cask/meta_file.cpp` 的常量：

- `kMetaMagicSize = 4` / `kMetaMagic = "BCME"`
- `kMetaVersionOffset = 4` / `kMetaModeOffset = 5`
- `kMetaVecMetricOffset = 6` / `kMetaVecDimOffset = 7` / `kMetaVecQuantOffset = 9`
  / `kMetaVecInmemInt8Offset = 10`
- `kMetaReservedSize = 12` / `kMetaCrcOffset = 14` / `kMetaCrcCoverLen = 14`
- `kMetaVersion = 3`（写端恒写 3）

`VecMetric` 枚举（`namespace bitcask::meta::VectorMetric`）：

| 枚举值 | 含义 |
|--------|------|
| `kNone` | 0，本集合无向量（旧 meta 全零字节自然解码为此值，无需版本升级） |
| `kCosineNormalized` | 1，写入时归一化，查询用内积（默认推荐） |
| `kL2` | 2 |
| `kDot` | 3 |

不变量校验（读路径）：`VecMetric == kNone` 与 `VecDim == 0` 必须同步——读端
检出 `(metric==kNone) != (dim==0)` 返回 `MetaError{0, "inconsistent vector
config"}`。

### 3.2 读取与版本策略

`meta::read_meta` 的版本判定（与头注释一致）：

| Ver 字段值 | 行为 |
|-----------|------|
| 1 | 大端 legacy 格式 → 干净拒绝（错误码 `0`, message 提示重建） |
| 2/3 | u32-tstamp 纪元（record 布局不兼容）→ 干净拒绝，message 提示重建 |
| 4 | 校验 CRC32（前 14 字节覆盖）→ 不匹配返回「CRC mismatch (corrupt)」 |

字段校验顺序：

1. magic（4 字节 `BCME`）
2. Ver == 4，拒绝 1/2/3（u32 纪元）与未知版本
3. CRC32 必须匹配 `kMetaCrcCoverLen = 14`
4. Mode ∈ {0, 1}，未知返回 `unknown mode`
5. VecMetric ∈ {0, 1, 2, 3}，未知返回 `unknown vector metric`
6. 一致性：`metric == kNone` ↔ `dim == 0`

写路径 `write_meta`：

- 字段全部填好（其它字段置 0）后算 CRC32（LE 主机原生零转换，memcpy host
  序即可）。
- 写入 18 字节单文件，无 trailing 数据。
- `bitcask.meta` 不是高写入频率文件，单 write + close 即足够。

### 3.3 与 LE-only flag-day 的关系

`bitcask.meta` 是 LE-only flag-day 后的「固定小端」格式：

- 所有多字节字段（VecDim u16、CRC u32）按 LE 落盘。
- BE 主机直接写会得到非 `BCME` 的字节序 magic（拒绝），不会与 v1 legacy
  大端 meta 文件混淆。
- Ver=1 显式拒绝并提示 rebuild——绝不静默把大端字节按小端读坏（设计意图
  见 `meta_file.cpp` 的注释与错误消息）。
- 旧 v2 文件（无 CRC）继续可读，但写端恒写 v3；下次成功 save 即升级。

跨目录迁移工具 `tools/migrate_le` 同时迁 `bitcask.meta` 与所有
`.bitcask.data` / `.bitcask.hint`，对每文件独立大小端读 + 重写写。

## 四、数据文件 record（27 字节 header）

每条数据 record 头部固定 27 字节，紧接着 Key 与 Value。本节对应头
`bitcask/format.hpp` 的「数据文件 record 布局」注释与
`codec::encode_data_record` / `decode_data_record`。

### 4.1 Header 字段表

数据 record 头固定 27 字节（`kHeaderSize = 27`；64 位时间戳 flag-day 前为
23 字节，Tstamp u32），定义在
`include/bitcask/format.hpp` 顶部的「数据文件 record 布局」注释里，偏移常量
在该头中按字节给名。

```
偏移  字节  字段        编码         含义
  0     4   CRC32       u32  LE      覆盖 [4, total)（Type..Value 整段）
  4     1   Type        u8           RecordType 枚举
  5     8   Tstamp      u64  LE      unix 秒；caller 给 0 时由 Cask 填 now_sec
 13     8   Ord         u64  LE      引擎单调分配的 per-write 序号，永不复用
 21     2   KeySz       u16  LE      Key 字节数（kMaxKeySize 上限）
 23     4   ValueSz     u32  LE      Value 字节数（kMaxValueSize 上限）
─────────────────────────────────────────
       27 字节合计（kHeaderSize）
[27 .. 27+KeySz)         Key        字节序列
[27+KeySz .. total)      Value      字节序列（kDoc 时为 DocValue，kTombstone 时
                                       通常为空；tombstone_version=2 时为 4B
                                       shadow file_id）
total = kHeaderSize + KeySz + ValueSz
```

各偏移常量名（`namespace bitcask::format`）：

| 字段 | 常量名 | 偏移字节 |
|------|--------|---------|
| CRC | `kCrcOffset` | 0 |
| Type | `kTypeOffset` | 4 |
| Tstamp | `kTstampOffset` | 5 |
| Ord | `kOrdOffset` | 13 |
| KeySz | `kKeySzOffset` | 21 |
| ValueSz | `kValueSzOffset` | 23 |
| Key 起点 | `kHeaderSize` | 27 |

### 4.2 RecordType 取值

`RecordType` 是 1 字节枚举（`namespace bitcask::format`）：

| 枚举 | u8 取值 | 语义 |
|------|---------|------|
| `kDoc` | 0 | 文档：Value 段是 §五 的 DocValue v4 打包 |
| `kTombstone` | 1 | 删除标记：Value 段通常为空（tombstone_version=2 时为 4B shadow file_id） |

墓碑识别走「一等 record 类型」，不再依赖 value 段的 magic 字符串。读端走
`RecordType == kTombstone` 直接进入 tombstone 分支（见 §七）。

### 4.3 CRC 覆盖范围

CRC32（zlib/IEEE 802.3 多项式）覆盖从 `kTypeOffset`（即偏移 4）开始到 record
末尾的整段（即 Type..Value 全段），**不**覆盖 CRC 字段自身——这是项目注释
里明确的设计选择（区别于 legacy bitcask 的「从 Tstamp 起」覆盖）。

实现侧 `encode_data_record`（`src/fileops/codec.cpp`）：

1. 先把 Type/Tstamp/Ord/KeySz/ValueSz/Key/Value 全部写入 buffer。
2. 末尾 `le_store_u32(p + kCrcOffset, crc32(covered))`，其中
   `covered = [Type..Value)` 长度 `total - kTypeOffset`。

读侧 `decode_data_record` 同样对 `[kTypeOffset, total)` 算 CRC，与存值比对：
不一致返回 `DecodeError::kBadCrc`。

### 4.4 全 record 总长公式

```
total_size = kHeaderSize + key.size() + value.size()
```

- `kHeaderSize = 27`（`format.hpp`）
- `key.size() ≤ kMaxKeySize`（u16 上限）
- `value.size() ≤ kMaxValueSize`（u32 上限）

写端在 `encode_data_record` 内 `assert(key.size() <= kMaxKeySize)` 与
`assert(value.size() <= kMaxValueSize)`，超限是 caller bug。

读端的 `DataFile::read(offset, total_size)` 要求 caller 把 `total_size` 一并
传入（避免在 read 时再做 header 探测），值由 keydir 的 `DocLoc::total_sz` 字
段携带。

## 五、DocValue v4 编码

写进 `RecordType::kDoc` record 的 VALUE 段。设计灵感来自 Lucene stored fields
与 Tantivy 字段编码（项目注释明确声明），但具体字节布局是本项目自有格式。

### 5.1 头部（Ver + Flags）

DocValue 二进制格式头部固定 2 字节（`kDocValueHeaderSize = 2`），定义在
`format.hpp` 的「kDoc value 打包布局」注释中：

```
偏移 字节  字段   编码   含义
  0    1   Ver    u8     格式版本号 = 4（kDocValueVersion；v4 = ExpiryAt u64）
  1    1   Flags  u8     位掩码（见 §5.2）
─────────────────────────────
       2 字节合计
```

读端 `decode_doc_value` 严格校验 `Ver == kDocValueVersion`，否则返回
`DecodeError::kUnsupportedVersion`——本项目「不考虑向后兼容」的设计意图
明确。

### 5.2 Flags 位定义

`namespace bitcask::format` 下的一组 `inline constexpr std::uint8_t kFlag*`
常量。每个位独立启用对应段；未置位段在 record 中**不存在**（不是空段）。

| 常量 | 含义 |
|------|------|
| `kFlagHasVector` | 存在 vector 段（未量化） |
| `kFlagHasText` | 存在 text 段 |
| `kFlagHasMeta` | 存在 meta 段（结构化 KV，见 §十一） |
| `kFlagVecQuantized` | vector 段是 int8 量化（P3a） |
| `kFlagHasFields` | 存在 fields 段（S8.6） |
| `kFlagHasExpiry` | value 末尾追加 expiry 段（S13-D5） |

互斥关系：`kFlagHasVector` 与 `kFlagVecQuantized` 互斥——前者对应
`DocValueParts.vector` 直接给出 f32 的情况，后者对应 `vec_quantized = true` +
`vector` 给出的对称 int8 量化路径。`encode_doc_value` 内编码逻辑保证二者不
会同时置位。

### 5.3 段顺序

各段在 record VALUE 中按以下顺序出现，缺则无对应字节：

1. vector 段（量化或未量化二选一）
2. text 段
3. meta 段
4. fields 段
5. expiry 段（永远在最后，旧读端「按位忽略尾部字节」即可向后兼容）

固定 vector→text→meta→fields 的顺序有两个目的：

- HNSW 重建时可以 `O(1)` 切片 vector 段，无需扫描字段。
- 字段表的 field schema 演进（增加字段）不破坏旧读端的 skip 顺序。

### 5.4 段布局

#### vector 段（未量化）

触发条件：`kFlagHasVector` 置位、`kFlagVecQuantized` 不置位。

```
Dim    : VByte  元素数（非字节数）
f32[]  : LE     Dim 个 float，原生 memcpy（x86/ARM64 LE 主机零转换）
```

实现 `static_assert(std::endian::native == std::endian::little)` 强制 LE
主机编译——BE 主机直接编失败而非静默写出错误字节序。

#### vector 段（量化，P3a）

触发条件：`kFlagVecQuantized` 置位。

```
Dim       : VByte        元素数
SchemeVer : u8           量化方案版本（当前 = kQuantizedVersion = 1，对称 int8）
scale     : f32 LE       每向量的重建标度
codes     : int8[Dim]    对称量化码字
```

大小 = `vbyte(Dim) + 1 + 4 + Dim`，与 f32 的 1/4 量级。重建：
`v̂[i] = codes[i] * scale / 127`。SchemeVer=1 = 对称 int8；未来 affine 等新
方案 bump 此版本，旧读端见未知版本返回 `kUnsupportedVersion`。

#### text 段

触发条件：`kFlagHasText` 置位。

```
Len   : VByte   UTF-8 字节数
Bytes : [Len]   UTF-8 文本
```

#### meta 段

触发条件：`kFlagHasMeta` 置位。

```
Len   : VByte   字节数
Bytes : [Len]   序列化字节（meta_codec.hpp 编码的结构化 KV 列表，见 §十一）
```

#### fields 段（S8.6）

触发条件：`kFlagHasFields` 置位。

```
FieldCount : VByte
重复 FieldCount 次：
    FieldId : VByte    字段 id（由 field.schema 注册表分配，见 §八）
    ValLen  : VByte    value 字节数
    Value   : [ValLen] 字段值的字节序列
```

字段名 ↔ id 映射由 `field.schema` append-only 注册表维护，避免每条 record
内联字段名（设计动机见头注释「字段名重复百万次」）。decode 是纯函数、只还
原 id，由 caller 用 schema 译回名字。

#### expiry 段（S13-D5）

触发条件：`kFlagHasExpiry` 置位（仅当 `expiry_at != 0`）。

```
ExpiryAt : u64 LE    绝对 unix 秒；0 = 永不（不写段）（v4 起 u64）
```

段追加在既有全部段之后 ⟹ 旧读端（不识别本位）按位忽略、跳过尾部字节——旧
库读带 TTL 的记录 = 永不过期（静默降级，非拒绝）。这是项目设计层面认可的
「best-effort 兼容」行为。

### 5.5 编码/解码接口

`namespace bitcask::codec` 下：

| 接口 | 语义 |
|------|------|
| `encode_doc_value(out, parts)` | 把 `DocValueParts` 追加到 `out`，返回写入字节数 |
| `decode_doc_value(buf)` | 返回 `DocValueView`；错版本返回 `kUnsupportedVersion`，截断返回 `kBufferTooShort` |
| `doc_vector_f32(view)` | 把 view 的 vector 段还原成 f32 vector；未量化 memcpy，量化则 dequant；无向量段返回空 |

`DocValueParts` 输入契约（`codec.hpp`）：

- 各段 `std::optional` — `nullopt` 即不写 flag，不写段。
- `vector` 是 f32 span；`vec_quantized = true` 触发 int8 量化路径。
- `fields` 非空时写 fields 段（每条 `DocField` 的 `id` 由 caller 经 schema
  解析）。
- `expiry_at = 0` 不写 expiry 段（等价于「永不过期」，与解码端的 0 默认值
  一致）。

## 六、Hint 文件：v2 与 v4

Hint 是 data file 的并行索引：fold(hint) 重建 keydir 只读 key + 元数据，省
掉绝大部分 I/O。本节对应头 `bitcask/hint_file.hpp` 与 `format.hpp` 的 hint
注释。

### 6.1 v2 布局（18 字节定宽）

v2 是「无文件头、无 trailer magic」的定宽裸记录流，定义在
`include/bitcask/format.hpp` 的「hint 文件 record 布局」注释里。

```
偏移  字节  字段     编码         含义
  0     4   Tstamp   u32  LE      unix 秒
  4     2   KeySz    u16  LE      Key 字节数
  6     4   TotalSz  u32  LE      对应 data file 里整条 record 的 total
 10     8   Packed   u64  LE      (Tomb ? kTombMaskV2 : 0) | Offset
─────────────────────────────────────────
       18 字节固定（kHintRecordSize）
[18 .. 18+KeySz)     Key      Key 字节序列
```

`Offset` 含义：data file 内字节偏移。`Packed` 把墓碑标志压到 64 位最高位
（`kTombMaskV2`），其余 63 位是 offset（`kMaxOffsetV2` 上限）。读取时反向
`packed & kTombMaskV2` 得 tomb、`packed & kMaxOffsetV2` 得 offset——节省 1
字节，跟 legacy wire format 一致。

### 6.2 v4 布局（变长 + 头尾 magic）

v4 = S23-A1 引入的 v3 变长格式 + tstamp u64（64 位时间戳 flag-day），
定义在 `format.hpp` 的「hint 文件 v4 布局」注释里：

```
头部 4 字节（kHintHeaderV4）：
  0..3   Magic   u32  LE   = kHintMagicV4  (ASCII "BCH4")

记录流（变长，vbyte 编码）：
  每条 record：
    gap       : VByte  offset − prev_end（首条 prev_end = 0，gap 经 u64 二
                               补数回绕；连续追加时 gap == 0 → 1 字节）
    total_sz  : VByte  对应 data file record 总长
    kt        : VByte  (KeySz << 1) | (tomb ? 1 : 0)
    tstamp    : u64 LE  （v3 时代为 u32；v4 起 8 字节）
    key       : [KeySz]   Key 字节
  prev_end 维护：encode 时返回 offset+total_sz 串联传下一条；
                decode 时 prev_end = offset + total_sz。

trailer 8 字节（kHintTrailerV4）：
  [-8..-4]  Magic    u32 LE   = kHintTrailerMagicV4  (ASCII "BCHE")
  [-4..-1]  CRC32    u32 LE   覆盖 [0, size-8) 全字节（含文件头 + 所有记录）
```

文件总大小 = `kHintHeaderV4 + records_len + kHintTrailerV4`。

典型 v4 记录 ~12-13B（含 key 字节），与 v2 定宽 18B 相比仍显著更小
（核心动机：减少 merge 输出写放大）。

### 6.3 v2 vs v4：读写分派

读写分派逻辑（`HintFile::fold` / `validate_trailer` / `fold_validated` 共同）：

1. 检查文件头 4 字节是否等于 `kHintMagicV4`。
   - 命中 → 走 `fold_v4` / v4 trailer 校验路径。
   - 不命中 → 走 v2 EOF sentinel + running CRC 路径。

2. v4 文件 < `kHintHeaderV4 + kHintTrailerV4`（即 12B）→ 视为未封口，返
   回 `false`（不健康的 hint）。
3. v2 文件 < `kHintRecordSize`（18B）→ 同上视为不健康。

写端：writer 恒写 v4——`HintFile::open(Mode::kCreate)` 在缓冲里
种入 `kHintMagicV4`（`HintFile::open` 实现），`write()` 经
`codec::encode_hint_record_v4` 变长编码，`finalize()` 落 8B trailer。
`kAppend` 模式不重写头部（生产零调用；既有文件追加不维护 CRC 连续性）。

读端：按文件头 magic 分派。v2/v3（BCH3）属 u32-tstamp 纪元，已被 meta v4
门禁整体拒开（重建），实际不再有读端；v2 分派代码保留为死路径。

兼容策略：u32 纪元旧库须重建；`tools/migrate_le` 从不迁 hint
（迁移输出的 hint 由 migrate 按 v4 重新生成）。

### 6.4 完整性保障：trailer CRC

完整性靠 trailer CRC 一次性兜底（不像 data file 每条 record 自带 CRC）。
失败时 `validate_trailer` 返回 `false`，caller 退 `fold(data)` 重建——
hint 是派生索引，丢失不影响正确性。

CRC 范围：

- v2：覆盖「EOF sentinel 之前的全部字节」；sentinel 的 `TotalSz` 字段实际
  放的是整文件 running CRC，由 `encode_hint_eof` 写入（复用
  `encode_hint_record` 但 `Tstamp=0, KeySz=0, Offset=kMaxOffsetV2`）。
- v4：覆盖 `[0, size-8)`，即文件头 + 所有记录字节；trailer 自身 8B 不在
  CRC 覆盖内。

HintFile 维护 `running_crc_` 状态字段：

- 每次 `write` 把刚编码的字节段 `crc32_update` 累加进 `running_crc_`。
- `finalize` 把 running CRC + magic 写入 trailer。
- `validate_trailer` 从盘尾 8B 取出 expected CRC，再流式从头累加整文件
  字节比对。

### 6.5 EOF sentinel（v2 独有）

v2 末尾复用 hint record 格式表达 sentinel，依赖以下三个字段同时等于特定
值识别：

```
Tstamp   = 0
KeySz    = 0
Offset   = kMaxOffsetV2
TotalSz  = running_crc  （借用字段放整文件 CRC）
Tomb     = false
Key      = 空
```

读取方调用 `codec::is_hint_eof(HintRecord)` 判定
（`HintRecord.tstamp == 0 && key.empty() && offset == format::kMaxOffsetV2`）。
EOF sentinel 不作为正常 hint entry 回调给 `fold(FoldFn)`——`HintFile::fold`
遇到 sentinel 就 break。

v4 没有 EOF sentinel——trailer magic 充当文件结构定界符。

## 七、墓碑（Tombstones）

删除由 `RecordType::kTombstone` record 表达，墓碑本身的 value 段由
`CaskOptions::tombstone_version` 控制。

### 7.1 v0：空 value

`CaskOptions::tombstone_version == 0`（默认）：写一条 `RecordType::kTombstone`
record，`ValueSz == 0`，整条 record 仅 `kHeaderSize + KeySz` 字节。

这是最简洁的墓碑表达——所有语义都靠 `RecordType` 字段承担，不需要任何额外
magic 字符串。读端看见 `RecordType::kTombstone` 即按墓碑处理。

### 7.2 v2：4 字节 shadow file_id

`CaskOptions::tombstone_version == 2`：在墓碑 record 的 Value 段追加 4 字节
小端 shadow file_id（来源 `cask.cpp::remove`）：

```
目的：告诉 merger「我是因为 file_id=N 的那条 entry 才存在；如果那条 entry
     已经不存在（被其他 tombstone 移除），我这条墓碑也失去意义」。
回退：若 key 不在 keydir，或当前 entry 的 file_id == 0，则降级为 v0（空 value）。
```

实现细节（`cask.cpp::remove`）：用 `tombstone_version == 2` 时，从 keydir
取出当前 entry 的 `file_id`，按 LE 字节序写入 4 字节 buffer，作为墓碑 record
的 Value 段。若取不到或 file_id == 0，回退为 v0。

读时三种格式（v0/v1/v2）都接受（v1 是 legacy 中间态，cask 不写但识别）。
merge 时把 shadow file_id 当 hint 信息用于决定「这条墓碑能否随原 entry 一同
回收」。

### 7.3 hint 侧墓碑标记

hint 文件独立标记墓碑（与 data file 的 v0/v2 编码正交）：

- v2 hint（`kHintRecordSize = 18B`）：墓碑标志压到 `Packed` 字段最高位
  `kTombMaskV2`。读端 `packed & kTombMaskV2` 得 tomb。
- v3 hint：墓碑标志嵌在每条 record 的 `kt` vbyte 最低位
  （`(KeySz << 1) | (tomb ? 1 : 0)`）。读端 `kt & 1` 得 tomb。

`fold(hint)` 重建 keydir 时，墓碑记录触发 `keydir_->remove(key, tstamp)` 而
非 `put`。

### 7.4 keydir 内存墓哨

keydir 内部另外维护两类内存墓碑（与磁盘 record 无关，见头
`bitcask/keydir.hpp` 及 `src/keydir/keydir.cpp`）：

- `sibling tombstone`：fold 期间删除已存在 key 时，往 sibling 链头部插入
  的墓哨 revision。三个 sentinel 字段同时取 `kMaxFileId / kMaxSize /
  kMaxOffset` —— `make_sibling_tombstone` / `is_sibling_tombstone` 检测。
- `pending tombstone`：写入 `pending_` map 的墓碑标记。仅以
  `offset == kMaxOffset` 一个字段判别（`is_pending_tombstone`）。

两类墓碑都是为了把「fold 期间临时 key」与「持久墓碑 record」在 keydir 内
部统一表达，避免 fold 期间 keydir 半边写坏。`CaskIter::next(include_tombstones
= true)` 时两类墓碑都作为带 `is_tombstone = true` 的 `EntryProxy` 返回。

## 八、Schema 注册表（field.schema）

字段名 → field id 的 append-only 注册表。设计动机见头
`bitcask/field_schema.hpp` 顶部注释：「字段名重复百万次 → 改为存 field id」。
本节描述文件字节布局。

### 8.1 文件头

```
偏移 字节  字段     编码       含义
 0   4    magic    u32 LE     "FSCH" 的小端（kMagic）
 4   4    version  u32 LE     = 1（kVersion）
─────────────────────────────────────────
     8 字节合计（kHeaderSize）
```

字段定义在 `FieldSchema::kMagic` / `kVersion` / `kHeaderSize`。新格式（带
header + CRC）是 LE-only flag-day 后的标准。

### 8.2 entry 编码

每条 entry 按出现顺序分配递增 id（0 基），编码如下：

```
偏移       字段      编码     含义
 0         NameLen   u16 LE   字段名字节数
 2         Name      [NameLen] 字段名 UTF-8
 2+NameLen CRC32     u32 LE   覆盖 [NameLen | Name] 整段
```

CRC 覆盖 `[NameLen | Name]`，单缓冲一次 fwrite 落盘（见
`FieldSchema::encode_entry_`）。

### 8.3 torn tail 与升级路径

append-only 文件在崩溃下尾部可能半条 entry，open 时的容错策略：

- **clean EOF / 短读**：返回 `true`，等价于「未持久化的 entry 等价于从未写
  入」。
- **完整 entry + CRC 错**：返回 `false`（真损坏 → caller fail-fast）。
- **未知 version**：返回 `false`。

legacy 兼容：

- 旧库的 `field.schema` 是「无 header 的 `[len][name]`」格式（flag-day 后
  小端）。open 时 peek 前 4 字节：== `kMagic` 走新格式（校验 CRC）；否则按
  legacy 无头照读，并在可写目录下**原子升级**为新格式（temp + fsync +
  rename，权威数据零丢失窗口）。
- 升级失败（如只读目录）则退回按 legacy 格式继续追加，保持该文件自洽。

`FieldSchema::open` 返回 `true` = 可用（含 legacy 兼容读、只读目录无法升级
等软路径）；`false` = 硬失败（magic 有但 version 未知 / 某条 entry CRC 不
符）→ caller 应中止 open。

## 九、组件 Checkpoint（search.ckpt 文件族）

`SearchCheckpoint` 是 per-component 文件的统一容器（base + .prev + .d<seq>
链），设计见 `doc/recovery-unified-checkpoint-design-zh.md` 与头
`bitcask/search_checkpoint.hpp`。

### 9.1 容器布局（base / .prev）

```
头部（kHeaderLen = 16）：
  0..3   Magic       4 字节 ASCII "BCSC"
  4..7   Version     u32 LE  = 1（kCkptVersion）或 2（kCkptVersion2）
  8..15  Watermark   u64 LE  覆盖 next_ord 上界

段载荷区：
  按写入顺序拼接各段 payload（位置/校验由页脚给出）

页脚（kTrailerLen = 12）：
  [-12..-8]  Directory  dirLen 字节（见 §9.2）
  [-8..-4]   FooterCRC  u32 LE  覆盖整段 Directory
  [-4..0]    Trailer    4 字节 ASCII "BCSC"
```

文件总字节数 = `kHeaderLen + sum(payload) + dirLen + kTrailerLen`。

`*.prev` 与 base 同结构，存放上一次成功写入的 base——本次 base 校验失败时
的回退目标。

### 9.2 页脚目录项

`Directory` 字段布局：

```
[cnt : u32 LE]   段数
[cnt 个 entry，每项 24 字节]：
    type    : u16 LE   CkptSectionType 枚举值
    flags   : u16 LE   段属性位（保留，当前 0）
    off     : u64 LE   段 payload 在文件内的起始字节偏移
    len     : u64 LE   段 payload 字节数
    crc     : u32 LE   段 payload 的 zlib CRC32
```

`dirLen = 4 + cnt × 24`。

### 9.3 段类型（CkptSectionType）

枚举定义在 `bitcask/search_checkpoint.hpp`，与姊妹引擎 cellar 对齐：

| 枚举 | u16 值 | 含义 |
|------|--------|------|
| `kDocmap` | 1 | ord→key/loc/live/doc_len 加速缓存 |
| `kBm25Default` | 2 | 倒排默认字段 |
| `kBm25Fields` | 3 | 倒排多字段 |
| `kHnsw` | 4 | HNSW 段（含 V7 header，vec payload 外置） |
| `kMeta` | 5 | 可选加速缓存 |
| `kTerms` | 6 | 可选加速缓存 |
| `kBm25DefaultDelta` | 7 | 倒排默认字段 delta |
| `kBm25FieldsDelta` | 8 | u32 count + 每字段 `[u16 nameLen|name|u64 len|delta]` |
| `kDeltaInfo` | 9 | base_gen u64 + prev_wm u64 + seq u32（链校验） |
| `kDocmapDelta` | 10 | 窗口 live 行 + 删除日志（按 ord 交错重放，v1 定宽） |
| `kHnswDelta` | 11 | 插入日志：count u64 + 每条 `ord u64 | f32[dim]` |
| `kKeydirDelta` | 12 | keydir 元数据（"BKMD"：水位/标量/fstats） |
| `kDocmapDeltaV2` | 13 | gap+vbyte 行编码的 docmap delta（v2 行编码） |

`kDocmapDeltaV2` 所在文件以 `kCkptVersion2` 写出——旧读端（只认 version 1）
整文件拒收 → 链断 → 退 fold；这是「降级安全」的设计选择。

### 9.4 写入流程（tmp + fdatasync + rename）

`SearchCheckpoint::write` 的步骤：

1. **拼 buffer**：写 header（magic + version + watermark）→ 各段 payload 顺
   序追加 → 拼 Directory（cnt + 各项）→ 算 footer_crc → 写 footer_crc +
   dirLen + trailer。
2. **tmp 文件**：写到 `<path>.tmp`，调 `fflush` + `fdatasync(fd)`。
3. **rename**：`rename(tmp, path)` 原子覆盖。
4. **失败回滚**：`std::remove(tmp)`。

fdatasync 是关键屏障：保证「组件数据先于 manifest 落盘」是契约而非运气——断
电后 manifest 已提交但组件页丢失 → CRC 坏 → 整组件退全量 fold。S21-2 A4
对齐 manifest 的相同模式。

### 9.5 读取与段级 CRC

`SearchCheckpoint::read` 流程：

1. 整文件读入内存（结构损坏风险下需全量算 footer CRC）。
2. 校验头部 magic 与 version（`kCkptVersion` 或 `kCkptVersion2`）。
3. 从尾倒走 4 字节 → trailer magic；前 4 字节 → dirLen；前 4 字节 →
   footer_crc。
4. 校验 `dirLen + kHeaderLen + kTrailerLen ≤ n`（结构越界 → nullopt）。
5. 算 Directory 的 CRC32，与 footer_crc 比对。
6. 遍历目录项 → 读 payload → 算段级 CRC；逐项把 `crc_ok` 标记写入
   `LoadedSection`。
7. 返回 `LoadedCheckpoint{watermark, sections}`。

段级 CRC 失败的段会被标记 `crc_ok = false`，由 caller 决定重试/丢弃。

### 9.6 链重放（walk_chain）

`bitcask/ckpt_chain.hpp::walk_chain` 是统一的链走读骨架：

```cpp
template <typename Apply>
ChainWalk walk_chain(const std::string& base_path,
                     std::uint64_t base_gen,
                     std::uint64_t base_coverage,
                     std::uint32_t chain_seq,
                     bool unbounded, Apply&& apply);
```

行为：

- 从 `<base>.d1` 开始逐个递增 `seq`，到 `chain_seq`（有界）或首缺文件
  （无界）。
- 每个 `.d<seq>` 调 `SearchCheckpoint::read`；校验 `kDeltaInfo` 段三元组
  `base_gen / prev_wm / seq` 必须与 `base_gen` / 当前 coverage / 当前 seq
  一致。
- 通过则调 caller 的 `apply(LoadedCheckpoint)`；失败（read 坏 / 三元组错 /
  apply 假）→ 断链返回 `ok=false`。

`unbounded == true` 时缺文件 = 正常链尾（legacy / shim 无 manifest 链长提示
时用）；`false` 时缺文件 = 链断（manifest 提示链长可信时用）。

### 9.7 base 落成与链坍缩

`save_docmap_base`（典型实例）的步骤：

1. `rename(base, base.prev)`（若 base 存在）。
2. 写新 base 到 `<base>`（含 `kDocmap` 段）。
3. `remove_chain_files(base)`（连续 miss 8 个序号即停）。
4. `Index::begin_delta_window(watermark)` + `clear_removals()` +
   `clear_dirty()` 收尾。

`remove_chain_files` 的「8 个 miss 即停」容许：链恒连续 `1..N`，8 空洞
orphan 扫尾足够（避免无界扫盘）。

## 十、DocMap 组件持久化

DocMap 即 `index::Index` 的活文档身份表。本节对应头
`bitcask/docmap_ckpt.hpp` 与 `src/keydir/docmap_ckpt.cpp`。

### 10.1 docmap base（kDocmap 段）

`save_docmap_base` 把 `Index::serialize_docmap` 产出的字节直接作为
`CkptSectionType::kDocmap` 段的 payload 嵌入 base 文件。`Index::serialize_docmap`
的字节布局（见 `src/keydir/index.cpp`）：

```
头部 28 字节：
  0..3   Magic       u32 LE     "BCIS" (kSidecarMagic)
  4..7   Version     u32 LE     = 3（kSidecarVersion；v3 = tstamp 定宽 8B）
  8..15  covers      u64 LE     覆盖 next_ord 水位
 16..23  rows        u64 LE     行数（后置回填）
 24..27  CRC32       u32 LE     覆盖 [8, end-4)（含 covers + 行数 + 行 bytes）

rows × 行编码（gap+vbyte；v3 起 tstamp 定宽 8B）：
  ord_gap   : VByte   ord - prev_ord（prev 累积；首条 prev=0）
  klen      : VByte   ext 字节数（≤ 0xFFFF）
  ext       : [klen]  ext 字节
  fid       : VByte   file_id
  off       : VByte   offset
  tsz       : VByte   total_sz
  tstamp    : u64 LE  定宽（v3 起 8B；时间戳 vbyte 反而更大）
  doc_len   : VByte   doc_len
```

固定 `kSidecarMagic` / `kSidecarVersion = 3` 是 namespace 内的匿名空间常量。
v1/v2 属 u32-tstamp 纪元，读端拒收（退 fold 重建）。

### 10.2 docmap delta（kDocmapDeltaV3 段）

`save_docmap_delta` 把窗口 `[from, watermark)` 内的 live 行 + 删除日志作为
`CkptSectionType::kDocmapDeltaV3` 段写入 `<base>.d<seq>`：

```
[kDeltaInfo 段]
  base_gen  : u64 LE  base 世代（= base.watermark）
  from      : u64 LE  窗口起点（= prev base/delta 的 watermark）
  seq       : u32 LE  本次 delta 序号

[kDocmapDeltaV3 段]
  顶部 VByte：
    rn     : VByte   行数
    mn     : VByte   删除日志条数（紧接 rows 之后）
  rows × 行编码（gap+vbyte，与 base 同 schema）：
    ord_gap   : VByte
    klen      : VByte   ≤ 0xFFFF
    ext       : [klen]
    fid       : VByte
    off       : VByte
    tsz       : VByte
    tstamp    : u64 LE  （V3 起定宽 8B）
    doc_len   : VByte
  removals × （同样 gap+vbyte）：
    tomb_gap  : VByte   tomb - prev_tomb
    klen      : VByte   ≤ 0xFFFF
    key       : [klen]

[kKeydirDelta 段]（可选）
  keydir 半边元数据；由宿主透传（see `DocmapReplayHook`）。
```

gap 用 u64 二补数回绕（prev + (v - prev) ≡ v），正确性不依赖升序——乱序
只损压缩率不损数据。

行/删除按 ord 升序交错重放：删后重写场景删除必须先于同 key 新行
（`apply_docmap_delta_section_v2` 实现）。

文件版本：`kCkptVersion3`（kDocmapDeltaV3：tstamp 定宽 8B）——旧读端整文件拒收 → 链断退 fold（降级安全）。

### 10.3 收尾：begin_delta_window / clear_dirty

写入成功后立即：

1. `Index::begin_delta_window(watermark)`：把 delta 窗口水位推进到本文件覆盖
   上界。后续 `remove()` 仅当 `tomb_ord >= delta_window_wm_` 才入 `removals_`
   日志，避免跨文件 stale removal 重放误杀复活文档。
2. `Index::clear_removals()`：清掉 `removals_` 已序列化条目。
3. `Index::clear_dirty()`：清自记账 dirty 位。

链重放成功后同样调这三步——链上某次失败的污染由载入方收尾清除。

## 十一、DocValue meta 段（结构化 KV）

DocValue v4 的 meta 段由 `meta_codec.hpp` 进一步编码为「按 key 升序排列的
KV 列表」，让 HNSW 过滤可二分跳读（设计见头注释）。这是本项目自有的结构化
KV 二进制格式（无公开规范）。

### 11.1 整体布局

```
[Ver : u8 = 1（kMetaFormatVersion）]
[NumEntries : VByte]
entries × NumEntries（按 key 字典序升序，无重复）：
    KeyLen    : VByte
    Key       : [KeyLen]    UTF-8 字节
    ValueType : u8          MetaType 枚举
    ValueData : 视 ValueType 而定
```

### 11.2 顶层：Ver + NumEntries

- `Ver = kMetaFormatVersion = 1`（`namespace bitcask::meta`）。
- `NumEntries` 是 VByte 变长整数。
- `encode_meta` 内 `assert` 兜底：entries 必须按 key 升序、无重复——违反
  是 caller bug。读端也假设这两条不变式，把 meta_lookup 当二分前提。

### 11.3 单条 entry 编码

每条 entry 头部：

```
KeyLen : VByte   key 字节数
Key    : [KeyLen] UTF-8（按字典序与上一条比较）
```

随后紧接值部分。

### 11.4 值类型 tag

`namespace bitcask::meta::MetaType` 枚举：

| 枚举 | u8 值 | 定长? | ValueData 布局 |
|------|-------|-------|---------------|
| `Null` | 0 | — | （无字段） |
| `Bool` | 1 | 是 | 1 字节：0 = false，1 = true |
| `Int64` | 2 | 是 | 8 字节小端 int64 |
| `Float64` | 3 | 是 | 8 字节小端 IEEE 754 double |
| `String` | 4 | 否 | `[Len : VByte] + [Len 字节]` |

定长类型（Bool / Int64 / Float64）的 length 直接由 tag 推断；String 需要
额外的 VByte 长度前缀。未知 tag → 读端返回错误（`"meta: unknown value type
tag"`）。

### 11.5 hot path 跳读：meta_lookup

HNSW 每访问一个节点过滤时调用一次（热路径）——`meta_lookup` 不全量 decode，
而是一次性线性扫建立「entry 起始 offset 表」+ 在 offset 表上二分定位 key：

```
对调用而言：O(n) 预扫 + O(log n) 二分
对全量 decode 相比：省去每条 string 的堆内存分配
```

未找到返回 `std::monostate`（与「无 meta」等价，filter 直接判 false 跳过）。

调用契约：

- 输入 `blob` 是 DocValue meta 段（即 `DocValueView::meta`）。
- 返回值是 `MetaValue` variant —— caller 按 `index()` 判定类型后 `std::get`。
- 是纯函数、只读 blob、线程安全。

## 十二、VByte 变长整数

文本与 field schema 注册表里的「长度前缀」「序号」字段统一使用 VByte（每字
节低 7 位数据，最高位 1 = 终止字节）。VByte 的全常量与边角行为见
`include/bitcask/vbyte.hpp`。

编码约定（与 codec.cpp 同算法）：

- 每字节 7 位数据 + 1 位 `more` 标志（最高位 1 = 终止字节）。
- 写端：值 < 128 直接 1 字节（高位 1）；≥ 128 输出低 7 位 + more=0，再
  继续。
- 读端：累计 `shift = 0..63`，shift ≥ 64 视为非法编码（防 u64 溢出）。

使用 VByte 的字段：

| 字段 | 字节预算 |
|------|---------|
| DocValue 段长度（text/meta/fields） | VByte |
| DocValue vector Dim | VByte（元素数） |
| DocValue fields FieldCount / FieldId / ValLen | VByte |
| hint v4 record 头（gap/total_sz/kt） | VByte |
| docmap sidecar 行（gap/klen/fid/off/tsz/doc_len） | VByte |
| `meta_codec.hpp` KeyLen / ValueLen（String） | VByte |

## 十三、文件锁

### 13.1 bitcask.write.lock

`Cask::open(opts_.read_write == true)` 路径创建。文件内容：写入者 PID（ASCII
十进制 + '\n'）。`lock::FileLock` 提供进程间互斥（fcntl F_SETLK），位于头
`bitcask/file_lock.hpp`。

语义：

- 持锁者写入 PID；释放即 fclose。
- stale 检测：若锁文件存在但持锁 PID 已死（kill(pid, 0) == ESRCH），caller
  可回收并重抢（实现见 `acquire_writer_lock`）。
- 多进程不允许同持此锁；merger 不抢（由 §13.2 独立）。

### 13.2 bitcask.merge.lock

`Cask::open(opts_.merge_only == true)` 路径创建。语义与 `bitcask.write.lock`
对偶：只允许一个 merger 实例持有；可与 writer 并发。

文件内容同样是 PID 行；stale 检测同样由 `FileLock` 提供。

持锁者由 `tools/migrate_le` 与 `Cask` 的 merge 线程创建；冲突时
`acquire_merge_lock` 返回错误 → caller 退避重试或报错。

## 十四、LE-only flag-day

格式变更历史中的一道分水岭：「Erlang bitcask 大端纪元 → 本项目小端 flag-day」。
本节简述读端语义与回退策略。

### 14.1 meta v1 的处理

`bitcask.meta` 的 Ver=1 是大端 legacy 纪元。`read_meta` 显式拒绝并返回
"incompatible legacy big-endian format (meta v1); little-endian flag-day
requires rebuild — re-ingest data"——绝不静默把大端字节按小端读坏。

其它文件族（`*.bitcask.data` / `*.bitcask.hint` / `field.schema`）本身不带
版本号，但 magic / header 解析路径只接受小端字节序；BE 字节下 magic 立刻
不匹配，触发 fail-fast。

### 14.2 跨目录迁移

`tools/bitcask_migrate` 是统一的离线目录级迁移入口（子命令式）：

```
bitcask_migrate detect   <dir>          # 读 meta 版本报告纪元 + 提示下一步
bitcask_migrate be2le    <src> <dst>    # v1 大端（meta v1）→ 当前纪元
bitcask_migrate tstamp64 <src> <dst>    # u32 纪元（meta v2/v3）→ 当前纪元
```

- `be2le`：解大端 23B record 头 → 当前 codec 重编码；meta 1→4、
  field.schema NameLen 大端→小端 + 补 CRC。
- `tstamp64`：解小端 23B record 头 → 27B 重编码（tstamp u32→u64 零扩展）；
  kDoc 的 DocValue v3→v4 转码（Ver 字节 + 尾部 expiry 段 u32→u64）；
  meta 2/3→4；field.schema 格式未变,原样拷贝。
- 两者都从迁移后的 data 重生成 hint（v4）。

均为非破坏性（只读 src、只写 dst）。迁移仅动 meta / data / hint /
field.schema；ckpt / seg / wal 等可重建文件由新库首开走 fold 重建。
详见 `include/bitcask/migrate.hpp`。（`tools/migrate_le` 是 be2le 的
保留旧入口。）

## 附录 A：常量速查

按头文件分组的常量索引：

| 头 | 命名空间 | 关键常量 |
|----|---------|---------|
| `format.hpp` | `bitcask::format` | `kHeaderSize`、`kCrcOffset`、`kTypeOffset`、`kTstampOffset`、`kOrdOffset`、`kKeySzOffset`、`kValueSzOffset`、`RecordType::*`、`kMaxKeySize`、`kMaxValueSize`、`kHintRecordSize`、`kHintHeaderV4`、`kHintTrailerV4`、`kHintMagicV4`、`kHintTrailerMagicV4`、`kMaxOffsetV2`、`kTombMaskV2`、`kDocValueVersion`、`kDocValueHeaderSize`、`kFlagHasVector`、`kFlagHasText`、`kFlagHasMeta`、`kFlagVecQuantized`、`kFlagHasFields`、`kFlagHasExpiry`、`kQuantizedVersion`、`kChunkSize`、`kMinChunkSize`、`kMaxChunkSize` |
| `meta_file.hpp` / `meta_file.cpp` | `bitcask::meta` | `Mode::*`、`VectorMetric::*`、`MetaConfig::*`、`kMetaMagicSize`、`kMetaMagic`、`kMetaVersionOffset`、`kMetaModeOffset`、`kMetaReservedSize`、`kMetaVecMetricOffset`、`kMetaVecDimOffset`、`kMetaVecQuantOffset`、`kMetaVecInmemInt8Offset`、`kMetaCrcOffset`、`kMetaCrcCoverLen`、`kMetaVersion`、`kMetaFileSize` |
| `field_schema.hpp` | `bitcask` | `FieldSchema::kMagic`、`kVersion`、`kHeaderSize` |
| `search_checkpoint.hpp` | `bitcask::search` | `CkptSectionType::*`、`kCkptMagic`、`kCkptVersion`、`kCkptVersion2`、`kHeaderLen`、`kTrailerLen` |
| `ckpt_chain.hpp` | `bitcask::search` | `ChainWalk` |
| `component_ckpt.hpp` | `bitcask::ckpt` | `ChainState`、`DeltaSaveResult`、`LoadResult` |
| `meta_codec.hpp` | `bitcask::meta` | `MetaType::*`、`kMetaFormatVersion` |
| `index.hpp` | `bitcask::index` | `DocLoc`、`DocSlot`、`DocHit`、`IndexInfo`、`kChunkOrds` |
| `keydir.hpp` | `bitcask::keydir` | `kMaxFileId`、`kMaxSize`、`kMaxOffset`（sentinel 三联） |

## 附录 B：完整文件清单

### 数据 / 元数据（不可丢）

| 文件 | 字节结构来源 |
|------|-------------|
| `bitcask.meta` | §三 |
| `<tstamp>.bitcask.data` | §四 |
| `bitcask.write.lock` | §13.1 |
| `bitcask.merge.lock` | §13.2 |
| `field.schema` | §八 |

### 派生缓存（可 fold 重建）

| 文件 | 字节结构来源 |
|------|-------------|
| `<tstamp>.bitcask.hint` | §六 |
| `bm25.ckpt` / `.prev` / `.d<seq>` | §九 |
| `vec.ckpt` / `.prev` / `.d<seq>` | §九 |
| `docmap.ckpt` / `.prev` / `.d<seq>` | §九 + §十 |
| `search.ckpt` / `.prev` | §九（legacy） |
| `search.vec` / `search.qc8` | HNSW payload 容器（mmap） |
| `kv.keydir.ckpt` | keydir 快照（BCKS v3：tstamp 定宽 8B） |
| `index.manifest` | 三组件 ckpt 的统一提交点 |

### DocValue 嵌入字段

| 字段 | 字节结构来源 |
|------|-------------|
| DocValue v4（kDoc record 的 Value 段） | §五 |
| DocValue meta 段 | §十一 |