# 有序 Key 索引（OKI）：WiscKey 式 Range 扫描 + keydir 内存天花板路线

> 前置阅读：
>   - [`keydir-sharding-design-zh.md`](keydir-sharding-design-zh.md)（keydir 分片/屏障/MVCC fold 现状，本设计的宿主）
>   - [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)（checkpoint 链 / manifest commit point / 水位语义）
>   - [`merge-policy-zh.md`](merge-policy-zh.md)（merge 触发与 CAS 搬迁模型）
>   - [`format-zh.md`](format-zh.md)（BCH4 hint 格式，OKI run 格式的直接母版）
>
> 状态：**设计草案（未实现）**。对应 S33 梯队。
>
> 兼容策略：**flag-day 停机迁移**（与 5.0.0 tstamp64 同模式）：meta v4 → v5，hint BCH4 → BCH5（记录加 ord），新旧互相干净拒开，配套 `bitcask_migrate hintord` 离线迁移。**data file / DocValue / checkpoint 格式全部不动**——本次 flag-day 范围刻意收窄，只动 hint 与 meta 门禁。
>
> 一句话：bitcask 的 keydir 是哈希表，做不了有序 Range 扫描，且全内存是 key 规模的硬天花板；本设计不换存储底座，而是走 WiscKey 的路——**data file 继续当 value log（本来就是），旁挂一个 LSM 形态的有序 key 索引（OKI）**。Level A 只解决 Range（不动 get 热路径），Level B（远期、有量化门禁）才把 keydir entries 落盘解决内存天花板。

---

## 1. 动机与替代方案否决

### 1.1 两个真实短板

1. **无有序 Range**。现有能力只有 S13-D4 的 `key_prefix` 过滤，且是 O(全表)：`IterHandle::start` 仍把全部 key 拷进快照缓冲（`src/keydir/keydir.cpp:931-946`），前缀过滤在 proxy 层逐条 `memcmp`（`src/cask/cask_iter.cpp:113-118`）。`cask.hpp:311-314` 的头注释已明言"有序范围扫描需有序索引"。C API 甚至连 `key_prefix` 形参都没有。
2. **keydir 全内存天花板**。每 key 常驻 = `SingleEntry` 40B + key 本体 + `SeqShardTable` 开放寻址槽位开销，实测口径约 60~100B/key。1 亿 key ≈ 8~10GB RSS，这是 bitcask 模型的结构性上限。

### 1.2 否决：整体替换为 LevelDB

已单独评估，结论是否决，理由记录在案：

- **LSN 模型崩塌**：搜索索引一致性锚在 `ord`（= LSN，`alloc_ord` 唯一发番）+ 数据文件字节水位双水位上（`cask.cpp:744-753` 的保存序不变量）。LevelDB 不暴露可用的 sequence→位置映射，等于重写 docmap/bm25/vec 三组件的 checkpoint/恢复模型。
- **零拷贝读没了**：`GetResultView` 借 sealed mmap / pread 缓冲；LevelDB `Get` 返回拷贝，block 随 compaction 消失。
- **读写路径双输**：O(1) keydir + 单 pread 变多层查找；merge 一次 O(live) 变 compaction 10~30× 写放大。
- **S30 已把 RSS 砍半**，剩余内存大头在 HNSW 图/倒排，换引擎动不到。

### 1.3 采纳：WiscKey 式「只补索引，不换日志」

WiscKey（FAST'16）= LSM 只存 `key → 值位置指针`，value 留 append-only 日志。本库结构上已是"半个 WiscKey"：data file 就是 vLog 且兼作 WAL，merge 就是 vLog GC。缺的只是那棵有序索引。本设计补的就是这一块，且刻意做成**旁挂**而非替换：

- **点查权威仍是哈希 keydir**，get/put/remove 热路径零改动（Level A）。
- OKI 只回答一个问题：**"按 key 序，[lo, hi) 里有哪些 key"**。值定位一律回查 keydir。
- OKI 全部文件是**派生缓存**（`.ckpt` 语义：可 fold 重建），丢了、坏了都不影响数据正确性；格式门禁走 flag-day（§7），但门禁对象是 hint，不是 OKI 文件本身。

### 1.4 否决：直接 vendored LevelDB 当 OKI

考虑过把 leveldb 以 submodule 引入只做 key 索引。否决：leveldb 自带 WAL/memtable/后台 compaction 线程/自己的文件布局与锁模型，与本库的单写者串行、文件命名契约、checkpoint 水位模型杂糅成本高；而我们需要的 run 格式与现成的 BCH4 hint（vbyte + 前缀差分 + trailer CRC）基本同构，自研量级可控且完全贴合现有恢复语义。本库"无 Boost/abseil"的依赖洁癖也是一票。

---

## 2. 概念模型

### 2.1 三层结构

```
                     写路径（Cask::put/remove，write_mu_ 串行）
                              │
              ┌───────────────┼──────────────────┐
              ▼               ▼                  ▼
        data file 追加   哈希 keydir 更新    OKI memdelta 追加   ← 新增，仅此一处
        （不动）          （不动）            （key, ord, tomb）
                                                  │ 达阈值 flush
                                                  ▼
                                          kv.oki.seg-<gen>（有序 run，不可变）
                                                  │ 后台/merge 时归并
                                                  ▼
                                          少量大 run（全归并时丢弃墓碑）
```

- **memdelta**：内存有序结构，只存 `(key, ord, tombstone)`。关键事实：**写路径在 Cask 层本就被 `write_mu_` 串行化**（单 append WAL），所以 memdelta 是单写者结构，无需分片、无需排进 keydir 的分片锁全序——一把独立轻锁（或 seqlock 快照）即可，读者是 range 扫描（冷路径）。
- **sealed run**（`kv.oki.seg-<gen>`）：按 key 排序的不可变文件，条目 `(key, ord, tomb)`，带稀疏块索引支持 `seek(lo)`。
- **哈希 keydir**：不变，仍是点查与活性的唯一权威。

### 2.2 核心不变量：OKI 允许陈旧，正确性靠回查

Range 查询 = k 路归并（runs + memdelta，按 key 归并、同 key 取 max ord、tomb 抵消）得到候选 key 流，**逐 key 回查哈希 keydir**：

- key 已不存在 / 是墓碑 → 跳过（run 里的死 key 无害）；
- keydir 里的 entry 是权威位置 → 值读取走现有 `get`（零拷贝 `GetResultView`）。

由此获得三条极简性质：

1. **merge 与 OKI 零交互**。merge 搬迁只改 `(file_id, offset)`，不改 key 集合；OKI 不存位置信息，所以 merger 的 `RelocateEvent` 无需广播给 OKI，`relocations_stuck` 等纵深防御一概不涉及。
2. **run 永不需要"更新"**，只有归并与删除；GC 是空间/速度优化而非正确性需求。
3. **完整性只要一条**：任意时刻，`(所有 sealed runs ∪ memdelta)` 的 key 集合 ⊇ 哈希 keydir 的活 key 集合。多余无害，缺失才是 bug。

### 2.3 水位

沿用双水位模型的 ord 半边：每个 run 携带 `cover_ord`（其内容覆盖到的 LSN 上界），OKI manifest 记录全体 runs 的联合覆盖 `oki_wm`。恢复时重放 `ord > oki_wm` 的写进 memdelta（§5）。字节水位不需要——OKI 不定位值。

---

## 3. 磁盘格式

### 3.1 文件与命名

遵守 `cask_internal.hpp:23-26` 的命名契约 `{kv|search}.{组件}.{ckpt|seg|wal|manifest}`：

| 文件 | 内容 | 性质 |
|---|---|---|
| `kv.oki.seg-<gen>` | 有序 run | 派生缓存，不可变 |
| `kv.oki.manifest` | 活跃 run 集合 + 各自 `cover_ord` + 联合水位 | 派生缓存，唯一 commit point |

**meta 版本 bump 到 v5（flag-day）**。本设计与 hint v5（§3.4）绑定一次 flag-day：`read_meta` 加干净拒绝分支（v4 → "u32-ord-less-hint era, run `bitcask_migrate hintord`"，措辞对齐 v2/v3 时代的拒绝提示），旧代码打开新目录同样被 meta v5 拒开。**注意区分**：flag-day 门禁的对象是 hint 格式；OKI 自身文件（run/manifest）仍是派生缓存语义——损坏/缺失 → 弃用重建，正确性零依赖。这样既拿到 flag-day 的简单性（无兼容矩阵、无降级路径），又不把 OKI 抬成"不可丢"的权威数据。

### 3.2 run 格式（"BCOK" v1）

母版是 BCH4 hint（`format.hpp:69-86`），差异只在按 key 序排列 + 稀疏索引：

```
头部  4B：magic "BCOK"（版本升级时换 magic 或加 ver 字段，读端 fail-fast）
数据块区（每块 ~4KiB 目标大小，块内 key 前缀差分）：
  块首条: [vbyte klen][key 全量][vbyte ord][flags u8(bit0=tomb)]
  后续条: [vbyte shared_len][vbyte suffix_len][suffix][vbyte ord_delta][flags u8]
稀疏索引区: [count u32] + 每块 [vbyte 首 key 长][首 key][块 offset u64]（支持 seek 二分）
尾部: [索引区 offset u64][running_crc u32][trailer "BCOE"]，CRC 覆盖 [0, size-12)
```

有序 key 的公共前缀压缩收益显著（真实 key 多为 `prefix:id` 形态），预计每条 ≤ 8~12B + suffix。写端复用 `atomic_write_bytes` 之外的流式路径：临时名写入 → fsync → rename。

### 3.3 manifest（"BCOM" v1）

小定长文件，格式对齐 `index.manifest` 的做法（`index_manifest.hpp`）：magic + ver + run 条目数 + 每条 `(gen u64, cover_ord u64, crc32)` + 全文件 CRC + trailer magic，`atomic_write_bytes(..., fsync_dir=true)` 落盘，**是 OKI 唯一 commit point**。无 `.prev`：损坏即整体弃用 OKI 并重建（派生缓存，降级安全）。

新增段型/文件的教训（`search_checkpoint.hpp:49-53`）在这里的对应物：**读端遇到未知 magic/ver 必须整体拒收 OKI 并走重建**，绝不静默跳条目。

### 3.4 hint v5（"BCH5"）：记录加 ord

现状缺口：hint 记录无 ord（`codec.hpp:51`），hint 快路径重建的 keydir 条目 ord 恒 0 且不 `advance_ord`（`cask_recovery.cpp:338`）——这使 hint 路径恢复无法判断"哪些写落在 OKI 水位之后"，是原兼容方案里最大的坑。flag-day 直接修掉：

```
头部 4B: magic "BCH5"
记录（v4 基础上插一个字段）:
  [vbyte gap][vbyte total_sz][vbyte (keysz<<1|tomb)][vbyte ord_delta][tstamp u64 LE][key]
  ord_delta = ord − prev_ord（同一 data file 内 append 串行，ord 严格递增，差分恒非负）
trailer 8B: "BCHE" + running_crc u32（同 v4）
```

配套语义修正（同一 flag-day 完成）：

- hint 快路径重建时 `put(..., ord)` 用真实 ord，并 `advance_ord`——消除现存的 ord=0 怪癖，hint 路径与 fold(data) 路径恢复结果完全等价；
- **BCH4 读端代码整体删除**（`fold_v4` 等按 magic 分派的分支），`HintFile::open` 遇 BCH4 magic 干净报错提示迁移——不留双格式读端。

flag-day 同步清单（对照 tstamp64 的教训——时间戳宽度当年散布 8 个格式，本次刻意只有 hint 一个，但同步点仍须一次列全）：`format.hpp` hint 常量与布局注释、`codec.hpp::HintRecord`（加 `ord` 字段）、`src/fileops/hint_file.cpp` 读写两端、`src/merge/merger.cpp:199-200`（merge 写 hint 处传 ord，`PendingUpdate` 已有该字段）、`src/cask/cask_recovery.cpp` hint 快路径、`meta_file.cpp` 版本门禁与沿革注释、`tools/bitcask_migrate.cpp`、`doc/format-zh.md` / `doc/migrate-le.md` / CHANGELOG、hint 相关测试与 bench。

---

## 4. 查询路径

### 4.1 新 API（C++）

```cpp
// cask.hpp
struct RangeOptions {
    std::span<const std::byte> lo;      // inclusive；空 = 从头
    std::span<const std::byte> hi;      // exclusive；空 = 到尾
    bool reverse = false;               // S33 首版可不做，格式已预留（块索引可倒序走）
    std::size_t prefetch = 0;           // >0 时值读取并发预取（§4.3）
};
[[nodiscard]] std::unique_ptr<CaskRangeIter> make_range_iter(const RangeOptions&);
// CaskRangeIter::next() → std::expected<std::optional<Entry>, CaskFault>，Entry 复用 CaskIter::Entry
```

前缀扫描 = `lo = prefix, hi = prefix 的字典序后继`，是 Range 的语法糖；顺手把 C API 缺失的 `key_prefix` 一并补齐（`bitcask_range_iter_start(lo, hi, ...)`，同时覆盖前缀场景）。

### 4.2 执行

1. manifest 快照（引用计数 pin 住 runs，防归并期间 unlink——复用 read-handle 缓存的 fd 生命周期管理模式）。
2. 各 run `seek(lo)` + memdelta `lower_bound(lo)`，k 路归并（复用 `segment_merge.hpp` 的 k 路归并骨架，如可行）。
3. 同 key 多源 → max ord 胜；胜者是 tomb → 跳过。
4. 候选 key → `keydir_->get(key)`：miss/墓碑 → 跳过；命中 → 现有读路径取值（`GetResultView`）。

### 4.3 一致性语义（诚实声明）

`CaskRangeIter` 是 **per-key 一致**（与 `parallel_scan` 同档），不是 `CaskIter` 的 fold 快照一致：迭代期间的并发写可能部分可见。理由：把 range 挂进 fold 的 sibling-chain/pending 机制会把 memdelta 拖进屏障协议，复杂度收益比极差。需要快照语义的调用方组合用法：先 `CaskIter::start()`（冻结 fold 快照）再跑 range——留作开放问题（§10），首版文档言明弱一致即可。

### 4.4 值预取（WiscKey 的 SSD-conscious 补偿）

key 序枚举 → 值读取是随机 pread。`prefetch > 0` 时用 `parallel_scan` 的既有线程池基建对归并出的 key 窗口并发预取值，吃 SSD 内部并行度。首版可后置（S33-5）。

---

## 5. 写入路径与恢复

### 5.1 写挂钩（唯一的热路径改动）

`Cask::put/remove/put_batch` 在 keydir 更新成功后追加 memdelta 一条 `(key, ord, tomb)`。成本：一次有序结构插入（或 append + 惰性排序），单写者无竞争。**put 路径回归预算 ≤ 3%**，超预算则改为 append-only 环形缓冲 + flush 时排序。

merge 的 `apply_pending` **不挂钩**（§2.2 性质 1）。TTL 过期的 `conditional_remove` 同样不挂钩——过期 key 留在 run 里由回查过滤，语义正确。

### 5.2 flush 与归并

- **flush 触发**：memdelta 条数/字节阈值，或搭 auto-ckpt 节流的车（`last_ckpt_ord_` 模式）。flush = 排序写 `kv.oki.seg-<gen>`，`cover_ord` = 当时 `peek_next_ord()`，然后 manifest 提交、清 memdelta。
- **初建 / 全量重建**：`save_snapshot` 已全量遍历 entries（`keydir.cpp:1538-1553`，哈希序）——重建路径复用该遍历 + 外部排序，产出单个全量 run。触发点与 keydir 快照相同（close/merge 收尾/成对 ckpt），但**不阻塞**这些路径：重建放后台，失败只影响 OKI 可用性。
- **归并策略**：极简两层。小 run 数量 > N（默认 8）→ 归并成一个；全归并时墓碑真正丢弃。不做 leveled compaction——OKI 条目小（无值），全归并 1 亿 key 也就 ~1-2GB 顺序 IO。
  > **落地状态（S33-6，2026-08-03）**：已实装（`OkiState::compact_all_locked`，阈值 `kCompactRunLimit=8`）。S33-4 首版只做了 flush（追加 run）与 rebuild（全量重来），本条漏实现——run 数随 flush 次数无界增长、墓碑永不回收，由 fd 探针实测发现后补上。墓碑丢弃的正确性**依赖「全归并」**这一前提，已在 `oki_state.hpp` 内注明约束。

### 5.3 恢复

open 时序（插在 `load_recovery_snapshots` 之后、fold 之前）：

1. 读 `kv.oki.manifest`：magic/ver/CRC 任一不符 → 弃用全部 OKI 文件，标记"需重建"。
2. 校验各 run trailer CRC（或惰性：首次 seek 时校验）。
3. `oki_wm` = manifest 联合水位。**tail 重放**：恢复遍历（fold(data) 或 hint 快路径——BCH5 起两者都有 ord，§3.4）顺路把 `ord > oki_wm` 的活 key 喂进 memdelta，墓碑喂 tomb 条目。恢复结果与"当时没 crash、正常 flush 过"等价。
4. 常态收尾：close 时 flush memdelta + 提交 manifest（`oki_wm` 追平 `peek_next_ord()`），下次 open tail 重放为空。全量重建只剩一种触发：OKI 文件自身损坏/缺失（派生缓存语义）。

### 5.4 KeyDirRegistry 与归属

OKI 与 keydir 快照同模式：**组件挂 `KeyDir`（数据归属、随共享）、路径由 Cask 注入**（KeyDir 不知目录，`keydir.hpp:352-359` 现状）。多 Cask 共享一个 KeyDir 时，flush/重建由持 `bitcask.write.lock` 的 writer Cask 负责——与现在谁写 `kv.keydir.ckpt` 的事实约定一致。memdelta 的锁独立于 keydir 分片锁全序（单写者 + range 读者），不进 `barrier_mu_ → … → fstats_grow_mu_` 链；OKI 内部若需锁序，排在全序末尾之后并文档化。`CaskIter` 的 `keydir_pin_` 语义顺延：OKI 的 fd/mmap 生命周期按 KeyDir 算，不按 Cask。

---

## 6. Level B（远期）：keydir 磁盘驻留

Level A 不解决内存天花板。Level B 的形态：run 条目扩展为全字段 `SingleEntry`（file_id/total_sz/offset/epoch/tstamp/ord），哈希 keydir 退化为"热写缓存 + bloom"，get 走 memtable → bloom → run 块二分。这会动 get 热路径、fold/MVCC、merge CAS、Registry 共享——是大工程。

**启动门禁（必须先量化，不达标不做）**：

1. `key_length_histogram()`（`keydir.hpp:389-396`，现只有测试在用）接入 `status()` 或诊断工具，实测目标负载的 keydir RSS 估算 = `key_count × (40 + avg_key + 槽位开销)`；
2. 该值占进程 RSS 比例 > ~40%（否则大头在搜索索引，Level B 白做）；
3. Range（Level A）已落地稳定——**run 格式 v1 从第一天就预留全字段扩展**（flags 位 + 可选字段区），避免 Level B 时二次 flag-day。

Level B 若启动，另立设计文档；本文档只锁定"格式可演化"这一条约束。

---

## 7. 迁移与兼容（flag-day）

与 5.0.0 tstamp64 同模式：新旧互相干净拒开，离线迁移一次通过。盘上 `bitcask.meta` = v5。

> **版本号（2026-08-03 复核后修订）**：原稿写「bump 到 6.0.0」，实际定为 **5.1.0**、`SOVERSION` 保持 `5`。理由：本轮 C API 纯增量（ABI 未破坏），而本仓库的规则是 **ABI 破坏才驱动 major/SOVERSION，盘上格式破坏不驱动**（先例 3.1.0：meta v2→v3 前向不兼容 + C API 增量 → MINOR）。盘上纪元隔离由 meta v5 门禁承担，与 soname 无关。

| 场景 | 行为 |
|---|---|
| 新代码打开旧目录（meta v4） | `read_meta` 干净拒开："ord-less-hint era (meta v4); run `bitcask_migrate hintord`" |
| 旧代码打开新目录（meta v5） | 现有 `ver != 4 → unsupported meta version` 兜底拒开（`meta_file.cpp:83-85`），无需改旧代码 |
| OKI 文件损坏/缺失 | 派生缓存语义：弃用 + 后台重建，数据零风险（不在 flag-day 门禁内）|

**`bitcask_migrate hintord` 子命令**（挂进现有 `detect` / `be2le` / `tstamp64` 工具链，`detect` 同步识别新纪元）：

1. 逐 data file 顺序扫描（ord 在 record header `kOrdOffset`），重写 `<tstamp>.bitcask.hint` 为 BCH5——hint 本是派生缓存，重写零风险；
2. 派生缓存（keydir 快照 / search checkpoint 链 / segments）一律**不迁移**——与 be2le/tstamp64 的既有工具惯例一致，新库首开由 fold 自动重建，避免任何 stale 缓存风险（实现时按此定案，取代草案期"原样保留"的设想）；
3. 可选 `--prebuild-oki`：迁移扫描本就全量过数据，顺手外排产出全量 `kv.oki.seg-1` + manifest，使新版本首次 open 即有 Range 能力（否则首次 open 后台重建）；
4. 最后一步原子重写 `bitcask.meta` v5（commit point，中途 kill 则 meta 仍为 v4，重跑幂等）。

**迁移成本远低于 tstamp64**：data file 一字节不动（ord 本来就在盘上），只重写 hint（派生）+ meta（18B）。停机窗口 ≈ 一次全量数据顺序读。

---

## 8. 诚实的难点

1. **完整性不变量靠纪律不靠结构**：漏挂一个写入点（未来新增的写路径）= range 静默丢 key，且回查过滤只删多不补少。对策：完整性对拍测试常态化（§9），且 memdelta 挂钩点收敛在 keydir 更新成功后的单一函数里。
2. **弱一致 range** 与 `CaskIter` 快照语义并存，API 文档必须讲清楚，否则用户按 fold 语义预期会踩坑。
3. **memdelta 无上界增长**：writer 长时间不 flush（只读打开、flush 失败）时 memdelta 膨胀。对策：字节上限 + 超限强制 flush；只读打开不建 memdelta（range 只用 runs + 打开时 tail 重放的一次性 delta）。
4. **run pin 与 unlink 竞态**：归并删旧 run 与在途 range iter 的生命周期，需要引用计数或延迟删除（`set_pending_delete` 模式可参考）。
5. **flag-day 的一次性纪律**：hint 同步点必须一次列全（§3.4 清单）——tstamp64 的教训是宽度散布 8 个格式、漏一处即数据错读；本次面小但性质相同，merge 写 hint 与恢复读 hint 两端尤其要对拍验证（BCH5 round-trip + merge 后 hint 快路径恢复 ord 等价性）。

---

## 9. 验证矩阵

| 维度 | 手段 |
|---|---|
| 正确性对拍 | 三方对拍惯例：`range(lo,hi)` vs「全表 `CaskIter` + 过滤 + 排序」vs「brute-force 影子 std::map」，随机写/删/merge/crash 交错的属性测试 |
| 完整性不变量 | 每轮对拍后校验 §2.2 性质 3（OKI key 集合 ⊇ keydir 活 key 集合） |
| crash 注入 | flush/manifest 提交各阶段 kill；重启后 run/manifest CRC 拒收 → 重建 → 对拍通过 |
| 并发 | TSan 树：writer 持续写 + N 个 range iter + merge 并发；ASan 树跑同套件 |
| 双树 API | build-rel 编 bench 抓 API 破坏（既有惯例，新增 `RangeOptions` 等公共结构体须双树验证） |
| 性能基线 | 新增 `bench/range_bench.cpp`：prefix scan OKI vs 现 O(全表) 路径（预期数量级差）；`keydir_bench` 加 put 挂钩前后对比（预算 ≤3%）；启用并扩展 RSS 探针（现 `DISABLED_S30RssProbe` 模式）量化 memdelta/重建峰值 |
| 回归 | get 路径零改动，跑既有全套件（641 ctest）确认零回归 |

---

## 10. 分阶段落地（S33 梯队）

| 阶段 | 内容 | 交付判据 |
|---|---|---|
| S33-1 | 量化探针：`key_length_histogram` 接诊断出口；range/O(全表) 基线 bench；目标负载 RSS 剖面 | 有数据决定 Level B 门禁参数 |
| S33-2 | **flag-day 基建**：hint BCH5（读写两端 + merge 写端 + 恢复读端，删 BCH4 读端）+ meta v5 门禁 + `bitcask_migrate hintord`（含 `detect` 纪元识别） | §3.4 同步清单逐项勾销；迁移幂等 + kill 注入测试；hint/fold 两路恢复 ord 等价性对拍 |
| S33-3 | run 格式（BCOK v1）+ writer/reader + seek + CRC，单元测试 | 格式 round-trip + 损坏拒收 |
| S33-4 | memdelta + 写挂钩 + flush + manifest + open 恢复 + `--prebuild-oki` | crash 注入套件过 |
| S33-5 | `make_range_iter` + k 路归并 + keydir 回查 + 三方对拍 | 对拍 + 完整性不变量测试过 |
| S33-6 | C API（range + 补 prefix 缺口）+ 值预取 + `range_bench` + 文档（api-cpp/api-c/format/migrate-le 同步） | 双树验证 + bench 入基线 |
| S33-7 | （门禁评审）Level B 立项与否，依 S33-1 数据 | 单独设计文档 |

S33-2 是发布边界：它单独构成 5.1.0 的盘上 flag-day（可先发，OKI 功能后续小版本跟进），也可与 S33-3..6 合并一次发布——取决于停机窗口安排，倾向合并（一次停机拿到完整 Range 能力）。

---

## 11. 开放问题

1. 快照一致 range（与 fold 屏障组合）是否有真实需求？首版弱一致，等用户场景。
2. `reverse` 迭代：格式已支持（块索引可倒序），API 首版是否暴露？
3. memdelta 数据结构选型：`std::map` / 自研 btree / append+惰性排序——等 S33-4 用 put 回归预算实测定，不预设。
4. run 归并是否搭 merge 的车（merge 本就全量遍历 live 数据，可顺手产全量 run）：倾向做，作为 S33-4 的加分项而非依赖。
5. ~~hint v5 加 ord~~：已定（§3.4）——随 flag-day 落地，不再是开放问题。

> 设计基线：一次 flag-day（hint BCH5 + meta v5，`bitcask_migrate hintord` 停机迁移，与 tstamp64 同模式）换取零兼容矩阵；OKI 文件本身仍是派生缓存，不进门禁。Level B 有数据门禁，不预支复杂度。
