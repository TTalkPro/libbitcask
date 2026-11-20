# meta 多值（字符串数组）设计

状态：**方案 A 已实现（S62，2026-11-20，未提交，版本未定）**。第 7 节待定项：用户定方案 A，其余按推荐默认
（只做字符串数组；既有操作符碰数组按类型不匹配；`bitcask_update_doc_ex` 同批；下游 Erlang 不在本项）。实现与设计的出入：
过滤求值顺带改成在 blob 上原地比较（不物化 `MetaValue`），以抵消新类型分支带来的热路径开销（见 TASK.md S62 落地记录）。
来源：下游反馈 [`feedbacks/2026-11-13-doc-capability-gaps.md`](../feedbacks/2026-11-13-doc-capability-gaps.md) 第 4 条（锦书 §Q-21）。
关联：`include/bitcask/meta_codec.hpp`（meta 二进制格式）、`include/bitcask/meta_filter.hpp`（过滤求值）、
`include/bitcask/meta_file.hpp` + `doc/atomic-batch-design-zh.md` §2（`bitcask.meta` 纪元与 v6 懒升级先例）、
`c_api/bitcask_kv.h`（C 侧 meta 编解码 / 过滤树）。

## 1. 结论（先说推荐）

- **推荐方案 A：meta 新增一种值类型「字符串数组」**（类型标记 5），过滤树新增 `Contains` / `ContainsAny` 两个操作符。
  兼容靠 **`bitcask.meta` 纪元懒升级到 v7**：目录里第一次写入数组值之前把 meta 改成 v7，旧二进制干净拒开（报
  `unsupported meta version`），**不会**悄悄读错。从不写数组值的目录永远停在 v5 / v6，对存量用户零影响。
- 代价：C++ `meta::MetaValue`（`std::variant`）要加一种类型，是 **C++ ABI 破坏，升 major（12.0.0）**；C API 全部做成纯新增
  （新枚举值、新结构体、新函数），C 侧 ABI 兼容。
- 备选方案 B（不改格式，把数组展开成多个标量键）放在第 6 节对比，不推荐：语义漏到所有读 meta 的地方，下游各自解码时还得知道这套约定。
- 范围：**只做字符串数组**（锦书的需求是别名 / 标签）。数值数组、嵌套结构不做；类型标记 6 以后留给将来。

## 2. 需求与现状

### 2.1 下游要什么

锦书的设定卡：「别名」天然多值（青龙 / 孟章 / 东方之神）。今天两种绕法都不行：

| 绕法 | 问题 |
|---|---|
| 拼成一个字符串存 meta（`"青龙|孟章"`） | 过滤只有 `Eq` / `In` 整值比较，只能下游取回再子串匹配；「龙」会误中「龙骨」 |
| 放进命名字段 | 能全文检索，但不能进 meta 过滤树做精确布尔过滤，也不能和其它 meta 条件 AND/OR 组合 |

要的是：`alias = ["青龙", "孟章"]` 存进 meta，过滤写「alias 包含 孟章」，能与其它条件组合，用在 `search_text` / 向量检索 /
迭代器 / range 的 filter 上。

### 2.2 meta 今天怎么存、怎么读

- 格式（`meta_codec.hpp`）：`[Ver:u8=1][N:varint]`，之后 N 条 `[KeyLen][Key][Type:u8][Value]`，**按 key 升序、无重复**。
  Type：0 null / 1 bool / 2 int64 / 3 float64 / 4 string。
- 读 meta 的地方（全部要跟着认新类型）：
  - `meta_lookup`（过滤热路径：HNSW 遍历期过滤每个节点一次、文本检索后过滤逐篇、迭代器 / range 的 filter）；
  - `decode_meta`（S61 `update_doc` 合并、C `bitcask_meta_iter_*`、各下游 NIF 的「meta → 宿主 map」）；
  - docmap 把 meta blob **原样**存进内存与 `docmap.ckpt`，过滤在 docmap 锁内对 blob 求值（`Index::eval_meta` / `fill_eval_meta`）。
- **没有 meta 二级索引**（`doc/roaring-meta-bitmap-design-zh.md` 只是预研、未立项）——过滤是逐篇对 blob 求值，所以加数组类型
  不涉及任何索引结构。

### 2.3 为什么不能「直接加个类型标记」就完事——旧读端会悄悄读错

旧版本遇到不认识的类型标记：

- `decode_meta` 返回错误（下游 NIF 解码这篇文档的 meta 会 `badarg`，至少是显式的）；
- **`meta_lookup` 把游标直接推到 blob 末尾**（`default: cur = blob.size()`）——查的 key 若排在数组 key **之后**，一律当「不存在」：
  `Eq` 变 false、`Neq` 变 true、`Exists` 变 false。**过滤结果悄悄变错，不报错。**

只要旧二进制还能打开含数组值的目录，这个静默错误就成立。所以兼容方案的核心不是「怎么编码」，而是「怎么让旧二进制打不开」。

## 3. 方案 A：字符串数组类型

### 3.1 盘上格式

meta 段格式版本**不变**（`Ver` 仍是 1）；新增类型标记：

```text
Type = 5  (StringArray)
Value     = [Count:varint] { [Len:varint][Bytes: UTF-8] } × Count
```

- 元素是 UTF-8 字符串；**保持写入顺序，不去重**（读回与写入逐个相同；`Contains` 是集合语义，重复无害）。
- 允许空数组（`Count = 0`）：`Exists` 为真、`Contains` 为假。
- 不另设元素个数上限：受整条记录大小上限（`kMaxValueSize`）约束，与 string 一致。
- key 排序与去重规则不变（数组就是一条 entry 的值）。

为什么不升 `Ver`：`Ver` 一变，旧读端对**所有**文档的 meta 都拒解，连不含数组的文档也读不了；而有了 3.2 的目录级门禁，
旧读端根本打不开含数组的目录，`Ver` 再变没有额外收益。

### 3.2 兼容门禁：`bitcask.meta` 懒升 v7

照搬原子批的 v6 先例（`doc/atomic-batch-design-zh.md` §2，`cask.cpp` 「meta 懒升 v6」处）：

- 新增 `kMetaVersionMultiValue = 7`。语义：**目录可能含字符串数组 meta ⟺ meta ≥ v7**。纪元单调：v7 蕴含 v6 的全部含义
  （可能含原子批记录）；v5 目录第一次写数组直接升到 7。
- 升级时机：任何**写入路径**在第一条含数组值的记录进入写缓冲**之前**，把 meta 原子重写为 v7（tmp + rename + fsync 目录；
  中途崩溃 meta 要么旧要么新，v7 下没有数组记录也合法）。覆盖的写入口：`put_doc`、`put_batch_atomic`（含 `kPutDoc`）、
  `TxnCask`、`update_doc`（S61）、C `bitcask_put_doc_ex` / 条件批 / 事务。
- 判定「这条 meta 含数组」：目录已是 v7 则不判（零开销）；否则对 meta blob 扫一遍类型标记（只扫 tag、跳过值，O(entry 数)，
  与 `meta_lookup` 同一套跳读）。**必须扫 blob 而不是信调用方**：C 调用方可以直接传自己编码的 blob。
- 读端：本库接受 5 / 6 / 7；旧二进制遇 7 走既有 `unsupported meta version` 干净拒开（`MetaError::Kind::kUnsupported`，
  C 侧 `bitcask_meta_read` 报 `BITCASK_ERR_UNSUPPORTED_FORMAT`）。
- **不提供自动降级**：一旦 v7，旧版本就打不开这个目录。要回退只能新版本导出重写（写进升级说明）。merge / 重开 / 重建都不会
  把 v7 降回去（`cask_recovery.cpp` 已有「保留纪元」的写法，v6 同款）。

覆盖面复核：docmap.ckpt、OKI run、search ckpt 里出现的 meta 都是同一目录的产物，跟着目录纪元走，不需要各自再加门禁。

### 3.3 过滤语义

新增两个操作符（`MetaOp` 枚举值追加，旧值不动）：

| 操作 | 操作数 | 存储值是字符串数组 | 存储值是 string | 其它 / 不存在 |
|---|---|---|---|---|
| `Contains = 8` | `value`（string） | 任一元素 == value | 等于 value（标量当单元素集合） | false |
| `ContainsAny = 9` | `values`（string 列表） | 任一元素 ∈ values | 值 ∈ values | false |

- 「标量当单元素集合」是有意的：同一个 key 有的文档存单值、有的存数组时，`Contains` 一种写法通吃，下游不用分情况。
- **既有操作符碰到数组：按「类型不匹配」处理**，沿用现有规则——`Eq` / `In` / `Gt…` 为 false，`Neq` 为 true，`Exists` 看 key 是否存在。
  不做「`Eq` 碰数组自动变包含」：那会让同一个 `Eq` 在两种存储上语义不同，过滤树的类型规则变成隐式的。
- 操作数不是 string（如 `Contains` 一个 int64）⇒ false（与现有类型规则一致）。

热路径不退化：`Contains` / `ContainsAny` 不物化数组。内部新增一个零分配的「按 key 找到值的原始区间」查找（`meta_lookup` 的
跳读逻辑抽出来），在区间上逐个比元素；`meta_lookup` 对外照旧返回 owning 的 `MetaValue`（命中数组时才分配）。

### 3.4 C++ 接口（破坏性，12.0.0）

```cpp
enum class MetaType : std::uint8_t { Null = 0, Bool, Int64, Float64, String, StringArray = 5 };
using MetaValue = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                               std::vector<std::string>>;           // 新增第 6 种
enum class MetaOp : std::uint8_t { /* 0..7 不变 */ Contains = 8, ContainsAny = 9 };
```

- `encode_meta` / `decode_meta` / `meta_lookup` / `MetaCondition::evaluate` 跟着认新类型；`detail::compare_values` 当前的
  `else` 分支默认当 string 处理，**必须改成显式分支**（否则数组会被误当字符串比较——这也是下游自己写 visitor 的代码会踩的坑，
  升 major 的主要原因）。
- `DocPatch::set_meta`（S61）天然支持数组（`MetaEntry` 的值就是 `MetaValue`）。
- `MetaValue` 变体数变化 = 布局 / ABI 变化 ⇒ C++ 下游须重新编译，自己 `std::visit` / `index()` 分支的代码要补一个分支。

### 3.5 C API（纯新增，C ABI 兼容）

C 侧的 `bitcask_meta_value_t` 出现在调用方自己的数组里（`bitcask_meta_entry_t[]`、过滤条件的 `values[]`、S61
`bitcask_doc_patch_t::set_meta`），**布局不能动**。所以：

| 用途 | 做法 |
|---|---|
| 枚举 | `BITCASK_META_VALUE_STRING_ARRAY = 5`；`BITCASK_META_OP_CONTAINS = 8`、`BITCASK_META_OP_CONTAINS_ANY = 9` |
| 过滤 | 两个新操作符的操作数都是标量 string，**复用现有** `value` / `values` 字段，过滤树结构不变 |
| 编码 | 新 `bitcask_meta_entry_ex_t { const char* key; bitcask_meta_value_t value; const bitcask_slice_t* items; size_t items_count; }` + `bitcask_meta_encode_ex`（`value.type == STRING_ARRAY` 时读 items） |
| 读取 | `bitcask_meta_value_view_t` 布局不变：数组时 `type = STRING_ARRAY`、`i64 = 元素个数`、`str = 元素区间（借 blob）`；新增 `bitcask_meta_array_next(bitcask_slice_t region, size_t* cursor, bitcask_slice_t* item)` 逐个取元素。`bitcask_meta_lookup` / `bitcask_meta_iter_*` 签名不变 |
| 部分更新 | `bitcask_doc_patch_t` 的 `set_meta` 是 `bitcask_meta_entry_t*`，表达不了数组 ⇒ 新 `bitcask_doc_patch_ex_t`（`set_meta` 换成 `bitcask_meta_entry_ex_t*`）+ `bitcask_update_doc_ex` |

旧 C 调用方：不用新类型就完全不受影响；旧 C 库收到 op 8/9 会在 `parse_meta_filter` 被拒成 `BITCASK_ERR_INVALID_OPTION`（已核对现有
越界检查），不会误判。

### 3.6 下游影响

| 下游 | 要做什么 |
|---|---|
| bitcask（Erlang NIF） | 值为「binary 列表」⇔ 字符串数组（今天单个值给列表会 `badarg`，不冲突；注意整个 meta 本身也可以是 `[{K, V}]` 列表，NIF 要按「容器 / 值」两层区分）；decode 方向数组 → binary 列表；结构化查询 DSL 加 `contains` / `contains_any` |
| clj-bitcask / Keelson（C#） | 走 C API 的改用 `bitcask_meta_encode_ex` / `bitcask_meta_array_next`；若有自写编码器（锦书 K-22 字节级对等）要同步类型 5 的编码 |
| bitcask-viewer | 显示数组值 |
| 所有下游 | 升级说明：目录一旦写入数组值即升 v7，**旧版本从此打不开**；混跑新旧版本的部署要先全部升级再开始写数组 |

## 4. 改动面与工作量（方案 A）

| 块 | 文件 | 量 |
|---|---|---|
| 编解码 + 零分配区间查找 | `meta_codec.hpp` | 中 |
| 过滤（两个操作符 + compare 显式分支） | `meta_filter.hpp` | 小 |
| 纪元门禁 + 写入口判定 | `meta_file.{hpp,cpp}`、`cask.cpp`（put_doc / 原子批 / update_doc）、`txn` 透传 | 中 |
| C API | `bitcask_kv.h/.cpp`、`internal.h`（值转换、filter 解析、view / iter / array_next、encode_ex、patch_ex） | 中 |
| 文档 | `doc/api-c.md`、`doc/api-cpp.md`、`meta_codec.hpp` 头注、CHANGELOG（含升级说明） | 小 |
| 测试 | 见第 5 节 | 中 |

## 5. 测试计划（方案 A）

1. 编解码往返：空数组、单元素、多元素含重复、UTF-8、与其它类型混排；数组 key 前后的标量 key 都能 `meta_lookup` 到（跳读正确）。
2. 过滤：`Contains` / `ContainsAny` 在数组 / 标量 string / 非 string / 不存在四种存储上的真值表；既有操作符碰数组按类型不匹配；
   与 AND / OR 嵌套组合；用在 `search_text` 后过滤、HNSW / DiskANN 遍历期过滤、迭代器与 range filter。
3. 纪元门禁：不写数组的目录停在 v5 / v6；`put_doc` / 原子批 / 事务 / `update_doc` / C 侧传入预编码 blob 各自触发升 v7，
   且升级先于记录落盘（崩溃注入：升级后、写入前崩溃 ⇒ v7 无数组，合法）；merge / 重开 / 重建后仍是 v7。
4. 旧版本实测：用 11.2.0 基线（`git archive` 单独编）打开 v7 目录 ⇒ 干净拒开；打开从未写数组的新目录 ⇒ 正常。
5. 持久化：docmap.ckpt / 重开 / merge 后数组值与过滤结果不变。
6. C API：`encode_ex` / `lookup` / `iter` / `array_next` / 新操作符 / `update_doc_ex`；旧入口行为零变化；Release（NDEBUG）下照常检查。
7. 性能：`BM_*` 过滤相关基准（含 HNSW 遍历期过滤）在无数组的库上不退化（新增分支只在 tag 5 时走）。

## 6. 方案 B（备选）：数组展开成多个标量键

做法：`alias = ["青龙", "孟章"]` 存成两条普通 string entry，key 用保留分隔符展开，例如 `alias\x1F0`、`alias\x1F1`；过滤树加
「按 key 前缀匹配任一条」的操作符；C++ / C 提供「按数组读写」的辅助函数在库内展开 / 折叠。

| | 方案 A（新类型） | 方案 B（展开键） |
|---|---|---|
| 盘上格式 | 新类型标记 5 | 不变 |
| 旧二进制 | 需 v7 门禁（干净拒开） | 能打开；但看到的是一堆 `alias\x1F0` 键，旧过滤树表达不了「包含」 |
| C++ ABI | 破坏（升 12.0.0） | 可做成纯新增（MetaOp 追加值 + 辅助函数） |
| 语义 | 一个 key 一个值，`get` / `decode` 读回就是数组 | 抽象泄漏：原始 `decode_meta` / C `iter` / 各下游自写解码器都会看到展开键，必须人人知道约定；`Exists alias` 为假、`Eq alias` 无意义 |
| 约束 | 无新增 | 用户 key 不得含分隔符（要在编码处拒）；元素序号的字典序（`\x1F10` < `\x1F2`）要么定宽要么读回时数值排序；`update_doc` 改数组要先删光旧展开键 |
| 下游 | 加一种值类型的映射 | 每个下游都要实现展开 / 折叠，K-22 字节对等的两侧要逐字节一致地实现同一约定 |

不推荐 B 的理由：它把「数组」这个概念从格式层推给了所有读 meta 的人，长期成本比一次 major 高；而 A 的兼容风险已经由 v7 门禁
变成「旧版本明确拒开」，不再是静默错误。

## 7. 待定（需要拍板）

1. 方案 A 还是 B（推荐 A）。
2. 只做字符串数组是否够（推荐够；数值数组等真有需求再加类型 6）。
3. 既有操作符碰数组按「类型不匹配」（推荐），还是让 `Eq` / `In` 对数组自动变「包含」。
4. 版本：方案 A 是 12.0.0（C++ ABI 破坏）；是否同一版里把 `bitcask_update_doc_ex` 一起做（推荐一起，否则 C 侧部分更新写不了数组）。
5. 下游 bitcask（Erlang）的映射与 DSL 是否同批跟进。
