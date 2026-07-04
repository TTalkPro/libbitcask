// bitcask 高层门面：把 keydir、data file、hint file、scanner、merger 串起来，
// 整合成一个进程级 handle。专为 M3.4 的「粗粒度 NIF 暴露」设计——
// 一次 open / close / get / put / delete 就够，替代 legacy 那 30+ 个细粒度
// keydir_*_int / file_*_int 调用。
//
// === 线程模型（S11：通用 C++ 库,handle 内部线程安全；docs/design/thread-safety.md）===
//
// 同一个 Cask handle 可被多线程安全共享：
//   - **读**（get / search* / search_vector / 批量搜索）：并发安全,无锁/共享锁。
//     keydir get + DataFile pread thread-safe;read_files_ cache 受 read_cache_mu_ 护;
//     搜索读 cache_/doc_texts_ shared_mutex、倒排/HNSW shared_lock、analyzer const。
//   - **写**（put / remove / put_doc / sync / close_write_file）：并发安全,由内部
//     `write_mu_` 串行化（S11-W1）。写在文件层本就串行 → 锁不损吞吐;需要更高写
//     并发 → 按目录分片多个 Cask 实例（横向扩展）。
//   - **读写并发**：安全;搜索可见性遵循 near-real-time 契约（prepare_search flush
//     覆盖调用前的写）。
//   - **merge**：与读写并发,经 keydir shared_mutex 协调（不取 write_mu_,写自有
//     输出文件）。
//
// 例外（非 handle 级线程安全,见各方法注释）：
//   - `CaskIter`：每线程一个迭代器（同 std 容器迭代器约定）;不同迭代器并发安全。
//   - `close()`：caller 须保证关闭时刻无在途操作。
// （同义词词典已改为 open-time 不可变配置 `CaskOptions::synonym_map`，不再是运行期
//  可变项 → 无并发竞态。）
//
// 底层 KeyDir 可在同目录多个 Cask 间共享（KeyDirRegistry 管 refcount）。

#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bitcask/data_file.hpp"
#include "bitcask/file_lock.hpp"
#include "bitcask/hint_file.hpp"
#include "bitcask/docmap_ckpt.hpp"    // S18-2:docmap 持久化归宿主
#include "bitcask/index_manifest.hpp"  // S17-2:per-component commit
#include "bitcask/keydir.hpp"
#include "bitcask/merge_policy.hpp"
#include "bitcask/merger.hpp"
#include "bitcask/meta_file.hpp"
#include "bitcask/field_schema.hpp"
#include "bitcask/meta_filter.hpp"
#include "bitcask/plugin_api.hpp"
#include "bitcask/hybrid_searcher.hpp"  // S19-2：Cask 直持融合器
#include "bitcask/search_config.hpp"    // S19-2：公开配置面（SearchLayerConfig）
#include "bitcask/text_plugin.hpp"
#include "bitcask/vector_plugin.hpp"
#include "bitcask/thread_pool.hpp"

namespace bitcask {

// 前置声明：registry_ 仅用作裸指针，完整定义在 cask.cpp 内 include。
namespace keydir { class KeyDirRegistry; }

// --- 配置 --------------------------------------------------------------------
struct CaskOptions {
    bool          read_write       = false;
    std::uint64_t max_file_size    = 2ULL * 1024ULL * 1024ULL * 1024ULL;  // 2 GiB
    // P9/S12-1：read 句柄缓存上限（每句柄 = 1 fd + 1 sealed mmap，故此值同时界定
    // fd 数与 mmap 映射数）。取值：
    //   0（默认）              → **自动**：由 RLIMIT_NOFILE 软上限推导安全上限
    //                            （约一半，下限 64），开箱即防大库无界累积 fd/mmap
    //                            撞 `ulimit -n` / `vm.max_map_count`；
    //   kUnlimitedReadHandles → 不限（旧默认行为：最大吞吐、无淘汰 churn，caller 自负 fd 预算）；
    //   其它 N                → 显式上限。
    // 超额时近似 LRU 淘汰**空闲**句柄（在途读者持 shared_ptr 续命，随最后引用析构才释放）。
    static constexpr std::size_t kUnlimitedReadHandles = static_cast<std::size_t>(-1);
    std::size_t   max_read_handles = 0;
    bool          o_sync           = false;
    // P4 单写者组提交：每 N 次写（put/remove）后对 active data file fsync 一次，
    // 兼顾持久性与吞吐（区别于 o_sync 的每条 durable）。0 = 关闭（默认）。
    // o_sync 为真时本项无意义（已逐条 durable）。
    std::uint32_t sync_every_n     = 0;
    // S14-1 自动 checkpoint：roll 封口点若自上次 ckpt 以来的 ord 增量 ≥ 本值，
    // 异步（reducer 线程 RunFn，fire-and-forget，不阻塞写者）落 keydir 快照 +
    // search.ckpt——把崩溃恢复重放窗口钳制在 ~本值 + 一个文件的写入量内。
    // 0 = 关闭（默认）。仅索引模式生效（纯 KV 恢复本就走 hint 快路径）；
    // 精细节奏控制用 checkpoint() API。
    std::uint32_t auto_checkpoint_min_docs = 0;
    bool          require_hint_crc = false;  // legacy 默认 false；M5 之后可能改 true
    // tstamp < (now - expiry_secs) 的 record 在 get/fold 中被过滤，
    // 同时进入「过期触发 merge」的候选。0 = 禁用。
    std::uint32_t expiry_secs      = 0;

    // merge_only 模式（M5.1 task 2）。true 时：
    //   - 拿 bitcask.merge.lock 而不是 bitcask.write.lock：原 writer 仍能
    //     正常运行（持有 write.lock），并发 merge 不冲突；
    //   - 不创建 active writer 文件——merger 自己生成新输出文件
    //     （keydir->increment_file_id()）；
    //   - open 时读 bitcask.write.lock，得知 live writer 当前 active file id，
    //     在 needs_merge 里把它从候选里排除（不能并别人正在写的文件）。
    //
    // 这是 bitcask:merge/N 在 cask_cpp 模式下的实现机制——周期性的
    // merge_worker 不会跟主 writer 互相阻塞。
    // 跟普通 read_write 模式互斥；merge_only 隐含 read_write 文件语义
    // （要写新文件），但不在该 Cask handle 上提供 put/delete API。
    bool          merge_only       = false;

    // remove() 写入哪种墓碑格式。读时三种 (v0/v1/v2) 都接受。
    //   0 → "bitcask_tombstone"            (17 B)  默认，最简单
    //   2 → "bitcask_tombstone2" + FileId  (22 B)  支持「shadow file_id 仍存在
    //                                              才允许 merge 时回收」的精细
    //                                              并发控制
    // (v1 在磁盘上跟 v2 同形不同前缀；legacy 用作中间态，cask 不写它，
    //  只读时识别。)
    std::uint8_t  tombstone_version = 0;

    merge::PolicyOptions policy{};
    // Phase 4: enable_search 用于 meta 检查；search_config 用于 SearchLayer 创建。
    // search_config.has_value() 时才真正创建 SearchLayer。
    bool enable_search = false;
    std::optional<search::SearchLayerConfig> search_config;
    // V3.1:向量配置(hnsw-design §1)。dim>0 即启用,要求 enable_search;
    // 创建时写入 meta,重开校验不符 → kModeMismatch。库内 dim 恒定。
    std::uint16_t vector_dim = 0;
    bool          vector_quantized = false;  // P3b：向量落盘 int8 量化（4× 磁盘，有损）
    bool          vector_inmem_int8 = false; // P5b：HNSW int8-only 内存（−80% 向量内存，仅 kDot；与 quantized 正交）
    meta::VectorMetric vector_metric = meta::VectorMetric::kCosineNormalized;
    // 同义词词典（**Cask 级、open-time、不可变**）。查询时 search_text/search_fields
    // 自动展开同义词。构造后只读 → 并发查询安全（取代了曾经的运行期
    // set_synonym_map setter，那是配置项里唯一的 reader-vs-writer 竞态源）。
    // 空 = 不展开。仅在 enable_search 时生效；运行期更换词典请重开库。
    std::shared_ptr<const text::SynonymMap> synonym_map;

    // S13-D7：日志回调（**open-time、不可变**——沿 synonym_map 的「不可变
    // 配置无竞态」模式）。库内 best-effort 静默失败点（checkpoint/快照保存
    // 失败、索引 worker 异常、merge 收尾 unlink 失败、stuck 重定位等）经此
    // 上报。空 = 不上报（默认，零开销）。
    // 契约：可能从**任意内部线程**（写线程、reducer、merge 调用线程）调用；
    // 回调自身必须线程安全、不得抛出（抛出被吞掉）、不得回调进本 Cask
    // （死锁风险）。消息为一行人类可读文本，仅在回调期间有效。
    enum class LogLevel : std::uint8_t { kWarn = 0, kError = 1 };
    std::function<void(LogLevel, std::string_view)> log_fn;
};

// --- 错误码 ------------------------------------------------------------------
enum class CaskError {
    kIo,
    kBadCrc,
    kNotFound,            // get 不到 key
    kKeyTooLarge,
    kValueTooLarge,
    kAlreadyExists,       // CAS race
    kReadOnly,            // 写操作给到只读 cask
    kWriteLocked,         // 别人已经持有 write.lock / merge.lock
    kInvalidOption,
    kNoIndex,           // KV 模式下调用了 search 接口
    kModeMismatch,      // 文件模式与打开选项不匹配
    kAnalyzerMismatch,  // 分析器类型不匹配
    kClosed,            // 对已 close 的 handle 发起调用（S12-5：与 kInvalidOption 区分）
};

struct CaskFault {
    CaskError kind;
    int errnum = 0;
    std::string detail;
};

// V6.1 零拷贝 get 结果——span 借用 ReadRecord 缓冲，无堆分配。
// 声明顺序决定初始化顺序：storage_ 必须在 spans 之前声明。
// (前置声明 GetResult:to_owned() 返回类型仅在 .cpp 中需要完整定义)
struct GetResult;
struct GetResultView {
private:
    friend class Cask;
    fileops::ReadRecord storage_;          // ① owned(pread)路径：持 pread 数据
    // P6:② mmap 路径——持 sealed DataFile 的 shared_ptr 锚定映射(view 生命内
    // 映射不撤,即便期间 merge unlink),value_bytes_ 指向映射,storage_ 空。
    std::shared_ptr<fileops::DataFile> map_holder_;
    // DocValue 原始字节来源(owned: 借 storage_.value;mmap: 指向映射)。
    std::span<const std::byte> value_bytes_{};
    format::RecordType rec_type_ = format::RecordType::kDoc;
    // P3b:量化文档落盘是 int8，无法零拷贝成 f32 span——dequant 进此拥有缓冲，
    // vector span 指向它。未量化时为空，vector span 直接借底层字节（零拷贝）。
    std::vector<float> vector_dequant_;

public:
    std::span<const std::byte> value{};    // text 段（指向底层字节内部）
    std::span<const std::byte> meta{};     // meta 段（可为空）
    std::span<const float> vector{};       // 向量段（空=无向量）
    std::uint32_t tstamp = 0;
    std::uint64_t ord = 0;
    std::uint32_t expiry_at = 0;           // S13-D5：per-key 过期时刻（0=永不）

    /// 拷贝为 owned 版本
    GetResult to_owned() const;

    // 可移动（std::expected 要求），不可拷贝
    GetResultView(GetResultView&& other) noexcept;
    GetResultView(const GetResultView&) = delete;
    GetResultView& operator=(const GetResultView&) = delete;

private:
    explicit GetResultView(fileops::ReadRecord&& rec);  // owned(pread)
    // P6:mmap 命中——holder 锚定映射,value_bytes 指向映射内的 DocValue 字节。
    GetResultView(std::shared_ptr<fileops::DataFile> holder,
                  std::span<const std::byte> value_bytes,
                  format::RecordType type,
                  std::uint32_t tstamp, std::uint64_t ord);
    // 从 value_bytes_ 解出 value/meta/vector span（量化则 dequant 进
    // vector_dequant_）。三个 ctor 共用，避免漂移。
    void derive_from_storage();
};

struct GetResult {
    std::vector<std::byte> value;  // DocValue 解码后的 text 段（纯 binary）
    std::vector<std::byte> meta;   // DocValue 解码后的 meta 段（可为空）
    std::vector<float> vector;     // V3.1:向量段(空 = 该文档无向量)
    std::uint32_t tstamp = 0;
    std::uint64_t ord = 0;
};

struct TextSearchResult {
    std::vector<search::SearchHit> hits;
};

// put_doc 的输入结构：text 是必须的，meta 可选。
// S8.6：fields 非空时走多字段路径（编码进 DocValue v2 fields 段 + 多字段索引）。
struct DocInput {
    std::span<const std::byte> text;    // required（多字段时可空，作默认字段）
    std::span<const std::byte> meta;    // optional
    // V3.1:文档向量(空 = 无)。长度必须 == meta 配置的 vector_dim;
    // cosine_normalized 度量下引擎写入前归一化(存储的即归一化值)。
    std::span<const float> vector{};
    std::vector<std::pair<std::string, std::span<const std::byte>>> fields;  // S8.6
    std::uint32_t expiry_at = 0;  // S13-D5：per-key 过期时刻（0 = 永不）
};

struct StatusInfo {
    std::uint64_t key_count = 0;
    std::uint64_t key_bytes = 0;
    std::uint64_t epoch     = 0;
    std::vector<merge::FileStatus> files;
    // indexed worker 抛异常时自增；非零 = 索引可能漂移，搜索结果可能陈旧
    std::uint64_t index_errors = 0;
    // S13-D8：观测扩展（无索引/不适用时为 0）。
    std::uint64_t hnsw_nodes = 0;            // HNSW 图节点数（含软删死节点）
    std::uint64_t search_cache_entries = 0;  // 查询缓存当前条目数
    std::uint64_t read_handles = 0;          // read 句柄缓存当前大小（fd+mmap 数）
    // 注：倒排 posting 总量（total_postings）未纳入——其统计需遍历
    // concurrent_hash_map，与 reducer 插入并发不安全（S13-F6 同类）；待
    // InvertedIndex 维护原子计数器后再暴露。
};

class Cask;

// --- fold 迭代器 -------------------------------------------------------------
// 遍历 make_iter() 时刻的全部活跃 (key, value)。snapshot 语义靠
// KeyDir::IterHandle 提供；每条 entry 的 value 在 next() 时按需 pread。
// 设计上是「per-step 一次 NIF 调用」，方便上层在 BEAM scheduler 之间让出。
//
// === 线程模型 ===
// CaskIter 自身不持锁，方法非线程安全——同一对象只能由一个线程使用。
// 但不同 CaskIter 对象之间可在多线程并发使用同一个 parent Cask
// （读路径并行 + KeyDir::IterHandle 支持多 fold）。
//
// === 生命周期契约（T9 / 见 X1）===
// 1. CaskIter **可以** 跨越 `Cask::close()` 存活：close() reset 自己的 KeyDir
//    shared_ptr，但 CaskIter 经 `keydir_pin_` 持一份引用让 KeyDir 续命；start()
//    时 pin_files() 已 pin 住 sealed fd，故 close() 后 `start/next/release` 仍
//    可用（next() 对未 pin 文件退回 parent_->read_file lazy-open，文件仍在盘上）。
//    多 iterator 交错 release 时，最后一个（keyfolders_→0）才触发 KeyDir 的
//    pending 应用 + MultiEntry 折叠——全在 pinned KeyDir 上安全完成。
// 2. CaskIter **必须先于 Cask 对象本身析构**。`parent_` 是裸 `Cask*`（非
//    weak/shared）：若 Cask 对象被销毁（持有它的 unique_ptr 析构）而 iterator
//    仍存活，则 `next()` 访问 `parent_->opts_/dirname_/read_file` 即悬空 UAF。
//    这与 close() 正交——close() 不销毁 Cask 对象。X1 的 pin 兜 KeyDir 生命周期，
//    但不兜 Cask 对象本身（裸 parent_ 的结构性问题，留待 zero-copy 重构时
//    用 weak_ptr/owning 句柄解决；在 next() 加 keydir_ 空检查无用——既会误杀
//    上述「合法的 close 后 next()」，又无法防住真正的 parent_ 悬空）。
class CaskIter {
public:
    explicit CaskIter(Cask* parent) noexcept : parent_(parent) {}
    ~CaskIter() noexcept;
    CaskIter(const CaskIter&) = delete;
    CaskIter& operator=(const CaskIter&) = delete;

    // see_tombstones：true 时被删除的 key 也会作为一条 entry 出现在
    // next() 里——is_tombstone=true，value 是墓碑标记字节
    // （对应 legacy fold/6 + fold_keys/6 的 SeeTombstones=true 行为）。
    // false（默认）下墓碑被静默跳过。
    //
    // 返回底层 keydir 的 StartIterResult：
    //   kOk         — 真的开始迭代了
    //   kOutOfDate  — pending 表 freshness 检查没过；caller 应稍后重试
    // CaskFault 留给真正的失败（比如 handle 已经在迭代）。
    // 线程安全: 否（修改自身字段）；同一 CaskIter 不可并发使用。
    //
    // S13-D4：key_prefix 非空时只产出以该前缀开头的 key——过滤发生在
    // keydir proxy 层（不 pread value），遍历命名空间（如 "user:"）无需
    // caller 全表扫 + 自行过滤。keydir 是哈希表，过滤版仍是 O(全表) 扫描
    // （省的是 value 读取与跨界拷贝）；有序范围扫描需有序索引，见 TASK.md。
    [[nodiscard]] std::expected<keydir::StartIterResult, CaskFault>
    start(int maxage = -1, int maxputs = -1, std::uint32_t now_sec = 0,
          bool see_tombstones = false,
          std::span<const std::byte> key_prefix = {});

    // 取下一项；end-of-iteration 返回 nullopt。Entry 内部的 vector 拥有
    // 自己的存储，调用方持有期间可任意使用。
    struct Entry {
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        std::uint32_t tstamp = 0;
        std::uint32_t file_id = 0;
        std::uint64_t offset = 0;
        std::uint32_t total_sz = 0;
        bool is_tombstone = false;
        std::uint64_t ord = 0;
    };
    // 线程安全: 否（推进 iter_ + 内部 pread）；同一对象不可并发使用。
    [[nodiscard]] std::expected<std::optional<Entry>, CaskFault> next();

    // 批量取最多 max_n 条 entry；内部循环调 next()。
    // 返回空 vector 表示迭代结束（EOI），非空表示本次批量结果。
    // 中途出错返回 unexpected。空 vector 和 nullopt 语义不同：
    //   empty vector = EOI（正常结束）
    //   unexpected  = 错误
    [[nodiscard]] std::expected<std::vector<Entry>, CaskFault>
    next_batch(std::size_t max_n);

    // 线程安全: 否；幂等。同一对象的 start/next/release 串行调用。
    void release() noexcept;
    [[nodiscard]] bool is_iterating() const noexcept { return iter_ != nullptr; }

private:
    friend class Cask;  // S11-W4：Cask::parallel_scan 调用 drain_live_keys()

    // S11-W4：排干所有 live key（仅 key 拷贝，**不读 value**——走 keydir proxy）。
    // 供 Cask::parallel_scan 取快照后分区并行 get。start() 之后调用,消费迭代器。
    [[nodiscard]] std::vector<std::vector<std::byte>> drain_live_keys();

    // S13：fold 启动时 pin 一份「目录下全部 data file」的只读句柄快照。
    // 并发 merge 在 fold 期间 unlink 旧文件时，已 open 的 fd 让 inode 在
    // Linux 上存活，next() 仍能从 pin 的句柄 pread——不会因文件被删而失败。
    // 不含 active write file（merge 从不合并它，交给 parent_->read_file）。
    void pin_files();

    Cask* parent_;
    // X1:pin 一份 KeyDir 的 shared_ptr，保证迭代器存活期间 KeyDir 不被
    // 释放。Cask::close() 会 reset 自己的 keydir_（并经 registry release
    // 递减引用计数），但 IterHandle 内部持 KeyDir* 裸指针——若迭代器在
    // close() 后才析构（release()→BarrierGuard 锁 KeyDir mutex），裸指针
    // 会悬空 UAF。pin 让 KeyDir 至少活到本迭代器析构。声明在 iter_ 之前
    // → 隐式析构序中后于 iter_ 释放（release() 也显式保证此序）。
    std::shared_ptr<keydir::KeyDir> keydir_pin_;
    std::unique_ptr<keydir::IterHandle> iter_;
    bool see_tombstones_ = false;
    std::string key_prefix_;  // S13-D4：空 = 不过滤
    std::unordered_map<std::uint32_t,
                       std::unique_ptr<fileops::DataFile>> pinned_files_;
};

// --- Cask ------------------------------------------------------------------
class Cask {
public:
    Cask() = default;
    ~Cask();
    Cask(const Cask&) = delete;
    Cask& operator=(const Cask&) = delete;

    // 打开一个 Cask。通过命名 keydir 跟同目录的其它 Cask 共享 keydir
    // （典型生产形态：每个 NIF 实例一个全局 registry）。
    // S6-P0-pre：registry **强制非空**——双池（异步索引 MapReduce）归属 registry，
    //   无 registry 则池无处可挂。传 nullptr 返回 kInvalidOption（无 nullptr fallback）。
    // 线程安全: 是（每次调用产生独立的 Cask 对象）；registry 自身的并发
    // 由 KeyDirRegistry 内部锁保证。
    // 锁要求: 无。
    [[nodiscard]] static std::expected<std::unique_ptr<Cask>, CaskFault>
    open(std::string_view dirname, const CaskOptions& opts,
         keydir::KeyDirRegistry* registry);

    // 离线升级：将 KV 模式目录升级为索引模式。
    // 前提条件：目录必须存在且当前为 KV 模式；目录必须处于离线状态（无活跃 writer）。
    // 流程：读取 bitcask.meta 验证 KV 模式 → 写入新 meta 标记为索引模式 →
    //       创建 SearchLayer → 扫描所有数据文件重建索引 → 返回只读索引模式 Cask。
    // 线程安全: 是（产生独立的 Cask 对象）。
    // 锁要求: 无（离线操作，不获取 write.lock 或 merge.lock）。
    [[nodiscard]] static std::expected<std::unique_ptr<Cask>, CaskFault>
    upgrade(std::string_view dirname, const search::SearchLayerConfig& search_config);

    // 释放资源。**幂等**（二次 close no-op）。
    // 线程安全: 否（生命周期方法，修改对象状态、释放资源）；caller 须保证关闭
    // 时刻没有其它线程仍在调用 get/put/remove/sync/iter。
    // S11-W3：close 后**新发起**的公共调用 fail-fast 返回 kClosed（"cask
    // is closed"，S12-5 前为 kInvalidOption）而非解引用已释放状态；但与 close
    // **并发在途**的调用仍是 UB
    // （上面的契约）——这是 best-effort 防误用,非完整 rundown。
    void close() noexcept;

    // 单 key 读：keydir.get → DataFile.read 一次 pread。kNotFound 用
    // {error, not_found} 表达，对应 NIF 的 atom not_found。
    // 线程安全: 是（读路径无锁；read_files_ cache 受 read_cache_mu_ 保护，
    // 底层 DataFile::read 用 pread 是 thread-safe 的）。
    // 锁要求: 无外部锁；内部按需取 read_cache_mu_ + keydir mutex。
    // 返回 zero-copy view：value/meta/vector 是指向 pread 缓冲的 span，
    // 生命周期与 returned GetResultView 绑定——move 走就是 move，复制则
    // 仍指向同一缓冲。NIF 即取即用，benchmark / 测试需要持久化用 get_owned。
    [[nodiscard]] std::expected<GetResultView, CaskFault>
    get(std::span<const std::byte> key);

    /// 拷贝语义版本——benchmark 等需要 owned 数据的场景
    [[nodiscard]] std::expected<GetResult, CaskFault>
    get_owned(std::span<const std::byte> key);

    /// P9:当前常驻的 read 句柄数（read_files_ 大小）。内省用（测试断言 fd
    /// 预算上限生效）。线程安全：共享锁读。
    [[nodiscard]] std::size_t read_handle_count() const;

    /// S12-1：把 `CaskOptions::max_read_handles` 解析为 evict 使用的有效上限。
    ///   kUnlimitedReadHandles → 0（evict 语义下的「不限」）；
    ///   0                     → 由 `nofile_soft`（RLIMIT_NOFILE 软上限）推导的
    ///                           安全默认（约一半，下限 64）；
    ///   其它 N                → N（原样）。
    /// 纯函数（不查询系统），便于确定性单测。
    [[nodiscard]] static std::size_t
    resolve_read_handle_cap(std::size_t opt, std::size_t nofile_soft) noexcept;

    // S11-W4：并行全表扫描回调。`fn(key, value)` 对每个 live 文档调用一次；
    // value 是借用本工作线程 read 缓冲的零拷贝 view（仅在本次回调内有效）。
    // **fn 必须线程安全**——不同工作线程并发调用,各处理不相交 key 段。
    using ScanFn = std::function<void(std::span<const std::byte> key,
                                      const GetResultView& value)>;

    // S11-W4：并行全表扫描——单次快照所有 live key（在调用线程串行,仅 key 拷贝,
    // 不读 value），按 `n_threads` 分段,各线程并发 `get()` 读值并调 `fn`。把 W1-W3
    // 建立的「多线程读安全」用于全表扫描（analytics/export/reindex）。读值的 pread+
    // decode 是被并行化的成本;单 append WAL 写串行不受影响。
    //   n_threads==0 → hardware_concurrency()。
    //   并发删除致某 key 在 get 时 kNotFound → 跳过（near-real-time,与搜索一致）；
    //   其它错误（IO/CRC）→ 停止并返回该错误。返回成功遍历到的 key 数。
    //   S13-D4：key_prefix 非空时只扫描以该前缀开头的 key（快照阶段在
    //   keydir proxy 层过滤，非匹配 key 零拷贝零 pread）。
    // 线程安全: 是（快照串行建立 + get 并发安全）。Cask 已 close → kClosed。
    [[nodiscard]] std::expected<std::size_t, CaskFault>
    parallel_scan(std::size_t n_threads, const ScanFn& fn,
                  std::span<const std::byte> key_prefix = {});

    // 写入。tstamp=0 表示用当前 wall-clock 秒。
    // 线程安全: **是**（S11-W1：内部 `write_mu_` 串行化整个写序列;同一 handle 可
    // 被多线程并发写而不损坏。写在文件层本就串行 → 锁不损吞吐;更高写并发 → 按
    // 目录分片多 Cask 实例）。与并发 merge / 并发读（get/search）安全。
    // S13-D5：expiry_at = per-key 过期时刻（绝对 unix 秒；0 = 永不过期）。
    // 过期后 get/iter/scan 视作不存在（kNotFound/跳过），空间在 merge 时回收
    // （merge 对过期记录同时清 keydir）。与整库 opts_.expiry_secs 叠加：任一
    // 判过期即过期。旧版本库读带 TTL 的记录 = 忽略 TTL（永不过期，静默降级）。
    [[nodiscard]] std::expected<void, CaskFault>
    put(std::span<const std::byte> key,
        std::span<const std::byte> value,
        std::uint32_t tstamp = 0,
        std::uint32_t expiry_at = 0);

    // S13-D1：批量写（语义同 put 的 KV 路径）。整批一次提交：记录经
    // write_buffered 聚合成 1MiB 块 pwrite、单次 flush **之后**才 apply
    // keydir / 提交索引任务并返回——批内 syscall 开销从 N 次摊到少数几次。
    // 语义契约：
    //   - 成功返回 ⟹ 整批已写入且全部可见（keydir apply 在数据 flush 之后，
    //     本进程内 all-or-nothing——读者不会观察到批的中间态）。durability
    //     与单条 put 的 sync 策略对齐：o_sync 即时（O_DSYNC）；
    //     sync_every_n>0 时整批为一次组提交、返回前 fdatasync；
    //     sync_every_n==0 时同单条 put，由 caller 的 sync() 控制。
    //   - 失败返回 ⟹ 整批在本进程内不可见（keydir 未动）。磁盘上可能残留
    //     批的前缀（每条记录独立自洽，崩溃重启 fold 后可见）——与连续单条
    //     put 的崩溃语义一致；本 API 不提供跨崩溃的原子性。
    //   - 校验（key/value 大小）在任何写发生前全批完成——校验失败零副作用。
    //   - 整批写入同一 active 文件；巨批允许该文件超出 max_file_size（软上限）。
    // 线程安全: **是**（同 put，内部 write_mu_）。
    struct BatchItem {
        std::span<const std::byte> key;
        std::span<const std::byte> value;
    };
    [[nodiscard]] std::expected<void, CaskFault>
    put_batch(std::span<const BatchItem> items, std::uint32_t tstamp = 0);

    // 软删除：写一条墓碑 record。空间在下一次 merge 时回收。
    // 线程安全: **是**（同 put，内部 write_mu_）。
    [[nodiscard]] std::expected<void, CaskFault>
    remove(std::span<const std::byte> key, std::uint32_t tstamp = 0);

    // 写入结构化文档（text + 选填 meta）。用于索引模式。
    // 线程安全: **是**（同 put，内部 write_mu_）。
    [[nodiscard]] std::expected<void, CaskFault>
    put_doc(std::span<const std::byte> key, const DocInput& doc,
            std::uint32_t tstamp = 0);

    // BM25 文本搜索（词袋模式）。
    // 线程安全: **是**（并发读安全：cache_/doc_texts_ 各 shared_mutex、倒排/HNSW
    // shared_lock、analyzer const;S6/S7 TSan 已证）。与并发写安全,可见性遵循
    // near-real-time 契约（prepare_search flush 覆盖调用前的写)。
    // V5:filter 非空时 meta 过滤(后过滤 overfetch k×4 再截断到 k)。
    // S13-D10：offset = 跳过排名前 offset 条（分页）。实现为 overfetch
    // k+offset 后截断——深分页成本线性增长（offset 大时考虑游标式方案）。
    // 不提供总命中数：WAND/BMW 剪枝下 total 只能给下界，误导大于价值（详见
    // TASK.md S13-D10 注）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_text(std::string_view query, std::size_t k = 10,
                const meta::MetaFilter* filter = nullptr,
                std::size_t offset = 0);

    // S7-4:批量 BM25 文本搜索——K 条**独立**查询并发跑在进程级共享「有界
    // Search 池」上（inter-query 并发；非每 Cask 一个线程），按输入序返回各自
    // 结果。每条查询内部仍串行。单条查询失败只影响该槽（其余照常）。一次
    // flush（prepare_search）覆盖全批。
    // 线程安全: **是**（并发只读 search_：cache_/doc_texts_ 各 shared_mutex、
    // 倒排/HNSW shared_lock、analyzer const）。与并发写安全（S11-W1 后写路径内部
    // 串行）;可见性遵循 near-real-time 契约（一次 flush 覆盖全批调用前的写）。
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    search_text_batch(std::span<const std::string_view> queries,
                      std::size_t k = 10,
                      const meta::MetaFilter* filter = nullptr);

    // BM25 文本搜索（短语模式）。
    // 线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_phrase(std::string_view query, std::size_t k = 10,
                  std::size_t offset = 0);  // S13-D10

    // BM25 布尔搜索（AND/OR/NOT）。线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    bool_search(std::string_view query, std::size_t k = 10,
                std::size_t offset = 0);  // S13-D10

    // V3.3:HNSW 向量检索。query 长度必须 == meta 配置的 vector_dim;
    // cosine 配置时内部归一化查询向量(零向量返回空命中)。ef=0 →
    // max(k,64)。结果按相似度降序(kDot:内积;kL2:-平方距离),
    // 死文档经 live 过滤不出现。
    // 无 search_ → kNoIndex;无向量配置 → kInvalidOption。
    // V5:filter 与 is_live 组合成 HNSW live callback(无需 overfetch);
    // 结果可能少于 k。
    // 线程安全: 是(HNSW 读路径线程安全,V3.3)。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_vector(std::span<const float> query, std::size_t k = 10,
                  std::size_t ef = 0,
                  const meta::MetaFilter* filter = nullptr);

    // S7-4:批量 HNSW 向量检索——K 条独立向量查询并发跑共享 Search 池，保序
    // 返回。HNSW 读路径本身线程安全（V3.3）。语义/约束同 search_text_batch。
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    search_vector_batch(std::span<const std::span<const float>> queries,
                        std::size_t k = 10, std::size_t ef = 0,
                        const meta::MetaFilter* filter = nullptr);

    // V3.6:RRF 混合检索(hnsw-design §4)。两路各取 K'=max(k×4,64):
    // BM25 走 search_text 内核,向量走 search_vector 内核;融合
    // score = Σ 1/(60+rank),rank 从 1 起;平局 → ord 小者在前。
    // text 空 → 纯向量;vec 空 → 纯文本;两路都空 / 无向量配置 /
    // vec 维度不符 → kInvalidOption;无 search_ → kNoIndex。
    // V5:filter 同时作用于两路(text 后过滤;vec 折 HNSW live callback),
    // 仅双路都通过的文档进 RRF 融合。
    // 返回沿用 TextSearchResult,score = RRF 分。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_hybrid(std::string_view text_query,
                  std::span<const float> vec_query, std::size_t k = 10,
                  const meta::MetaFilter* filter = nullptr);

    // S7-4:hybrid 批量查询项（文本 + 向量一对）。
    struct HybridQuery {
        std::string_view       text;
        std::span<const float> vec;
    };
    // S7-4:批量 RRF 混合检索——K 条独立 (text, vec) 查询并发跑共享 Search 池，
    // 保序返回。每条 hybrid 内部仍串行两路（见 S7-3）；并发发生在查询之间。
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    search_hybrid_batch(std::span<const HybridQuery> queries,
                        std::size_t k = 10,
                        const meta::MetaFilter* filter = nullptr);

    // BM25 多字段搜索（S8.6）：支持 `field:term^boost` 语法，跨字段加权合并。
    // 无字段限定的词等价于默认字段词袋搜索。线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_fields(std::string_view query, std::size_t k = 10);

    // BM25 近邻搜索（S8.7）：term 按序出现且相邻间隙 ≤ slop。slop=0 即短语。
    // 线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_near(std::string_view query, std::uint32_t slop, std::size_t k = 10);

    // S8.3：BM25 模糊搜索（Levenshtein 编辑距离匹配）。
    // 线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_fuzzy(std::string_view query, std::size_t k, std::uint32_t max_edit_distance);

    // S8.4：BM25 通配符搜索（* / ? 模式匹配）。
    // 线程安全: **是**（并发读安全，同 search_text）。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    search_wildcard(std::string_view pattern, std::size_t k);

    // S13-D3：带高亮的 BM25 文本搜索。README 功能表早已宣称本方法，此前却
    // 只在 SearchLayer 上——绕过门面调用会丢失 closed_ fail-fast 与
    // prepare_search flush（near-real-time 可见性）契约。命中含高亮片段
    // （SearchHitEx.highlights），截取策略由 opts 控制。
    // 线程安全: **是**（并发读安全，同 search_text）。
    struct HighlightSearchResult {
        std::vector<search::SearchHitEx> hits;
    };
    [[nodiscard]] std::expected<HighlightSearchResult, CaskFault>
    search_text_highlight(std::string_view query, std::size_t k = 10,
                          const search::HighlightOptions& opts = {});

    // 同义词词典在 **open 时**经 `CaskOptions::synonym_map` 配置（不可变、并发安全）；
    // 运行期 setter 已移除（曾是配置项里唯一的竞态源，见 docs/design/thread-safety.md）。

    // S19-2：`search()` 访问器已删（shim 降级测试夹具）；查询走 Searcher
    // 门面或下方插件句柄。
    [[nodiscard]] bool has_search() const { return text_ != nullptr; }

    // ---- S19-1：插件句柄与读屏障（Searcher 门面的消费面，设计 §3.5）----
    // 查询前的 read-your-writes 读屏障（原私有 prepare_search 公开化）：
    // closed 检查 + flush 索引流水线（submitted ⇒ applied）。未启用搜索
    // 返回 kNoIndex。
    [[nodiscard]] std::expected<void, CaskFault> drain_plugins();
    // 插件句柄（未启用搜索 = nullptr）。生命周期 = Cask open..close。
    [[nodiscard]] const text::TextPlugin* text_plugin() const {
        return text_.get();
    }
    [[nodiscard]] const vec::VectorPlugin* vector_plugin() const {
        return vec_plugin_.get();
    }
    [[nodiscard]] const search::HybridSearcher* hybrid_searcher() const {
        return hybrid_ ? &*hybrid_ : nullptr;
    }
    // SearchError → CaskFault 翻译（Cask 门面与 Searcher 门面共享）。
    [[nodiscard]] static CaskFault search_error_fault(search::SearchError e);
    // S16-1：DocMap 宿主服务句柄（索引模式下非空；与插件借用的 docmap
    // 同一实例——所有权在 Cask，Text/Vector 插件借用）。
    [[nodiscard]] const std::shared_ptr<index::Index>& docmap() const {
        return docmap_;
    }

    void flush_index() {
        if (index_pool_ && index_lane_) index_pool_->flush(index_lane_);
    }

    // fsync active data file。o_sync 模式下退化为 no-op。
    // 线程安全: **是**（S11-W1：内部 write_mu_，与 put/remove 互斥）。
    [[nodiscard]] std::expected<void, CaskFault> sync();

    // 强制关 active write file：finalize hint trailer、丢掉 active data/hint
    // 句柄、释放 bitcask.write.lock。Cask 仍可用——下次 put/delete 自动
    // 重新拿锁、新建 active file（对应 legacy bitcask:close_write_file 语义）。
    // 只读 / merge_only 句柄返回 kReadOnly。
    // 线程安全: **是**（S11-W1：内部 write_mu_，与 put/remove/sync 互斥）。
    [[nodiscard]] std::expected<void, CaskFault> close_write_file();

    // 线程安全: 是（只读 keydir + opts 快照）；不需任何锁。
    [[nodiscard]] StatusInfo status();
    // O(1) 估算「keydir 是否为空」。写过 key 后即使删光也不会再回 true。
    // 线程安全: 是（仅读 keydir info）；不需任何锁。
    [[nodiscard]] bool is_empty_estimate();
    // keydir 是否被某个 fold/iterator pin 住（影响 pending 表合并时机）。
    // 线程安全: 是（仅读 keydir info）；不需任何锁。
    [[nodiscard]] bool is_frozen();

    // 包装 decide()：返回是否需要 merge + 候选文件列表。
    struct NeedsMerge {
        bool needs;
        std::vector<std::string> files;
        std::vector<std::string> expired_files;
    };
    // 线程安全: 是（读 keydir info 拿快照 + 纯函数策略）；不需外部锁。
    [[nodiscard]] NeedsMerge needs_merge(std::uint32_t now_sec = 0);

    // 在指定文件上跑 merge。files 为空时先调 needs_merge。caller 自己负责
    // 外部调度 / 锁——这个方法只是把 run_merge 包了一层。
    // 线程安全（S13-F7 统一措辞，对齐 thread-safety.md §7.6）: **是**。
    //   - KV 路径：merge 与并发 put/remove/get 安全——keydir 重定位是条件
    //     CAS（newest_put=false，S13-F1），收尾对 stuck 文件跳过 unlink 兜底；
    //     get 对 merge unlink 窗口有一次重查重试（S13-F5）。
    //   - 索引模式：merge 内的 compact/ckpt 序列化经 RunFn 任务在 reducer
    //     线程内执行（S13-F6，同 RebuildHnsw 先例）——concurrent_hash_map
    //     的遍历与 add_doc 插入始终同线程串行，无并发窗口。
    // 锁要求: caller 须保证同一 dirname 上同时仅一次 merge 在跑。
    [[nodiscard]] std::expected<merge::MergeStats, CaskFault>
    merge(std::vector<std::string> files = {}, std::uint32_t now_sec = 0);

    // S13-D6：不停机备份到 dst_dir（不存在则创建）。流程：持 write_mu_ 关闭
    // active writer（finalize hint trailer，下一次 put 自动重建）→ 快照文件
    // 清单 → 逐文件 hardlink（跨设备回退 copy）data/hint + bitcask.meta +
    // field.schema + keydir/search checkpoint（有则带上，加速备份目录首次
    // open）。sealed 文件不可变 ⟹ hardlink 即一致快照。
    // 备份目录可直接以只读或读写模式 open。
    // 锁要求：**caller 须保证 backup 与 merge 不并发**（merge 收尾会 unlink
    // 输入文件；与同目录 merge 的单实例约束同级）。与并发 put/get 安全
    // （put 被 write_mu_ 挡在备份期间外，get 不受影响）。
    [[nodiscard]] std::expected<void, CaskFault> backup(std::string_view dst_dir);

    // 手动 checkpoint（s13-review §P1 后续②）：把 keydir 快照 + search.ckpt
    // 主动落盘，把崩溃恢复的重放窗口收敛到「自本次调用以来的增量」——否则
    // ckpt 只在干净 close / merge 收尾保存，长期运行不重启的大库崩溃后要
    // 重放全部历史（10M 级库重分词 + HNSW 重建可达小时级）。调用节奏由
    // caller 决定（每 N 万写 / 定时 / 业务低峰），库内不做周期策略。
    // 保存顺序与 merge 收尾一致（成对性）：先落 keydir 快照（较早水位），
    // 后存 search.ckpt（覆盖必然 ≥ 快照水位）——并发写入下方向安全（下次
    // open 从快照水位重放尾部，重叠区由各索引 ord 自门幂等丢弃）。
    // 阻塞语义：search.ckpt 序列化经 RunFn 在 reducer 线程按 ord 序执行
    // （S13-F6：concurrent_hash_map 遍历只允许发生在 reducer），本调用等待
    // **自己的 RunFn** 完成（其 ord 之前的索引事件此时必然已全部 apply），
    // 不等整条队列排空——持续写入下等待仍有界。大库序列化可达秒~分钟级，
    // 期间 reducer 停摆、队列积压（H1 后背压只阻塞提交中的写者）。
    // 纯 KV 库（无 search）只落 keydir 快照。
    // 线程安全: **是**。checkpoint 间由内部 ckpt_mu_ 串行；与 put/get 并发
    // 安全（不取 write_mu_）；与 close 并发由 WriteOpGate 收敛（close 等待
    // 本调用完成）。与 merge 收尾并发时快照/ckpt 为最后写者赢——两者内容
    // 皆自洽无损坏风险（ckpt 有 .prev 代际回退，快照损坏退全量 fold），但
    // 建议与 merge 同一运维线程串行调度。
    // 只读 / merge_only 句柄返回 kReadOnly。
    [[nodiscard]] std::expected<void, CaskFault> checkpoint();

    // 线程安全: 是；不需任何锁。返回的 CaskIter 自身非线程安全。
    [[nodiscard]] std::unique_ptr<CaskIter> make_iter() {
        return std::make_unique<CaskIter>(this);
    }

    [[nodiscard]] std::string_view dirname() const noexcept { return dirname_; }
    [[nodiscard]] keydir::KeyDir&  keydir()  noexcept { return *keydir_; }
    [[nodiscard]] const CaskOptions& options() const noexcept { return opts_; }

private:
    friend class CaskIter;

    std::string dirname_;
    CaskOptions opts_;

    // 字段名 ↔ id 注册表（#1）：put_doc 把多字段名 intern 成 id 写进 DocValue。
    // open/upgrade 时加载 <dir>/field.schema。
    FieldSchema field_schema_;

    // keydir（多个 Cask 可能通过 registry 共享同一个）
    std::shared_ptr<keydir::KeyDir> keydir_;
    // 非拥有指针：KeyDirRegistry 由 Erlang/NIF 层创建和销毁，Cask 仅借
    // 用以调 acquire/release。close() 时 release 后置 nullptr，不 delete。
    keydir::KeyDirRegistry* registry_ = nullptr;
    std::string keydir_name_;

    // 当前 active write file。只读 / merge_only 时为 nullptr。
    // shared_ptr:读路径(read_file)可能在锁外持有 active 句柄,roll/close
    // 时旧对象由在途读者的引用计数续命,不会析构正在被 pread 的对象。
    std::shared_ptr<fileops::DataFile> active_data_;
    std::unique_ptr<fileops::HintFile> active_hint_;
    // S13-F4：atomic——写者仅持 write_mu_、读者（read_file/needs_merge/
    // CaskIter::pin_files）持 read_cache_mu_ 或无锁，双方无公共锁、无
    // happens-before。值仅作提示（读到陈旧值最多多开一次文件句柄，良性），
    // relaxed 序即可，但按 C++ 内存模型必须是原子避免 UB/TSan 报告。
    std::atomic<std::uint32_t> active_file_id_{0};
    // P4 组提交计数：自上次 fsync 以来的写次数。写路径单线程（caller 串行），
    // 无需原子。sync_every_n>0 时由 maybe_group_commit() 维护。
    std::uint32_t writes_since_sync_ = 0;

    // S11-W1：写路径互斥——把「调用方串行化所有写」的外部契约**内化**为内部锁，
    // 使同一 Cask 可被多线程并发写而不损坏数据（通用 C++ 库定位，
    // docs/design/thread-safety.md）。覆盖 put/remove/put_doc/sync/close_write_file
    // 的整个写序列（含内部 ensure_active_writer/roll_active/maybe_group_commit/
    // write_and_keydir）。保护对象：active_data_ 的 current_offset_/write_buf_/
    // batch_buf_、writes_since_sync_、active_file_id_、active_hint_ 等写态。
    // 锁序：write_mu_ 最外层 → 内部再取 read_cache_mu_/keydir 锁；**读路径不取
    // write_mu_**（get/搜索保持无锁/共享锁，吞吐不变）。merge 不取本锁（写自有
    // 输出文件，经 keydir shared_mutex 协调，与写并发）；flush_index 不取本锁
    // （读/写共用、IndexPool flush 自带 cv 同步）。
    std::mutex write_mu_;

    // S11-W3：生命周期 fail-fast 标志。close() 置位后,公共方法入口检查 → 返回
    // 错误码而非解引用已释放的 keydir_/search_/active_data_（UB）。这是**尽力
    // 而为的 fail-fast**,非完整 rundown：已在途的操作与 close() 并发仍是 caller
    // 责任（契约：close 时刻无在途调用）。close() 用 exchange 兼作幂等门。
    std::atomic<bool> closed_{false};
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    // H1（s13-review §P1）：在途写操作计数。写路径（put/put_batch/remove/
    // put_doc）与 checkpoint() 在整个调用期间持有 WriteOpGate——含释放
    // write_mu_ 之后的索引提交尾段。close() 置 closed_ 后等待归零，才注销 index lane /
    // 清空 index_pool_ 指针；否则锁外的 submit_index_task 可能解引用已
    // erase 的 lane（UAF）。这是对「close 时刻无在途调用」契约（见上）的
    // 防御性收敛：违约的并发 close 从 UB 变为阻塞等待写者退出。收敛性：
    // 池由 registry 持有、close 不停池，队列背压中的 push 必然返回；
    // closed_ 置位后新写者在入口检查处退出，计数单调排空。
    // 内存序：fetch_add/fetch_sub 与 close 侧 load 均 seq_cst——写者
    // 「inc 后读 closed_」与 close「写 closed_ 后读计数」构成 store-buffer
    // 形状，RMW 的全序 + seq_cst load 保证两侧不会同时读到旧值。
    // checkpoint() 调用间互斥（并发手动 checkpoint 串行化；ckpt 文件的实际
    // 写入统一在 reducer 线程 RunFn 内做，见 checkpoint()/S14-1 注释）。
    // 不与 write_mu_ 交叉：checkpoint 不取 write_mu_，写路径不取本锁。
    std::mutex ckpt_mu_;

    // S14-1 自动 checkpoint 状态。pending：roll_active 置位（有文件封口），
    // 写路径锁外提交点消费；inflight：防重入（一次只挂一个 RunFn），RunFn
    // 完成时清；last_ckpt_ord_：上次 ckpt 覆盖水位（open 末尾初始化为当前
    // next_ord，RunFn/checkpoint() 保存成功后推进），增量 = peek_next_ord −
    // 本值，达 opts_.auto_checkpoint_min_docs 才真正提交。
    std::atomic<bool> auto_ckpt_pending_{false};
    std::atomic<bool> auto_ckpt_inflight_{false};
    std::atomic<std::uint64_t> last_ckpt_ord_{0};
    // 写路径释放 write_mu_ 后调用（WriteOpGate 持有中 → close 竞态安全）：
    // 消费 pending 标记，增量达阈值则 fire-and-forget 提交 ckpt RunFn。
    void maybe_submit_auto_checkpoint();

    // IndexPool 分发闭包的命名方法（原 register_lib 内联 lambda 体提取）。
    // lambda 退化为薄捕获委托——逻辑可独立测试，消除契约测试里的闭包复刻。
    std::vector<plugin::PreparedPtr> prepare_index_task(const IndexTask& task);
    void reduce_index_entry(ReorderEntry& entry);
    void on_index_worker_error() noexcept;

    // S14-7：成对保存——search.ckpt（delta 或 base）+ keydir 推进的统一
    // 入口。delta 路径把 keydir 元数据（水位/标量/fstats，caller 于提交
    // 时刻构建）内联进 delta 文件（**同文件原子成对**，无写序窗口，且
    // 不再每次全量写 kv.keydir.ckpt——它曾是 delta 时代 per-save I/O 的
    // 大头）；base 路径照旧全量快照（先 ckpt 后快照）。
    // S17-3 改造为 P3：先写 per-component 段（docmap/bm25/vec），再写
    // manifest（commit point），最后才写 keydir 快照（仅 base 路径）。
    bool save_search_ckpt_paired(
        const std::string& path, std::uint64_t wm,
        const std::optional<std::vector<
            std::pair<std::uint32_t, std::uint64_t>>>& wms,
        const std::vector<std::byte>& keydir_delta);

    // S17-3 显式 commit 入口：先 per-component base（或 delta），再写
    // manifest 作为 commit point，最后 base 路径下写 keydir 快照。
    // delta 路径不写 keydir 快照（元数据已内联进 delta 文件）。返回
    // manifest 是否成功提交。
    bool save_checkpoint_paired(
        const std::string& dir, std::uint64_t wm,
        const std::optional<std::vector<
            std::pair<std::uint32_t, std::uint64_t>>>& wms,
        const std::vector<std::byte>& keydir_delta);

    // S17-2:当前 manifest。save 时按组件结果更新；load 时初始化。每组件
    // 链状态也镜像一份（SearchLayer 内部维护，Cask 在 commit 时同步）。
    bitcask::Manifest current_manifest_;

    // S18-2：docmap 组件链状态镜像（宿主直驱 docmap 持久化后，替代原
    // SearchLayer comp_*[kDocmap]）。组件文件写成功即推进——独立于 manifest
    // commit 成败：manifest 写失败后重试续写 .d{seq+1}，链走 .d1..dN 完整
    // 重放（与旧 SearchLayer 行为一致，防止已清空的删除日志丢失）。
    bitcask::ManifestEntry docmap_chain_{};

    std::atomic<std::uint32_t> writes_in_flight_{0};
    struct WriteOpGate {
        Cask* cask;
        explicit WriteOpGate(Cask* c) : cask(c) {
            cask->writes_in_flight_.fetch_add(1, std::memory_order_seq_cst);
        }
        WriteOpGate(const WriteOpGate&) = delete;
        WriteOpGate& operator=(const WriteOpGate&) = delete;
        ~WriteOpGate() {
            if (cask->writes_in_flight_.fetch_sub(
                    1, std::memory_order_seq_cst) == 1) {
                cask->writes_in_flight_.notify_all();
            }
        }
    };

    // 按 file_id 缓存的 DataFile 读句柄。read 路径懒打开。
    // 多读者并发，read_cache_mu_ 保护 unordered_map 本身；DataFile 内部
    // 的 pread 是 thread-safe 的。
    mutable std::shared_mutex read_cache_mu_;  // 命中走共享锁;lazy open/清理走独占（const 内省也需锁）
    // P9:近似 LRU read 句柄缓存。值含 atime(命中在共享锁下置位,近似 LRU);
    // 超 opts_.max_read_handles 时在独占锁下淘汰最旧的**空闲**(use_count==1)
    // 句柄——在途读者持 shared_ptr 续命,fd/mmap 随最后引用析构才释放(与
    // O10/merge-unlink 同模式)。atomic atime 非可移/拷,unordered_map 节点
    // 稳定(rehash 不移动节点),emplace 原地构造即可。
    struct ReadHandle {
        std::shared_ptr<fileops::DataFile> df;
        mutable std::atomic<std::uint64_t> atime{0};
        ReadHandle(std::shared_ptr<fileops::DataFile> d, std::uint64_t a)
            : df(std::move(d)), atime(a) {}
    };
    std::unordered_map<std::uint32_t, ReadHandle> read_files_;
    std::atomic<std::uint64_t> read_clock_{0};  // 近似 LRU 单调访问计数

    // 目录锁。read_write 模式下是 bitcask.write.lock（live writer 持有），
    // merge_only 模式下是 bitcask.merge.lock（merger 跟 writer 并行）。
    std::optional<lock::FileLock> write_lock_;

    // merge_only 模式下，open 时从 write.lock 里读出来的「live writer 的
    // active file id」。needs_merge 用它把 live writer 正在写的文件从候选
    // 里排除——不能并别人正在写的文件。
    // 0 表示「没探测到 live writer」（保守：不额外排除）。
    std::uint32_t merger_writer_active_id_ = 0;

    // bitcask.meta 配置（open 时读写）
    meta::MetaConfig meta_config_{};

    // S16-1：DocMap 宿主服务（ord↔ext/live/meta 身份表）。Cask 持有、
    // Text/Vector 插件借用（设计 §4：docmap 属「文档身份」域而非「搜索」域）。
    // reducer 先于插件 apply 它（S16-2）。
    std::shared_ptr<index::Index> docmap_;

    // 搜索插件（enable_search 时创建）。
    // S19-2：Cask 直持插件（SearchLayer shim 已降级为测试夹具）。声明序 =
    // 析构逆序：hybrid_ 引用两插件须先析构；插件借用 docmap_（shared_ptr）。
    std::unique_ptr<text::TextPlugin>  text_;
    std::unique_ptr<vec::VectorPlugin> vec_plugin_;
    std::optional<search::HybridSearcher> hybrid_;
    // S14-4 legacy 全局 rebase 标志（自 shim 迁来）：管 docmap base 决策 +
    // 收链联动（force_ckpt_rebase 同步两插件自持标志）。
    std::atomic<bool> ckpt_rebase_needed_{true};

    // S18-5：插件分发表 = {TextPlugin, VectorPlugin}（实体归 search_ 持有，
    // 指针注册；注册序 = reducer 扇出序 = 原 reduce_apply 内顺序）。
    std::vector<plugin::CaskPlugin*> plugins_;

    // S18-5：plugin::PluginHost 落地——read_at（重建/回填读回）、
    // run_serialized（RunFn 通道正式化：reducer 静止点串行，S18-7 merge
    // 收尾经此变异单写者状态）、log。生命周期 = Cask（覆盖插件 open..close）；
    // S18-6 经 OpenContext 注入插件。
    class CaskPluginHost final : public plugin::PluginHost {
    public:
        explicit CaskPluginHost(Cask* c) : cask_(c) {}
        std::optional<std::string> read_at(plugin::RecordLoc loc) override;
        void run_serialized(std::function<void()> fn) override;
        void log(plugin::LogLevel level, std::string_view msg) override;
    private:
        Cask* cask_;
    };
    CaskPluginHost plugin_host_{this};

    // S6-P3: 索引双池现由 registry 共享所有（非本 Cask 拥有）。index_pool_
    // 是借用指针（= registry_->index_pool()）；index_lane_ 是本库在共享池里
    // 注册的车道句柄（register_lib 返回，close 时 unregister_lib）。
    IndexPool* index_pool_ = nullptr;
    IndexLane* index_lane_ = nullptr;
    // indexed worker 异常计数器：catch(...) 时 fetch_add(1)；非零 = 索引可能漂移
    std::atomic<std::uint64_t> index_errors_{0};

    // T3: 提交索引任务到 IndexPool（异步索引）。背压由 IndexPool 的有界
    // 队列提供：队列满（capacity 10240）时 submit 内部的 push 阻塞写线程，
    // 自然限速，避免任务无限堆积撑爆内存。
    // H1（s13-review §P1）：常规路径（put/put_batch/remove/put_doc 的
    // Add/Delete）在 **write_mu_ 释放之后** 调用——背压只阻塞本写者，
    // 不冻结其他写路径。失败补偿的 Skip（OrdSkipGuard/写内重试）可能仍在
    // 锁内提交（罕见路径，可接受）。锁外调用由 WriteOpGate 保护，close()
    // 等其归零后才清 index_pool_/index_lane_。
    void submit_index_task(IndexTask task);

    // S13-D7：日志上报（best-effort：未配置 log_fn 为 no-op；回调抛出被吞）。
    void log(CaskOptions::LogLevel lvl, std::string_view msg) const noexcept {
        if (!opts_.log_fn) return;
        try {
            opts_.log_fn(lvl, msg);
        } catch (...) {
            // 回调契约不得抛出；违约吞掉，日志不能反过来搞挂引擎。
        }
    }
    void log_warn(std::string_view msg) const noexcept {
        log(CaskOptions::LogLevel::kWarn, msg);
    }
    void log_error(std::string_view msg) const noexcept {
        log(CaskOptions::LogLevel::kError, msg);
    }

    // S13-F2: ord 泄漏守卫。写路径 alloc_ord 后、真任务提交前的任何错误
    // return 都必须给该 ord 补一条 Skip——否则 reducer 的 next_apply_ord
    // 出现永久空洞，此后 flush/merge/close 全部在 flush_cv_ 上永久阻塞
    // （一次 ENOSPC 即卡死句柄）。析构时未 disarm 则自动提交 Skip；
    // 真任务（Add/Delete）或等价 Skip 已覆盖该 ord 后调 disarm()。
    struct OrdSkipGuard {
        Cask* cask;
        std::uint64_t ord;
        bool armed = true;
        OrdSkipGuard(Cask* c, std::uint64_t o) : cask(c), ord(o) {}
        OrdSkipGuard(const OrdSkipGuard&) = delete;
        OrdSkipGuard& operator=(const OrdSkipGuard&) = delete;
        void disarm() { armed = false; }
        ~OrdSkipGuard() {
            if (armed) {
                cask->submit_index_task(
                    IndexTask::make(IndexOp::Skip, {}, ord, {}, 0, 0, 0, 0, 0));
            }
        }
    };

    // S7-4: 批量搜索公共骨架（去 search_*_batch 三方法的重复）。
    //   ① 空批早退；② 一次 prepare_search()（flush，覆盖全批）；
    //   ③ require_vector 时校验 vector_dim；④ N 条查询经共享 Search 池并发跑，
    //      保序写各自结果槽（槽间不重叠 → 无锁）。
    // run_one(i) 执行第 i 条查询并返回其结果（含 expected 错误）。
    [[nodiscard]] std::vector<std::expected<TextSearchResult, CaskFault>>
    run_search_batch(
        std::size_t n, bool require_vector,
        const std::function<
            std::expected<TextSearchResult, CaskFault>(std::size_t)>& run_one);

    // S8-R3: 单条搜索公共骨架（去 9 个 search_* 单查询方法的重复）。
    //   prepare_search()（flush）→ require_vector 时校验 vector_dim → run() 跑内核
    //   → 失败经 search_fault 把 SearchError 翻译成 CaskFault → 包成 TextSearchResult。
    // S9-P2-d：run() 返回 expected<vector<SearchHit>, SearchError>（强类型错误），
    //   不再由 caller 静态指定 err_kind——边界统一翻译，消除 leaky abstraction。
    [[nodiscard]] std::expected<TextSearchResult, CaskFault>
    run_search_one(
        bool require_vector,
        const std::function<
            std::expected<std::vector<search::SearchHit>, search::SearchError>()>& run);

    // A4:落 keydir 段快照(best-effort;close/merge 末尾调)。
    void write_keydir_snapshot() noexcept;
    // S14-1：水位捕获与快照写入拆分。RunFn 路径（checkpoint()/自动 ckpt）
    // 必须在**提交时刻**（writer 侧）捕获字节水位、reducer 执行时刻写快照
    // 本体——执行时取水位会被并发写者推进，反转「keydir_covered ≤
    // search_covered」保存序不变量（路线 A §4），fold 跳过 search 未覆盖区。
    [[nodiscard]] std::optional<
        std::vector<std::pair<std::uint32_t, std::uint64_t>>>
    collect_snapshot_watermarks() const noexcept;
    void write_keydir_snapshot(
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& wms) noexcept;

public:
    // 访问共享 IndexPool（借用自 registry）
    [[nodiscard]] IndexPool* index_pool() { return index_pool_; }

    // 内部辅助
    [[nodiscard]] std::expected<void, CaskFault> load_keydir_from_disk();
    [[nodiscard]] std::expected<void, CaskFault> ensure_active_writer();

    // P4：组提交。每次写后调用；sync_every_n>0 且累计写数达阈值时 fsync 一次
    // active data file 并清零计数。force=true（close/sync 收尾）则只要有未落盘
    // 写就立即 fsync。o_sync 模式或 sync_every_n==0 时为 no-op。
    [[nodiscard]] std::expected<void, CaskFault> maybe_group_commit(bool force = false);
    [[nodiscard]] std::expected<void, CaskFault> roll_active_if_needed(std::size_t about_to_write);
    // 无条件 finalize 当前 active writer 并开新一轮（新 file_id）。
    // put() 在 keydir.biggest_file_id 被并发 merger 顶过去时调用——
    // 必须放弃当前文件，让出 file_id 单调递增的不变量。
    [[nodiscard]] std::expected<void, CaskFault> roll_active();
    [[nodiscard]] std::shared_ptr<fileops::DataFile>
    read_file(std::uint32_t file_id);
    // P9：read_files_ 超 max_read_handles 时淘汰最旧空闲句柄。
    // 调用方须已持 read_cache_mu_ 独占锁。
    void evict_read_handles_locked();

    // ---- open() 拆分出来的私有阶段 ----

    // T2.4:open 阶段一——锁分配(writer / merger / 只读不锁)。
    // 出错时返回 unexpected,失败路径由 caller 回滚(RAII 自管)。
    [[nodiscard]] std::expected<void, CaskFault> acquire_open_locks();

    // T2.4:open 阶段二——bitcask.meta 读取或创建(决定 KV / 索引模式、
    // 向量配置一致性校验)。必须先于 SearchLayer 创建。
    [[nodiscard]] std::expected<void, CaskFault> check_or_create_meta();

    // T2.4:open 阶段三——搜索插件（Text/Vector）+ IndexPool 创建(只在 search_config
    // 配置时启动 worker)。opts 是 caller 的选项快照,内含 search_config。
    [[nodiscard]] std::expected<void, CaskFault>
    create_search_infra(const CaskOptions& opts);

    // P14e/P14b:加载 keydir 快照 + search.ckpt 分段快照。snap_loaded=true
    // 表示搜索索引健康（全段 CRC 通过）且 keydir 快照可用 → fold 阶段从
    // keydir 水位起跳过已覆盖字节。snap_wms 是每文件水位表。
    struct RecoverySnapshots {
        bool snap_loaded = false;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> snap_wms;
    };
    [[nodiscard]] std::expected<RecoverySnapshots, CaskFault>
    load_recovery_snapshots();

    // checkpoint delta 链重放钩子——把 delta 行/删除/keydir 元数据应用到
    // keydir，推进恢复水位。S18-2：类型换 index::（docmap 持久化归宿主，
    // 钩子只被 index::load_docmap 消费）。
    void replay_delta_to_keydir(
        const std::vector<index::DocmapDeltaRow>& rows,
        const std::vector<index::DocmapDeltaRemoval>& rems,
        std::span<const std::byte> keydir_meta,
        RecoverySnapshots& recovery);

    // S17-5:legacy search.ckpt → 3 个组件文件 + manifest 的一次性迁移。
    // 仅在 open 阶段、manifest 不存在但 search.ckpt 存在时触发。成功
    // 后写 index.manifest + docmap.ckpt/bm25.ckpt/vec.ckpt + 删旧文件。
    // 失败返回 false（caller 退全量 fold）。
    [[nodiscard]] bool migrate_legacy_search_ckpt();
    // S14-4/S19-2：merge/close 收链入口——置全局标志 + 联动两插件自持标志。
    void force_ckpt_rebase();

    // ---- 搜索方法共用基础设施 ----

    // 搜索前置检查 + flush。返回错误则 caller 直接 propagate。
    [[nodiscard]] std::expected<void, CaskFault> prepare_search();

    // ---- 写入共用基础设施 ----

    // write_and_keydir：写 data record + hint record + keydir put，
    // 若 keydir put 返回 kAlreadyExists 则 roll_active 后重试一次。
    // 返回最终使用的 ord / offset / total_size（供 caller 构造 IndexTask）。
    struct PersistedRecord {
        std::uint64_t ord;
        std::uint64_t offset;
        std::uint32_t total_size;
        std::uint32_t file_id;
    };
    [[nodiscard]] std::expected<PersistedRecord, CaskFault>
    write_and_keydir(std::span<const std::byte> key,
                     std::span<const std::byte> encoded,
                     std::uint32_t tstamp, std::uint64_t ord);

    // 向量校验 + 可选 L2 归一化。norm_buf 仅在 cosine 指标时填充；
    // 非 cosine 返回的 span 直接指向 input（零拷贝）。
    [[nodiscard]] std::expected<std::span<const float>, CaskFault>
    prepare_vector(std::span<const float> input,
                   std::vector<float>& norm_buf) const;
};

}  // namespace bitcask
