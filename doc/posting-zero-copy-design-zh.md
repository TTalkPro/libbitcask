# 查询路径 PostingList 数据布局与零拷贝（P1 / P2-min / S22-M6 现状）

> 范围：**已落地实现文档**——`bm25/inverted.cpp` 与
> `include/bitcask/inverted.hpp` 的 PostingList 数据布局、查询路径的
> 读取协议、CoW 写路径、块级元数据维护。**不再**讨论未启动的「远期
> 形态」（Lucene segment / deque + `published_count` / mmap 直读
> disk）——未来若启动走专用设计稿。
>
> 实施批次对应：
>
> - **P1**：`PostingList::snapshot_flat`（扁平快照：拷 `ords` /
>   `tfs` 列 + `blocks` / `max_tf`，不拷 `positions` / 不拷 `dl`）；
>   5 条非-phrase 查询路径（`search` / `search_wand` / `bool_search` /
>   `search_fuzzy` / `search_wildcard`）消费 `FlatPostings`。
> - **P2-min**：phrase / near 持 `std::shared_ptr<const PostingList>`
>   引用 + `use_count()` CoW 协议（写者见 `mutable_pl`）。
> - **S22-M6**（commit `bf4da8c`）：`PostingList` 内存布局
>   **`AoS → SoA`**——`Posting{ord, tf, dl, positions}` 单条 40B
>   加独立堆块改成 `ords[]` / `tfs[]` / `dls[]`（无位置库 16B/条，
>   有位置库 24B/条 + 紧凑 `pos_data` / `pos_off`）平行数组。
>   `inverted_wal` 模块同 commit 退役（S14-4 delta 链承担增量
>   持久化）。
> - **S16-2 / S18-1**：DocMap 端 `doc_lens_` SoA 副本（`fill_doc_lens`
>   SIMD gather 与 `OrdSeq::set_doc_len` 配套）。

## 1. 当前两套读取接口：`FlatPostings` 与 `shared_ptr<const PostingList>`

| 接口 | 形态 | 行数拷贝 | 位置 (positions) | 块元数据 | 适用路径 |
|---|---|---|---|---|---|
| `FlatPostings` | 拷 `ords`/`tfs` 列（`assign` = memcpy，**2 次堆分配**）+ `blocks` 浅拷 + `max_tf` | 是（仅 (ord, tf)） | 无 | 浅拷 | `search` / `search_wand` / `bool_search` / `search_fuzzy` / `search_wildcard` |
| `shared_ptr<const PostingList>` | 仅持 `shared_ptr`（O(1) atomic 增减） | 零 | 原始 | 原始 | `search_phrase` / `search_near` / `explain` |

**两种接口的来源不同**：P1 是「accessor 下深拷出扁平快照即可释放桶
锁」的扁平路径；P2-min 是「accessor 下拷 `shared_ptr` 引用（CoW
协议靠 `use_count()`）即可释放桶锁」的引用路径——后者必须保证读
者持引用期间对象不被突变。

### 1.1 P1 的 `FlatPostings` 与 `snapshot_flat`

```cpp
struct FlatPostings {
    std::vector<std::uint64_t> ords;
    std::vector<std::uint32_t> tfs;
    std::vector<PostingBlock>  blocks;   // 浅拷（N/128 量）
    std::uint32_t              max_tf = 0;
};

void PostingList::snapshot_flat(FlatPostings& out) const;
```

`PostingList::snapshot_flat`（`bm25/inverted.cpp` 全文一处定义）实现
退化为 SoA 列的整列拷贝：

- `out.ords.assign(ords.begin(), ords.end())` —— 单趟 memcpy。
- `out.tfs.assign(tfs.begin(), tfs.end())`。
- `out.blocks = blocks` —— 浅拷，里头是 POD 索引块，没有共享指针。
- `out.max_tf = max_tf` —— 整数赋值。

注：**`dls` 不拷**（v5 impacts 字段在 `PostingList` 留作索引时元数据，
查询期由 `live_checker.fill_doc_lens(tp.fp.ords, tp.dls)` 在路径内
独立批量取，且 `dls` 之于计算 BM25 tf 归一化分母是查询期每 doc 重
读而非副本缓存）。**`pos_data` / `pos_off` 不拷**（phrase 路径不
走 FlatPostings 也不需要它们）。

### 1.2 P2-min 的 `shared_ptr<const PostingList>` + `mutable_pl`

词桶 map 值类型为 `std::shared_ptr<PostingList>`（`include/bitcask/
inverted.hpp::InvertedIndex::PostingMap`）。`bool_search` / `search_fuzzy` /
`search_wildcard` / `search` / `search_wand` 在读路径中持
`const_accessor` 拿到引用（轻）；`phrase` / `near` 同样持 `const
_accessor` 拿到引用，把 `acc->second` 拷到本地 `std::shared_ptr<const
PostingList>`（共享引用计数的累加），**整个评分循环期间不再触桶**。

写者（`add_doc` / `apply_delta` / `compact` / `finalize_all_postings`/
`rebuild_*`）经 `mutable_pl` 拿可写引用：

```cpp
PostingList& mutable_pl(std::shared_ptr<PostingList>& sp) {
    if (!sp) sp = std::make_shared<PostingList>();
    else if (sp.use_count() > 1) sp = std::make_shared<PostingList>(*sp);
    else std::atomic_thread_fence(std::memory_order_acquire);
    return *sp;
}
```

- `use_count() == 1` ⇒ 无 phrase/near 读者持引用 → 原地改（`append`
  + `note_appended`）。
- `use_count() > 1` ⇒ 克隆替换（CoW），旧版本由读者引用计数自然续命。
- `use_count() == 0` ⇒ 第一次建空表。

`use_count()` 是 `relaxed` 减；观察到 1 后补 `acquire` fence，与读者
析构 `shared_ptr` 的 `release` 递减配对——保证读者的最后一次数据读
`happens-before` 写者的后续原地修改。**注意**：`PostingList::append`
内部**保留**写入者侧的对称 fence（详见 §3 的字段布局）：读者只是
持有 `shared_ptr<const>`，写者不直接读到 reader 状态但通过 `use_count`
共时序对偶。

## 2. S22-M6 内存布局：AoS → SoA 对比

### 2.1 旧（AoS，commit bf4da8c 之前）

```
struct Posting {
    uint64_t  ord;             // 8B
    uint32_t  tf;              // 4B
    uint32_t  dl;              // 4B (v5 padding 槽)
    std::vector<uint32_t>      // 24B (libstdc++ vector 头)
        positions;             // + 每条 posting 一次堆块分配
};
// sizeof(Posting) ≈ 40B（positions 为空），追加即 push_back(moved)
//                      ≈ 64B（含 vector 头 + 首次分配）。
// std::vector<Posting> items; // 紧凑 vector of struct，cache 友好
```

特点：单条 posting 一次堆分配（`positions` 头次 push 时分配底层
`uint32_t[]`）。`compact_flags` 走 `Posting` 级 move。CoW 克隆走
`vector<Posting>` 的 `copy` ⇒ 「N+1 元素 + N 条 posting positions」
的堆分配（N 次 element 拷贝 + N 次 vector 拷贝）。

### 2.2 新（SoA，commit bf4da8c 之后）

```cpp
struct PostingList {
    std::vector<std::uint64_t> ords;      // 8B/条 (strict ascending)
    std::vector<std::uint32_t> tfs;       // 4B/条
    std::vector<std::uint64_t> pos_off;   // 8B/条 (惰性：空 positions
                                          //      库恒为 empty)
    std::vector<std::uint32_t> pos_data;  // 0B/条均值（累加进各条）
    std::vector<uint32_t>       dls;      // 4B/条 (v5 impacts 索引时存)
    std::vector<PostingBlock>  blocks;    // 28B/块 (1/128 条)
    uint32_t                   max_tf = 0;
};
```

| 形态 | 行大小 | 总分配 |
|---|---|---|
| 无位置库（`index_positions_ = false`） | 16B/条（`ords` + `tfs` + `dls`） | 4 次堆分配 |
| 有位置库（`index_positions_ = true`） | 24B/条 + 紧凑 `pos_data` | 6 次堆分配 |

**关键 S22 变化**：

- `dls` 列从 0 → 4B/条：在原 AoS 的 `padding` 槽落地，**内存零增量**
  （v5 impacts 本来就准备这个字段，只是 AoS 时期被压缩到 4B padding
  里；SoA 化后独立成列，物理摊到每条仍是 4B，cache line 内仍与
  `tfs` 同列连续）。`min_dl` 在 `seal_full_blocks` / `finalize` 内
  按块聚合时直接读 `dls[i]` 即可，无需重新遍历原始 `tf`。
- `pos_off` **惰性物化**：增量为空库的 `index_positions_ = false`
  路径零 `positions` 开销。首个非空 `positions` 追加前恒为 `empty`
  —— 这意味着无位置库的词**永远不会**为 `pos_off` 付 8B/条的开销。
- `compaction` 走原地双指针：

```cpp
bool compact_flags(std::span<const char> live) {
    // 原地双指针 w/i；pos_data 显式搬移 + pos_off 重写
    // 每轮迭代先读 pos_off[i]/[i+1] 再写 pos_off[w]，w ≤ i 保证读写不冲突
    ...
}
```

- CoW 克隆走 `vector<uint64_t>` / `vector<uint32_t>` 的列式 `copy`
  ——「N+1 元素」N 次拷贝，**每列独立堆分配**，比 AoS 「N+1 + N 条
  positions」少一组分配（节省 N/N+1 ≈ 50%）。

### 2.3 与 v6 落盘格式的对齐

v6 落盘（`bm25/inverted.cpp` 内 `kInvVersion = 6`，`save` / `load`）
本就列式：

- ord 列 —— `FOR`（Frame-of-Reference）128/块（每块 frame + bits +
  packed bytes）；
- tf 列 —— `VByte` varint 整组编码；
- dl 列 —— `VByte` varint 整组编码；
- positions 列 —— 单独 `VByte` 段。

SoA 内存布局是其天然镜像——`save()` 的「按列依次写」与内存排布
**字节零转化**，`load()` 的「按列依次读」是单趟 memcpy 后立即上
SoA。S22 这一改**没有序列化层迁移**——加载老版快照（v1..v5）已
不再支持（v6 起断代，旧库需外部迁移工具或重建）。

> 注：`kInvVersion` 同 commit 已升级（`inverted.cpp` 内
> `static constexpr std::uint32_t kInvVersion = 6`），不接受旧版文件
> 头。注释与 `restore_unified_checkpoint` 的失败降级路径需特别注意。

## 3. `PostingList::append` —— 唯一追加入口

`append(ord, tf, dl, positions)`（`include/bitcask/inverted.hpp`）收敛
add_doc / apply_delta 的写入面——保证 SoA 五列不会漏列错位：

```cpp
void append(uint64_t ord, uint32_t tf, uint32_t dl,
            span<const uint32_t> pos) {
    size_t idx = ords.size();           // 同步记录
    ords.push_back(ord);
    tfs.push_back(tf);
    dls.push_back(dl);
    // 惰性物化:首个非空 positions 才建 pos_off
    if (!pos.empty() && pos_off.empty())
        pos_off.assign(idx + 1, 0);
    if (!pos_off.empty()) {
        pos_data.insert(pos_data.end(), pos.begin(), pos.end());
        pos_off.push_back(pos_data.size());
    }
}
```

调用方随后照旧调 `note_appended()`：

- `note_appended` 做两件事——增量维护 `max_tf`（new row 必在末尾，
  `tfs.back() > max_tf` 才更新），并把已满的整块封进 `blocks[]`（满
  块通过 `seal_full_blocks` 单趟扫描 + push_back）。
- `finalize` 用在「可能含不满尾块」的场景（load 后、compact 后）——
  先 `clear` 已有块（消除增量阶段可能封入的满块），再按统一规范重
  建含部分尾块的规范集。

## 4. `PostingBlock`：块级元数据布局

```cpp
struct PostingBlock {
    uint64_t base_ord;
    uint64_t end_ord;       // 块最后一条 posting 的 ord
    uint32_t max_tf;        // v5 保留：块内 tf 上界
    uint32_t min_dl;        // v5 impacts：块内最小 dl
                             // 1 = 旧快照/dl 未知回退 (admissible)
    size_t   start_idx;     // 行下标区间起点
    size_t   count;         // 行数 (满块 = kBlockSize = 128)
};
```

`block_for_ord(ord)`（`PostingList::block_for_ord` 与
`FlatPostings::block_for_ord` 共用 `bm25/inverted.cpp` 内匿名命名空间
的 `block_for_ord_in` 函数）：`blocks[]` 按 `end_ord` 二分（`lower_bound`
），落到 `[base_ord, end_ord]` 区间内的块即所求。

`block_upper_bound(idf, params, avgdl)`：读缓存 `max_tf`（SoA 后
不再重扫全表，详见 commit `bf4da8c` 的 message），经 `upper_bound_from`
（`bm25/inverted.cpp` 匿名命名空间闭包）算出包含 δ 下界项的
`idf * (tf_norm_max_tf + params.delta)`，admissible 块级分数上界。

## 5. 查询路径的零拷贝读取链

### 5.1 5 条非 phrase 路径：`FlatPostings` 链

以 `InvertedIndex::search_wand` 为例：

1. **accessor 取 PostingMap 引用**：
   `PostingMap::const_accessor acc; shard.inverted.find(acc, term);`
2. **扁平快照**（accessor 持锁期间）：
   `acc->second->snapshot_flat(tp.fp);` —— 单趟列拷贝，O(ord 列字节)。
3. **`accessor` 立刻析构**（走出 const_accessor 作用域），桶锁释放。
4. **主循环**（无锁 / 无虚调用）：
   `tp.live` / `tp.dls` 由 `live_checker.fill_*` 一次性批量取；
   `tp.block_upper_bounds` 一次性按块算好。
5. **共享 top-k 小顶堆**与 path 间 `std::priority_queue` 标准器：
   `std::priority_queue<Entry, std::vector<Entry>, std::greater<>>`。

P1 的「零拷贝」是**单趟列 memcpy**（2 次堆分配：ords + tfs），与
「整列表深拷含每条 posting positions」相比是数量级的字节量差距——
对热词 100K posting 从 ~4MB + 100K positions 堆块降到 ~1.2MB
（`ords` 800KB + `tfs` 400KB）。

### 5.2 phrase / near 路径：`shared_ptr<const PostingList>` 链

以 `InvertedIndex::search_phrase_impl`（`slop = 0` 表严格短语，
`slop > 0` 表有序近邻）为例：

1. **每 term 持 `const_accessor`**：
   `tps.push_back({term, acc->second});` —— 拷 `shared_ptr`，O(1) 原子
   加 1。
2. **`accessor` 立刻析构**，桶锁释放。
3. **短语候选集**取自**最稀有词**（`drv` 索引：`fp.size()` 最小的
   term），其 `PostingList` 的 `ords` 列本身就是主驱：
   `std::vector<std::uint64_t> cand_ords(cand_pl.ords);`（SoA 后
   整列拷贝）。
4. **`fill_is_live` / `fill_doc_lens`**：与 `tps[i]` 的 `ords` 列一次
   性批量取。
5. **短语位置匹配**：每候选 ord 上对**所有**查询 term 取
   `pl.positions(i)` 返回的 `std::span<const uint32_t>`（`PostingList::
   positions(std::size_t)` 在 `include/bitcask/inverted.hpp` 内定义，
   无 positions 或未物化时返空 `span`）——**直接读** PostingsList 内
   紧凑的 `pos_data[]`，零拷贝。

### 5.3 块元数据在查询期的访问路径

`block_for_ord(pivot_ord)` 单趟二分 → 取 `block.max_tf` 与 `block.
min_dl` → 经 `upper_bound_from(...)` 算块级上界。**`search_wand` 进
一步预计算** `tp.block_upper_bounds`（`bm25/inverted.cpp` 内 `search_wand`
初始化阶段），主循环读 `block_upper_bounds[block_idx]` 即可避免每
pivot 重算 6 FMA + 1 div（AVX-512 kernel 调用一次的开销）。

**`PostingBlock` 字段说明（紧凑 ABI）**：`28B/块（base + end + max_tf
+ min_dl + start_idx + count，8+8+4+4+8+8 跨 32/64 位布局对齐）`
—— `FlatPostings::blocks` 一行 `std::vector<PostingBlock>` 浅拷，
访问期间 0 字节额外分配。

## 6. live / doc_len 批量接口（`LiveChecker`）

`include/bitcask/live_checker.hpp`：

```cpp
class LiveChecker {
public:
    virtual ~LiveChecker() = default;
    virtual bool is_live(uint64_t ord) const = 0;
    virtual uint32_t doc_len(uint64_t ord) const = 0;
    // P2.1：批量接口
    virtual void fill_is_live(span<const uint64_t> ords,
                              span<char> out) const;
    virtual void fill_doc_lens(span<const uint64_t> ords,
                               span<uint32_t> out) const;
};
```

P2.1 的批量接口默认实现是逐元素退化，**不要求外部实现者改动**；
`include/bitcask/index.hpp` 内 `Index` 类（同时是 `LiveChecker` 实现
者）覆写为：一次 `shared_lock`，读 `live_[]` 与 `doc_lens_[]`（SoA
副本，平坦保持兼容 SIMD gather）整段直写。

⚠️ **v5 不变量**：`doc_len(ord)` 必须等于该文档 `add_doc` 时的
`Σtf`（`TextPlugin` 两者同源自同一次分词，天然成立）。块级分数
上界用索引时 `min_dl` 收紧——若自定义实现返回比索引时更小的
`doc_len`，上界不再 admissible，BMW 剪枝可能漏掉真 top-k。

## 7. 同步面（index.hpp ↔ inverted.hpp）

- `Index::live_`（`std::vector<char>`）与 `Index::doc_lens_`（`std::vector
  <uint32_t>`，与 `live_` 同长度、对齐可 SIMD 化）由 `Index` 类拥有；
  `InvertedIndex` 仅在每查询入口取**只读快照**（`fill_is_live` /
  `fill_doc_lens` 一次性写入调用者提供的 scratch buffer）。
- `InvertedIndex` 自身不复制 ord ↔ key 映射；`extract_doc_table` /
  `apply_doc_table_delta` 等跨接口通过 `Bitcask` 协调，本文档
  不展开（详见 `recovery-unified-checkpoint-design-zh.md`）。
- `AppendableIndex` / `meta_codec` 等与 S22-M6 关系：`bitcask.meta`
  v3 编码只动 DocMap 物理 layout，与倒排 PostingList SoA 化无耦合。

## 8. 写入面：写者侧 CoW 触发条件

`add_doc`（`bm25/inverted.cpp` 全文一处主入口）：

```cpp
for (auto& [term, data] : term_data) {
    auto& [tf, positions] = data;
    auto& shard = shard_for(term);
    PostingMap::accessor acc;
    const bool is_new_term = shard.inverted.insert(acc, term);
    PostingList& pl = mutable_pl(acc->second);  // ← P2-min CoW
    pl.append(ord, tf, doc_len,
              index_positions_ ? span<const uint32_t>(positions)
                               : span<const uint32_t>{});
    pl.note_appended();
    ...
}
```

`mutable_pl`（详 §1.2）：`use_count() > 1` ⇒ 整表克隆替换。**触发
场景**：phrase/near 查询与同 term 写入重叠。**常态开销**：词命中
相位无 phrase/near 引用时 `use_count() == 1`，`acquire fence` 单次，
原地 `append`。

> Phase 2-min 与 §4.2「deque + published_count」的差异：实施时走
> 「shared_ptr + use_count CoW」而非 published_count 前缀只读。
> 优势：vector 保留（cache 友好）+ 复杂度最低；劣势：罕见 CoW 时
> 整表克隆——但冷路径，常态零开销。**未来若启动 Phase 2**：将走
> `deque` 化与 `atomic published_count` 的 release/acquire 协议，
> 与本 §1.2 的 CoW 协议**位级兼容**（reader 视对象为不可变接口不变）。

## 9. 关键代码符号索引

| 概念 | 符号 | 位置 |
|---|---|---|
| SoA `PostingList` 主类型 | `struct PostingList` | `include/bitcask/inverted.hpp` |
| 块元数据 | `struct PostingBlock` | `include/bitcask/inverted.hpp` |
| 扁平查询快照 | `struct FlatPostings` | `include/bitcask/inverted.hpp` |
| 唯一追加入口 | `PostingList::append(ord, tf, dl, pos)` | `include/bitcask/inverted.hpp` |
| 块级元数据三阶段 | `seal_full_blocks` / `note_appended` / `finalize` | `include/bitcask/inverted.hpp` |
| 死点压实（SoA 双指针） | `PostingList::compact_flags(live)` | `include/bitcask/inverted.hpp` |
| 块二分查找 | `block_for_ord_in`（匿名） + `PostingList::block_for_ord` / `FlatPostings::block_for_ord` | `bm25/inverted.cpp` |
| 列表级上界计算 | `upper_bound_from`(max_tf, idf, params, avgdl, min_dl) | `bm25/inverted.cpp` 匿名命名空间 |
| 缓存的全局上界 | `PostingList::max_tf`（note_appended 增量、compact_flags 重算） | `include/bitcask/inverted.hpp` |
| FlatPostings 生成 | `PostingList::snapshot_flat(FlatPostings&)` | `bm25/inverted.cpp` |
| P2-min CoW 协议 | `mutable_pl(shared_ptr<PostingList>&)` | `bm25/inverted.cpp` 匿名命名空间 |
| 桶 map 值类型 | `using PostingMap = tbb::concurrent_hash_map<...>` | `include/bitcask/inverted.hpp` |
| live / doc_len 批量接口 | `LiveChecker::fill_is_live` / `fill_doc_lens` | `include/bitcask/live_checker.hpp` |
| 5 条扁平快照消费者 | `search` / `search_wand` / `bool_search` / `search_fuzzy` / `search_wildcard` | `bm25/inverted.cpp` |
| 2 条 Phrase 路径消费者 | `search_phrase` / `search_near`（`search_phrase_impl` 共享实现） | `bm25/inverted.cpp` |
| v6 落盘版本号 | `static constexpr std::uint32_t kInvVersion = 6` | `bm25/inverted.cpp` |

## 10. 关键不变式（评审 checklist）

- `PostingList::ords[]` 严格升序无重复（`add_doc` 的水位幂等保护 +
  `apply_delta` 的「ord > 列尾」守卫）。
- `PostingBlock::end_ord` 必须等于 `ords[start_idx + count - 1]`（与
  `base_ord` 即 `ords[start_idx]` 共同定义块边界；`search_wand` /
  `bool_search` 的 `block_end()` 与 `block_for_ord()` 二分依赖）。
- `PostingList::max_tf` 必须等于 `tfs[]` 全局最大值（`note_appended`
  增量、`compact_flags` 重算；`find`/`block_for_ord` 不参与维护）。
- `pos_off.size() == ords.size() + 1` 当且仅当**至少一次**非空
  positions 追加之后（惰性物化未触发时 `pos_off.empty()`）。
- `use_count() == 1` 是 `mutable_pl` 选择原地改的**唯一依据**——
  失去该不变量会让 phrase/near 读取到不一致的中间状态（持有
  `shared_ptr<const>` 但写者原地动了它）。

## 11. 相关文档

- `doc/kway-blockmax-bmw-zh.md` —— k-way 交集 + 块级元数据 + BMW
  的设计路线与现状（含本数据结构 §3 的关联段）。
- `doc/wand-blockmax-zh.md` —— Block-Max WAND 的算法讲解与符号
  映射表，与本数据结构 §5.1 的 `search_wand` 路径互引。
- `doc/inoue-simd-intersection-zh.md` —— SIMD 块过滤 + Inoue
  permutation 内核（`intersect.cpp`）；消费本数据结构的 `ords[]`
  平行数组（必升序去重不变量）。
- `doc/concurrency-zh.md` —— `tbb::concurrent_hash_map` 的桶锁粒
  度与 `mutable_pl` 的 memory order 协议。
- `doc/format-zh.md` —— v6 落盘字节布局；本 §2.3 描其与 SoA 内存
  镜像。
