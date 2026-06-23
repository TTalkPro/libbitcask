# 设计评审稿：异步索引 MapReduce 流水线（全局双池）

> 状态：**评审中**（未实现）
> 作者：—　日期：2026-06-23
> 关联：TASK.md S1（否决）/ S3（恢复期已验证的并行 analyze 先例）
> 一句话：把「每库一个串行 worker」改为「**全局共享 Map 池（并行分词）+ 全局共享 Reduce 池（per-库串行 apply）**」，解决热点库吞吐瓶颈 + 库数膨胀导致的线程爆炸。

---

## 1. 问题陈述

当前每个 `Cask` 实例各持一个 `IndexPool`，内部只有**一个** worker 线程（`thread_pool.hpp:249` `std::thread worker_;` 单数；构造参数 `concurrency` 被 `(void)concurrency` 丢弃）。该 worker 串行执行 `on_write_fields`，内部把两件本质不同的事**焊在一起**：

1. **analyze**（`search_layer.cpp:364` `analyze_with_positions`）—— CPU 重，纯函数；
2. **insert**（`add_doc` / `index_.put_doc` / HNSW 插入）—— 改共享索引，必须串行。

由此产生两个生产问题：

| 问题 | 触发条件 | 根因 |
|---|---|---|
| **P-吞吐**：热点库索引吞吐被单 worker 锁死 | 个别库持续高频 put | analyze（CPU 大头）被绑在串行车道上，吃不到多核 |
| **P-膨胀**：线程数随库数线性增长 | 同时 open 大量库（数量不定） | 每库一个常驻 worker，大多阻塞空转在 `queue_.pop()` |

`S1`（批量入队）只优化 producer 侧 atomic RMW，**碰不到消费端单线程瓶颈**，已否决。本设计是 S1 的正确替代。

---

## 2. 目标 / 非目标

**目标**
- G1：热点库的 analyze 并行化，吃满多核。
- G2：索引线程总数与**库数量解耦**（恒定 ≈ 2×核数）。
- G3：与现串行 worker **字节等价**的索引结果（可对拍验证）。
- G4：不削弱任何现有正确性不变量（LWW、墓碑序、durability、checkpoint 一致性）。

**非目标**
- N1：不碰 data WAL 写路径（put 仍每条 immediate pwrite，⑪ 红线不变）。
- N2：不改 `put_doc` 单写线程契约（见 §4-I1）。
- N3：不追求 insert 阶段并行（HNSW 单写者 + 索引锁，物理不可能；且 insert 极轻）。
- N4：本期不引入跨进程/分布式。

---

## 3. 现状事实清单（带出处，评审请逐条核对）

| # | 事实 | 出处 |
|---|---|---|
| F1 | `put_doc` 无写锁；写路径单线程是**调用方契约**，库内不强制 | `cask.hpp:11-16`、`data_file.hpp:20/87/102` |
| F2 | `ord = alloc_ord()` 在 put_doc 的**调用线程**上分配，紧接着 `submit` | `cask.cpp:1588 → 1621` |
| F3 | `alloc_ord` = `next_ord_.fetch_add(1)`，密集递增 | `keydir.cpp:333-334` |
| F4 | **单写线程下 submit 顺序天然 == ord 顺序** | F1 ∧ F2 ∧ F3 推论 |
| F5 | `index.put_doc`/`remove` 是**到达序 LWW**（无条件改指 ext2ord，清旧 ord live），**非 ord 比较** | `index.hpp:83-92` |
| F6 | delete 也走 IndexPool，带 ord（`IndexOp::Delete`） | `cask.cpp:634` |
| F7 | analyze 是纯函数（仅读 const 配置；cppjieba `Cut` const 线程安全） | S3 已论证并 TSan 验证 |
| F8 | insert 取 `unique_lock`（put_doc/remove/set_meta 同锁） | `index.hpp:86/92/98` |
| F9 | `flush()` 被 4 处依赖：close、checkpoint save、merge-rebuild、search | `cask.cpp:694/1853/1869`、`cask.hpp:436` |
| F10 | `IndexTask` 已携带 `op/buf(key⧺text)/ord/fields/vec/meta/...` 全部 insert 所需字段 | `thread_pool.hpp:60-107` |

**关键耦合（F4 ∧ F5）**：当前正确性 = 「单写线程让到达序 == ord 序」⇒「到达序 LWW 恰好等价 ord 序 LWW」。**一旦 analyze 并行化，完成顺序 ≠ ord 顺序，F5 立即失效**（delete 可能先于其要盖的 put 被 apply → key 复活，S3 实测过）。这是本设计所有复杂度的来源，也是评审的核心。

---

## 4. 不变量（设计必须维持）

- **I1 单写线程契约不变**：put_doc 仍由调用方保证单线程。本设计**不放宽**它（放宽需给整条写路径加锁，超出范围）。
- **I2 ord 序 apply**：同一库内，index 的可见状态必须等价于「严格按 ord 升序逐条 apply」。
- **I3 HNSW 单写者**：任一时刻最多一个线程改某库的 HNSW。
- **I4 durability 不变**：data WAL 每条 immediate pwrite + 既有组提交，索引异步性不影响已落盘数据。
- **I5 flush 契约**：`flush()` 返回后，**该库**所有已 submit 的索引事件已完全 apply（用于 checkpoint/search/merge/close 追平）。

---

## 5. 架构

```
  put 线程（单写/库，I1）
    ├─ 写 data WAL（immediate pwrite，不动）
    └─ enqueue MapJob{ lib_id, ord, op, raw fields/key/vec/meta }
                         │
                         ▼
   ┌──────────────────────────────────────────────┐
   │  全局 Map 池  (analyze)                         │   N = hardware_concurrency
   │  纯函数并行；所有库共享；无状态                  │   线程数与库数无关 → G2
   │  out: ReduceJob{ lib_id, ord, op, term_data,   │
   │                  field_lens, vec, meta, loc }  │   完成顺序乱序
   └──────────────────────┬───────────────────────┘
                          ▼  路由到 lib_id 对应车道
   ┌──────────────────────────────────────────────┐
   │  全局 Reduce 池                                 │   M 线程（≈核数）
   │  per-库 reorder buffer：按 ord 收齐             │   同一 lib 同时只 1 个在 apply
   │  per-库串行令牌：apply 临界区（HNSW+索引锁）    │   （I2 ∧ I3）
   └──────────────────────────────────────────────┘
```

**两个池都全局共享、进程级单例**（或挂在某个 registry 上，所有 Cask 引用同一对）。线程数 = N + M ≈ 2×核数，**与库数量无关**。

---

## 6. 数据结构演化

### 6.1 任务拆成两级

`IndexTask`（现 `thread_pool.hpp:60`）拆为：

```cpp
// Map 池输入：原始数据 + 定位 + ord。等价于现 IndexTask 去掉「已分词」语义。
struct MapJob {
    LibId                lib;        // 路由用，新增
    IndexOp              op;
    std::string          buf;        // key ⧺ text（沿用现合并存储）
    std::uint32_t        key_len;
    std::uint64_t        ord;
    std::vector<std::pair<std::string,std::string>> fields;  // 多字段原文
    std::vector<float>   vec;        // 透传，Map 不碰
    std::vector<std::byte> meta;     // 透传
    DocLoc               loc;        // file_id/offset/total_sz/tstamp/doc_len
};

// Map 输出 / Reduce 输入：已分词，apply 只做内存写。
struct ReduceJob {
    LibId                lib;
    IndexOp              op;
    std::string          key;        // 拆出 owning key（apply 要用）
    std::uint64_t        ord;
    // 已 analyze + catch-all 合并后的「每字段 term_data」+ field_lens
    std::vector<FieldTerms> per_field;   // {field_name, TermPositionsMap}
    std::vector<std::pair<std::string,std::uint32_t>> field_lens;
    std::uint32_t        total_doc_len;
    std::vector<float>   vec;
    std::vector<std::byte> meta;
    DocLoc               loc;
};
```

### 6.2 把 catch-all 合并下推进 Map（重要优化）

`on_write_fields`（`search_layer.cpp:358-388`）的 catch-all 跨字段合并**只读分词结果、只写本地变量**，不碰共享索引。**把它整段移进 Map 阶段**，使 Reduce 的串行临界区压到最小——只剩：

```
for each FieldTerms: field_index(f).add_doc(ord, terms);
index_.put_doc(key, ord, slot);           // 或 op==Delete → index_.remove
[op!=Delete] index_.set_meta / on_vector(HNSW)
ord_field_lens_[ord] = field_lens;
```

Reduce 阶段无任何 analyze、无 NFKC、无 codepoint —— 纯锁下内存写。

---

## 7. 关键机制

### 7.1 ord 序重建：per-库 reorder buffer（策略 A）

**选定策略 A（保留到达序 LWW 语义，不改 index 核心）**，理由见 §10 对比。

每库一个 reorder buffer：
- 状态：`next_apply_ord`（下一个应 apply 的 ord）+ `std::map<ord, ReduceJob> pending`（乱序到达的暂存）。
- Reduce 线程拿到某 lib 的 `ReduceJob` 后，若 `job.ord == next_apply_ord` → 立即 apply，然后连续 drain `pending` 中 `next_apply_ord+1, +2...`；否则塞进 `pending` 等待。
- apply 全程持 per-库串行令牌（§7.3），保证 I2 ∧ I3。

> 因为单写线程（F4），ord 流密集递增，buffer 逻辑退化为「等下一个连续 ord」，无需处理多生产者乱序——比 S3 还简单。

### 7.2 ord 空洞 → skip-marker

并非每个 alloc 的 ord 都进索引（`cask.cpp:1518/1528` 纯 KV / search 关闭时 alloc 了 ord 却不 submit）。reorder buffer 等连续 ord 时会**永久 stall**。

**解法**：单写线程在**每次 alloc_ord 后都发一个事件**——真任务，或 `IndexOp::Skip{ord}` 轻量 marker。Reduce 收到 Skip 等同「该 ord 已 apply」，直接推进 `next_apply_ord`。因发生在单写线程上，配对 trivial、无并发。

> 备选：让 reorder buffer 支持「带超时/水位跳过」，但引入时间语义和不确定性，**不推荐**——skip-marker 确定性更强。

### 7.3 per-库串行令牌

Reduce 池是共享线程，但同一库的 apply 必须串行（I2 ∧ I3）。每库一把 `std::mutex apply_mu`（或一个 `atomic_flag` 令牌）：
- 库**间**并发：库 A、库 B 的 apply 在不同池线程同时跑；
- 库**内**串行：同一库的 apply 排队，一次一个进临界区。

reorder buffer 的 `next_apply_ord`/`pending` 也由该锁保护。

### 7.4 flush 语义重定义（最易漏的正确性点，F9/I5）

现 `flush()` = 「单 worker pending_ 归 0」。双池后 `flush(lib)` 必须保证**该库**：
1. Map 池中属于该 lib 的所有 job 已完成（产出 ReduceJob）；
2. 该 lib 的 reorder buffer 已全部 apply（`pending` 空 ∧ `next_apply_ord` 追上已 submit 的最大 ord）。

实现：per-库维护 `submitted_ord_hwm`（已 enqueue 的最大 ord）+ `applied_ord`（reorder buffer 已 apply 到的连续 ord）。`flush(lib)` 在 cv 上等 `applied_ord >= submitted_ord_hwm`。Map 池无需单独等——只要 Reduce 追平，Map 必然已完成（apply 是 Map 的下游）。

**4 个依赖点逐一复核**：
- close（694）：`flush(lib)` 后再 stop，排空该库在途。
- checkpoint save（1853）：flush 后 bm25/sidecar 覆盖必 ≥ keydir 快照（既有契约不变）。
- merge-rebuild（1869）：RebuildHnsw 仍走 Reduce 串行车道（它本就是 apply 类操作）。
- search（cask.hpp:436）：查询前 flush 保证可见性——语义不变，只是等待对象从「单 worker」变「该库 reorder buffer 追平」。

### 7.5 背压（双队列）

两处都要有界，防内存爆：
- **Map 池入队**：有界，满则阻塞 put 线程（与现 `queue_capacity=10240` 同款，自然减速写入）。
- **reorder buffer**：`pending.size()` 上限（in-flight ord 数）。某条 doc 分词慢会造成队头阻塞、pending 堆积；超阈值则**反压 Map 池出队**（Map 暂停产出该 lib 的新 ReduceJob）。

> 队头阻塞是策略 A 的固有代价（§10）。评审需接受：单条超大文档分词会暂时拖慢其后该库的索引可见性（不影响 durability，data 已落盘）。

### 7.6 墓碑序——reorder buffer 自然解决

delete 带 ord 走同一 Map→Reduce 流水线（F6）。因 Reduce 严格按 ord 升序 apply，墓碑（ord=K）必然在它要盖的 put（ord<K）之后 apply。**无需 S3 那种「墓碑前强制 flush」hack**——按 ord 重排已天然保证。这是策略 A 相对 S3 的简化收益。

---

## 8. 生命周期与池所有权（D2 定稿：registry 级 + registry 强制）

- **池归属 = registry 级**：双池挂在 `KeyDirRegistry` 上，**同一 registry 的所有 Cask 共享一对池**。registry 是调用方拥有、有明确创建/销毁时点的对象 ⇒ 池随 registry 析构而干净回收，**规避进程级 static 单例的初始化/析构顺序坑**。
  - 典型生产：每进程/NIF 实例 1 个 registry（`cask.hpp:307`）⇒ 线程数 = N+M 恒定，等价进程级单例。
  - 多 registry（少见，K 个）⇒ 线程数 (N+M)×K，随 **registry 数**（极小，1~2）而非库数增长——仍解 G2。
- **registry 强制（决策升级，见 §14-D2）**：`open()` **移除 `=nullptr` 默认值** + 运行期 null 校验返回 `CaskError`。**无 nullptr fallback**——既消除「无 registry 的 Cask 池放哪」的设计分叉，也让池所有权链路单一。代价：152 处测试调用点需显式传 registry（见 S6-P0-pre）。生产 host 本就传 registry，零影响。
- **LibId 注册**：Cask open 时向所属 registry 的 Reduce 池注册 lib，分配 reorder buffer + apply 锁 + cv；close 时 `flush(lib)` → 注销 → 释放该 lib 状态。
- **崩溃/异常**：apply 抛异常仍走现「best-effort 丢弃本次更新、保活池」策略（`cask.cpp:655`），但要保证 `next_apply_ord` 仍推进（否则 reorder buffer 在该 ord 处永久 stall、后续全堵）——**apply 失败也必须推进 `next_apply_ord`**（记一次 metric）。
- **open 失败回滚**：Cask 注册到池后若 open 失败，回滚需注销 lib（对齐现 `index_pool_` RAII 自管，`cask.cpp:485`）。

---

## 9. 正确性论证

1. **I2（ord 序 apply）**：reorder buffer 严格 `next_apply_ord` 递增 + skip-marker 填洞 ⇒ apply 序 == ord 升序。∎
2. **F5 LWW 仍成立**：apply 序 == ord 序（I2）⇒ 到达序 LWW 等价 ord 序 LWW，与现状逐字节一致。∎
3. **I3（HNSW 单写者）**：per-库 apply 锁 ⇒ 同库同时仅一个 apply ⇒ HNSW 单写。∎
4. **墓碑序**：§7.6。∎
5. **I5（flush）**：§7.4 水位等待。∎
6. **库间隔离**：不同 lib 的 reorder buffer/锁互不共享；Map 纯函数无共享可变态（F7）⇒ 库间无数据竞争。∎

### 9.1 乱序敏感性定位：唯一脆弱点是 `ext2ord_` 的到达序 LWW

> 本节是 reorder buffer（策略 A）存在理由的**精确边界证明**——它守的不是「整个 apply」，而是 `Index::put_doc`/`remove` 里**一行** `ext2ord_` 改指。审阅者据此可判断临界区可以多小。

逐字段核对 `Index` 在 apply 阶段写的所有可变状态（`src/keydir/index.cpp`），按「对 apply 乱序是否敏感」分类：

| 状态 / 操作 | 出处 | 乱序敏感？ | 论证 |
|---|---|---|---|
| `next_ord_ = max(next_ord_, ord+1)` | `index.cpp:69/99` | **否** | `max` 幂等且可交换：先 apply ord=1000 再 apply ord=5，结果 `next_ord_` 同为 1001。与 apply 序无关。 |
| `ensure_capacity_locked(ord)`（resize `live_`/`doc_lens_`/`meta_blobs_`） | `index.cpp:43-58` | **否** | 只按 ord 把平坦数组撑大；高 ord 先到只是提前扩容，纯空间、不改语义、不出错。 |
| `chunk->slots[si] = slot` / `ord2ext[si]` / `doc_lens_[ord]` / `meta_blobs_[ord]` | `index.cpp:89-93` | **否** | 下标即 ord，**每个 ord 是独占槽位**，不同 ord 写不同地址，互不覆盖。乱序写各自的格子，最终态相同。 |
| `live_[ord] = true`（**新** ord 置位） | `index.cpp:92` | **否** | 同上，ord 独占槽位；自己这格只被自己这次 apply 置位。 |
| **`ext2ord_[ext_id] = ord`（改指当前版本）+ 清 `live_[old_ord]`** | `index.cpp:76-87` / `remove:101-111` | **是（唯一）** | `ext_id`（按 key 而非 ord 索引）被多个 ord 争用，**无条件**改指最后 apply 的那个、并清掉它认为的「旧」ord 的 live 位。**无 `if (ord > old_ord)` 比较** ⇒ 谁后 apply 谁定胜负。 |

**推论（reorder buffer 的精确职责）**：除最后一行外，所有 apply 写都对乱序免疫（幂等 max / 独占 ord 槽位）。**唯一**因乱序而错的是 `ext2ord_` 的到达序 LWW —— 它把「同一 key 的多个 ord 版本」折叠成「当前存活版本」，而折叠规则是「最后 apply 者赢」。一旦 apply 序 ≠ ord 序：

- put(k, ord=5) 与 delete(k, ord=6) 若乱序成 6→5，则 `ext2ord_[k]` 终值指向 ord=5、`live_[5]=true` ⇒ **被删的 k 复活**（§7.6 / AT2 实测的故障）。

reorder buffer（I2）正是把 apply 序拗回 ord 序，使这一行的「最后 apply」== 「最大 ord」，从而到达序 LWW 退化为 ord 序 LWW、与现状逐字节一致（衔接论证 2）。

**对策略选择的启示**：
- **策略 A** 用「全 apply 串行 + ord 序」覆盖此脆弱点——简单、临界区= 整个 reduce_apply，但够轻（纯锁下内存写）。
- **策略 B** 若实施，只需把这**一行**改成 ord 比较 LWW（`ext2ord_` 仅当 `ord > 该 key 含墓碑水位` 才改指 + 清旧），其余写本就乱序安全 ⇒ apply 可完全乱序、无 reorder buffer。代价是 `ext2ord_` 需扩成「ord + 是否墓碑」并对每 key 维护单调水位（防止已 apply 的高 ord 删除被迟到的低 ord put 翻案）。本节同时给出了 B 的**最小改动面定位**，供 D1 决策参考。

---

## 10. 策略 A vs B（评审需确认选 A）

| 维度 | **A. reorder buffer（推荐）** | B. 改 index 为 ord 比较 LWW |
|---|---|---|
| index 核心语义 | **不变**（到达序 LWW） | 改：put_doc/remove 仅当 ord > 该 key 水位才生效 |
| apply 可乱序 | 否（严格 ord 序） | 是（任意序，只需库内锁） |
| 队头阻塞 | **有**（慢分词拖后续可见性） | 无 |
| 新增状态 | per-库 reorder buffer | per-key「含墓碑 max ord」水位 |
| 改动面 | 局部（新增一层） | 触碰 index 核心不变量 + 全套乱序测试 |
| 风险 | 低 | 中 |
| 对拍模板 | S3 `S3BatchedRecoveryMatchesSerial` 直接复用 | 需新建乱序等价性测试 |

**推荐 A**：不动 index 核心、可直接复用 S3 对拍范式、墓碑序自然成立；队头阻塞用背压 + in-flight 上限缓解，且不影响 durability。

---

## 11. 测试矩阵

| ID | 测试 | 守护的不变量 |
|---|---|---|
| AT1 | **管线 vs 串行字节等价**：同一组 put/delete 混合，双池结果索引 == 现串行 worker（复用 S3 范式） | G3、I2、F5 |
| AT2 | **墓碑不复活**：高位 key 删除穿插，乱序分词下断言被删 key 不可搜（移除 reorder 必复现复活，做成负向护栏） | §7.6 |
| AT3 | **ord 空洞**：纯 KV + doc 混写造空洞，断言 reorder 不 stall（skip-marker 生效） | §7.2 |
| AT4 | **flush 追平**：flush 后立即 search/checkpoint，断言全部已 apply | I5、F9 |
| AT5 | **库间并发**：N 库并发写，TSan 跑，断言零 race + 各库结果独立正确 | §9-6 |
| AT6 | **背压**：堵住 Map 池/塞满 reorder buffer，断言 put 阻塞而非 OOM、释放后追平 | §7.5 |
| AT7 | **慢分词队头阻塞边界**：超大文档夹在中间，断言其后该库可见性延迟但最终一致 | §7.5 |
| AT8 | **崩溃恢复仍正确**：双池下 fork+SIGKILL，重开 fold 一致 | I4 |
| 全量 | TSan 插桩跑 AT1-AT8 + 现有 crash/merge/checkpoint 套件零 race | — |

---

## 12. 改动文件清单（预估）

| 文件 | 改动 |
|---|---|
| `include/bitcask/thread_pool.hpp` | `IndexPool` → 拆 `MapPool` + `ReducePool`（全局单例）；`IndexTask` → `MapJob`/`ReduceJob`；新增 `IndexOp::Skip` |
| `src/search/search_layer.cpp` | `on_write_fields` 拆为 `map_analyze()`（纯，含 catch-all 下推）+ `reduce_apply()`（锁下写）；`recover_*` 复用 map_analyze |
| `src/cask/cask.cpp` | open：注册 lib 到全局池；put_doc/delete：alloc_ord 后发 MapJob 或 Skip；`flush()` → `flush(lib)` 水位等待；close/checkpoint/merge 三处 flush 改调 |
| `include/bitcask/cask.hpp` | `index_pool_` 成员 → 全局池引用 + LibId + per-库 reorder 状态；flush 契约注释更新 |
| `tests/` | 新增 AT1-AT8（多数可并入既有 crash_recovery / search / checkpoint 套件，省 CMake 改动） |
| `bench/` | 扩展 `index_pool_bench`：双池吞吐 + 热点库多核加速比 |

---

## 13. 分阶段实施（每阶段独立可验证、可回滚）

1. **Phase 0｜重构无行为变更**：把 `on_write_fields` 拆成 `map_analyze`（纯）+ `reduce_apply`（锁下），仍在**同一线程顺序调用**。全量 ctest 必须逐字节不变。← 把风险最大的逻辑拆分先做掉、单独验证。
2. **Phase 1｜引入 reorder buffer，仍单 worker**：worker 走 `map→buffer→reduce`，但 map 仍同步。验证 reorder + skip-marker + flush 水位逻辑正确（AT3/AT4），结果不变。
3. **Phase 2｜Map 池并行**：analyze 移入全局 Map 池，Reduce 仍 per-库串行车道。跑 AT1/AT2/AT5/AT7 + TSan。← 真正拿到 G1。
4. **Phase 3｜池全局共享化**：把每库一个池收敛为进程级单例双池，LibId 路由。跑 AT5 大规模库数 + 验证线程数恒定。← 拿到 G2。
5. **Phase 4｜背压调优 + bench**：in-flight 上限、队列容量、bench 加速比基线。

> 任一 Phase 失败可停在前一 Phase（前几个 Phase 行为等价于现状，安全）。

---

## 14. 决策记录（2026-06-23 定稿）

- **D1 ✅ 定为策略 A（reorder buffer，按 ord 序 apply）**：Reduce 前插 per-库 reorder buffer，把并行分词的乱序结果排回 ord 升序才 apply ⇒ apply 序 == ord 序，**`index.cpp` 现有「到达序 LWW + 删除即 erase」一行不改即正确**（§7.1）。墓碑序天然成立（§7.6）。
  - **代价**：队头阻塞——单篇慢分词文档会暂时拖住其后整条 ord 序的索引可见性（不影响 durability，§7.5 / D5）。
  - **不选 B 的理由**：B（ord 比较 LWW）虽免队头阻塞，但要改 `ext2ord_` 核心不变量 + per-key 含墓碑单调水位 + 墓碑水位回收 + 全套乱序新测试，**风险中**；A 不碰核心、可复用 S3 字节等价对拍，**风险低**。改索引消费核心的改动优先求稳。
- **D2 ✅ 定为 registry 级 + registry 强制**：双池挂 registry；`open()` 移除 nullptr 默认并校验报错，**无 fallback**（§8）。
- **D3**：Map 池 N=hardware_concurrency，Reduce 池 M≤4（apply 轻 + 库内串行）。**默认值待 P4 bench 校准**。
- **D4**：reorder buffer in-flight 上限（`pending` 上限）默认值——内存 vs 队头阻塞容忍度折中，**待 P4 bench 校准**；超阈值反压 Map 池出队（§7.5）。
- **D5 ✅ 接受**：慢分词致该库索引可见性短暂延迟（A 的队头阻塞）为已知行为（durability 不受影响）。
- **D6 ✅ 单写线程契约 I1 不放宽**：无「库内多线程并发 put」需求。

---

## 附录 A：设计决策备忘——`live_` 平坦数组**刻意**不用 set（勿重复"优化"）

> 留档原因：`live_[ord]=true` 看似留下大量死槽，容易被反复提议「换成只存活 ord 的 `set`」。这是**刻意的空间换时间**决策，不是疏漏。下次再有人提，先读本节。

**现状**：`live_` 是 `std::vector<std::uint8_t>`，**下标即 ord**（`index.hpp:159`）。ord 单调递增、永不复用，每次覆写/删除留下一个 `live_[old_ord]=false` 的死槽。覆写密集负载下死槽很多。

**为什么不能换 set**：`is_live`/`fill_is_live` 在 **BM25 评分内循环**（`inverted.cpp:119/525` 给每个 term 的 posting 批量查存活），快路径是 **AVX2 `vpgatherdq` gather**（`index.cpp:190` `out[i]=live_arr[ords[i]]`）。gather **要求平坦、下标即 ord 的数组**：

- 换 `unordered_set<ord>` ⇒ AVX2 gather 直接报废，每次存活检查从「一次访存」退化为「哈希 + bucket 链走」，发生在查询最热内循环（TASK.md 第一梯队优化的正是此处）。
- 内存上 set 也未必省：`unordered_set<uint64_t>` 每**活** entry ~16–50B（payload + 桶/节点/负载因子）；平坦数组每**死** ord 才 1B。只有存活极度稀疏才省，且要拿查询延迟成倍变慢去换——不划算。

**死槽代价其实很小（分两层内存）**：

| 层 | 结构 | 死 ord 代价 | 回收策略 |
|---|---|---|---|
| 轻量平坦层 | `live_`(1B/ord)、`doc_lens_`(4B/ord) | ~5B/死 ord | **不回收**（故意保平坦给 SIMD） |
| 重数据层 | `slots_`(DocSlot)、`ord2ext_`(ext 字符串 ~2MB/chunk)、`meta_blobs_` | 几十~上百 B/死 doc | **按 chunk 回收**：整个 64K-ord chunk 全死(`live_count==0`)即释放（`index.hpp:61`） |

即真正贵的 DocSlot/ext/meta 走分块回收；不回收的只剩 1B+4B/ord 的平坦小数组（100 万死 ord ≈ 1MB，可忽略）。

**结论**：用 ~5B/死 ord 的浪费，换 O(1) + SIMD 的存活检查。与 `index.hpp:62` 注释「live_ 和 doc_lens_ 保持平坦（SIMD fill_is_live / fill_doc_lens 需要）」一致。**不是没想到 set，是故意没用。**

**与本设计的关联**：`live_[ord]=true` 写的是**独占槽位**（下标=自身 ord），不同 ord 写不同地址 ⇒ 对 apply 乱序**天然免疫**（§9.1）。平坦设计与 MapReduce 并行化不冲突。

---

## 附：与 TASK.md 的关系

- 本设计**取代 S1**（S1 仅 producer 入队批量化，碰不到消费端瓶颈，已否决）。
- 复用 **S3** 的并行 analyze 纯函数性证明 + 字节等价对拍范式。
- 落地后建议在 TASK.md 第六梯队新增条目 **S6「异步索引 MapReduce 流水线」**，并把 S1 标注为「被 S6 取代」。
