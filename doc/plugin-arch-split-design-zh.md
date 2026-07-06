# libbitcask 插件化架构（SearchLayer 拆分后）

本文描述 `search_engine` 子系统在 `S15`（接口层通用化）、`S18`（插件自治与持久化收编）和 `S19`（Cask 直持、SearchLayer 退役）之后的最终形态：`CaskPlugin` 抽象接口把 BM25 文本域、HNSW 向量域与宿主 KV 完全解耦，混合检索作为更上层融合器独立成 `HybridSearcher`，所有事件流、merge 参与、checkpoint 落盘都以 `plugin_api.hpp` 描述的 KV 语义词汇对外暴露。

---

## 1. 目标与边界

整条演化收敛到「**依赖方向反转**」：

- **Cask 只认 `CaskPlugin` 接口**。它把 KV 固有事件（put / delete / merge begin / relocate / merge commit / merge abort / maintain / close-time flush）广播给一组已注册的 `CaskPlugin*`；具体是 BM25 还是 HNSW、Cuckoo filter 还是 TTL manager，对 KV 透明。
- **插件只看宿主服务接口**（`PluginHost`）、只看宿主提供的只读 `DocTable`。它不会接触 `keydir`、`data_file`、`hint_file` 等数据结构，只能通过 `PluginHost::read_at` 取回 record value、通过 `PluginHost::run_serialized` 把需要单写者上下文的副作用投递到 reducer 静止点。
- **混合检索不属于插件**。`HybridSearcher` 同时持 `text::TextPlugin&` 与 `vec::VectorPlugin&`，是上层融合器，部署可以「只装 BM25」「只装 HNSW」「两者都装」中的任意一种，对应不同的目标依赖图。

术语与 `cpp-arch.md`、`concurrency-zh.md`、`format-zh.md` 保持一致：单写者 reducer 由 `IndexPool` 的 `IndexLane` 持有，`ord` 全局单调。

---

## 2. 整体架构

### 2.1 运行时组件图

```
                                  ┌────────────────────────────────────────────┐
                                  │            公共查询门面 (S19-1)              │
                                  │   text::Searcher / vec::Searcher / Hybrid    │
                                  └─┬──────────────────┬─────────────────┬─────┘
                                    │                  │                 │
                          ┌─────────▼────────┐ ┌───────▼─────────┐ ┌────▼────────────┐
                          │ TextPlugin       │ │ VectorPlugin    │ │ HybridSearcher   │
                          │ (BM25 域)        │ │ (HNSW 域)       │ │ (RRF 融合器)     │
                          │ name()="bm25"    │ │ name()="hnsw"   │ │ 算法层，非插件   │
                          │ 实现 CaskPlugin  │ │ 实现 CaskPlugin │ │ 引用前两者       │
                          └─┬────────────────┘ └─┬───────────────┘ └─────────────────┘
                            │    只读消费          │
                  ┌─────────▼────────────────────▼─────────┐
                  │   bm25::DocTable + DocLenWriter         │  ← 宿主服务
                  │   +  CompactionStats（节流统计）        │
                  └──────────────▲─────────────────────────┘
                                 │ 通过 Cask 构造注入（shared_ptr<index::Index>）
              ┌──────────────────┴───────────────────────────────────────┐
              │ Cask（KV 核心：data / hint / keydir / IndexPool / merge） │
              │  ├─ unique_ptr<text::TextPlugin>  text_                   │
              │  ├─ unique_ptr<vec::VectorPlugin>  vec_plugin_            │
              │  ├─ optional<search::HybridSearcher> hybrid_              │
              │  ├─ shared_ptr<index::Index>      docmap_   ← 宿主拥有    │
              │  ├─ vector<plugin::CaskPlugin*>   plugins_   ← 注册表     │
              │  └─ CaskPluginHost plugin_host_     ← 实现 plugin::PluginHost │
              └──────────────────┬────────────────────────────────────────┘
                                 │
                                 │ on_* 事件 / flush / merge span
                                 ▼
                       plugin_api.hpp（CaskPlugin + PluginHost）
```

要点：

- Cask 是 **DocMap 的宿主**：`docmap_` 由 Cask 持有；TextPlugin、VectorPlugin 通过构造期注入的 `const bm25::DocTable&` 借用，生命周期不归自己管。
- `plugins_` 是「**扇出表**」（`std::vector<plugin::CaskPlugin*>`），注册序就是 reducer 的扇出序（text 先、vector 后，镜像原 `SearchLayer::reduce_apply` 的 `add_doc → on_vector` 顺序）。
- `HybridSearcher` 不是 `CaskPlugin`，它只装在「双插件」部署里，因此被隔离成 `bitcask_hybrid` 目标——只装 BM25 的库不链接它。

### 2.2 宿主侧聚合面（`plugin_api.hpp`，自包含）

宿主服务的反向接口只有三个方法，对应插件与宿主的全部窄通路：

```text
plugin::PluginHost
├── optional<string> read_at(RecordLoc loc)   // 按 file_id/offset/size 读回原始 record value
├── void run_serialized(function<void()> fn)   // 投递到 reducer 静止点（同提交序 FIFO）
└── void log(LogLevel, string_view msg)        // warn/error 上报（Cask 转发到 opts.log_fn）
```

`bitcask::Cask::CaskPluginHost` 在 `Cask` 内嵌实现这个接口：

- `read_at` 走 `cask_->read_file` → `DataFile::read`，失败或遇 tombstone 返回 `nullopt`。
- `run_serialized` 包装成 `IndexOp::RunFn` 任务、走 `submit_index_task` 在 reducer 线程执行；有 `IndexPool` 时入队执行，无池（KV 模式理论不可达）直跑。
- `log` 翻译为 `cask_->log_warn/log_error`。

`OpenContext` 把目录、宿主句柄和「上次提交的 chain 状态」一次性注入插件：

| 字段 | 含义 |
|------|------|
| `dir` | 库目录；插件以 `name()` 为前缀自管文件（"bm25" → `bm25.ckpt`，"hnsw" → `vec.ckpt` + `.vec` / `.qc8`） |
| `host` | `PluginHost` 句柄，生命周期与 Cask 同寿 |
| `committed_base_watermark` | manifest 里本组件对应 entry 的 base 水位；与组件 base 不符 → 视为损坏，回退 `.prev` 或自降级 |
| `committed_chain_watermark` | 链累计水位，作为链重放起点 |
| `committed_chain_seq` | 链序号；超界则截断到 manifest 接受的位置 |

---

## 3. CMake 目标依赖图

`bitcask_plugin_api` 是头文件 INTERFACE 库，只暴露 `include/`、`plugin_api.hpp` 自包含；其余搜索目标全部反向依赖它。

```text
                                  ┌────────────────────────┐
                                  │ bitcask_plugin_api      │  INTERFACE / header-only
                                  │ (plugin_api.hpp + ...)  │
                                  └───────────▲────────────┘
                                              │ PUBLIC
        ┌──────────────────────────┬──────────┴────────┬──────────────────┐
        │                          │                   │                  │
┌───────▼────────┐     ┌───────────▼────────┐ ┌──────▼──────────┐   ┌─────▼──────────────┐
│ bitcask_docmap │     │ bitcask_bm25       │ │ bitcask_vector  │   │ bitcask_keydir      │
│ (index::Index) │     │ (InvertedIndex +   │ │ (HnswIndex)     │   │ (KeyDir / MVCC)     │
│                │     │  query / kernels)  │ │                 │   │                     │
└───────▲────────┘     └───────────▲────────┘ └──────▲──────────┘   └─────▲────────────────┘
        │ PUBLIC                   │ PUBLIC          │ PUBLIC              │ PUBLIC
        │                          │                 │                     │
        │ bitcask_text_plugin ◄────┘                 │                     │
        │ (TextPlugin + Analyzer + Cache + Highlighter) │
        │ PUBLIC: docmap + bm25 + text + plugin_api    │
        │                                                 │
        │                       bitcask_vector_plugin ◄───┘
        │                       (VectorPlugin + delta log)
        │                       PUBLIC: vector + format + plugin_api
        │
        │   bitcask_hybrid (HybridSearcher; 双插件部署可选)
        │   PUBLIC: bitcask_text_plugin + bitcask_vector_plugin
        │
        │   bitcask_merge (run_merge 调度 — 仅依赖 plugin_api)
        │
        ▼ PUBLIC
┌────────────────────────┐
│ bitcask_cask           │  Cask handle（CRUD + IndexPool + merge + 搜索门面）
│ PUBLIC: ... bitcask_merge bitcask_hybrid
└────────────────────────┘
```

部署裁剪：

- **纯 KV**：`bitcask_cask` 直接连 `bitcask_keydir + bitcask_fileops + bitcask_io + bitcask_format + bitcask_merge`，无任何搜索目标。
- **KV + BM25**：`bitcask_cask` 链 `bitcask_hybrid`（hybrid 即时降级为仅 text 路径），下游只拉 `bitcask_text_plugin`，不链 `bitcask_vector / utf8proc / cppjieba` 之外的 HNSW 相关物。
- **KV + HNSW**：通过 `CaskOptions` 关 text 入口后，hnsw 仍可独立启用；消费者可主动卸载 `bitcask_text_plugin` 子依赖。
- **完整搜索域**：`bitcask_text_plugin + bitcask_vector_plugin + bitcask_hybrid` 全部链入。

`bitcask_search`（原 `SearchLayer`）作为整体 shim，已在 `S19-3` 降级为测试夹具目标 `bitcask_search_shim`，定义在 `tests/CMakeLists.txt`，**生产代码不再链接它**。

---

## 4. `CaskPlugin` 接口契约

`include/bitcask/plugin_api.hpp` 给出 14 个虚函数：

| 类别 | 函数 | 默认实现 | 纯虚 |
|------|------|---------|------|
| 元信息 | `string_view name() const` | — | ✅ |
| 元信息 | `uint64_t watermark() const` | — | ✅ |
| 生命周期 | `PluginStatus open(const OpenContext&)` | — | ✅ |
| 生命周期 | `PluginStatus close()` | — | ✅ |
| 数据事件 | `on_put(const PutEvent&, PreparedPtr prep)` | — | ✅ |
| 数据事件 | `on_delete(const DeleteEvent&)` | — | ✅ |
| 可选预处理 | `bool wants_prepare() const` | `false` | ❌ |
| 可选预处理 | `PreparedPtr prepare(const PutEvent&) const` | `nullptr` | ❌ |
| 存储维护 | `on_relocate(const RelocateEvent&)` | 空实现 | ❌ |
| 存储维护 | `on_merge_begin(const MergeBeginEvent&)` | 空实现 | ❌ |
| 存储维护 | `on_merge_commit(const MergeCommitEvent&)` | 空实现 | ❌ |
| 存储维护 | `on_merge_abort()` | 空实现 | ❌ |
| 维护 | `maintain(const MaintainEvent&)` | 空实现 | ❌ |
| 持久化 | `FlushResult flush(const FlushRequest&)` | — | ✅ |

下面逐条说明语义与调用上下文。

### 4.1 元信息

`name()`

- 返回插件的**算法身份**（不是查询接口命名空间），例如 `"bm25"`、`"hnsw"`、`"metrics"`。该字符串是持久化 / manifest 识别：Cask 在 `prepare_search` 之前就是用 `component_of_plugin(p->name())` 反查 manifest entry，再把对应的 `committed_*` 注入 `OpenContext`。
- 命名与查询 API 的命名空间（`text::` / `vec::`）是两套词汇：前者按算法命名，后者按领域命名。

`watermark()`

- 插件自报的「已 apply」水位，Cask 据此定**恢复重放起点**：`min(各插件 watermark)` 才是整个恢复重放的前沿，低于此的 ord 不重放。
- 返回 0 ≡「没覆盖任何 ord」；可以是损坏自降级后的产物，也可以是新库的状态。

### 4.2 生命周期

`open(const OpenContext& ctx)`

- 加载自身持久化状态（`bm25.ckpt` / `vec.ckpt` 组件文件），续接链，估算 `watermark_`。
- 损坏 / 缺失 **必须自行降级**：把 `watermark()` 报 0 即可让宿主进入「全量 fold 重放」路径，把 `rebase_needed` 置位让下次 `flush` 写全量 base。这两条是接口契约，宿主不做兜底。
- 一律返回 `PluginStatus::kOk`：失败报告已沉淀到 `watermark()` 与 `rebase_needed` 上。

`close()`

- 含**终止性 flush**（链坍缩、`.prev` 收敛）。`TextPlugin::close` 与 `VectorPlugin::close` 当前均为 no-op + `kOk`——终止性 flush 由宿主的 `save_search_ckpt_paired`（在 `close()` 顶端调用 `force_ckpt_rebase` + 派发 `flush(kClose)`）统一承担。

### 4.3 数据事件（reducer 单写者，ord 严格升序）

`on_put(const PutEvent& e, PreparedPtr prep)`

- 单写者上下文（reducer 线程），所有 put 路径最终都收敛到这一条入口。
- `prep` 是可选 `Prepared`（见 §4.4），由 `prepare()` 阶段跨线程移交过来；`nullptr` 表明该插件不参与 / 准备失败（异常被宿主吞）。
- `e.doc` 为 `nullptr` 表示纯 KV 写，TextPlugin 把 `value` 当作默认字段文本入索引。
- `e.replay` 为 true 时表示这是恢复重放阶段（recover fold 路径），TextPlugin 在 `replay` 路径上让单文本也走 `prepare` 并行分析，与活写路径路由不同。

`on_delete(const DeleteEvent& e)`

- `prior_ord == kNoPriorOrd` 表示原始 key 不存在，宿主因「删不存在的 key」不动 docmap、不广播写路径；插件接到该哨兵必须视为 no-op（与旧 SearchLayer 语义一致）。
- `prior_ord` 在 docmap remove 之前捕获，删除统计需要旧 ord（BM25 用它按字段精确扣减）；插件无需也不能反查已删行。
- 由 HNSW 视角看，软删经 `DocTable::is_live` 过滤，`on_delete` 在 `VectorPlugin` 里是 no-op，物理清理由 `on_merge_commit → rebuild()` 完成。

数据回调抛出的异常由宿主吞并（错误计数 `index_errors_` 自增 + 日志），流水线保活、`ord` 推进不受影响。

### 4.4 可选预处理（并行 map 阶段）

`wants_prepare()`

- 返回 true 才会被宿主在 map worker 线程上调 `prepare()`；否则直接进入 `on_put`。
- `TextPlugin` 报 true（多字段路径上各 map worker 并行分析），`VectorPlugin` 报 false（写入端归一化在 Cask put 路径同步完成，事件里已是最终向量）。

`prepare(const PutEvent& e) const`

- **纯 const**、不得触碰插件可变状态，可在 N 个 map worker 线程并发对不同文档调用。
- 返回类型擦除的 `PreparedPtr`，由同插件在 `on_put` 内 `static_cast<TextPrepared*>` 消费。
- `VectorPlugin` 不需要 prepare：`on_put` 直读 `e.doc->vec`（已归一化的 span）。

### 4.5 存储维护 / merge 参与

四个 merge 事件动词构成一条 **merge 生命周期协议**，由 `merge::run_merge`（`src/merge/merger.cpp`）在 merge 线程直接派发：

```
on_merge_begin({input_file_ids, watermark})
   │
   │   …fold + CAS 切换 keydir, 每次切换成功对**每个插件** 派发
   ▼
on_relocate({ord, key, new_loc, value_view})   ← 每条搬迁记录一条
   │
   │   …fold 完成, CAS 全部切换
   ▼
on_merge_commit({output_file_ids, dead_ratio})  ← 收尾
   │
   │   失败分支
   ▼
on_merge_abort()                                 ← 任一折失败后兜底
```

每个动词的具体语义：

- `on_merge_begin`：merge 启动通告；`MergeBeginEvent.watermark` 是 merge 开始时刻的全局 ord 水位。
- `on_relocate`：每条 key 从旧定位迁移到新定位时广播；`RelocateEvent.value` 仅在回调期间有效（merge fold 此刻正持有整条记录缓冲），分批 apply 模式下也可能为空。
- `on_merge_commit`：merge 成功收尾，插件可在这一刻触发 GC / rebase（典型动作见 §6、§7）。
- `on_merge_abort`：merge 失败时清理任何 `on_merge_begin` 留下的影子状态。

调用线程上下文（merge 线程 vs reducer 线程）：

| 动词 | 线程 | 是否单写者 | 是否与 reducer 并发 |
|------|------|----------|-------------------|
| `on_merge_begin` | merge 线程 | 否 | 是（reducer 同时在推进 put 任务） |
| `on_relocate` | merge 线程 | 否（同 fold 推进） | 是 |
| `on_merge_commit` | merge 线程 | 否（插件收尾需自管） | 是 |
| `on_merge_abort` | merge 线程 | 否 | 是 |

**merge 回调里的副作用必须经 `PluginHost::run_serialized`** 投递到 reducer 静止点。同一提交序 FIFO 执行：插件在 `on_merge_commit` 内提交的 `run_serialized` 闭包**先于**宿主随后提交的成对保存点 RunFn（同队列 FIFO），这让旧硬编码的「compact → rebuild → ckpt」顺序自动涌现，而不再写在 host 里。

`maintain(const MaintainEvent& e)`

- reducer 静止点派发的维护提示；`Reason` 区 `kPostMerge` 与 `kAuto`，dead_ratio_hint 是阈值建议。
- 默认空实现；本批次的 TextPlugin / VectorPlugin 都未覆写，预留给未来的看护式任务（如自适应降采样、cache 暖化等）。

### 4.6 持久化

`flush(const FlushRequest& req)`

- 把插件状态落盘到 `req.watermark`；返回 `FlushResult { status, covered_ord, generation, chain_seq, chain_wm }`。
- `req.force_rebase == true`（典型场景：close / merge 末尾）强制走 base 路径（`bm25.ckpt` / `vec.ckpt` 单文件，覆盖完整旧链）。
- `req.reason` 是 `kClose / kMerge / kAuto / kManual` 之一，插件当前不细分——所有 flush 都是「落盘到当前 ord 上界」。
- `FlushResult.covered_ord == req.watermark` 才视为「覆盖推进」，Cask 据此决定 `manifest.entries[component_id]` 是否更新；不足则保留旧 entry，不破坏成对不变量「keydir 水位 ≤ min(各组件覆盖水位)」。
- 三元组回执 `generation / chain_seq / chain_wm` 替代旧 host 下探 `chain_state()` 的做法，第三组件零 else 分支即可接入。

错误契约：

| 错误位置 | 行为 |
|---------|------|
| `prepare` / `on_put` / `on_delete` / `on_relocate` 抛出异常 | 宿主吞并 + `index_errors_` 自增 + `log_error`，流水线保活，ord 照常推进 |
| `open` / `flush` / `close` 失败 | 以 `PluginStatus` 返回值报，**宿主必须决定降级策略**（标准实现：跳过该组件在 manifest 的更新，下次重启自检） |

---

## 5. Cask 对插件的持有与扇出

Cask 是把『`CaskPlugin` 注册表』『DocMap』『PluginHost』『HybridSearcher』胶合起来的容器，胶合面集中在 `src/cask/cask.cpp`：

| 持有量 | 类型 | 生命周期 | 用途 |
|--------|------|---------|------|
| `text_` | `unique_ptr<text::TextPlugin>` | `enable_search` 时构造，close 末析构 | BM25 文本域 |
| `vec_plugin_` | `unique_ptr<vec::VectorPlugin>` | `enable_search` 时构造 | HNSW 向量域 |
| `hybrid_` | `optional<search::HybridSearcher>` | 上述两者就绪后 `emplace` | RRF 融合器 |
| `docmap_` | `shared_ptr<index::Index>` | `create_search_infra` 创建，**先于插件** | 文档身份表 / 宿主服务对象 |
| `plugins_` | `vector<plugin::CaskPlugin*>` | 注册表，仅持指针 | reducer 扇出表 |
| `plugin_host_` | `CaskPluginHost` | 值成员 | 实现 `plugin::PluginHost` |
| `ckpt_rebase_needed_` | `atomic<bool>` | close()/merge() 收链联动 | S14-4 legacy 全局 rebase 标志 |

### 5.1 插件装配

`create_search_infra` 按以下顺序构建：

1. **docmap 优先**：`docmap_ = make_shared<index::Index>()`，因为后续两个插件都需要引用它。
2. **text 后于 docmap**：`TextPlugin(scfg.text_config(), *docmap_, *docmap_, *docmap_)` —— 第三个参数既是 `DocTable` 也是 `DocLenWriter` 还是 `CompactionStats` 三个窄接口的宿主实现。analyzer 构造失败 → 干净拒绝 `kInvalidOption`，不进入脆弱状态。
3. **vector 后于 docmap**：`VectorPlugin(scfg.vector_config(), *docmap_)`，仅借用 `DocTable`。
4. **hybrid 最后**：`hybrid_.emplace(*text_, *vec_plugin_)` —— 它是两者之上的非 owning 引用。
5. **注册表**：`plugins_ = { text_.get(), vec_plugin_.get() }`。

构造期间失败的容差：analyzer 失败 → 整个 `enable_search` 路径整体 fail-fast（索引模式与不健康状态不可妥协），错误沿 `CaskFault` 返回调用方。

### 5.2 写路径扇出

`Cask::reduce_index_entry` 是 reducer 线程入口，对一个 `PutEntry`：

1. **docmap 先 apply**：`docmap_->put_doc` 与 `set_meta`，与历史 KV 顺序一致。
2. **prepare 已 by-value 进 `e.preps`**：map 阶段由 `Cask::prepare_index_task` 把 `preps[i]` 拍平到 `ReorderEntry.preps`，reducer 不再调 prepare。
3. **按注册序 on_put**：每个插件拿自己的 `prep`，类型擦除到具体插件的 `TextPrepared*` 之类。

`DeleteEntry` 上：

1. **`prior_ord` 在 docmap remove 之前捕获**（`docmap_->get` 旧 slot 拿 ord，再 `docmap_->remove`）。key 不存在 → `prior = kNoPriorOrd`，宿主不动 docmap、广播哨兵。
2. **删除事件按注册序扇出**到各 `on_delete`。

`SkipEntry`（ord 空洞）与 `RunFnEntry`（维护收尾）是 host-only 动词，不广播给插件。

### 5.3 回收链：prepare 阶段

`Cask::prepare_index_task` 是 map 阶段入口，行为：

- 构造 `DocView` 与 `PutEvent`（`make_doc_view` / `make_put_event` 是 host 内的薄转换）。
- 对每个 `plugins_[i]`：若 `wants_prepare()` 为 true，调 `prepare(ev)` 入 `preps[i]`；否则留 `nullptr`。
- 与 `IndexPool` 注册的 `MapFn` 协调：map worker 线程上并发跑，不同文档可同时跑同一插件。

---

## 6. TextPlugin 内部结构

`bitcask::text::TextPlugin` 实现 `plugin::CaskPlugin`，`name()` 返回 `"bm25"`。

### 6.1 内存拓扑

```
TextPlugin (final, plugin::CaskPlugin)
├── 配置：TextPluginConfig（独立成 text_plugin_config.hpp）
│   ├─ AnalyzerConfig / Bm25Params
│   ├─ cache_max_entries / doc_text_cache_max
│   ├─ index_positions / auto_compact_dead_ratio
│   ├─ synonym_map（open-time 不可变 shared_ptr）
│   └─ max_delta_chain（链长上限，达则下次 flush 强制 base）
├── 依赖注入（构造期注入，shared_ptr 借用 + 引用借用）
│   ├─ const bm25::DocTable&        docs_         ← 查询面读
│   ├─ bm25::DocLenWriter&          doc_len_writer_ ← reducer 写
│   └─ bm25::CompactionStats&       stats_         ← S12-2 节流统计
├── 字段倒排：fields_（unordered_map<field, unique_ptr<InvertedIndex>>）
│   ├─ shared_mutex fields_mu_（保护 map 结构，InvertedIndex 自带锁）
│   ├─ intern_field_name（shared_mutex + unordered_set，避免 owning string）
│   └─ ord_field_lens_（ord → [(field, flen)]，删除时按字段精确扣减）
├── analyzer：unique_ptr<Analyzer>（NFKC + analyzer，可能为 null 表示配置无效）
├── 缓存：search::SearchCache（query + k_req 为 key；params_override 禁用缓存）
├── 原文 LRU：DocTextLru（cap=doc_text_cache_max，mut 自身 mutex，高亮读用）
├── 同义词：shared_ptr<const SynonymMap>（open-time 不可变）
├── 持久化元数据：
│   ├─ atomic<bool> dirty_default_ / dirty_fields_（初值 true）
│   ├─ ChainState chain_（{base_gen, chain_wm, next_seq}）
│   ├─ std::string dir_       （open 注入，flush 复用）
│   ├─ plugin::PluginHost* host_   （open 注入；merge_commit 经 run_serialized 投递）
│   ├─ uint64_t watermark_    （open 后覆盖水位）
│   └─ atomic<bool> rebase_needed_（初值 true：未知态一律 base）
└── prepare 阶段产物：TextPrepared { search::ReduceJob job; }（类型擦除）
```

### 6.2 写路径实现

`on_put(e, prep)` 决策树（按 `e.doc` / `e.replay` 路由）：

- **`prep != nullptr`**：直接 `apply_job(sp->job)`，job 的 doc_text 走 move（原 LRU 零拷贝路径）。
- **`prep == nullptr` && `e.doc && !e.doc->fields.empty()`**（多字段但 prepare 抛过异常）：`apply_job({})`，空 job 守卫兜底。
- **单文本 + 活写**：`apply_text(e.key, e.ord, text)`，reducer 内部分析 + 默认域倒排 + doc_len 回填 + LRU 装入 + 失效 cache。
- **单文本 + 重放批**：`e.replay == true` 时由 `prepare()` 路径并行分析，行为对齐 recover 阶段的并行语义。

`on_delete(e)`：

- `e.prior_ord == kNoPriorOrd` → 直接 return（哨兵即 no-op）。
- 否则按 `prior_ord` 取 `docs_.doc_len(prior_ord)`，对每个字段精确扣减（优先查 `ord_field_lens_`，否则按默认字段使用 `doc_len`）。`dirty_default_` / `dirty_fields_` 同时置位（删除触全局统计）。

### 6.3 Map 阶段（`map_analyze`）

纯 const 函数，可在 N 个 map worker 上并发对不同文档调用。流程：

1. NFKC + analyzer 各字段（`analyze_with_positions` 产 `(term, (tf, positions))`）。
2. catch-all 合并：非默认字段的 `term_data` 按 `ca_pos_base` 平移 position 进 `ca_data`，使 `search_text/phrase/near`（仅看默认字段）也能命中多字段文档。
3. 产出 owning `ReduceJob`（key/ord/fields/term_data/total_doc_len/ca_data/doc_text）。

### 6.4 Reduce 阶段（`apply_job_impl`）

reducer 单写者，按字段顺序：

1. `ord_field_lens_` 登记字段长度（字段名 intern，复用 node 稳定 string_view）。
2. 各字段 `add_doc` 进倒排 + 置对应脏位。
3. `wrote_default == false` 时 `ca_data` 合并进默认字段（避免双写）。
4. `doc_len_writer_.set_doc_len(ord, total_doc_len)` 回填（DocMap 行由宿主先落，doc_len 由 BM25 侧分析产物回填）。
5. `doc_texts_.put(ord, doc_text)`，move 原文进高亮 LRU。
6. 失效 cache：用本次 job 的词集做选择性失效（安全但粗粒度的全缓存失效已淘汰）。
7. `maybe_auto_compact()`：阈值 `auto_compact_dead_ratio > 0` 才启用；`retired_since_compact() ≥ max(min_deaths, live_docs/2)` 触发 reducer 线程内串行 `compact`。

### 6.5 持久化（`flush` 决策表）

```
want_base = req.force_rebase || rebase_needed || (max_delta_chain && chain.next_seq > cap)
├─ want_base                      → save_component_base(wm)：rename .prev → 写 .ckpt → 链坍缩
├─ !want_base && !dirty()          → no-op，covered_ord = chain.chain_wm（宿主不推进 manifest）
└─ !want_base && dirty()           → save_component_delta(wm)：链 .dN 加 kDeltaInfo + 段
```

返回体三段：`status`、`covered_ord`、`{generation, chain_seq, chain_wm}`。损坏/缺失自降级在 `open` 已经做完（损坏 → watermark=0，rebase 置位，**首次 flush 必然 base**）。

### 6.6 merge 收尾（`on_merge_commit`）

无条件 `run_serialized([this]{ compact(0.2); })` —— 把「死 posting 压实」投递到 reducer 静止点：

- 阈值 0.2 = 原 `kMergeCompactDeadRatio`，与旧硬编码常量一致。
- `compact` 内部自置 `rebase_needed_`（破坏 base+delta 可重构性），下次 `flush` 强制走 base。
- 无宿主（standalone 测试）时直跑，因为没有 reducer 通道。

文档非默认字段路径：当前实现以 `map_analyze` 之 `fields[]` 为准，`maybe_auto_compact` 在 reducer 线程内运行；多字段引起的 `dirty_fields_` 与 `dirty_default_` 两段独立脏位分别追踪。

### 6.7 维护接口（测试面与历史契约）

为了让既有 BM25 单测不改动，仍保留一系列「legacy 段序列化」入口：`serialize_default / serialize_fields`、`serialize_*_delta`、`deserialize_default / deserialize_fields`、`apply_*_delta` 与 `rebuild_index`。这些是 `tests/support/bitcask_search_shim` 还在消费的能力，P5 收编后删除。

---

## 7. VectorPlugin 内部结构

`bitcask::vec::VectorPlugin` 实现 `plugin::CaskPlugin`，`name()` 返回 `"hnsw"`。

### 7.1 内存拓扑

```
VectorPlugin (final, plugin::CaskPlugin)
├── 配置：VectorPluginConfig（独立成 vector_plugin_config.hpp）
│   ├─ dim（0 = 无向量）
│   ├─ VectorMetric（cosine → HnswMetric::kDot 映射，因写入端已归一化）
│   ├─ hnsw_m / hnsw_ef_construction（0 = 保持 HnswConfig 默认）
│   ├─ inmem_int8（P5b 增量：int8-only cosine/dot 模式）
│   └─ max_delta_chain
├── 依赖注入：const bm25::DocTable& docs_
├── 图：atomic<shared_ptr<HnswIndex>> hnsw_（rebuild 经 atomic swap 发布）
├── delta 插入日志：delta_ords_（parallel array） + delta_data_（按 dim 步长紧凑）
│   └─ uint64_t delta_window_wm_（只有 ord ≥ 该水位才入账）
├── 持久化元数据（同 TextPlugin 三件套）：
│   ├─ atomic<bool> dirty_（初值 true）
│   ├─ ChainState chain_
│   ├─ std::string dir_ / plugin::PluginHost* host_ / uint64_t watermark_
│   └─ atomic<bool> rebase_needed_（初值 true）
└── 写入端归一化：normalize_for_write（SIMD AVX-512F > AVX2/FMA > 标量）
```

### 7.2 写入端归一化与 on_put

`normalize_for_write(input, norm_buf)` 在 `Cask::put_doc` 路径**同步**调用，结果编码进 data file——「存储即归一化」（merge / 恢复不再重算）。错误以静态 `const char*` 消息返回，Cask 边界翻译成 `CaskFault`：

| 场景 | 返回 |
|------|------|
| `input.empty()` | 空 span（合法：无向量） |
| `dim == 0` | `unexpected("collection has no vector config")` |
| dim 不符 | `unexpected("vector dim mismatch")` |
| `cosine` + 零向量 | `unexpected("zero vector not allowed under cosine metric")` |
| 其它 | 归一化结果（写入 `norm_buf`），`cosine` 之外直接返回 `input` 零拷贝 |

`on_put(e, prep)` 不需要 prep，归一化已在 put 同步完成：

- `e.doc && !e.doc->vec.empty()` → `insert(e.ord, e.doc->vec)`。
- 删除无动作：HNSW 软删经 `DocTable::is_live` 过滤，merge 时由 `rebuild` 物理清理。

`insert(ord, v)`：

1. 无图 / dim 不符 → 直接 return（防御，不崩）。
2. `dirty_.store(true)`。
3. `ord ≥ delta_window_wm_` 时入插入日志（fold 重叠区已在链里）。
4. `hnsw_->insert(ord, v)` 直插。

### 7.3 查询：`search(query, k, ef, filter)`

- 查询开头 `hnsw_.load(acquire)` 一次图快照，与 rebuild 的 atomic swap 并发安全。
- cosine 模式下对查询向量同样入口归一化（用同一 SIMD 路径）；零向量返回空结果（写入端零向量被拒，查询端宽容）。
- `ef == 0` → `max(k, 64)`，放大 k 减少图遍历层 miss。
- `filter` 与 `is_live` 合成 HNSW live callback（V5）：被拒节点从图遍历源头就不入候选集；空 meta blob 一律不通过。

### 7.4 merge 收尾：`on_merge_commit`

`enabled()` 即 `dim > 0`，否则 no-op。`enabled` 时把 HNSW 重建（物理清死节点）投递到 reducer 静止点：

- `host_->run_serialized([this]{ rebuild(); })`，无宿主直跑。
- `rebuild()` 内部走 `clone_live`(用 `docs_.is_live` 作过滤器) 旁路建新图 + atomic swap；rebuild 后 `dirty_` 置位 + `rebase_needed_` 置位（与 TextPlugin 的 compact 收尾同样的「破坏可重构性即 rebase」策略）。

### 7.5 持久化（`flush` 决策表）

```
want_base = req.force_rebase || rebase_needed || (max_delta_chain && chain.next_seq > cap)
├─ dim == 0 (无向量配置)
│     ├─ want_base → 清残留文件 + return covered_ord = chain.chain_wm（host 不记账）
│     └─ !dirty    → no-op
├─ dim > 0 && want_base → save_component_base：rename → 写 .vec / .qc8 侧车 + kHnsw 段
├─ dim > 0 && (!dirty || delta 空) → no-op（covered_ord = chain.chain_wm）
└─ dim > 0 && 否则 → save_component_delta：链 .dN 加 kHnswDelta 段（插入日志重放）
```

delta log 序列化格式：`count u64` + `dim u16` + 每条 `ord u64` + `f32[dim]`（`sizeof(float)` 字节序小端写）。

### 7.6 链回执

`flush` 末尾固定回填 `chain_.base_gen / (chain_.next_seq - 1) / chain_.chain_wm` 到 `FlushResult.generation / chain_seq / chain_wm`，host 直接组装 `ManifestEntry` 不再下探 `chain_state()`。

---

## 8. HybridSearcher

`bitcask::search::HybridSearcher` **不是** `CaskPlugin`。它是一层薄融合器，构造期拿两个插件引用，部署可选：

```text
HybridSearcher(const TextPlugin& text, const VectorPlugin& vec)
└── search(text_query, vec_query, k, filter) → vector<SearchHit>
```

算法为 RRF（Cormack, Clarke, Buettcher 2009）：

- 两路都空 → `SearchError::kEmptyHybridQuery`（S7）。
- `K' = max(k * 4, 64)`：text 路 overfetch（filter 后过严耗损弥补）；vec 路 ef=0 + filter 让 HNSW live callback 自过滤，不需额外 overfetch。
- 融合公式：`score(doc) = Σ_lane  1 / (60 + rank_lane)`，`rank` 从 1 起；单路文档只累加该路项（无需分数归一化）。
- 平局：`score` 相等则 `ord` 小者在前（测试锁此行为）。
- filter：text 路后过滤（cached 内层不变），vec 路折进 HNSW live callback——同时通过才进 RRF。
- 单路空 → 退化为另一路的 RRF 重打分（首项保留另一路项作为唯一信号）。

线程 / 部署语义：与两个查询内核同样的多读者并发；只装单插件的部署通过 `bitcask_hybrid` 的依赖关系「自动卸载」——`bitcask_hybrid` 不依赖 `bitcask_text_plugin` 或 `bitcask_vector_plugin` 中**任一缺失**时无法装配。

---

## 9. DocMap 宿主服务（正文章，不是横幅）

本节定位 docmap 的角色边界：它属于「文档身份」域，不属于「搜索」域；`Cask` 拥有所有权，插件借用只读视图。这一点是 Phase-2 拆分的关键护栏，构造期注入 + 接口只读纪律的根因。

`Cask` 的 `docmap_`（`shared_ptr<index::Index>`）是 live / ext_id / doc_len / meta_filter 的统一持有方，向插件暴露三套窄接口：

### 9.1 `bm25::DocTable`（查询面只读）

继承 `bm25::LiveChecker`（`is_live(ord)` + `doc_len(ord)` + 批量接口 `fill_is_live / fill_doc_lens`），扩展三个查询面身份翻译 / 过滤函数：

| 方法 | 用途 |
|------|------|
| `optional<string> ord_to_ext(ord) const` | ord → ext_id（hit 翻译）；越界 / 已删 → `nullopt` |
| `bool eval_meta(ord, MetaFilter&) const` | meta 过滤锁内求值，避免 heap copy 出锁 |
| `optional<uint64_t> ord_of(ext_id) const` | ext_id → ord（explain 等 key→ord 反查） |

实现者是 `index::Index` —— 已有全部方法，加 override 与 `ord_of` 薄包装。`LiveChecker` 父接口保留专评分热路径，`DocTable` 在其上扩展翻译 / 过滤——BM25 单测的 `FakeLiveChecker` 仅覆 `LiveChecker` 不受影响。

### 9.2 `bm25::DocLenWriter`（reducer 单写者写）

只暴露 `set_doc_len(ord, len)`，签名从 Index 内部抽出。P4 拆分后 TextPlugin 构造注入——不经 `PluginHost`（文本域与宿主的构造期专属契约，不污染通用插件接口）。契约：仅 reducer 单写者上下文可调。

### 9.3 `bm25::CompactionStats`（维护窄读）

只暴露三个读数（`retired_since_compact / reset_retired_since_compact / live_docs()`），供 `maybe_auto_compact` 做节流决策。同样构造期注入，避免把整个 Index& 暴露给插件。

### 9.4 注入与生命周期

`Cask::create_search_infra` 一行式注入：

```
text_ = make_unique<TextPlugin>(
    scfg.text_config(),
    *docmap_,   // const DocTable& docs_
    *docmap_,   // DocLenWriter& (实现是 Index 内部)
    *docmap_);  // CompactionStats&
```

`shared_ptr<index::Index>` 让 Index 与 docmap 共同持有（Cask 与 docmap_ 都持引用；解引用时不延寿）。`DocTable` 接口是查询面只读纪律的护栏：插件不应接触 `Index` 的写方法，构造期之外与 `Index` 完全解耦。

---

## 10. 错误处理与失败回退

### 10.1 插件失败分类

| 失败类型 | 谁负责 | 反馈 |
|---------|-------|------|
| 数据回调（prepare / on_put / on_delete / on_relocate）抛异常 | 宿主吞并 + `index_errors_++` + log_error | 流水线保活，ord 照常推进 |
| `open` 失败（组件加载 / 链续接） | 插件自降级：watermark=0、rebase_needed=true | 宿主全量 fold 重放 |
| `flush` 失败（I/O 错） | 报 `PluginStatus::kFailed` | `FlushResult.covered_ord = chain.chain_wm`，宿主保留旧 manifest entry |
| `close` 失败 | 报 `PluginStatus::kFailed` | 终结路径吞并 + log_error，下次 start 走 chain_state 重整 |
| `on_merge_commit` 提交的 `run_serialized` 闭包抛 | 经 RunFn 失败路径回吐 | reducer 计数器 + 与普通 put 失败同路径 |

### 10.2 索引漂移的检测与收敛

`index_errors_`（Cask 成员的 `atomic<uint64_t>`）非零时，「索引可能漂移」已被记录在 `StatusInfo::index_errors`。查询面不感知——选择是把漂移消化在「下次 flush」与「下次 merge」的重新整理里：

- 下次 `flush(kAuto/kManual)`：`rebase_needed` 已经为真，强制走 base 即可重建覆盖。
- 下次 `merge`：bm25 / vec 自动 commit 阶段都强制 rebase，merge 收尾一并重新整理。

### 10.3 关闭路径（`Cask::close`）

按以下顺序（破坏次序即破坏状态一致性）：

1. `closed_` 标志置位（fail-fast 入门）。
2. 等所有 `writes_in_flight_` 归零（`writes_in_flight_.wait`）。
3. `maybe_group_commit(force=true)` 把最后一批未 fsync 的写落盘。
4. finalize active hint + 关 active data + 清 read cache。
5. **强制 rebase + paired search ckpt**：`force_ckpt_rebase()` 联动两插件自持标志 + `save_search_ckpt_paired(watermark=peek_next_ord)`：插件 flush 跟着走 base 路径 + keydir snapshot。
6. `index_pool_->unregister_lib(index_lane_)`（flush 排空 + 从 lanes_ 移除），停 lane 不停池。
7. `write_keydir_snapshot()` 落 keydir 尾态。
8. 析构序（绿区）：先 `hybrid_.reset()` → `text_.reset()` → `vec_plugin_.reset()` → `docmap_.reset()` → release write/merge lock。

`text_->close()` 与 `vec_plugin_->close()` 当前为 no-op；终止性 flush 由 `save_search_ckpt_paired` 集成完成，这样能让「全局 rebase」与「双插件 rebase」联动一致。

---

## 11. 线程模型总览

| 组件 | 线程上下文 | 访问模式 |
|------|----------|---------|
| `plugins_` 注册表 | 单写者（Cask 自身在 `create_search_infra` / `close`） | 注册表指针本身由 `unregister_lib` 之后的闭包豁免保护 |
| `on_put` / `on_delete` | reducer 单写者（`IndexPool` reducer 线程） | 严格 ord 升序 |
| `wants_prepare` / `prepare` | 任意 map worker（只在 `enable_search` 时存在） | 纯 const，可并发 |
| `on_merge_*` / `on_relocate` | merge 线程 | 与 reducer 并发；副作用必须经 `run_serialized` |
| `flush` | Cask::close / merge 收链 / RunFn 任务 → reducer | reducer 调用 flush 或通过 RunFn 切换 |
| `maintain` | reducer 静止点（RunFn 同路径） | 可作用于插件单写者 |
| `search_*`（HybridSearcher 内部） | 任意查询线程 | 多读者并发，多路共享同一 reducer 写者保护 |
| HNSW 图 | 单写者 + 多读者（`atomic<shared_ptr<>>` 发布 + per-node 自旋锁） | rebuild swap 前后读者拿旧图续命 |

各插件查询内核的并发模型与原 SearchLayer 一致：InvertedIndex 按词 hash 分片锁 + CoW posting；HNSW 单写者 + 多读者。

---

## 12. 事件流时序（参考图）

### 12.1 put 路径（活写）

```text
user code  : Cask::put_doc(key, doc)
   │
   ▼
Cask 写线程 : write_and_keydir → alloc_ord → submit_index_task(PutTask)
   │   (write_mu_ 外)
   ▼
IndexPool N 个 map worker :
   │   for each registered plugin:
   │      if wants_prepare(): preps[i] = plugins[i]->prepare(ev)     ── pure const
   ▼  reorder buffer (按 ord 排序)
reducer 单线程 :
   │   docmap_->put_doc(key, ord, slot, /*doc_len=*/0)               ── docmap 先
   │   if has meta: docmap_->set_meta(ord, meta)
   │   for each plugin in plugins_:                                     ── 注册序
   │      plugins_[i]->on_put(ev, std::move(preps[i]))                 ── reducer 单写者
   ▼
查询面 : 后续 search_* 通过 prepare_search 的 flush 屏障看见该 ord
```

### 12.2 merge 路径

```text
user code  : Cask::merge({}, now_sec)
   │
   ▼
Cask::merge : DocmapRelocator (栈上, 不入注册表) + plugins_ → merge_plugins
   │
   ▼
merge::run_merge (merge 线程) :
   │   for each p in merge_plugins: p->on_merge_begin({watermark})
   │
   │   ┌─ fold input files
   │   │   每批累计达 kApplyBatch / 单文件 fold 完：
   │   │     1. flush + fsync 输出 data
   │   │     2. CAS 更新 keydir
   │   │     3. for each p in merge_plugins: p->on_relocate(...)    ── 每条搬迁
   │   └─
   │
   │   on success:
   │     for each p in merge_plugins: p->on_merge_commit(...)      ── 与 reducer 并发
   │       TextPlugin::on_merge_commit  → host_->run_serialized(compact 0.2)
   │       VectorPlugin::on_merge_commit → host_->run_serialized(rebuild)
   │   on failure:
   │     for each p in merge_plugins: p->on_merge_abort()
   ▼
Cask 主线程 : index_pool_->flush(lane) → 排干 RunFn
   │   (compact / rebuild / ckpt 全部在 reducer 静止点执行)
   ▼
reducer RunFn (FIFO) :
   │   1. docmap_->compact_chunks()
   │   2. force_ckpt_rebase()
   │   3. save_search_ckpt_paired(wm=peek_next_ord, wms, /*keydir_meta*/{})
   ▼
manifest : docmap entry + bm25 entry + vec entry 三元组更新
```

### 12.3 close 路径

```text
user code  : Cask::close()
   │
   ▼
Cask::close :
   │   closed_.store(true)
   │   await writes_in_flight_ == 0
   │   maybe_group_commit(true)        ← fsync active data
   │   finalize hint
   │   close active data + read cache
   │   index_pool_->unregister_lib(lane)  ← flush 排空 + 从 lanes_ 移除
   │
   │   ┌── plugins (text_->force_rebase() + vec_plugin_->force_rebase())
   │   │   save_search_ckpt_paired(peek_next_ord, collect_snapshot_watermarks())
   │   │     └──> plugins_[i]->flush({force_rebase, watermark=peek_next_ord})
   │   │             └──> save_component_base (rename .prev → 写 bm25.ckpt/vec.ckpt + 侧车)
   │   │                    → chain 坍缩：chain = {wm, wm, 1}
   │   │                  save_component_base NoOp if dim==0
   │   └──
   │
   │   write_keydir_snapshot()
   ▼
析构序 : hybrid_.reset() → text_.reset() → vec_plugin_.reset() → docmap_.reset()
                                                       → release write_lock
```

---

## 13. 接口稳定性的内部约定

`plugin_api.hpp` 是自包含 INTERFACE 头（只 include 标准库），不引入任何 bitcask 类型。新插件作者参考本接口加实现即可，无需触碰 KV / merge / IndexPool。具体护栏：

- **接口层不引入 `bm25::` / `vec::` / `search::` 类型**。`DocView` 用 `span<const FieldKV>` / `span<const float>` / `span<const byte>` 等中性容器，`FieldKV` 是 `pair<string_view, string_view>` 的 type alias。
- **错误模型极简**：`PluginStatus { kOk, kFailed }` + `LogLevel { kWarn, kError }`。不引入 `CaskError` / `SearchError` 这些上层错误分类；上层在边界翻译。
- **跨线程移交类型擦除**：`Prepared` 是空虚基类，避免 plugin_api 引入具体插件的依赖。`on_put` 内具体插件 `static_cast` 回来。
- **`RunFn` 通过 `PluginHost::run_serialized` 走 `IndexOp::RunFn`**，与现有 `RunFnEntry` 完全复用——这是 merge 收尾插件与 host 的唯一耦合点。

破坏任一条会触发 1) 子目标重编译风暴，或 2) 接口层循环依赖，或 3) 错误模型与上游语义不一致之一。

---

## 14. 已确认的一致性检查

本文档内容对照 `include/bitcask/plugin_api.hpp`、`include/bitcask/text_plugin.hpp`、`include/bitcask/vector_plugin.hpp`、`include/bitcask/hybrid_searcher.hpp`、`include/bitcask/text_plugin_config.hpp`、`include/bitcask/vector_plugin_config.hpp`、`include/bitcask/doc_table.hpp`、`src/search/text_plugin.cpp`、`src/search/vector_plugin.cpp`、`src/search/hybrid_searcher.cpp`、`src/cask/cask.cpp`、`src/cask/cask_recovery.cpp`、`src/cask/cask_search.cpp`、`src/merge/merger.cpp`、`CMakeLists.txt` 编写。

后续 Phase-3 文档（HNSW-core / SIMD-intersect）可以按本文件指出的接口边界引用 `CaskPlugin` 的动词集与 merge 协议，无需重新描述插件挂接层。
