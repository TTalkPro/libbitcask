# S36：keydir 磁盘驻留（OKI Level B）设计稿

> 前置阅读：
>   - [`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md)（Level A 定稿；本文是其 §6 预告的 Level B 独立设计文档）
>   - [`keydir-sharding-design-zh.md`](keydir-sharding-design-zh.md)（分片/屏障/MVCC 现状）
>   - [`merge-policy-zh.md`](merge-policy-zh.md)（CAS 搬迁模型——本文要动它的活性权威）
>
> 状态：**设计草案（未实现）**。对应 S36 梯队。
>
> 一句话：哈希 keydir 从「全量权威」退化为「热点缓存」，点查权威变成
> **cache → memdelta → BCOK v2 全字段 runs** 的组合视图；bloom 挡负查询、
> 块 LRU 吃热块。1 亿 `doc:` 形态 key 的 keydir 常驻从 **11 GB → ~1.2 GB**。
> 全程**零 flag-day**：v2 run 仍是派生缓存，新旧二进制互开靠自愈重建。

---

## 0. 门禁结论（S33-7 收口）

门禁判据（Level A 设计 §6）：keydir RSS 占进程 RSS > ~40%。实测
（2026-08-05，`key_length_histogram` 口径，`doc:<n>` 真实形态直灌）：

| 规模 | keydir 常驻 | B/key | 构成 |
|---|---|---|---|
| 1M | 96 MB | 100.7 | key 9.9 + 槽位 83.9 + 桶 16.8 |
| 10M | 1.4 GB | 147.6 | 槽位扩容相位最差点 |
| **100M** | **11 GB** | 118.1 | key 11.9 + **槽位 107.4** + 桶 10.7 |

纯 KV 模式下 11 GB 几乎必然远超 40% 线（value 不占 RSS）——**门禁通过，
Level B 立项**。关键观察：**大头是槽位（`pair<string,Entry>` 80B ×
capacity 余量），不是 key 本身**——把 40B 的 `SingleEntry` 落盘、内存只
留热点，是对症下药。

## 1. 总体形态

```
                写路径（write_mu_ 串行，不变）
                        │
        ┌───────────────┼────────────────────────┐
        ▼               ▼                        ▼
   data file 追加   哈希 keydir 更新          OKI memdelta 追加
   （不变）        （降级为热点缓存，          （行升级为全字段：
                    容量预算 + 逐出）            key + SingleEntry + tomb）
                                                  │ 阈值 flush
                                                  ▼
                                        kv.oki.seg-<gen>（BCOK v2：
                                        全字段行 + 内嵌 bloom）
点查（get）：
   cache 命中(seqlock O(1)) ──────────────→ 命中即权威，走现有读路径
   cache miss → memdelta 点查 → 各 run bloom → 稀疏索引二分
             → 块读（LRU 缓存 / 1 次 pread）→ 行解码 → 值 pread
```

与 Level A 的本质差异一句话：**Level A 的 OKI 允许陈旧、keydir 回查兜底；
Level B 的组合视图自身就是权威**——为此付出的三笔账（merge 搬迁入 delta、
同 ord 冲突判定、活性语义翻转）在 §3 逐笔算清。

## 2. 内存与延迟预算（100M key 目标形态）

| 组件 | 预算 | 依据 |
|---|---|---|
| 热点缓存（哈希 keydir） | 默认 5M 条 ≈ **560 MB** | 118 B/key 实测；容量可配 |
| bloom（全部 run 合计） | **~125 MB** | 10 bits/key，k=7，FP≈1% |
| 稀疏块索引（内存常驻） | **~40 MB** | 4 KiB 块 ≈ 80 行/块 → 1.25M 块 × ~32B |
| 块 LRU 缓存 | 可配，默认 **256 MB** | 热块驻留 |
| memdelta | ≤ 64 MB | 既有 `kFlushByteLimit` |
| **合计** | **~1.0-1.2 GB** | vs 现状 11 GB，**-90%** |

盘上：v2 行 ~45 B/key（前缀差分 key + 全字段 vbyte）→ 单 run ~4.5 GB
（vs v1 620 MB；换来的是免回查的权威定位）。

延迟：**热命中 = 现状**（seqlock 乐观读不动）；冷 miss = bloom(内存) +
二分(内存) + 块读(LRU miss 时 1 次 4KiB pread) + 值读(1 次 pread)——
**最多 2 次 pread**，SSD 上 ~100-200 µs。负查询由 bloom 挡住(99%)。
回归预算门：热 get ≤3%、put ≤3%（memdelta 行加宽 40B）、merge 吞吐 ≤10%、
冷 get P99 ≤ 300 µs（tmpfs 基准另立锚点）。

## 3. 三个核心决策（每个都改自 Level A 的既有结论）

### D1：活性/位置权威 = 组合视图；merge 搬迁必须入 delta

Level A 最优雅的解耦——「merge 与 OKI 零交互」（keydir 挂钩明文跳过
`old_file_id != 0` 的搬迁 CAS，`keydir.cpp:481-486`）——**在 Level B 必须
反转**，原因有二，都是探查坐实的硬事实：

1. **merge 活性判定现依赖哈希表 miss = 死亡**（`merger.cpp:169-175`：
   `keydir_.get(key)` 不匹配 `(in_file_id, offset)` 即 `records_stale` 跳过
   不搬运）。缓存逐出后，**活 key 会被判死、不被搬运、随输入文件 unlink
   丢失**——数据丢失级 bug。merge 的活性判定必须改查组合视图（§7）。
2. run 行存了位置 ⇒ 搬迁后旧位置陈旧且**悬空**（输入文件已 unlink），
   组合视图若无新行可查即返回死指针。搬迁必须产出新行。

搬迁入 delta 的形态：merge `apply_pending` 逐条成功后追加
`(key, SingleEntry{new_loc, 同 ord}, tomb=false)` 到 memdelta。
**顺序不变量**：输入文件 unlink（`cask.cpp:2931-2949`）之前，全部搬迁行
必须已进入 memdelta（内存可见即可，无需落盘——崩溃后数据真源是新输出
文件，fold/hint 重建自然得到新位置）。

### D2：同 ord 冲突用 (ord, 来源序) 判定——**弃用 epoch**

搬迁行与原行 **ord 相同**（merge CAS 保留 `u.ord`，`merger.cpp:97-138`），
Level A 的 max-ord-wins 无法分辨新旧。Level A 设计 §6 预告用 `epoch`,
探查后**否决**：epoch 是会话内 MVCC 计数器（`epoch_.fetch_add`），
**不写入 data/hint 记录**；快照丢失走全量 fold 重建时从头重计——若此时
旧 run 幸存（携带高 epoch 旧行），新行永远输给旧行，静默错位。

替代：**(ord, 来源序) 字典序胜出**。来源序 = run 的 `gen`（manifest 持久、
单调）；memdelta 视作 gen=∞（同 key 同 ord 在 delta 内取后追加者）。
搬迁行必然落在更高 gen 的 run（或 delta）⇒ 新位置胜。链式搬迁
（A→B→C）同理。**免 CAS 的附带收益**：并发用户写 vs merge 搬迁的竞态
由格自然消解——用户写 ord 更大恒胜，搬迁行 ord 旧、只在无更新写时生效，
与现有 CAS 语义等价（§7 详证）。盘上行**不含 epoch 字段**。

### D3：BCOK v2 = 纯派生缓存演进，**零 flag-day**

探查修正了一个 S33-3 记录的偏差：v1 并没有「可选字段区」，只有 per-entry
flags 的 7 个保留位，且**未知位 = 整文件拒收**（`oki_run.cpp:334-337`），
行无长度前缀、不可跳行。因此 Level B 不玩 flags 位扩展，直接
**version 字段 1 → 2**（行结构整体换新，§4）。兼容性矩阵完全靠派生缓存
自愈，**meta 纪元不动**（对比 S35：批头混在 data file 里才需要 v6 门禁；
OKI run 是独立文件、坏了/不认就重建）：

| 场景 | 行为 |
|---|---|
| 旧二进制开 Level B 目录 | v2 run version 不识 → 弃用 OKI → 全量 fold 重建 keydir（全内存，旧行为）+ 重写 v1 run。正确性无损 |
| 新二进制（Level B 开启）遇 v1 run | 无全字段 → 弃用 → 后台全量重建 v2（一次 fold + 外排代价，log_warn 明示） |
| 新二进制（Level B 关闭 = 现状模式） | 读 v1 或 v2 都可（v2 行是 v1 行的超集，range 路径只用 key/ord/tomb） |

### D4：语义翻转——「cache miss ≠ 不存在」

现状 keydir miss 是权威 kNotFound（seqlock 的 `kMiss` 亦然）。Level B 下
miss 只表示「不在热点缓存」，必须继续查 delta/runs。三个连锁调整：

- **删除**：`remove` 照旧写 tomb 行进 delta（既有挂钩）；缓存内条目可
  物理清除（现状的原地墓碑哨兵保留亦可——组合视图命中哨兵即 kNotFound
  短路，反而省一次盘查。墓碑哨兵参与逐出）。S33-B1 复活门在组合视图下
  由行的 ord 全序自然覆盖（行皆带 ord，LWW 与到达序无关）。
- **逐出安全条件**：条目的最新状态已进入 delta 或 run。**恒成立**——
  写路径在 keydir 更新后同步追加 delta 行（挂钩在咽喉点），故任意条目
  随时可逐。逐出 = 物理 erase（复用 swap-delete + limbo），**不是**删除
  语义。策略首版用分片内 CLOCK（省 LRU 链表的 16B/条），容量预算
  `CaskOptions::keydir_cache_entries`（0 = 不限 = 现状模式，默认 0——
  Level B 是 opt-in）。
- **fstats/key_count**：现由插入/删除路径增减（`key_count_++` 等）——
  改为逻辑计数（逐出不减、插入只在「组合视图确认新 key」时加）。
  首版从简：计数继续由写路径维护、恢复期由重建校准，逐出零干预
  （逐出不改逻辑计数，本就正确）。

## 4. 盘上格式：BCOK v2

```
头部 8B: magic "BCOK" u32 | version u32 = 2
数据块区（~4KiB 目标，块内解码状态复位，同 v1）：
  行: [vbyte shared_len][vbyte suffix_len][suffix]
      [vbyte ord_delta（回绕差分，同 v1）][flags u8]
      -- flags bit0 = tomb；bit1 = has_loc（tomb 行无位置字段）
      有 loc 时追加: [vbyte file_id][vbyte total_sz][vbyte offset]
                     [vbyte tstamp_delta（对块内 prev_tstamp 回绕差分）]
稀疏索引区: 同 v1（[count u32] + 每块 [vbyte klen][首 key][off u64]）
bloom 区:  [n_bits u64][k u8][位数组]（按行数 × 10 bits 建，k=7；
           整 run 一个,构建期流式置位）
尾部 32B: [entry_count u64][index_off u64][bloom_off u64]
          [crc u32（覆盖 [0, size-8)）][magic "BCOE"]
```

- 位置字段全 vbyte（file_id/total_sz 小值居多、offset 单调局部性好、
  tstamp 差分小），实测预估 ~45 B/行（`doc:` 形态）；
- **tomb 行不带位置**（bit1=0）——墓碑只需抵消,省 ~20B/行；
- bloom 内嵌而非 sidecar：同一次 `AtomicFileWriter` 原子落盘、同一 CRC
  覆盖，不引入第二个文件的一致性问题；open 时随稀疏索引一并载入内存；
- manifest BCOM **v2**：条目加 `format_ver u8`（区分 v1/v2 run,混布期间
  读端据此拒载/触发重建）；其余不变。

## 5. 查询路径

### 5.1 统一内部点查 `locate(key) → optional<SingleEntry>`

新增 KeyDir 内部原语,get / merge 活性 / TTL 过期检查共用（**一份实现,
杜绝三处各查一遍的漂移**——S34 apply 共用的同款教训）：

```
1. 哈希缓存 probe（现有 seqlock 乐观读 → 加锁回退,不动）
   命中 SingleEntry → 返回（哨兵墓碑 → nullopt 短路）
2. memdelta 点查（mu_ 下二分或辅助哈希,行少,O(log n)）
   命中 → 取该 key 最高 (ord,∞) 行；tomb → nullopt
3. 逐 run（gen 降序）：bloom 试探 → miss 下一个；hit → 稀疏索引二分
   → 块 LRU / pread → 块内线性扫 → 命中即停（gen 降序 ⇒ 首个命中
   即 (ord,gen) 最大者？**不成立**——低 gen run 可能有更高 ord 行?
   不可能：flush 按时序,高 ord 行恒在高 gen run 或 delta。成立,
   块内同 key 多行取末行。命中行 → 回填热点缓存（读升温）
4. 全 miss → nullopt（权威不存在）
```

步骤 3 的「gen 降序首命中即权威」依赖不变量：**行只随时间进入更高 gen**
（flush 时序 + compact 保序,归并输出 gen 取 max+1）——写进 oki_state
注释并测试钉死。

### 5.2 get / CaskIter / parallel_scan / range

- `Cask::get`：`keydir_->get` 换 `locate`,其余（read_file、S13-F5 重试、
  mmap/pread、TTL）零改动;
- `CaskRangeIter`：归并层不变（v2 行是超集）,**回查 keydir 改为回查
  locate**——顺带修复 Level A 的一个隐含限制（range 只能看到缓存内 key
  ——Level B 下缓存是子集,回查必须走组合视图）;
- `CaskIter`（fold 快照）：**快照构成改为「缓存屏障快照 + memdelta 拷贝
  + manifest pin」三元组**。runs 不可变、delta 拷贝原子、缓存走既有
  BarrierGuard——快照时刻之后的写全部不可见,快照语义保住。逐 key 取值
  走 locate 的冻结版（查快照三元组,不回填缓存）。`pin_files` 机制照旧
  （pin 住 fd,merge unlink 不影响快照读）;
- `parallel_scan` 构建在 CaskIter 上,自动继承。

## 6. 写路径

挂钩仍在 KeyDir 咽喉点（Level A 的结构性对策,零逐点改动),行加宽：

- `put` 成功（old_file_id==0）→ `oki_->append(key, SingleEntry{...}, false)`;
- `remove` → `append(key, {ord}, tomb=true)`（无位置字段）;
- **新增**：merge 搬迁 CAS 成功（old_file_id!=0）→ `append(key,
  SingleEntry{new_loc, 原 ord}, false)`（D1;Level A 在此明文跳过,反转）;
- TTL `conditional_remove` → 照旧走 remove 挂钩（现状已覆盖）。

memdelta 行 `DeltaRow{key, SingleEntry, tomb}`（+40B/行）;flush 阈值
沿用（64 MiB 先到）。put 回归预算 ≤3%,超了改 append 环形缓冲。

## 7. merge 交互（全设计最深的一刀）

- **活性判定**（`fold_record`）：`keydir_.get` → `locate`,匹配
  `(in_file_id, offset)` 才搬运。代价：被逐出 key 的活性查询可能触盘
  ——但 merge 本就是后台 IO 任务,且被 merge 文件的 key 大概率冷
  （热 key 早被覆盖写走了）,块 LRU 顺带吃掉局部性;
- **CAS 语义**：缓存内条目照旧 shard 锁下 CAS 更新;缓存外**不需要 CAS**
  ——(ord,gen) 格保证:搬迁行仅当该 key 无更新写时胜出,与「CAS 失败=
  已被覆盖=跳过」语义等价。`relocations_stuck` 纵深防御保留（缓存命中
  路径仍可检测 stuck）;
- **unlink 顺序不变量**（D1）：搬迁行入 delta 先于输入文件 unlink;
- **RelocateEvent 插件广播**不变（搜索侧 docmap 搬迁与本设计正交）。

## 8. 恢复与 checkpoint

- **kv.keydir.ckpt → BCKS v4**：entries 段变为「缓存子集 + 逻辑计数」
  （快照不再承诺全量——Level B 模式下全量在 runs）。v3 读端见 v4 拒收
  → 全量 fold 重建,自愈。Level B 关闭时照写 v3（全量）,双模共存;
- **tail 重放**：沿用 wm 排他语义;fold(data) 与 hint v5 快路径**都能产出
  全字段行**（hint 记录有 tstamp/total_sz/offset/ord/tomb,file_id 取自
  文件名——S33-2 flag-day 的意外红利）,`ord > wm` 喂 delta;
- **全量重建**（runs 损坏/缺失/版本不认）：现 `rebuild` 是全内存
  `std::sort`（`finish_oki_recovery` 物化全部活 key + reserve）——100M 档
  这就是 ~11GB 峰值,**Level B 必须换外排**：fold 流式喂行,每 64MiB 排序
  落临时 v2 run 分段,尾部 k 路归并成单 run。重建期缓存只装热点,峰值
  = 64MiB + 归并缓冲;
- **B1 遗留项在此顺带收口**（backlog B1,checkpoint 跑赢未 fsync 数据）：
  Level B 的 ckpt v4 写入前对 active data file 记录「已 fsync 水位」,
  快照只收录水位内条目——设计上直接消除该暴露面（实现排在 S36-5,
  先以失败注入测试证实）。

## 9. 并发与锁序

- 缓存：现有分片锁 + seqlock 乐观读**不动**（热路径零改）;逐出走
  shard 锁内 erase（swap-delete + limbo,读者安全性同现状）;
- memdelta：沿用 `flush_mu_ → mu_`;点查在 `mu_` 下（行有界:64MiB）;
- 块 LRU：独立 shard 化小锁（或 seqlock 只读快照）,不进 keydir 锁序链;
- run Reader/bloom/稀疏索引：不可变,无锁并发读（现状);
- CaskIter 屏障协议不变（barrier 只管缓存;delta 拷贝在 `mu_` 下原子）。

## 10. 分期（每期独立可交付、可回退）

| 期 | 内容 | 验收 |
|---|---|---|
| S36-1 | BCOK v2 格式（全字段行 + bloom 内嵌 + (ord,gen) 归并）+ BCOM v2 + 外排 rebuild | 单测:roundtrip/seek/bloom FP/损坏拒收/v1 拒载重建;重建峰值内存探针 |
| S36-2 | memdelta 全字段 + 搬迁/TTL 挂钩 + `locate()` **影子模式**：缓存不逐出,每次 get 双查对拍（debug 断言 locate == 哈希结果） | 全量 ctest + 对拍零漂移;put/merge 回归 bench |
| S36-3 | get 接 locate + 块 LRU + 读升温回填 | 冷/热 get bench 锚点;S13-F5 重试语义回归 |
| S36-4 | 逐出（CLOCK + `keydir_cache_entries` 选项）+ CaskIter/parallel_scan 三元组快照 + BCKS v4 | 100M 档 RSS 实测 ≤1.5GB;快照一致性 stress |
| S36-5 | merge 活性/搬迁切 locate + unlink 顺序不变量 + 崩溃注入全套 + B1 fsync 水位 | merge 数据完整性 stress（逐出态下 merge 千轮无丢 key）;ASan/TSan |
| S36-6 | C API 选项透出 + 文档/CHANGELOG + 门禁复测 | 全矩阵 + 双树 |

**风险闸**：S36-2 的影子对拍是整个工程的安全网——组合视图与哈希权威
并行运行、逐条对拍,漂移即断言失败;确认零漂移后才允许 S36-4 打开逐出。
任何一期可独立回退（选项默认关,主线行为 = 现状）。

## 11. 开放问题

1. 读升温回填的抖动（扫描型负载污染缓存）——首版回填加简单频度门
   （二次命中才回填）,数据说话;
2. bloom 常驻 125MB 是否分块惰性载入（首版整载,门禁后评估）;
3. `MultiEntry`（fold 竞态窗口的多版本链）与逐出的交互——首版规定
   **MultiEntry 不可逐出**（fold 活跃期本就短暂,collapse 后恢复可逐）;
4. 双写模式（Level B 开启但缓存不设限）作为长期「影子验证」运维档位
   是否保留。
