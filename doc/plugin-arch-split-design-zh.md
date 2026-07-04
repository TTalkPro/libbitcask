# 插件化架构拆分设计（KV 回调接口 + BM25/HNSW 解耦）

状态：设计稿 v2（接口层按评审意见通用化——KV 事件词汇，2026-07-03）
进度：**P1 已落地**（S15 批次，2026-07-03，TASK.md 第十五梯队）——plugin_api
接口层 + thread_pool 去搜索化 + SearchLayerAdapter 唯一插件接入；clang 522/522、
TSan 521/522（唯一失败为预存问题）、put_doc bench −0.3%。**P2 已落地**（S16
批次，DocMap 宿主服务化）：S16-1 所有权上提（Cask 持有 docmap_、SearchLayer
借用）；S16-2 写路径反转（宿主先 apply DocMap 再广播插件 + DeleteEvent.prior_ord
修正 + doc_len 回填通道）；S16-3 查询面 DocTable 化（DocTable : LiveChecker，
查询代码经 const DocTable& 消费，不再直摸 index_）；S16-4 文档与契约测试收口。
clang 524/524、TSan 523/523（唯一失败为既知预存项 ThreadCountIndependent）。
前置：S14 全系收官（增量 checkpoint、keydir 快照增量化、int8 码字外置）

---

## 1. 需求解读

三条需求，本质是一条依赖方向的反转：

1. **KV 存储层是底层结构，制定 callback 接口** —— Cask 不再持有具体搜索类型，
   而是定义一套事件回调接口，由上层实现者注册进来。接口动词必须是 KV 数据库
   的固有事件（写入、删除、打开、关闭……），不携带任何搜索概念。
2. **KV 层只负责 data / hint / keydir 的读写**，对「搜索」没有概念，
   它看到的只是一组响应回调的 plugin。
3. **BM25 层与 HNSW 层分开**，不杂糅在一个 SearchLayer 里，可以只装 BM25、
   只装 HNSW，或两者都装（混合检索作为更上层的融合器存在）。

目标依赖图（自上而下只允许向下依赖）：

```
  C API / 门面（text::Searcher / vec::Searcher / HybridSearcher）
        │
  ┌─────┴──────────┬─────────────────┬──────────────┐
  │ TextPlugin     │ VectorPlugin    │ （任意第三方： │  ← 插件层（互不依赖）
  │ (bm25+text)    │ (hnsw)          │  metrics/TTL/ │
  │                │                 │  changelog…） │
  └─────┬──────────┴───────┬─────────┴──────────────┘
        │  读接口           │  读接口
        ▼                  ▼
     DocMap（ord↔ext / live / meta）      ← 宿主服务（原 index::Index）
        ▲
        │ 事件回调（plugin_api，纯接口头文件，KV 词汇）
  ┌─────┴───────────────────────────────┐
  │ Cask 核心：data / hint / keydir      │  ← 只认识 CaskPlugin 接口
  │ + IndexPool 流水线 + merge + 恢复编排 │
  └─────────────────────────────────────┘
```

## 2. 现状耦合盘点（侦查结论摘要）

### 2.1 Cask ↔ SearchLayer

好消息：**真正的接缝已经存在**。写路径不是同步调用，而是经
`IndexPool` 的 `MapFn/ReduceFn/ErrorFn` 三元组（thread_pool.hpp:243-245），
Cask 在 open 时用 lambda 捕获 `*search_` 装配（cask.cpp:562-608）。
本设计做的事就是把这组 lambda 的形状固化成正式接口。

残余的硬耦合点：

| 耦合点 | 位置 | 性质 |
|---|---|---|
| `unique_ptr<search::SearchLayer> search_` 组合持有 | cask.hpp:840 | 类型耦合 |
| `#include "bitcask/search_layer.hpp"` | cask.hpp:54 | 头文件传染 |
| `thread_pool.hpp` include search_layer.hpp（ReduceEntry 携带 `search::ReduceJob`） | thread_pool.hpp:51,210 | 中间层被反向污染 |
| 结果类型 `SearchHit/SearchHitEx/HighlightOptions` 进入 Cask 门面 | cask.hpp:221,600,604 | 门面耦合 |
| `run_merge(..., SearchLayer*)` → `on_relocate` 直调 | merger.cpp:41,131-135 | merge 硬编码 |
| 恢复：`load_keydir_from_disk(SearchLayer*)`、`recover_doc_batch/recover_tomb` | cask.cpp:910,944,1011 | 恢复编排硬编码 |
| checkpoint 成对性：`save_search_ckpt_paired`、delta 内联 `kKeydirDelta` | cask.cpp:2731, cask.hpp:781-790 | 持久化协议耦合 |
| remove 非池路径锁内直调 `search_->on_delete` | cask.cpp:2108-2110 | 双路径 |
| `prepare_vector` 余弦归一化在 Cask 写路径 | cask.cpp:2184 | 领域逻辑上移错位 |
| C API：搜索选项扁平进 `bitcask_options_t`，KV/搜索入口同文件 | bitcask_c.h:165-189 | 配置杂糅 |

### 2.2 SearchLayer 内 BM25 ↔ HNSW 杂糅

CMake 层 `bitcask_bm25` 与 `bitcask_vector` 已互不依赖，杂糅全部集中在
`bitcask_search` 聚合层的 SearchLayer 类里。共享可变状态按拆分难度排序：

1. **`index::Index index_`（docmap）**（search_layer.hpp:534）——
   ord↔ext、live 位图、doc_len、meta blob、`next_ord_` 水位。
   BM25 打分/存活/翻译要它，HNSW 存活过滤/meta 过滤/翻译也要它。**最难点。**
2. **共享 ord 空间** —— 由 Cask 的 `keydir_->alloc_ord()` 分配（cask.cpp:1832,2094,2188），
   倒排 posting 与 HNSW 节点同键。（这一条其实是资产不是负债：宿主分配 id、
   插件消费，天然符合插件模型。）
3. **单一 `search.ckpt` 容器 + 单条 delta 链** —— kBm25*/kHnsw*/kDocmap*/kKeydirDelta
   全部段类型混在一个文件族（search_checkpoint.hpp:32-46），一个
   `ckpt_chain_wm_/ckpt_next_seq_/ckpt_base_gen_` 管全部；`compact` /
   `rebuild_index` / `rebuild_hnsw` 任一发生都强制**整体** rebase。
4. **`reduce_apply` 融合 reducer**（search_layer.cpp:530-596）——
   一次调用里完成 BM25 postings + docmap put_doc + set_meta + HNSW 插入。
5. **cache 失效耦合** —— 纯向量/纯 meta 写也会打掉 BM25 查询缓存（search_layer.cpp:594）。
6. **恢复交织** —— `recover_doc_batch` 与 ckpt 加载在同一循环里交替恢复两个子系统。

查询路径反而是好拆的：纯 BM25 查询完全不碰 `hnsw_`，纯向量查询完全不碰
`fields_/analyzer_`，混合只有 `search_hybrid` 一处 RRF 融合（search_layer.cpp:318-388）。

### 2.3 与既有决策的关系

TASK.md:544-549 曾于 2026-06-25 搁置「Cask/InvertedIndex god class 拆分」，
理由是「风格问题非正确性问题」。本设计与之不冲突：那次否决的是**类内部的
美学拆分**（SearchOps/ScoringEngine 抽取，依赖方向不变）；本次做的是
**依赖方向反转 + 部署形态解锁**（可单独发布 BM25-only / vector-only 构建），
是能力变化而非风格变化。但那次决策提示的风险仍然有效：reducer 单写者
不变量与 TSan-clean 并发核心不能动 —— 见 §8 迁移策略。

## 3. 接口设计：plugin_api（通用 KV 事件契约）

新增 header-only 目标 `bitcask_plugin_api`（只依赖 `bitcask_format`），
Cask、merge、thread_pool 只 include 它，不 include 任何 bm25/vector/search 头。

### 3.0 设计原则（v2 修订依据）

1. **词汇来自 KV，不来自搜索**。接口动词 = KV 数据库的固有事件：
   打开、关闭、写入、删除、搬迁（merge）、落盘（flush）、维护（GC）。
   「分词」「倒排」「索引 job」不出现在任何签名里。检验标准：一个没读过
   搜索代码的人能凭此接口实现统计 / TTL / 变更流插件（见 §3.6）。
2. **查询不进通用接口**。查询面是各插件私有能力——调用方自持具体插件对象，
   经类型化门面访问；Cask 只提供读屏障（drain）。这就是「KV 层对搜索没有
   概念」在 API 面的落点。
3. **并行预处理是可选能力，不是接口形状**。现有 map/reduce 两相是 BM25
   分词的性能优化，不应强加给所有插件。默认路径 = 事件直达单写者上下文；
   声明 `wants_prepare()` 的插件才进入并行相。
4. **恢复重放与在线写是同一事件**。插件以自身水位自门实现幂等（现 HNSW
   `max_inserted_ord_` 机制升格为接口义务），不设 recover_* 专用动词。

### 3.1 数据类型

```cpp
namespace bitcask::plugin {

struct RecordLoc { uint32_t file_id; uint64_t offset; uint32_t total_sz; };

struct FieldKV { std::string_view name; std::string_view value; };

// 结构化 value 视图（DocValue 解码，宿主免费附带——它本就持有各部件；
// 纯 KV put 时为 nullptr，插件自行决定如何对待原始 value）
struct DocView {
  std::string_view           text;
  std::span<const FieldKV>   fields;   // 已经 field.schema 解析的命名字段
  std::span<const float>     vec;      // 原始向量（归一化是向量插件的事）
  std::span<const std::byte> meta;
};

// 写事件。所有 view/span 仅在回调期间有效，插件要留就拷贝。
struct PutEvent {
  uint64_t         ord;     // 宿主(keydir)分配，全局单调、不复用
  std::string_view key;
  std::string_view value;   // 原始 value 字节（KV 视角，恒有效）
  const DocView*   doc;     // 结构化视图；纯 KV 写为 nullptr
  RecordLoc        loc;
  uint64_t         tstamp;
};

// prior_ord（P2 修正）：被删文档原 ord，宿主在 docmap remove 前捕获；
// 原不存在 = UINT64_MAX。插件（尤其 BM25 统计调整）不必反查 docmap。
struct DeleteEvent   { uint64_t ord; std::string_view key;
                       uint64_t prior_ord; };

// merge 搬迁事件。value 视图免费附带——merge fold 此刻正持有整条记录的
// 缓冲，插件可借 merge 的这遍 I/O 做影子重建（见 §3.9），不需要就忽略。
struct RelocateEvent {
  uint64_t ord; std::string_view key; RecordLoc loc;
  std::string_view value;   // 仅回调期间有效
};

// merge 生命周期事件（见 §3.9 merge 参与协议）
struct MergeBeginEvent  { std::span<const uint32_t> input_file_ids;
                          uint64_t watermark; };
struct MergeCommitEvent { std::span<const uint32_t> output_file_ids;
                          double dead_ratio; };

struct MaintainEvent {
  enum class Reason { kPostMerge, kAuto } reason;
  double dead_ratio_hint;
};

struct FlushRequest {
  enum class Reason { kClose, kMerge, kAuto, kManual } reason;
  bool force_rebase;   // close 等要求收链的场合
};
struct FlushResult { Status status; uint64_t covered_ord; uint64_t generation; };

// prepare 相产物（类型擦除；由产出它的插件在 on_put 中消费）
struct Prepared { virtual ~Prepared() = default; };
using PreparedPtr = std::unique_ptr<Prepared>;

}  // namespace bitcask::plugin
```

### 3.2 CaskPlugin 接口

```cpp
struct OpenContext {
  std::string_view dir;    // 库目录；插件以 name() 为前缀自管自己的文件
  PluginHost*      host;   // 宿主服务句柄，生命周期覆盖 open..close
};

class CaskPlugin {
 public:
  virtual ~CaskPlugin() = default;
  virtual std::string_view name() const = 0;   // "bm25" / "hnsw" / "metrics" …

  // ---- 生命周期 ----
  // open：载入自身持久化状态。返回后 watermark() 必须反映已覆盖的 ord 水位，
  // 宿主据此决定恢复重放起点。损坏/缺失时自行降级（水位=0 → 全量重放重建）。
  virtual Status   open(const OpenContext&) = 0;
  virtual uint64_t watermark() const = 0;
  virtual Status   close() = 0;                // 含终止性 flush

  // ---- 数据事件（单写者上下文，ord 严格升序、可能有洞）----
  virtual void on_put(const PutEvent&, PreparedPtr prep) = 0;
  virtual void on_delete(const DeleteEvent&) = 0;

  // ---- 可选能力：并行预处理（纯函数，任意线程，不得触碰插件可变状态）----
  virtual bool        wants_prepare() const { return false; }
  virtual PreparedPtr prepare(const PutEvent&) const { return nullptr; }

  // ---- 存储维护 / merge 参与（默认空实现，不参与的插件零成本；见 §3.9）----
  virtual void on_relocate(const RelocateEvent&) {}  // merge 搬迁：ord 不变、只换定位
  virtual void on_merge_begin(const MergeBeginEvent&) {}   // merge 线程，fold 前
  virtual void on_merge_commit(const MergeCommitEvent&) {} // merge 线程，切换后
  virtual void on_merge_abort() {}                         // merge 失败，弃影子态
  virtual void maintain(const MaintainEvent&) {}     // GC/压实提示（reducer 静止点）

  // ---- 持久化 ----
  virtual FlushResult flush(const FlushRequest&) = 0;  // 落盘到当前已 apply 水位
};
```

要点：

- **一次 PutEvent 扇出 N 个插件**：reducer 对同一 ord 按注册序依次
  `on_put`（宿主 DocMap 恒在所有插件之前 apply），等价现 reduce_apply
  内部的语句顺序。
- **base/delta 是插件内部策略**：宿主只说「落盘到当前水位」，链长上限、
  rebase 时机、WAL 与否全是插件私事——S14-5 的链长上限从全局变每插件。
- **恢复没有专用动词**：宿主从 min(全插件 watermark) 起 fold data 文件，
  重放同样走 on_put/on_delete；插件跳过 ord ≤ 自身水位的事件（幂等义务）。

### 3.3 线程与错误契约

| 事件 | 触发源 | 线程上下文 | 序契约 |
|---|---|---|---|
| open / close | Cask open/close | 调用者线程，流水线静止 | 各一次 |
| prepare | 写/恢复流水线 | map worker，跨任务并行 | 无序（纯函数） |
| on_put / on_delete | put/put_doc/remove/恢复 fold | reducer 单写者 | ord 严格升序 |
| on_relocate | merge 线程 | **与 reducer 并发** | 仅已存在的 ord |
| on_merge_begin/commit/abort | merge 线程 | **与 reducer 并发** | begin→(relocate…)→commit/abort |
| maintain / flush | 宿主调度（RunFn） | reducer 静止点 | — |

- on_relocate 与 reducer 并发是现状语义（merger.cpp:131 直调、keydir CAS
  成功后逐条通知）；实现者必须保证被触状态自身线程安全（现 DocSlot 原子
  更新即满足）。P4 评估是否收进 RunFn 归一，暂不改语义。
- **错误契约**：数据事件回调抛异常由宿主吞并——计数器自增 + `host->log`
  上报、lane 保活（现 ErrorFn/S13-D7 语义，cask.cpp:598-604）；open/flush
  返回 Status，由宿主决定降级（open 失败=禁用该插件并告警，或 fail-fast，
  由 CaskOptions 策略位决定）。

### 3.4 宿主服务 PluginHost（较 v1 瘦身）

```cpp
class PluginHost {
 public:
  // 按存储定位读回原始记录（重建/回填场景；现 DocReader 的正式化）
  virtual std::optional<std::string> read_at(RecordLoc) = 0;
  // 在 reducer 静止点串行执行 fn（单写者上下文）——现 IndexOp::RunFn 的
  // 正式化。fire-and-forget；同一提交序 FIFO 执行。插件在 merge 线程等
  // 并发上下文里要变异自身单写者状态时，必须经此通道（见 §3.9）。
  virtual void run_serialized(std::function<void()> fn) = 0;
  virtual void log(LogLevel, std::string_view) = 0;
};
```

v1 曾把 `replay_rows`（DeltaReplayHook 的正式化）放进宿主接口——v2 **移除**：
keydir delta 成对推进是 docmap 持久化的内部协议，而 docmap 已定为宿主服务
（§4），该回调降级为宿主内部机制，与插件无关。P1 过渡期 SearchLayerAdapter
仍需此 hook，作为 adapter 构造参数私有传递（不进通用接口），P2 docmap
上提后自然消亡。

### 3.5 注册、所有权与查询访问

- `CaskOptions::plugins = std::vector<std::shared_ptr<CaskPlugin>>`。
  注册序 = reducer 分发序。open 时静态确定；运行中动态挂载不支持
  （需要回填=重建语义，列 §10 开放问题）。
- **调用方自持插件对象**，查询走类型化门面：
  `text::Searcher{cask, bm25_plugin}`、`vec::Searcher{cask, hnsw_plugin}`、
  `HybridSearcher{text_searcher, vec_searcher}`。门面查询前调
  `cask.drain_plugins()`（现 prepare_search/flush_index 读屏障的通用化改名，
  语义不变：submitted ⇒ applied 的 read-your-writes 屏障）。
- **Cask 本体零查询方法**。搜索命中只含 key/ord/score（现状），要 value
  的调用方拿 key 走 `cask.get()`——读路径与插件查询路径天然分离。

### 3.6 通用性检验：非搜索插件草图

```cpp
// 统计插件：不要 prepare、不要持久化，五个 override 完事
class MetricsPlugin final : public bitcask::plugin::CaskPlugin {
  std::string_view name() const override { return "metrics"; }
  Status open(const OpenContext&) override { return Status::ok(); }
  uint64_t watermark() const override { return UINT64_MAX; }  // 无持久化=恒不需重放
  Status close() override { return Status::ok(); }
  void on_put(const PutEvent& e, PreparedPtr) override {
    ++puts_; value_bytes_ += e.value.size();
  }
  void on_delete(const DeleteEvent&) override { ++dels_; }
  FlushResult flush(const FlushRequest&) override {
    return {Status::ok(), /*covered_ord=*/UINT64_MAX, 0};
  }
  std::atomic<uint64_t> puts_{0}, dels_{0}, value_bytes_{0};
};
```

同理可实现：TTL 插件（on_put 记 tstamp，maintain 扫过期），变更流插件
（on_put/on_delete 追加导出日志，flush 定位点，watermark=已导出 ord），
布隆过滤器插件。**接口没有搜索假设**——第 1、2 条需求的证明。

### 3.7 SearchLayer 的适配映射（v1 → v2 名称对照）

| v1（索引流水线词汇，已废弃） | v2（KV 事件词汇） |
|---|---|
| `IndexPlugin` | `CaskPlugin` |
| `WriteEvent` | `PutEvent`（原始 value 为主 + `DocView` 附件） |
| `map()` / `PluginJob` | `wants_prepare()` + `prepare()` / `Prepared`（可选能力） |
| `apply(job)` | `on_put(event, prepared)` |
| `recover` / `recover_tomb` | （删除）复用 on_put/on_delete + 水位自门 |
| `recovered_watermark()` | `watermark()` |
| `save_ckpt` / `load_ckpt` | `flush(FlushRequest)` / `open(OpenContext)` |
| `PluginHost::replay_rows` | （删除）降为宿主 docmap 内部机制 |

SearchLayerAdapter（P1）的对应实现：`wants_prepare()=true`、
`prepare → map_analyze`、`on_put → reduce_apply`（`doc == nullptr` 时
`text := value`，在 adapter 层保持「纯 put 也入全文索引」的现行为——
这是搜索插件的语义决定，不是宿主强加）、`on_delete/on_relocate` 直委托、
`maintain → compact/compact_index_chunks/rebuild_hnsw` 按 hint 分发、
`flush → save_search_ckpt`、`open → load_search_ckpt`、
`watermark → ckpt 覆盖水位`。

### 3.8 thread_pool 去搜索化

现状核实（源码抽查确认）：`thread_pool.hpp:51` include search_layer.hpp，
`ReduceEntry` 持 `search::ReduceJob`；且 `ReorderEntry` variant 里烧死了
六个搜索领域分支（ReduceEntry / OnWriteEntry / DeleteEntry / SkipEntry /
RebuildEntry / RunFnEntry），reduce lambda 在 cask.cpp:571-593 逐一
`std::visit` 分发。

目标：variant 塌缩为四类通用条目——
`PutEntry{PutEvent 的 owning 载体（现 IndexTask 字段即是）,
small_vector<PreparedPtr>}`（吸收 ReduceEntry/OnWriteEntry：单文本与
多字段路径统一）、`DeleteEntry`（广播全插件）、`SkipEntry`（ord 空洞填充，
保留）、`RunFnEntry`（吸收 RebuildEntry——rebuild_hnsw 本就是塞进 reducer
静止点的闭包，无需专用分支）。`MapFn/ReduceFn/ErrorFn` 形状不变，
entry 类型泛化。`thread_pool.hpp` 从此只 include `plugin_api`，
反向污染消除。热路径代价是每（任务×声明 prepare 的插件）一次虚调用 +
一次堆分配；如 bench 显示回退，可给 Prepared 加 arena/freelist，
但先测后优化。

### 3.9 merge 参与协议（插件同步参与 merge 触发的操作）

**问题**：merge 触发的插件侧操作现实存在一整串——倒排 `compact`、
`compact_index_chunks`、`rebuild_hnsw`、收官成对 ckpt（cask.cpp:2842-2910）
——且全部是 **Cask 硬编码的搜索知识**。仅靠 `maintain` 提示无法让插件自主
完成等价工作，也没有「与 merge 同一维护窗口内同步进行」的通道。

**协议**（时序，事件均带默认空实现）：

```
merge 线程:  on_merge_begin ──► fold + 写新数据文件 + fsync
                │                  │（每条 live：keydir CAS 成功 → on_relocate，
                │                  │  事件携带 value 视图——插件可白嫖 merge
                │                  │  这遍全量 live 扫描做影子重建）
                │                  ▼
                │             keydir 批量切换完成 ──► on_merge_commit
                │                                        │ (失败: on_merge_abort)
插件侧:         │ 开影子结构                              │ 自决收尾：
                │ （可选）                                │ 轻量: 置脏等 maintain
                │                                        │ 重量: host->run_serialized(
                │                                        │   [GC / rebase / 发布影子])
宿主收尾:                                     全插件 commit 返回后，宿主提交
                                              成对保存点 RunFn（flush → docmap
                                              → keydir 快照 → manifest）
```

**关键机制**：

1. **同步性靠事件在 merge 线程上直接派发**：on_merge_begin/commit 与 merge
   同步执行，插件可在 merge 进行的同时并行做自己的 merge 等价物（影子重建、
   rebase 准备），而不是事后被动收提示。
2. **单写者不变量不破**：merge 线程回调里**不得直接变异**插件的单写者状态；
   两条安全路径——(a) 影子构建 + 原子发布（HNSW 的
   `atomic<shared_ptr>` 发布模式现成，hnsw_ 即如此）；(b)
   `host->run_serialized(fn)` 把变异逻辑投递到 reducer 静止点（现
   `IndexOp::RunFn` 机制的正式化，从 Cask 专用变插件可用）。
3. **收尾顺序确定性靠 RunFn FIFO**：插件在 on_merge_commit 里提交的 GC/rebase
   RunFn，先于宿主随后提交的成对保存点 RunFn 执行（同队列 FIFO）——
   等价现状「compact/rebuild 在前、ckpt 在后」的硬编码顺序，但由提交序
   自然涌现，宿主不再知道插件收尾的内容。
4. **现有行为的映射**：`search_->compact(0.2)` + `compact_index_chunks` →
   TextPlugin::on_merge_commit 里 run_serialized；`RebuildHnsw` →
   VectorPlugin::on_merge_commit 里 run_serialized（内部仍是影子建图 +
   原子发布）；`dead_ratio` 阈值判断从 Cask 迁入各插件（用
   MergeCommitEvent.dead_ratio）。
5. **RelocateEvent.value 的定位**：可选便利，不是义务——merge fold 此刻本就
   持有整条记录缓冲，附带零成本；想借 merge 的 I/O 重建自身的插件
   （如从头重建二级索引）可直接消费，不需要的插件忽略。

## 4. DocMap 的归属：宿主服务，不是插件

这是整个拆分的**中心决策**。`index::Index`（ord↔ext、live 位图、meta blob）
被 BM25 和 HNSW 双向依赖，是 §2.2 排第一的杂糅点。三个选项：

- (a) 复制两份，各插件自持 —— 内存翻倍、live 位图双写、meta 过滤语义分叉。否决。
- (b) 作为「0 号插件」，其他插件依赖它 —— 引入插件间依赖与初始化顺序问题，
  违背「插件互不感知」。否决。
- **(c) 上提为宿主侧服务 `DocMap`（选定）** —— 它本来就是 keydir 的影子
  （ord→key 反视图 + 存活 + meta），领域属于「文档身份」而非「搜索」。
  CMake 里 `bitcask_index` 本来就是独立目标，物理上零搬迁。

落法：

- `DocMap` 由 Cask 持有，在 reducer 里**先于所有插件** apply
  （put_doc/set_meta/remove），等价现 reduce_apply 的 ④⑤ 步顺序。
- 插件构造时注入一个**只读窄接口** `const DocTable&`：
  `is_live(ord)`、`ord_to_ext(ord)`、`eval_meta(ord, filter)`。
  实现基础现成：`index::Index` 已实现 `bm25::LiveChecker`（is_live/doc_len +
  SIMD fill 族），DocTable 在其上扩展而非新造。
  BM25 打分存活过滤、HNSW live-callback（hnsw.cpp:924 的 `std::function`
  形状不变）都消费它。
- **doc_len 迁移缓行（P2 实现期修正，2026-07-03）**：doc_len 语义上是 BM25
  打分专属输入，但它是 `DocSlot` 持久化字段（kDocmap 段行内）且以平坦 SoA
  支撑打分的 SIMD gather（`Index::fill_doc_lens`）——迁移同时牵连盘上格式与
  热路径。P2 保持「存储在 DocMap、语义归属 BM25」，P4 与 ckpt 格式变更（P3）
  合并评估。**S16-2 落地通道**：宿主 `put_doc` 时 `doc_len=0` 占位，BM25 侧
  （reducer 单写者）经 `Index::set_doc_len(ord, len)` 回填——同时更新 slots_
  AoS 与 doc_lens_ SoA。
- **DeleteEvent.prior_ord 修正（P2 实现期修正，2026-07-03）**：`SearchLayer::
  on_delete` 旧实现先 `index_.get(key)` 查旧 ord 再调 BM25 统计——宿主先 remove
  后插件查不到。修正：宿主在 docmap remove **前**捕获 `prior_ord`，`DeleteEvent`
  增 `prior_ord` 字段（哨兵 `kNoPriorOrd`=原不存在）。这对 P4 的独立 BM25 插件
  同样必要（插件不该为拿旧 ord 反查 docmap）。
- **DocTable 最终形态（S16-3 落地，2026-07-03）**：新设 `include/bitcask/
  doc_table.hpp`——`DocTable : public LiveChecker`（IS-A），扩展 `ord_to_ext`/
  `eval_meta`/`ord_of`（ext_id→ord 窄投影，explain 用）。`Index` 基类从
  `LiveChecker` 改为 `DocTable`（已有方法仅补 `override`）。BM25 单测的 5 个
  FakeLiveChecker 仅实现 LiveChecker，不受影响（DocTable 不添评分侧方法）。
  SearchLayer 全部查询路径（text/phrase/bool/fields/fuzzy/wildcard/vector/
  hybrid/highlight/explain）经 `const DocTable&` 消费——`materialize_hits` 与
  HNSW live-callback 改为 DocTable 形参，不再直摸 `index_` 具体类型。
- DocMap 自己的持久化 = 现 ckpt 的 `kDocmap/kMeta/kTerms + kDocmapDelta`
  段，随 keydir 快照一起归宿主 checkpoint 管（见 §5），
  S14-7 的 `kKeydirDelta` 同文件成对不变量因此**原样保留**。

## 5. Checkpoint 拆分：宿主 manifest + 每插件独立文件族

现状单一 `search.ckpt` + 单 delta 链的两大痛点：① 任一子系统 rebase
强制全体 rebase（search_layer.cpp:1140,1107,118）；② BM/HNSW 无法独立装配。

新布局：

```
kv.keydir.ckpt              ← 宿主：keydir 快照（现状保留）
docmap.ckpt / .d<seq>       ← 宿主：DocMap 基线+delta 链（含 kKeydirDelta 成对段）
bm25.ckpt  / .d<seq> / .wal ← TextPlugin 自治（倒排 WAL 归它）
vec.ckpt   / .d<seq> / .vec / .qc8 ← VectorPlugin 自治（S14-8 侧车归它）
index.manifest              ← 宿主：各组件 {generation, watermark} 清单，最后原子 rename
```

恢复协议（`load_recovery_snapshots` 的推广，动词对应 §3.2）：

1. 载 keydir 快照 → 载 docmap（其 delta 链内联的 keydir 元数据由宿主
   内部机制推进 keydir，即现 DeltaReplayHook 逻辑，cask.cpp:1196-1218
   原样迁移为宿主私有）。
2. 各插件 `open(ctx)`，各自报告 `watermark()`。
3. 宿主 fold data 文件，起点 = **min(所有组件水位)**；每条恢复记录经
   on_put/on_delete 广播给全部插件，插件按自身水位自门跳过已覆盖区间
   （现有机制的推广：HNSW `max_inserted_ord_` 幂等、S14 各段水位自门）。
4. 某插件 ckpt 损坏/缺失 → 只有它水位归零重建，其他组件不受牵连
   （现状是整个 search.ckpt 报废退全量 fold）。

成对性不变量的新表述：**keydir 水位 ≤ min(各组件覆盖水位)**。
保存顺序：先各插件 `flush()` → docmap ckpt → keydir 快照 → manifest。
delta 路径下 keydir 元数据继续内联在 docmap 的 delta 文件里（S14-7 机制），
成对原子性不减。

rebase 解耦红利：`rebuild_hnsw` 只 rebase vec 链，BM25 链不动——
S14-5 的链长上限从「全局链」变「每插件链」，语义更精准。

代价与对策：ckpt 文件数从 1 变 4+manifest；save-point 从单 rename
变多文件+manifest 终结。崩溃窗口分析：manifest 是唯一 commit 点，
任何中途崩溃回退到旧 manifest 指向的旧 generation（各组件文件带
generation 后缀或 .prev 机制沿用 search_layer.cpp:1752-1758 的做法）。

## 6. SearchLayer 一分为三

```
SearchLayer(现)  →  TextPlugin        (bitcask_text_plugin)
                 →  VectorPlugin      (bitcask_vector_plugin)
                 →  HybridSearcher    (bitcask_hybrid，可选，门面层)
```

### TextPlugin（BM25）
拿走：`fields_`（每字段 InvertedIndex）、`analyzer_`、`ord_field_lens_`、
`field_names_intern_`、`doc_texts_`(高亮 LRU)、`synonym_map_`、
`SearchCache`、倒排 WAL、doc_len。
`wants_prepare()=true`：prepare = 现 `map_analyze`（分词，纯函数）；
on_put = 现 reduce_apply 的 ①②③ 步。查询面 = search_text/phrase/near/
fuzzy/bool/fields/wildcard/highlight（现已不碰 hnsw_，平移即可）。
**cache 失效顺带修复**：只在自身 on_put 时失效，纯向量写不再打掉文本缓存
（消除 §2.2-5 的伪共享）。

### VectorPlugin（HNSW）
拿走：`hnsw_`、`delta_vecs_` 插入日志、`.vec/.qc8` 侧车、
`vector_dim/metric/hnsw_m/ef/inmem_int8` 配置、**以及从 Cask 下沉的
`prepare_vector` 归一化**（cask.cpp:2184 的领域逻辑回家）。
prepare = 归一化/量化准备（纯函数，正好并行）；on_put = 现 `on_vector` 插入。
查询面 = search_vector（live 过滤经 DocTable）。
`rebuild_hnsw` 变成它的 `maintain()` 实现。

### HybridSearcher（融合器，非插件）
现 `search_hybrid` 的 RRF 融合（search_layer.cpp:318-388）整体上移：
持两个插件的查询接口引用，超采 K'=max(4k,64)、RRF(60)、ord 决胜——
纯算法平移。只装一个插件的部署根本不链接它。

### 配置拆分
`SearchLayerConfig` 一分为三：`TextPluginConfig`（analyzer/bm25_params/
cache/positions/synonym/wal_batch/auto_compact）、`VectorPluginConfig`
（dim/metric/m/ef/int8）、每插件各自的 `max_delta_chain`。
`CaskOptions::search_config` 让位给 `CaskOptions::plugins`（插件对象
自带配置），C API 老字段原位映射保持 ABI 兼容。

## 7. merge / 门面 / C API 收尾

- **merge 去搜索化**：`run_merge` 的 `SearchLayer*` 参数改为
  `std::span<CaskPlugin*>`，按 §3.9 协议派发 on_merge_begin →
  逐条 on_relocate（携带 value 视图）→ on_merge_commit/abort；
  merge 收尾的 compact/rebuild/ckpt 硬编码序列（cask.cpp:2842-2910）
  改由各插件在 on_merge_commit 里经 `run_serialized` 自主提交。
  `bitcask_merge` 的 PUBLIC 依赖从 `bitcask_search` 降为 `bitcask_plugin_api`。
- **Cask 门面瘦身**：`search_text/search_vector/...` 系列方法迁出 Cask，
  改由 `text::Searcher{Cask&, TextPlugin&}` / `vec::Searcher{...}` 门面提供；
  Cask 保留通用的 `drain_plugins()`（现 `prepare_search`/`flush_index`
  读屏障，属宿主流水线机制）。cask.hpp:54 的 search include 删除，
  `SearchHit` 等结果类型迁入插件头。
- **C API 兼容**：`bitcask_c.h` 现有函数签名全部保留，实现改为组装门面；
  内部按 `bitcask_kv.h / bitcask_text.h / bitcask_vec.h` 分文件，
  旧头聚合 include。

## 8. CMake 目标终态

```
bitcask_plugin_api   INTERFACE  （新，仅依赖 format）
bitcask_cask         → keydir fileops io format merge docmap plugin_api   ← 不再依赖 search
bitcask_merge        → keydir fileops io format plugin_api               ← 不再依赖 search
bitcask_docmap       ←(改名自 bitcask_index) index.cpp + meta
bitcask_text_plugin  → bm25 text docmap(读接口) plugin_api  （含 highlighter/search_cache）
bitcask_vector_plugin→ vector docmap(读接口) plugin_api
bitcask_hybrid       → text_plugin vector_plugin （可选）
bitcask_search       （过渡期兼容 shim，迁移完成后删除）
```

三种发布形态由此解锁：KV-only（现有 enable_search=false 路径，jieba/TBB
不进链接）、KV+BM25（无向量依赖）、KV+HNSW（无 jieba/utf8proc）。

## 9. 迁移阶段（每阶段独立可回归、TSan 全绿再进下一阶段）

| 阶段 | 内容 | 风险 |
|---|---|---|
| P1 ✅ | `bitcask_plugin_api` 头 + thread_pool 类型擦除（ReduceEntry 去 ReduceJob）；SearchLayer 原样套 adapter 变「唯一插件」 | 低：纯接口化，行为零变——已落地（S15，2026-07-03） |
| P2 ✅ | DocMap 抽离为宿主服务，SearchLayer 改消费 `const DocTable&`；doc_len 迁 BM25 侧 | 中：动 reduce_apply 内部顺序，需 TSan + 恢复回归——已落地（S16，2026-07-03；含 prior_ord 修正 + doc_len 回填通道 + DocTable 查询面化） |
| P3 ✅ | checkpoint 拆分（manifest + 每组件文件族 + min-水位恢复协议） | **高**——已落地（S17，2026-07-03；含单组件注错测试与旧 search.ckpt 一次性迁移器） |
| P4 ✅ | SearchLayer 拆 TextPlugin/VectorPlugin，hybrid 上移；merge/恢复改插件广播 | 中——已落地（S18，2026-07-04；11 子任务，归一化改同步调用等偏离见 TASK.md S18 批次头） |
| P5 ✅ | Cask 门面瘦身、C API 分文件、配置拆分、删 shim、文档 | 低——已落地（S19，2026-07-04；门面取薄委托温和版 + Searcher 门面，shim 降级测试夹具，配置换代留待第三方插件需求） |

P3 是唯一真正的高风险阶段，建议单独成批次（S15-x），
且 P1/P2/P4/P5 均不依赖 P3 的文件布局——若 P3 评审不过，
可退化为「统一容器保留、段按插件归属划分接口」的方案 B，
其余拆分照常成立。

## 10. 开放问题（带倾向）

1. **恢复 fold 的广播成本**：min-水位起跳后每条记录广播全插件、
   插件自门跳过——水位差大时有无谓解码。倾向：宿主按各插件水位分段 fold
   （区间 [wm_i, wm_j) 只喂水位更低的插件），实现简单收益实在。
2. **Prepared 堆分配**：reduce 热路径 bench 回退超 3% 再上 arena，不预优化。
3. **meta 过滤归属**：`MetaFilter` 求值留在 DocMap（双方共用），
   过滤器解析留在各查询门面。倾向已定，列出备查。
4. **`bitcask_index` 目标改名 `bitcask_docmap`**：破坏下游链接名。
   倾向：ALIAS 兼容一个版本期。
5. **运行中动态挂载插件**：attach-after-open 需要回填语义（从插件水位 fold
   到当前头），等价一次受控重建。非目标，仅记录可行性（read_at + fold
   基建都在）。
