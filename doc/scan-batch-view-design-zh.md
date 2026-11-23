# 扫描 / range 批视图接口（免分配）设计

> 来源：下游反馈 [`feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md`](../feedbacks/2026-11-23-keydir-scan-preads-each-value-and-range-prefetch-slower.md)
> 第 3 条（S71-4 ⏸ 遗留）：扫描与 range 每条 entry 两次堆分配，批接口给调用方缓冲 / 视图版可省掉。
> 2026-11-23 下游拍板：**视图归库所有（借用语义，需保留外部复制）**、**新入口不动老 ABI**、**扫描与 range 都做**。

---

## 1. 现状与拷贝点分析

### 1.1 扫描（`CaskIter`，`src/cask/cask_iter.cpp:100-246`）

```
next_merged_key()            OKI 归并游标产出 key（cur_key_，string）
  ↓
read_mmap / pread            mmap 视图（零拷贝）或 owned ReadRecord（一次 pread 拷贝）
  ↓
decode_doc_value(rval)       纯函数，返回 DocValueView——text/meta 是 span，零拷贝
  ↓
Entry.assign                 拷贝 ①：key/value/meta 逐个 assign 进 owned vector
  ↓
fill_iter_entry (C API)      拷贝 ②：vector → malloc，entry_ex_free 释放
```

关键事实：mmap 命中时 value 字节在**文件映射里**，decode 后的 `text`/`meta`
span 也指向映射内——拷贝 ①②都是纯浪费；pread 兜底路径的 ReadRecord 是
owned 的，`GetResultView`（`doc/getresult-view-design-zh.md`）已验证
「持 `storage_` 所有权 + span 借用」可行。key 来自 `cur_key_`（string，
每次推进被覆盖），需要独立存放。

映射生命周期已有保障：`start()` 时 `pin_files()` 钉住全量封口文件
（`shared_ptr<DataFile>`）；不在 pin 表里的文件（并发落段滚出来的新封口文件）
走 `parent_->read_file` 拿到 `shared_df`——**这个必须由当批 holder 接住**，
否则 LRU 逐出后映射可能撤销。

### 1.2 range（`CaskRangeIter`，`src/cask/cask_range_iter.cpp:100-146`）

```
next_merged_key()            归并出 key（string）
  ↓
cask_->get(key)              返回 GetResultView（mmap 锚定 / pread owned，零拷贝）
  ↓
fetch_entry → Entry.assign   拷贝 ①：key/value/meta 逐个拷进 owned Entry
  ↓
fill_range_entry (C API)     拷贝 ②：vector → malloc
```

`GetResultView` 本身就是现成的零拷贝载体（`map_holder_` 锚定映射 /
`storage_` 持 pread 缓冲）——range 视图版只需**把它留在当批 holder 里**，
不必拷。预取路径（`prefetch > 1`）同理：`buf_` 从 owned Entry 换成
`{GetResultView + key 切片}` 结构，按 key 序压实的现有逻辑不变。

---

## 2. C++ API

### 2.1 `CaskIter::next_batch_view`（`include/bitcask/cask.hpp`）

```cpp
struct EntryView {
    std::span<const std::byte> key;
    std::span<const std::byte> value;   // 墓碑恒空
    std::span<const std::byte> meta;    // set_want_meta(true) 时填，否则空
    std::uint64_t tstamp = 0;
    std::uint32_t file_id = 0;
    std::uint64_t offset  = 0;
    std::uint32_t total_sz = 0;
    bool is_tombstone = false;
    std::uint64_t ord = 0;
};

// 返回的 span 指向迭代器内部 view_entries_（容量跨批复用），
// 生命周期见 §4。错误契约：unexpected 时整批不可依赖（§5）。
[[nodiscard]] std::expected<std::span<const EntryView>, CaskFault>
next_batch_view(std::size_t max_n);
```

迭代器新增内部状态（capacity 跨批保留 ⇒ 稳态零分配）：

| 成员 | 用途 | 先例 |
|---|---|---|
| `view_key_buf_` + `view_key_offs_` | 当批 key 扁平缓冲（N+1 哨兵） | S24-M7 `FlatKeys` |
| `view_holders_` | 当批存活担保：`shared_ptr<DataFile>`（mmap 路径 / pin 表外的文件）+ `std::optional<ReadRecord>`（pread 兜底路径）；**span 指向这些存储，不拷贝** | `GetResultView::storage_` / `map_holder_` |
| `view_entries_` | EntryView 数组（返回值指向它） | — |

`want_meta` / `set_filter` 语义不变：filter 在 `dv->meta` 上求值（decode 后、
产出前），不通过的条目不进 `view_entries_`；view 的 meta span 与 value 同源
（同一记录字节），比 `_ex` 版还省一次 malloc。

墓碑：`see_tombstones=true` 时 `is_tombstone=true`、`value` 为空 span
（与现有 `next()` 行为一致——tomb 路径 `e.value.clear()`，cask_iter.cpp:149-152）。

**key 的来源与回填规则（评审补，实施必读）**：

- 读了记录的条目（mmap / pread 两条路径）直接借记录里的 key——`next()` 已做过
  `key_matches` 校验（cask_iter.cpp:214-222），不必再拷。
- 只有 OKI 墓碑行（`row.tomb`，不读记录，cask_iter.cpp:149-153）的 key 落
  `view_key_buf_`。
- ⚠ 扁平缓冲**批内增长会让本批前面已填的 key span 悬垂**（realloc 搬家）。
  规则：批内只记 `(off, len)`，`view_entries_` 的 key span 在**整批填完后
  统一回填**；`view_key_buf_` 只 `clear()` 不 `shrink`。

### 2.2 `CaskRangeIter::next_batch_view`

```cpp
struct EntryView {
    std::span<const std::byte> key;
    std::span<const std::byte> value;
    std::span<const std::byte> meta;
    std::uint64_t tstamp = 0;
    std::uint64_t ord = 0;
};
```

内部状态与扫描侧同构，差异：

- **holder 是 `GetResultView`**（move-only）：`buf_view_` 为
  `struct { GetResultView view; size_t key_off, key_len; }` 的 vector，
  预取路径沿用 `fill_prefetch` 的「归并 N 个 key → 并发 get → 按 key 序
  压实」，只是压实对象从 owned Entry 换成 view 结构。预取并发取值
  （JoiningPool）与自适应串行（S71-3）全部保留——冷缓存下预取仍是
  视图版的主要价值场景。
- key 来自归并行的 string，必须落 `view_key_buf_`（一次小 memcpy，摊还零分配）；
  回填规则同 §2.1（批内只记偏移，批末统一回填 span）。
- filter 不通过的 view 要**当场销毁**，不能留到下一批（否则白持一份记录）。

---

## 3. C API

命名跟 `bitcask_meta_value_view_t` / `GetResultView` 的 `*_view` 先例。
**批接口一个**（单条用 `max_n = 1`），不加单条版——保持接口面最小。

```c
/* 免分配批视图（S75）。key/value/meta 借库内存储，**无 free 函数**——
 * 有效期见下，需保留请外部复制。entries 数组本身归调用方（纯值结构，
 * 可栈分配 / 复用）。 */
typedef struct {
    bitcask_slice_t key;
    bitcask_slice_t value;      /* 墓碑恒空 */
    bitcask_slice_t meta;       /* 沿用迭代器已有的 want_meta 开关 */
    uint64_t tstamp;
    uint32_t file_id;
    uint64_t offset;
    uint32_t total_sz;
    int      is_tombstone;
    uint64_t ord;
} bitcask_iter_entry_view_t;

BITCASK_API int bitcask_iter_next_batch_view(bitcask_iter_t* iter,
    bitcask_iter_entry_view_t* entries, size_t max_n, bitcask_fault_t* fault);

typedef struct {
    bitcask_slice_t key;
    bitcask_slice_t value;
    bitcask_slice_t meta;
    uint64_t tstamp;
    uint64_t ord;
} bitcask_range_entry_view_t;

BITCASK_API int bitcask_range_iter_next_batch_view(bitcask_range_iter_t* iter,
    bitcask_range_entry_view_t* entries, size_t max_n, bitcask_fault_t* fault);
```

返回值契约：

| 返回 | 含义 | entries 状态 |
|---|---|---|
| `> 0` | 本批条数 | `entries[0..count-1]` 有效，直到同一迭代器的下一次推进调用 |
| `0` | 迭代结束（EOI） | 内容不可依赖 |
| `-1` | 故障（`*fault` 已填） | **整批不可依赖**（与 `_ex` 的「已填条目仍有效」不同，见 §5） |

---

## 4. 生命周期与失效规则（总表）

| 事件 | 上一批视图 |
|---|---|
| 同一迭代器任何推进调用（`next` / `next_batch` / `next_batch_view` / `start` / `release`） | **失效**（缓冲复用） |
| 混用 owned 与 view 接口 | 同上——任何推进调用都失效 |
| `Cask::close()` | **仍有效**（映射被 pin 表 / `map_holder_` 锚定；pread 记录在迭代器 holder 里） |
| 并发写 / merge / flush / OKI 落段 | 有效（快照 + 钉住映射不受影响；S59 healed 重读路径照旧） |
| 迭代器析构 / `release` | 失效 |

「`close()` 仍有效」只针对**已取到的那一批**。`close()` 之后继续推进：扫描侧
照常（pin 表里的封口文件仍在，现有契约）；range 侧会失败——推进要走
`cask_->get()`，close 后新发起的调用返回 `kClosed`（现有行为，不是本设计引入）。

一句话契约（写进头注释）：**「借库内存储，下一次推进即失效；要保留，
当下复制。」** 与 `bitcask_parallel_scan` 回调「仅本次回调内有效」的既有
措辞同一风格，只是窗口从单回调放宽为「直到下一次推进」。

⚠ 比 `bitcask_parallel_scan` 更容易错：那个是「回调内有效」，出回调就没法碰；
这里的窗口跨多次调用，典型误用是「先把一批 key 收进数组，再回头读 value」——
回头时可能已经调过 next。头注释要带这个反例。

---

## 5. 错误契约与 `_ex` 版的差异（有意为之）

`_ex` 版中途出错时已填条目仍有效（各持 malloc，S13-M1 契约）；视图版的
holder 在错误路径上不保证完整清点，**错误即整批作废**（包括本次已填的
条目）。理由：视图调用方本来就是「即取即用」画像，为错误路径保留半批
借用没有意义，反而把失效规则复杂化。文档明写，不做兼容折衷。

OOM：`view_key_buf_` / holder vector 预留失败 → `kIo` fault，整批作废。
BadCrc / 定位不符（`kIndexCorrupt`）→ 同 `next()` 现行为，整批作废。

---

## 6. ABI / 版本 / 文档

- 纯加法：新 typedef ×2 + 新函数 ×2；不碰既有结构体布局，SOVERSION 不动。
- `bitcask_iter_entry_view_t` 含 `file_id/offset/total_sz`（对齐
  `bitcask_iter_entry_t` 全字段）+ `meta`（对齐 `_ex` 的能力集）——视图版
  一步到位补齐两代条目的并集，后续不再出第三代。
- 文档：`doc/api-c.md` / `doc/api-cpp.md` 各加一节 + 头注释；发版时
  CHANGELOG 补条目（现约定发版时写）。

## 7. 测试计划

1. **parity**：同一数据集，view 批与 `_ex` 批逐字段一致（scan / range 各一）。
2. **生命周期**：view 批跨 `close()` 后逐字节可读；下一次推进后内容不保证
   （用「重新写入的哨兵值」验证失效路径）；混用 owned 调用后失效。
3. **组合矩阵**：墓碑（`see_tombstones`）× 前缀 × filter × `want_meta`。
4. **双读路径**：封口文件 mmap（重开后扫描）与 pread 兜底（构造无映射场景）
   都能出正确视图——覆盖 holder 的两种担保。**平台分工**：活跃文件的 growing
   map 只在 Linux / FreeBSD 生效（`data_file.hpp:174-184`），Windows 上刚写完
   的库扫描全走 pread + `ReadRecord` holder；mmap 零拷贝路径只有 Linux CI
   覆盖。两条路径的用例要各自标明靠哪个平台兜底，不能「Windows 全绿就算过」。
5. **range 预取**：`prefetch > 1` 的 view 版输出序与内容同惰性版；
   自适应串行分支（S71-3）在 view 路径同样生效。
6. **分配计数（可选）**：稳态 N 批堆分配次数为常数（现有测试基建若无
   allocator hook 则降级为「容量不回缩」断言）。

## 8. 工作面

| 文件 | 改动 |
|---|---|
| `include/bitcask/cask.hpp` | `EntryView` ×2 + 方法声明 + 迭代器内部状态 |
| `src/cask/cask_iter.cpp` / `cask_range_iter.cpp` | 视图实现（复用现有读路径） |
| `c_api/bitcask_kv.h` / `bitcask_kv.cpp` | C API 两个入口 + 契约注释 |
| `tests/` | §7 用例（新文件或并入 `flush_after_iter_test.cpp` 风格的专项） |
| `doc/api-c.md` / `doc/api-cpp.md` / 反馈回没回 | 实施后更新 |

预估 ~500 行含测试。风险集中在失效规则的文档精确性（§4/§5），
实现本身是对现有读路径的「少拷一次」改造，无新并发面。

## 9. 评审结论（2026-11-23）

**可行**。逐条核实过设计依赖的前提：

| 前提 | 核实结果 |
|---|---|
| 批次存活期内映射不会被撤换 | 成立。活跃文件映射一次按 `reserve_bytes` 建好、文件增长不重映射（`data_file.hpp:174-184`）；封口文件映射随 `DataFile` 对象走，holder 持 `shared_ptr` 即锚定 |
| `read_file` LRU 逐出会让 span 悬垂 | 不会。逐出只删缓存项（`cask.cpp:1445-1450`），已持 `shared_ptr` 的句柄不受影响；设计要求 holder 接住 pin 表外的 `shared_df` 是对的 |
| 持有的记录被搬动会让 span 悬垂 | 不会。`ReadRecord` / `GetResultView` 被 move 时堆缓冲不搬 |
| ABI | 纯加法，成立 |

**但收益要重新估，并设门槛**：

- S71 实测扫描 214～436 ns/行，perf 里 `malloc`/`free`/`memmove` 各约 5%
  ⇒ 理论上限省约 15%（30～60 ns/行；百万行 30～60 ms）。
- 报告方 keelson 走 JVM 绑定：宿主本来就要把 value 拷成 host 字符串，视图版
  只省掉「库内 malloc + free」这一道，端到端大概是个位数百分比。
- range 更低：每行 570～793 ns，大头在点查定位而不是分配。

⇒ 建议分两步，不推翻「两个都做」：先做扫描半边（S75-2 的扫描部分 + S75-4 的
`bitcask_iter_next_batch_view`），用反馈附的 C 程序在 Linux 上量前后；
扫描路径 ≥ 10% 再做 range，否则 range 半边关掉并在反馈里写明数字。
