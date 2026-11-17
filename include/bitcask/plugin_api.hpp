// plugin_api：KV 存储层的插件回调接口（S15 P1，设计 doc/plugin-arch-split-design-zh.md §3）。
//
// === 定位 ===
// Cask（KV 核心：data/keydir）对上层索引/观测能力零概念——它只认识本头
// 定义的 CaskPlugin 接口，把 KV 固有事件（写入/删除/搬迁/merge/落盘/开关）广播
// 给注册的插件。搜索（BM25/HNSW）只是插件的一种实现；统计、TTL、变更流等
// 非搜索插件同样凭本接口接入。
//
// 本头自包含：只依赖标准库，不 include 任何 bitcask 头。Cask、merge、
// thread_pool 只 include 本头，不 include bm25/vector/search。
//
// === 写入契约（S45-4：索引管线去全局顺序化，设计
// designs/index-pipeline-unordered-design-zh.md §4.7）===
// - **到达序任意**：事件不再按 ord 升序到达。宿主保证 ① 同 key 事件串行
//   （同一应用线程，按提交序）；② 已按「key + ord」裁决——乱序到达的旧版本
//   与已被后续删除覆盖的写不会送达插件（规则 R）。插件仍须对**同 ord 重复**
//   幂等（恢复重放 / 重复投递）。
// - 并发能力由 concurrency() 声明：
//     kSharded —— 不同 key 的事件在 N 个分片线程上**并发**到达（PutEvent/
//                 DeleteEvent::shard 标明分片，同分片恒同一线程；OpenContext::
//                 shards 给出分片数，供插件预建每分片的单写者结构）；
//     kSerial  —— 全部事件在同一个串行线程到达（默认；单写者结构如 HNSW）。
//                 串行道在分片阶段之后执行：插件执行时宿主 docmap 可能已应用
//                 同 key 的更新版本（docmap 恒先于插件，但不停留在本版本）。
// - prepare：可选纯函数预处理（wants_prepare()），宿主在同一应用线程、紧挨
//   on_put 之前调用（独立 map 阶段已退役）；不得触碰插件可变状态。
// - accepts_put()/accepts_delete()：串行插件的事件过滤（如向量插件只收带
//   向量的写），减少串行线程负担；分片插件同样适用。
// - 恢复重放复用 on_put/on_delete（无 recover_* 专用动词）：宿主从
//   replay_floor_of(min(全插件 watermark)) 起 fold data 文件重放，重放任务
//   与在线写走同一分片执行器（并行、到达序任意，PutEvent::replay = true）。
//   插件幂等义务：watermark() 之上的事件**可能已应用过**（乱序应用、早于
//   落盘；或旧版本 ckpt 的水位高估——宿主为此多重放 kReplayFloorMargin），
//   须按 **key + LSN** 判重（该 key 已有 LSN ≥ 本事件即跳过；删除保留墓碑），
//   不得用单调水位门。内置实现：TextPlugin 的 key_to_location_、向量插件的
//   detail::VectorKeyLocations。
// - ord 由宿主（keydir alloc_ord）分配，全局单调、不复用；插件只消费。
//
// === 独占维护（run_serialized）===
// PluginHost::run_serialized(fn)：维护线程上 FIFO 执行；执行期间本库暂停
// 应用（无任何 on_put/on_delete 并发）。插件的压实、重建、封口合并等需要
// 「无并发写」的操作必须经此通道。闭包内不得提交写、不得等读屏障。
//
// === merge 参与协议（设计 §3.9）===
// on_merge_begin → (逐条 on_relocate，携带 value 视图) → on_merge_commit/abort，
// 全部在 merge 线程上与应用线程并发派发。插件在 merge 回调里不得直接变异
// 自身单写者状态——要么影子构建 + 原子发布，要么经 run_serialized 投递。
// 插件在 on_merge_commit 内提交的 run_serialized 闭包先于宿主随后提交的
// 成对保存点执行（同队列 FIFO）。

// === 错误契约 ===
// 数据事件回调（prepare/on_put/on_delete/on_relocate）抛出的异常由宿主吞并：
// 错误计数自增 + 日志上报、流水线保活、ord 照常推进（S13-D7 语义）。
// open/flush/close 以返回值报告失败，宿主决定降级策略。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace bitcask::plugin {

// S45-5：恢复重放起点的安全余量。宿主从 max(0, min(全组件 watermark) − 本值)
// 起重放（插件按 key + LSN 判重，多重放的部分幂等跳过）。
// 余量吸收 S45 之前版本 checkpoint 的覆盖水位微小高估（旧水位取「提交时刻
// 的 peek_next_ord」，可能计入数个尚未应用的 LSN）。
inline constexpr std::uint64_t kReplayFloorMargin = 4096;
[[nodiscard]] inline constexpr std::uint64_t replay_floor_of(std::uint64_t wm) noexcept {
    return wm > kReplayFloorMargin ? wm - kReplayFloorMargin : 0;
}

// 插件操作结果（本头自包含，不复用 CaskError——插件失败语义只有成败与描述）。
enum class PluginStatus : std::uint8_t {
    kOk = 0,
    kFailed = 1,
};

enum class LogLevel : std::uint8_t { kWarn = 0, kError = 1 };

// 记录的存储定位（data 文件三元组）。merge 搬迁时更新。
struct RecordLoc {
    std::uint32_t file_id  = 0;
    std::uint64_t offset   = 0;
    std::uint32_t total_sz = 0;
};

// 命名字段（名字已由宿主经 field.schema 还原）。
// P1 沿用 IndexTask 的 pair 布局实现零拷贝借用（.first=名 .second=值）；
// P4 视需要升格为具名 struct。
using FieldKV = std::pair<std::string_view, std::string_view>;

// 结构化 value 视图。宿主在 put_doc 路径本就持有各部件，附带零成本；
// 插件各取所需（向量归一化等领域处理是插件自己的事，宿主不预加工）。
struct DocView {
    std::string_view           text;
    std::span<const FieldKV>   fields;
    std::span<const float>     vec;
    std::span<const std::byte> meta;
};

// 写事件。所有 view/span 仅在回调期间有效，插件要留存就拷贝。
struct PutEvent {
    std::uint64_t    ord = 0;   // 宿主分配，全局单调、不复用
    std::string_view key;
    std::string_view value;     // 原始 value 字节（KV 视角）
    const DocView*   doc = nullptr;  // 结构化视图；纯 KV 写为 nullptr
    RecordLoc        loc;
    std::uint64_t    tstamp = 0;
    // S18-8：恢复重放标记。恢复与在线写是同一事件（接口契约），但插件可
    // 据此调整**执行策略**（非语义）：如 TextPlugin 在重放批里让单文本也走
    // prepare 并行分析（S3 恢复期并行语义），活写路径路由不动。
    bool             replay = false;
    // S45-4：分片下标（kSharded 插件：同分片恒同一线程；kSerial 插件忽略）。
    unsigned         shard = 0;
};

// S16-2：prior_ord = 被删文档原 ord，宿主在 docmap remove **前**捕获
// （删除统计调整需要旧 ord，插件不必也不能反查已删行）；key 原不存在 =
// kNoPriorOrd（插件应跳过，与「删不存在的 key」的历史语义一致）。
inline constexpr std::uint64_t kNoPriorOrd = ~std::uint64_t{0};

struct DeleteEvent {
    std::uint64_t    ord = 0;   // 墓碑 record 的 ord
    std::string_view key;
    std::uint64_t    prior_ord = kNoPriorOrd;
    unsigned         shard = 0;  // S45-4：同 PutEvent::shard
};

// merge 搬迁事件：ord 不变、只换存储定位。value 视图免费附带——merge fold
// 此刻正持有整条记录缓冲，供插件借 merge 的 I/O 做影子重建，不需要则忽略。
struct RelocateEvent {
    std::uint64_t    ord = 0;
    std::string_view key;
    RecordLoc        loc;        // 新定位
    std::string_view value;      // 仅回调期间有效
};

// merge 生命周期事件（merge 线程派发，与 reducer 并发——见头注释协议）。
struct MergeBeginEvent {
    std::span<const std::uint32_t> input_file_ids;
    std::uint64_t                  watermark = 0;  // merge 启动时的 ord 水位
};
struct MergeCommitEvent {
    std::span<const std::uint32_t> output_file_ids;
    double                         dead_ratio = 0.0;  // 本轮回收的死记录占比
};

// 维护提示（reducer 静止点派发）。
struct MaintainEvent {
    enum class Reason : std::uint8_t { kPostMerge = 0, kAuto = 1 };
    Reason reason = Reason::kAuto;
    double dead_ratio_hint = 0.0;
};

// 落盘请求/结果。base/delta、链长、rebase 全是插件内部策略——宿主只说
// 「落盘到当前已 apply 水位」并收水位回执（用于成对性不变量
// keydir 水位 <= min(各插件覆盖水位)）。
struct FlushRequest {
    enum class Reason : std::uint8_t { kClose = 0, kMerge = 1, kAuto = 2, kManual = 3 };
    Reason reason       = Reason::kManual;
    bool   force_rebase = false;  // close 等要求收链的场合
    // S18-6：本次落盘的覆盖水位（宿主在提交时刻捕获的全局 ord 上界——
    // 各插件文件以同一水位 stamping，成对不变量「keydir 水位 ≤ min(各组件
    // 覆盖水位)」由此成立）。
    std::uint64_t watermark = 0;
};
struct FlushResult {
    PluginStatus  status      = PluginStatus::kOk;
    std::uint64_t covered_ord = 0;  // 本次落盘覆盖到的 ord 水位
    std::uint64_t generation  = 0;  // 插件自定义的代号（manifest 记录用）
    // S20-3 B-B2：链回执——manifest entry 三元组 {generation, chain_seq,
    // chain_wm} 由 flush 直接回传，宿主无须下探具体插件类型读 chain_state()。
    // 仅在 status==kOk 且 covered_ord==req.watermark（entry 将被更新）时有意义。
    std::uint32_t chain_seq   = 0;
    std::uint64_t chain_wm    = 0;
};

// prepare 相产物（类型擦除；由产出它的插件在 on_put 中消费）。
struct Prepared {
    virtual ~Prepared() = default;
};
using PreparedPtr = std::unique_ptr<Prepared>;

// 宿主服务（插件 → 宿主的窄反向接口）。生命周期覆盖 open..close。
class PluginHost {
public:
    virtual ~PluginHost() = default;

    // 按存储定位读回原始记录 value（重建/回填场景）。失败返回 nullopt。
    virtual std::optional<std::string> read_at(RecordLoc loc) = 0;

    // 独占执行 fn（S45-4：维护线程 FIFO；执行期间本库暂停应用，无任何
    // on_put/on_delete 并发）。fire-and-forget。插件在 merge 线程等并发上下文
    // 要变异自身单写者状态、或需要「无并发写」的压实/重建时，必须经此通道。
    // 可在任意线程（含 on_put 内）调用——只入队不等待，不会自锁。
    // ⚠️ fn 内不得提交写、不得等读屏障（二者都要等应用推进，而应用正被 fn
    // 暂停）。
    virtual void run_serialized(std::function<void()> fn) = 0;

    virtual void log(LogLevel level, std::string_view msg) = 0;
};

struct OpenContext {
    std::string_view dir;            // 库目录；插件以 name() 为前缀自管文件
    PluginHost*      host = nullptr; // 宿主服务句柄
    // S18-6：宿主 commit 簿（manifest）里本插件的已提交状态提示——插件据此
    // 校验/续接自己的持久化代际。全零 = 无已提交状态（新库/损坏 → 插件
    // 从零自建，watermark() 报 0 让宿主全量重放）。通用字段，无搜索概念。
    std::uint64_t committed_base_watermark  = 0;
    std::uint64_t committed_chain_watermark = 0;
    std::uint32_t committed_chain_seq       = 0;
    // S45-4：应用分片数（kSharded 插件据此预建每分片单写者结构；≥ 1）。
    unsigned      shards = 1;
};

// KV 存储层插件接口。线程/顺序/错误契约见头注释。
class CaskPlugin {
public:
    virtual ~CaskPlugin() = default;

    // name() 是组件的**持久化/manifest 身份**（标识落盘算法与文件族），
    // 与查询 API 的命名空间（text::/vec::）是两套词汇——前者按算法命名
    // （"bm25"/"hnsw"），后者按领域命名。宿主用 name() 反查 ComponentId。
    virtual std::string_view name() const = 0;  // "bm25" / "hnsw" / "metrics" …

    // ---- 生命周期 ----
    // open：载入自身持久化状态；返回后 watermark() 必须反映已覆盖 ord 水位
    // （宿主据此定恢复重放起点）。损坏/缺失自行降级（水位=0 → 全量重放重建）。
    virtual PluginStatus open(const OpenContext& ctx) = 0;
    virtual std::uint64_t watermark() const = 0;
    virtual PluginStatus close() = 0;  // 含终止性 flush

    // ---- 数据事件（S45-4：同 key 串行、到达序任意；并发度见 concurrency()）----
    virtual void on_put(const PutEvent& e, PreparedPtr prep) = 0;
    virtual void on_delete(const DeleteEvent& e) = 0;

    // ---- S45-4：并发能力与事件过滤 ----
    enum class Concurrency : std::uint8_t {
        kSerial  = 0,  // 全部事件同一线程（默认，最保守）
        kSharded = 1,  // 不同 key 并发；同分片同线程
    };
    virtual Concurrency concurrency() const { return Concurrency::kSerial; }
    virtual bool accepts_put(const PutEvent& e) const {
        (void)e;
        return true;
    }
    virtual bool accepts_delete() const { return true; }

    // ---- 可选能力：并行预处理（纯函数，任意线程）----
    virtual bool wants_prepare() const { return false; }
    virtual PreparedPtr prepare(const PutEvent& e) const {
        (void)e;
        return nullptr;
    }

    // ---- 存储维护 / merge 参与（默认空实现，不参与的插件零成本）----
    virtual void on_relocate(const RelocateEvent& e) { (void)e; }
    virtual void on_merge_begin(const MergeBeginEvent& e) { (void)e; }
    virtual void on_merge_commit(const MergeCommitEvent& e) { (void)e; }

    // S27-4 P2:读屏障钩子——宿主在查询前(prepare_search,已 flush 索引
    // 流水线之后)调用;插件内部若有异步 apply(如 DWPT builder),须在返回
    // 前排干,保证 read-your-writes。默认 no-op。
    virtual void drain() {}
    virtual void on_merge_abort() {}
    virtual void maintain(const MaintainEvent& e) { (void)e; }

    // ---- 持久化 ----
    virtual FlushResult flush(const FlushRequest& req) = 0;
};

}  // namespace bitcask::plugin
