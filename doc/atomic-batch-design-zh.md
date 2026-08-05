# S35：引擎原生原子批（方案 C——kBatchHeader + 跨崩溃 all-or-nothing）

> 接替 [`multikey-txn-impl-design-zh.md`](multikey-txn-impl-design-zh.md)(方案 B)
> 的提交路径:多键事务的原子性下沉进引擎,**消除意图日志的 2-3× 写放大**。
> `TxnCask` 公共接口保留,`commit` 内部改走本机制;`recover` 保留意图重放
> (兼容旧目录遗留 pending)。模式原理文档:[`multikey-txn-zh.md`](multikey-txn-zh.md)。

---

## 1. 核心机制:批头声明区间,完整即提交

### 1.1 盘上形态

```
[kBatchHeader 记录]  [成员1: kDoc]  [成员2: kTombstone]  ...  [成员N]
 └ value 声明: N 条、共 B 字节 ┘└──────── 声明区间,B 字节 ────────┘
```

- **批头**:新 RecordType `kBatchHeader = 2`,标准 record 帧(CRC/type/
  tstamp/ord/ksz/vsz),key 为空,value = `[u8 ver=1][u32 count][u64 span_bytes]`
  (小端)。
- **成员**:**普通 `kDoc` / `kTombstone` 记录**,与单条 put/remove 写出的
  字节完全同构——这是本设计的关键:get/iter/parallel_scan/merge 的读路径
  **零改动**(keydir 只会指向成员,永远不指向批头)。
- 批头 + 全部成员经 `write_buffered` 聚进同一 `batch_buf_`,
  **一次 `flush_batch()` pwrite 落盘**——commit point 与现 put_batch 相同。

### 1.2 提交判定(恢复时)

> **声明区间 [header_end, header_end + B) 完整存在且逐条 CRC 有效 ⟺ 已提交。**

无需批尾 commit marker:头与成员是同一次 pwrite,区间完整即写入完成。
掉电导致的任何撕裂(区间不满 B 字节 / 区间内任一条 CRC 坏 / 区间内出现
嵌套 kBatchHeader)→ **整批不可见,`last_valid_end` 停在批头起点**,
写打开恢复照既有语义截断——等价于「从未写过」。

### 1.3 与三条恢复路径的交互

| 路径 | 处理 |
|---|---|
| **data fold**(`cask_recovery.cpp:377-402` 回调) | 见批头 → 进入 staging:成员**拷贝暂存**(不即时 apply keydir);推进到区间末端 → 依序 apply(put/remove + `advance_ord`,含批头 ord);文件在区间内结束 → 弃暂存(截断由 lve 兜底) |
| **hint 快路径** | **零改动**。不变量:hint trailer 只在干净封口时 finalize,封口 ⟹ 批已提交 ⟹ hint 里的成员条目直接 apply 正确。批头无 hint 条目。torn batch 只可能在未封口 active 文件尾,而未封口 hint 无 trailer → `fold_validated` 失败 → 必走 data fold |
| **`DataFile::fold` 的 lve** | 唯一的 fold 层改动:见批头解析出区间,区间内**不推进** `out_last_valid_end`,推进到区间末端一次性推到位;区间不完整 → lve 停在批头起点后 break(torn-tail 语义)。记录本身仍**流式**交给回调(staging 是恢复消费者的职责,merge 不需要) |

### 1.4 与 merge / migrate 的交互

- **merge**(`merger.cpp::fold_record`):加一行——`kBatchHeader → return`
  (计 stat)。成员是普通类型,走现有路径:墓碑丢弃、kDoc 查 keydir 活性。
  **keydir 即提交状态权威**:未提交成员从未进过 keydir(keydir apply 在
  flush 成功之后)→ 活性检查天然过滤,merge 无需批语义。merge 输出永不
  含批头 ⇒ 合并产物是纯 v5 形态记录流。
- **偶发位腐蚀批头**:merge 场景成员活性由 keydir 判定,不经批判定,
  已提交成员不受批头腐蚀影响(与现状单条记录 CRC 腐蚀同风险面)。
- **migrate**:`hintord` 是 v4→v5,遇不到批头;`detect` 认识 v6。

## 2. meta v6 门禁(懒升级)

老二进制对未知 type 盲转、把批头当活 doc(探查确认 `codec.cpp:89` 无
校验)⇒ 含批记录的目录**必须**挡住 v5 读端:

- `kMetaVersion = 6`;v5 读端遇 v6 → 既有 `unsupported meta version`
  干净拒绝。
- **懒升级**:目录创建仍写 v5;**首次写入批头之前**(同一 `write_mu_`
  临界区内、任何批字节落盘之前)把 meta 重写为 v6。
  语义:**目录含批记录 ⟺ meta ≥ v6**。从不用引擎原子批的目录永远停在
  v5,与 5.1.0 读端完全互通。
- 本库读端:v5 与 v6 都接受(v6 仅表示「可能含批头」)。
- **顺手修**:`write_meta` 现为裸 ofstream(非原子、无 fsync)——升级
  改用 `detail::atomic_write_bytes(fsync_dir=true)`,防 meta 重写中途
  崩溃留半个 meta。

## 3. 新公共 API

```cpp
// cask.hpp
struct BatchOp {
    enum class Type : std::uint8_t { kPut = 0, kRemove = 1 };
    Type type = Type::kPut;
    std::span<const std::byte> key;
    std::span<const std::byte> value{};   // kRemove 忽略
};

// 跨崩溃原子批:整批在崩溃后要么全可见要么全不可见。
// 首次调用把目录 meta 懒升级为 v6(此后 5.1.0 及更老读端拒开,提示见 §2)。
// 批内 op 依序 apply(同 key 多次 = 批内 LWW);durability 同 put_batch
// (o_sync / sync_every_n / caller sync())。
[[nodiscard]] std::expected<void, CaskFault>
put_batch_atomic(std::span<const BatchOp> ops, std::uint64_t tstamp = 0);
```

- 既有 `put_batch` **语义不变**(不写批头、不触发 v6)——存量用户零影响。
- 终于补上批内 REMOVE:墓碑成员用 v0 编码(空 value;v2 shadow 优化不适
  用于批,注释说明)。
- 写路径镜像 `put_batch`(cask.cpp:1700-1884):全批校验 → 懒升 v6 →
  roll(批头+成员总量)→ 批头 ord + `write_buffered(kBatchHeader,…)` →
  成员逐条 alloc_ord + write_buffered → `flush_batch` + 组提交 → hint
  (成员;墓碑带 tomb 标志)→ keydir apply(put/remove)→ 索引任务
  (Add/Delete;`BatchOrdGuard` 覆盖含批头 ord 的 Skip)。
  merge-race(`kAlreadyExists`)重试沿用 put_batch 的 `write_and_keydir`
  路径——重试记录落在区间之外,是独立完整记录,正确。

## 4. TxnCask 重接(方案 B → C)

| 面 | 变化 |
|---|---|
| `TxnCask::commit` | 校验规则不变;实现改为**一次 `put_batch_atomic`**(+按 `TxnSyncPolicy` 补 `sync()`)。不再写意图日志 → 写放大 2-3× → **1×**,`_txn:` 热路径清零 |
| `TxnCask::recover` | **保留意图重放**——处理方案 B 时期目录遗留的 pending(含手写测试构造);正常目录仍 O(0) |
| `pending_txns` | 不变(只剩 legacy 视图) |
| C API `bitcask_txn_*` | 签名与语义不变,内部随 TxnCask |
| S34 测试 | 提交类用例行为超集通过;意图重放类用例继续钉 recover 路径 |

## 5. ord / OKI / 索引一致性

- 批头占一个真实 ord(先于成员分配);恢复 apply 时对批头与成员都
  `advance_ord`,水位不重(S33-B1 教训)。
- 被弃的未提交成员:其 ord 从未进入 keydir/OKI/索引——keydir apply 与
  索引提交都在 flush 成功之后;掉电撕裂时这些 apply 从未发生。截断后
  ord 复用无碰撞面。OKI 极端窗口(数据撕裂但 OKI run 幸存)由既有
  「wm/快照不符 → 全量重建」兜底(派生缓存自愈)。
- 墓碑成员 apply = `keydir_->remove(key, tstamp, ord)`(运行期语义,
  newest 路径,不涉复活门收紧)。

## 6. 测试计划

| 用例 | 覆盖 |
|---|---|
| codec 金测 | `kBatchHeader = 2` 钉死;批头 value 布局手写对拍 |
| data_file fold | 完整批:成员全部流出、lve = 区间末;截断批(resize_file 掐区间中部/掐进批头):lve = 批头起点 |
| cask 崩溃注入 | 正常单条 + 完整批 + 掐尾批 → reopen:单条与完整批可见、掐尾批全体不可见、文件截回批头起点(复用 `resize_file` 确定性手法,不依赖 fork) |
| meta 懒升级 | 首批前 v5、首批后 v6;v6 目录 reopen 正常;`write_meta` 原子性(atomic_write_bytes 落点) |
| merge 交互 | 含批文件 merge:幸存者正确、输出无批头、批墓碑成员正常抵消 |
| TxnCask 回归 | S34 全套 9 用例照跑(commit 走新路径);新增:commit 后目录 meta = v6、无 `_txn:` 记录 |
| hint 快路径 | 含批文件干净封口 → 删 data 重开?(不适用)→ 改为:封口后 hint 路径恢复结果 == data fold 结果对拍 |

## 7. 落点清单与工作量

| 文件 | 改动 |
|---|---|
| `format.hpp` | `kBatchHeader = 2` + 批头 value 布局常量 |
| `codec.{hpp,cpp}` | 批头 value encode/decode helper |
| `data_file.cpp` | fold lve 批区间语义(唯一 fold 层改动) |
| `meta_file.cpp` | `kMetaVersion=6`(读端收 5/6)+ `write_meta` 原子化 + 懒升级入口 |
| `cask.cpp` | `Cask::put_batch_atomic` + `BatchOp` + 懒升 v6 |
| `cask_recovery.cpp` | data fold 回调批 staging |
| `merger.cpp` | 批头 skip + stat |
| `src/cask/txn.cpp` | commit 重接;recover/pending 保留 |
| `tools/bitcask_migrate.cpp` | `detect` 认 v6 |
| tests | 上表 §6 |
| 文档 | format-zh §record、multikey-txn 系、api-cpp/api-c、CHANGELOG |

执行序:S35-1 格式+fold → S35-2 meta v6 → S35-3 写路径+恢复+merge →
S35-4 TxnCask 重接+文档。版本:并入 5.1.0(未发布)条目,标注 v6 懒升级
语义;`SOVERSION` 保持 5。
