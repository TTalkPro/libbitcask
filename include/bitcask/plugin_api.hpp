// plugin_api：KV 存储层的插件回调接口（S15 P1，设计 doc/plugin-arch-split-design-zh.md §3）。
//
// === 定位 ===
// Cask（KV 核心：data/hint/keydir）对上层索引/观测能力零概念——它只认识本头
// 定义的 CaskPlugin 接口，把 KV 固有事件（写入/删除/搬迁/merge/落盘/开关）广播
// 给注册的插件。搜索（BM25/HNSW）只是插件的一种实现；统计、TTL、变更流等
// 非搜索插件同样凭本接口接入。
//
// 本头自包含：只依赖标准库，不 include 任何 bitcask 头。Cask、merge、
// thread_pool 只 include 本头，不 include bm25/vector/search。
//
// === 两相写入契约（现有 IndexPool 流水线 map/reduce 契约的固化）===
// - prepare：纯函数、任意 map worker 线程并发调用、不得触碰插件可变状态。
//   可选能力（wants_prepare() 声明）——不需要预处理的插件事件直达 on_put。
// - on_put / on_delete：reducer 单写者上下文，按 ord 严格升序到达（可有洞：
//   ord 被 KV 层浪费时以 Skip 填充，插件不感知）。
// - 恢复重放复用 on_put/on_delete（无 recover_* 专用动词）：宿主从
//   min(全插件 watermark) 起 fold data 文件重放，插件必须跳过
//   ord <= 自身 watermark() 的事件（幂等义务；HNSW max_inserted_ord_ /
//   倒排 WAL 水位自门机制的接口化）。
// - ord 由宿主（keydir alloc_ord）分配，全局单调、不复用；插件只消费。
//
// === merge 参与协议（设计 §3.9）===
// on_merge_begin → (逐条 on_relocate，携带 value 视图) → on_merge_commit/abort，
// 全部在 merge 线程上与 reducer 并发派发。插件在 merge 回调里不得直接变异
// 自身单写者状态——要么影子构建 + 原子发布，要么经 PluginHost::run_serialized
// 投递到 reducer 静止点。插件在 on_merge_commit 内提交的 run_serialized 闭包
// 先于宿主随后提交的成对保存点执行（同队列 FIFO）。
//
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
    std::uint32_t    tstamp = 0;
};

// S16-2：prior_ord = 被删文档原 ord，宿主在 docmap remove **前**捕获
// （删除统计调整需要旧 ord，插件不必也不能反查已删行）；key 原不存在 =
// kNoPriorOrd（插件应跳过，与「删不存在的 key」的历史语义一致）。
inline constexpr std::uint64_t kNoPriorOrd = ~std::uint64_t{0};

struct DeleteEvent {
    std::uint64_t    ord = 0;   // 墓碑 record 的 ord
    std::string_view key;
    std::uint64_t    prior_ord = kNoPriorOrd;
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
};
struct FlushResult {
    PluginStatus  status      = PluginStatus::kOk;
    std::uint64_t covered_ord = 0;  // 本次落盘覆盖到的 ord 水位
    std::uint64_t generation  = 0;  // 插件自定义的代号（manifest 记录用）
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

    // 在 reducer 静止点串行执行 fn（单写者上下文）。fire-and-forget；
    // 同一提交序 FIFO 执行。插件在 merge 线程等并发上下文要变异自身
    // 单写者状态时，必须经此通道。
    virtual void run_serialized(std::function<void()> fn) = 0;

    virtual void log(LogLevel level, std::string_view msg) = 0;
};

struct OpenContext {
    std::string_view dir;            // 库目录；插件以 name() 为前缀自管文件
    PluginHost*      host = nullptr; // 宿主服务句柄
};

// KV 存储层插件接口。线程/顺序/错误契约见头注释。
class CaskPlugin {
public:
    virtual ~CaskPlugin() = default;

    virtual std::string_view name() const = 0;  // "bm25" / "hnsw" / "metrics" …

    // ---- 生命周期 ----
    // open：载入自身持久化状态；返回后 watermark() 必须反映已覆盖 ord 水位
    // （宿主据此定恢复重放起点）。损坏/缺失自行降级（水位=0 → 全量重放重建）。
    virtual PluginStatus open(const OpenContext& ctx) = 0;
    virtual std::uint64_t watermark() const = 0;
    virtual PluginStatus close() = 0;  // 含终止性 flush

    // ---- 数据事件（reducer 单写者，ord 严格升序）----
    virtual void on_put(const PutEvent& e, PreparedPtr prep) = 0;
    virtual void on_delete(const DeleteEvent& e) = 0;

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
    virtual void on_merge_abort() {}
    virtual void maintain(const MaintainEvent& e) { (void)e; }

    // ---- 持久化 ----
    virtual FlushResult flush(const FlushRequest& req) = 0;
};

}  // namespace bitcask::plugin
