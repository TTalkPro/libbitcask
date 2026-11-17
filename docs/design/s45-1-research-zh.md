# S45-1 调研记录：索引管线全局顺序假设盘点

日期：2026-11-17 · 基线 HEAD `4d3b506`（7.0.3）· 配套设计
[`designs/index-pipeline-unordered-design-zh.md`](../../designs/index-pipeline-unordered-design-zh.md)

本文记录 S45-1 逐项核对的结论，以及它们如何决定了 S45-2～S45-5 的实现选择。

## 1. 全库顺序假设清单

| # | 位置 | 假设 | 判定 | 处置 |
|---|---|---|---|---|
| 1 | `IndexPool` reducer / reorder ring | 应用严格按 ord +1 | 随管线删除 | S45-4 删除 |
| 2 | `Index::put_doc`（`index.cpp`） | 无条件覆盖：后到即最新 | 需改 | S45-2：ext 已有 ord ≥ 新 ord → 拒收（同 ord 幂等 no-op，保留既有行的 doc_len/meta） |
| 3 | `Index::remove` | 无条件删除 | 需改 | S45-2：只删 ord < 墓碑 ord 的当前版本 |
| 4 | `DocmapRelocator::on_relocate`（`cask.cpp`） | `get` → `put_doc` 两步 | **既有竞态** + 需改 | S45-2：新增 `Index::relocate(key, ord, loc)` 原子 CAS（当前版本 == ord 才改定位）。旧实现两步之间 reducer 应用了更新版本时会把它改回旧版本——这是 7.0.3 已存在的竞态 |
| 5 | TextPlugin LSN 守卫（`apply_text_in` / `apply_job_impl_in`） | 「已有 ord **>** 我才跳过」 | 需改 | S45-2：改为 ≥（同 ord 重放幂等）。旧语义下同 ord 重放会「重加 + 标死旧副本」——**每次重开都全量重新分词并重写段**（实测 20 万文档重开 1.6 s，段数逐次 +1） |
| 6 | `TextPlugin::rebuild_key_locations` | 段序 + 段内 docid 序 == LSN 序，后写直接覆盖 | 需改 | S45-2：同 key 多份 live 副本按 LSN 裁决，败者标死 |
| 7 | `TextPlugin::flush_building_slot` | 段内尾行即最高 LSN | 需改 | S45-2：逐行取最大 |
| 8 | `HnswIndex::insert` | `ord <= max_inserted_ord_` 即丢弃（单调水位门） | 需改 | S45-2 移除；终版：图无判重门，向量插件以 key + LSN 定位表裁决（`detail/vector_key_loc.hpp`） |
| 9 | `SealedSegmentVectorPlugin::insert` | `ord <= replay_gate_` 即丢弃 | 需改 | 同上，共用 key + LSN 定位表；原 `<=` 还会误丢 ord == 水位的新事件 |
| 10 | HNSW 图文件（`serialize` / `deserialize`） | 节点 ord 随 id 严格递增，头部最大 ord = 尾节点 ord | 需改（见 §4） | S45-4：头部写真实最大值；载入不再要求递增 |
| 11 | 恢复重放 `flush_recover`（`cask_recovery.cpp`） | 插入序 == fold 序；单线程应用 | 需改 | S45-5：重放走分片执行器，正确性由 ord 判重承担 |
| 12 | checkpoint 覆盖水位（各 RunFn） | 提交时刻 `peek_next_ord()` | **既有高估** + 需改 | S45-3：执行时刻的 LSN 连续完成前缀。旧取法会把「RunFn 自身 ord 之后、peek 之前」被并发写分配的 LSN 计入覆盖 |
| 13 | `IndexPool::flush` 读屏障 | `in_flight==0 && applied ≥ hwm` | 需改 | S45-3：等 LSN 前缀 ≥ 入口时刻的下一个待分配 LSN（持续写入下不饿死） |
| 14 | 段归并 `merge_segments_v2` | docid 重编 | 不依赖 LSN 序 | 保留（输出 hi_lsn 本就逐行取最大） |
| 15 | 段内倒排 `InvertedIndex::add_doc` 水位门 | 段内 **docid** 单调 | 不依赖 LSN 序 | 保留（docid 是段内本地序号，单写者分配） |
| 16 | DiskANN `lower_bound` | 按距离排序 | 与 ord 无关 | 保留 |
| 17 | docmap 检查点增量链（`docmap_ckpt.cpp`） | delta 行取 `for_each_live_in(from, wm)`；删除日志全量写出后清空 | 不依赖应用序（见 §2） | 保留 |

## 2. docmap 乱序语义（设计 §8 第 1 项）

- **行以 LSN 为键**：`live_` / `meta_` 都以 ord 为键，覆盖写在写路径同步淘汰旧 ord。乱序下只需 2/3 两条守卫即可保证终态。
- **`set_doc_len` / `set_meta` 按 ord 寻址**：行不存在（已被更新版本替换）即 no-op——天然乱序安全。
- **delta 行**：`save_docmap_delta` 用 `for_each_live_in(from, watermark)` 扫 `live_`，并不依赖 `delta_live_` journal。水位 = 暂停时刻 LSN 前缀 ⇒ `[from, wm)` 全部已应用，行集完整；`≥ wm` 但已应用的行留给下一个窗口（届时仍是 live 且在范围内）。
- **删除日志**：`removals_` 全量写出后清空；包含 `tomb ≥ wm` 的条目无害——载入时按「只删更旧」重放，与之后的 tail 重放幂等。
- **结论**：docmap 不需要改格式，也不需要墓碑驻留。

## 3. 规则 R 的实现选择（设计 §4.1）

- `OkiState::locate(key)` 直接返回 `{status, ord, tomb}`，刚写入的 key 命中 memdelta（内存）。
- 选定 **R-b（权威裁决）为主 + docmap 守卫为辅**：
  - 写任务：`locate` 判 kHit 且非墓碑且 ord 不大于任务 ord 才放行；kMiss / 墓碑 / 更新版本 ⇒ 丢弃；kUnavailable（受控重建中）⇒ 放行，由结构守卫兜底。
  - 删除任务：不查 OKI，只删 docmap 中更旧的版本（更准确：删除到达时更新版本若尚未应用，先删旧版本，新版本到达后再登记）。
  - docmap 不保留墓碑（与 S42 live-only 目标一致）——「删除后更旧的写才到达」由 OKI 裁决挡住。
- **恢复重放期规则 R**：初版关闭（fold 时 OKI tail 未补齐）；终版 search 重放移到 `finish_oki_recovery` 之后，用严格版（OKI 命中、非墓碑、LSN 恰好相等才放行）。

## 4. 盘上格式逐项确认（设计 §8 第 4 项）

| 文件 | 结论 |
|---|---|
| 段文件 / 段清单（`bm25_segments/`、kSegManifest） | 不变 |
| docmap base / delta | 不变 |
| index.manifest / OKI manifest / run | 不变 |
| vec.ckpt / .vec / .qc8（HNSW） | **字节布局不变，校验放宽**：节点 ord 不再要求随 id 递增；头部 max_ord 写真实最大值。S45 写出的图文件若含乱序节点，7.0.x 读端会拒收并退回全量重放重建向量（降级，不损坏数据）；S45 读端可读旧文件 |
| IVF / DiskANN 段 | 不变（建段时按 id 遍历窗口，不依赖 ord 序） |
| vec / ivf / diskann 检查点 | **新增两个段型**（S45-A）：`kVecKeyLoc`（22，base 定位表快照）、`kVecKeyLocDelta`（23，delta 窗口条目）。仅追加、文件版本不动；7.0.x 读端忽略未知段，行为同旧版 |

HNSW 一项是唯一触及「文件内容不变量」的改动。备选方案（保存时按 ord 重排节点 id 并重写 payload）会破坏 `.vec` 的增量追加（S14-2 v2）且改动面大；在串行道内按 LSN 重排插入则重新引入「等某个 LSN」。权衡后选择放宽校验，并在 CHANGELOG 明示降级影响。

## 5. 纪律核对（设计 §4.4）

- **持 `write_mu_` 时的阻塞式索引提交**：`put_batch` / `put_batch_atomic` 的失败分支在锁内 `flush_adds()`。新管线下提交只会被分片队列背压阻塞，分片线程不取 `write_mu_`，不成环；保留现状。
- **独占闭包取 `write_mu_`**：逐个检查 checkpoint / 自动 checkpoint / merge 收尾 / 文本压实 / 向量重建闭包，均不取 `write_mu_`、不提交索引任务、不调读屏障。
- **`maybe_flush_oki_unlocked`**：写线程在锁外调用，与维护闭包无交集。

## 6. 向量插件逐个结论

| 插件 | 并发能力 | 判重 |
|---|---|---|
| `VectorPlugin`（HNSW） | 串行道（单写者） | key + LSN 定位表：该 key 已有 LSN ≥ 本事件即跳过；删除登记墓碑；定位表随向量检查点落盘（`kVecKeyLoc` / `kVecKeyLocDelta`），重开不依赖 docmap；旧检查点回退 docmap 反查 |
| `SealedSegmentVectorPlugin`（IVF / DiskANN） | 串行道 | 同上（sealed 记录 + 窗口节点） |

二者都声明 `accepts_put = 带向量`；删除事件也接收（不改图，只登记墓碑），软删仍经
docmap `is_live` 过滤。

## 7. 基线数据

见 [`s45-6-bench-zh.md`](s45-6-bench-zh.md)（同一基准程序分别链接 7.0.3 与 S45 树）。
