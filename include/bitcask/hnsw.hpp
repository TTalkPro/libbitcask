// HNSW 近似最近邻索引(V3.3:单写者 + 多读者;设计 doc/hnsw-design-zh.md §3)。
//
// === 算法参考文献 ===
// Malkov, Yashunin, "Efficient and robust approximate nearest neighbor search using
//   Hierarchical Navigable Small World graphs", arXiv:1603.09320, 2016; TPAMI 2018.
//   关键参数：M（邻居容量，上层 M，L0 层 2M），ef_construction（构建时搜索宽度），
//   mL = 1/ln(M)（层生成概率参数，控制图的高度与宽度 tradeoff）。
//
// 边界(§1):只收向量不算向量;dim 库内恒定(构造时定);cosine 由上游
// 写入归一化 → 本模块只见 kDot/kL2 两种度量。
//
// === 结构(§2)===
//   - 内部节点 id:u32 插入序紧凑分配;ords 翻译回 ord。
//   - 存储分段 chunk(kChunkBits=16 → 65536 节点/chunk):chunk 内
//     vecs/ords/levels/adj 指针/per-node 自旋锁全部定容预分配,地址稳定。
//   - chunk 目录:定容 std::array<std::atomic<NodeChunk*>, kMaxChunks>,
//     写者安装(release)、读者 load(acquire)——绝不用会搬迁的 vector。
//   - 每节点邻接块 insert 时按层数一次性 new u32[] 零初始化,指针进
//     adj 后永不搬迁(arena 化留 V3.x)。层内布局 [count][ids...],
//     L0 容量 2M、上层 M。
//
// === 并发协议(V3.3,§3)===
//   单写者(IndexPool worker)+ 多读者(查询线程);**多写者不支持**
//   (insert 内有 debug assert 声明)。
//   - count_ 发布序:写满本节点数据(vec/ord/level/空邻接块)→
//     count_.store(id+1, release) → 再做连边。读者以自己 load 的 count
//     为可见边界;邻接里可能出现 >= 该边界的 id(发布后才追加的反向边),
//     读侧一律跳过——visited 数组按本地 count 定界即安全。
//   - per-node 自旋锁(1 字节):写者改某节点邻接(正向写入/反向追加/
//     超容收缩)持该节点锁;读者读某节点邻居前持同一把锁把 [count][ids]
//     拷到栈缓冲再放锁遍历。临界区 ~百 ns(收缩路径微秒级,可接受)。
//   - entry_point/max_level 合并单 atomic u64:高 32 位 = level+1
//     (0 表示空图),低 32 位 = id;insert 完整连边后才更新。search
//     开头先 load entry_meta_(acquire)再 load count_——entry 发布
//     happens-after count 发布,故 entry id 必 < 本地 count。
//   - visited 标记:thread_local 版本化数组(实现注释见 .cpp)。
//
// === 删除 ===
//   本模块不感知删除;上层经 live 过滤回调在结果侧滤死,死节点留作
//   图内路标,merge 重建时物理清除。

#pragma once

#include <sys/types.h>  // S14-2: dev_t/ino_t（.vec 追加目标身份）

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>  // S32-M2:qc8 mmap 记录尾 scale/sum 的 memcpy 取值
#include <functional>
#include <memory>
#include <random>
#include <span>

#include "bitcask/io.hpp"  // B3：MappedFile（payload 只读映射）
#include <string_view>
#include <vector>

#include "bitcask/detail/int8_kernels.hpp"

namespace bitcask::vec {

enum class HnswMetric : std::uint8_t {
    kDot = 0,  // 内积相似度(cosine_normalized 上游已归一化)
    kL2  = 1,  // 平方欧氏距离
};

struct HnswConfig {
    std::uint16_t dim = 0;
    HnswMetric    metric = HnswMetric::kDot;
    std::uint32_t M = 16;                 // 上层邻居容量;L0 = 2M
    std::uint32_t ef_construction = 200;
    std::uint64_t seed = 0x5EEDF00D;      // 层数抽样种子(测试可复现)
    // P5:int8-only 内存模式。true → NodeChunk 不存常驻 f32(vecs 容量 0),
    // 建图/查询全程 int8,向量内存 ~−80%。仅 kDot(int8 距离=重建内积);
    // kL2 不支持(上游 open 接线拒绝)。设计 doc/hnsw-int8-only-design-zh.md。
    bool          inmem_int8 = false;
    // S29-11-②:建图导航 int8 混合精度(默认开;仅 kDot + int8 内核在时
    // 生效)。导航(上层贪心 + 逐层 ef 搜索)用 int8 副本——工作集 4×
    // 塌缩打掉建图超线性;**入选邻居仍 f32 精选**(与 inmem_int8 的纯
    // int8 端到端不同,召回损失远小)。false = 回退全 f32 导航
    // (S32 召回门:recall@10 降 >1pt 须可回退,本开关即回退闸)。
    bool          build_nav_int8 = true;
};

class HnswIndex {
public:
    explicit HnswIndex(const HnswConfig& cfg);
    ~HnswIndex();

    HnswIndex(const HnswIndex&) = delete;
    HnswIndex& operator=(const HnswIndex&) = delete;

    // 插入(仅单写者线程)。前置:vec.size()==dim;ord 全局单调。
    // 水位幂等:ord <= max_inserted_ord_ 时丢弃(崩溃回放重叠区安全,
    // 与 InvertedIndex::add_doc 同协议)。
    void insert(std::uint64_t ord, std::span<const float> vec);

    struct Hit {
        std::uint64_t ord;
        float score;   // kDot:内积(越大越近);kL2:-平方距离(同向比较)
    };
    // 查询 top-k。线程安全(多读者,可与单写者 insert 并发)。
    // ef >= k(内部取 max);live 非空时结果侧过滤(死节点仍参与导航)。
    [[nodiscard]] std::vector<Hit> search(
        std::span<const float> query, std::size_t k, std::size_t ef,
        const std::function<bool(std::uint64_t)>* live = nullptr) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::uint64_t max_inserted_ord() const noexcept {
        return max_inserted_ord_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const HnswConfig& config() const noexcept { return cfg_; }

    // ---- V3.5:重建用只读访问(merge 物理清死,VectorPlugin 重建入口)----
    // 前置:id < size()(已发布节点)。vec 的底层存储地址稳定(chunk 定容)。
    [[nodiscard]] std::uint64_t node_ord(std::uint32_t id) const {
        return ord_of(id);
    }
    // P5:int8-only 下无常驻 f32,反量化到 thread_local 缓冲返回(仅
    // rebuild_hnsw 单写者即时消费,下次调用前已被 insert 拷走)。
    [[nodiscard]] std::span<const float> node_vec(std::uint32_t id) const;

    // S13-P8：结构化拷贝活子图（merge 期 rebuild 用）。替代「从零逐点重插」
    //（每点 ~ef_construction 次全维距离，100k 节点分钟级、阻塞 merge）：
    // 保留原图的层数与邻接结构，只做 id 重映射 + 死邻过滤——O(节点+边) 的
    // memcpy 级拷贝，无距离计算。死邻过滤的召回补偿：某层邻居全死时做
    // **一跳路径收缩**（借道死邻的活邻居补边，去重、限 cap）；极端删除
    // 模式下个别节点仍可能孤立（无出边），由后续 merge 轮或重插自愈。
    // int8-only 模式直接拷 qcodes/scale/sum——顺带消掉旧重插路径的
    // 反量化→再量化往返（audit P8 项）。
    // 线程模型：调用方须为单写者（reducer）；旧图并发读者不受影响（只读）。
    //
    // S32-M2b：payload 外溢模式（设计 vector-dual-engine-selection §6.2 前
    // 提、§1.2 P1）。spill_vec_path/spill_qc_path 非空时，活集 f32/int8
    // 码字**不进 fresh 堆**——从旧图（含 mmap 段）按 remap 直接流式写新
    // payload 文件（tmp+rename、新 payload_gen），fresh 以 mmap attach
    // （checkpoint_count_ = 活节点数）。堆峰值从「旧图 + 新图全量」降到
    // 「旧图 + 新图邻接/元数据（~165B/节点）」。外溢失败自动回退堆拷贝
    // （行为同旧版）。空路径 = 旧行为（测试/standalone）。
    // 崩溃窗口：新 payload 已 rename 而新 ckpt 未落 → gen 守卫拒载 →
    // fold 全量重建（与旧「rebuild 后 flush 全量重写」同窗口，宿主在
    // merge 收尾后 FIFO 紧跟成对保存点，窗口极窄）。
    [[nodiscard]] std::shared_ptr<HnswIndex>
    clone_live(const std::function<bool(std::uint64_t)>& is_live,
               std::string_view spill_vec_path = {},
               std::string_view spill_qc_path = {}) const;

    // ---- V7:BCVS v2 快照(header in search.ckpt + vecs_ in search.vec mmap)----
    //
    // 双文件模型:search.ckpt 的 kHnsw 段存 header(qcodes/adj/ords/levels +
    // entry/count),search.vec 存 vecs_ f32 字节流(mmap 只读)。写入顺序:
    // save_vec_payload → serialize → SearchCheckpoint::write,任一步崩溃由
    // watermark + CRC 校验回退 fold 重建。inmem_int8 模式下 has_payload=false,
    // 不产生 search.vec(无常驻 f32)。
    //
    // save_vec_payload:把 vecs_[0..count_) 写入 payload 文件。BCVP 格式:
    // header(48B) + 每 4KB 页 CRC32 表 + 页对齐 vecs 数据区。tmp+rename 原子。
    // S14-2:增量追加路径。目标文件身份（dev/ino）与本对象的追加状态
    // （vec_file_，见私有区）匹配时，只把 [vec_file_.count, count_) 的新
    // 向量 pwrite 到数据区尾 + fdatasync + 原地重写 64B header（version=2；
    // 不再维护页 CRC 表——该表从未被 load 校验）；否则退全量重写（v1 字节
    // 格式原样，成功后收养新文件身份，使后续 save 走追加）。
    // **前缀不变契约**：追加只写 offset ≥ 旧 count 数据尾的区域——文件里
    // ckpt 声称的 [0, n) 前缀在任何 torn append 下保持完好；配合「先 .vec
    // 后 .ckpt 原子发布」顺序，崩溃后要么用旧 n（尾部垃圾被忽略/下次覆盖）
    // 要么用新 n（数据已 fdatasync），方向恒安全。
    [[nodiscard]] bool save_vec_payload(std::string_view path) const;
    // load_vec_payload:mmap payload 文件。MAP_SHARED 只读 + madvise(RANDOM)。
    // PRECONDITION:deserialize() 已设 count_/dim;本调用设置 vecs_mmap_base_/
    // checkpoint_count_。校验 header 的 dim/header_crc；S14-2 前缀契约：
    // 接受 header.count ≥ n（.prev 回退时 .vec 比旧代 ckpt 长是常态），
    // 只要求文件持有 [0, n) 前缀字节。version 1/2 均接受。
    [[nodiscard]] bool load_vec_payload(std::string_view path);

    // ---- S14-8:int8 码字外置（search.qc8，BCQ8 v1）----
    // qcodes/qscale/qsum（dim+8 B/节点，高维 int8 部署下占 BVH2 的 ~95%）
    // 拆出 append-only 文件，与 .vec 同款前缀契约/身份收养/追加机制：
    // 非 rebuild 的 base 保存（close/链满 rebase）只追加窗口，免全量重写。
    // BVH2 v3 段不再内嵌码字（ord/level/邻接留在段内——邻接可变无法外置）。
    // needs_qcodes_ 为假（kL2/无 int8 内核）不产生 qc8 文件。
    //
    // **payload 代号（generation nonce）**：v3 段头与 .vec/.qc8 头共同携带
    // payload_gen——rebuild 全量重写 payload 后若走 .prev 回退，旧图配新
    // payload（node id 已重映射）而单纯的「前缀 count ≥ n」会错误接受；
    // 代号不匹配即拒载（gen==0 的 legacy 文件跳过校验，首次 rebuild 后
    // 自动进入保护）。此校验同时闭合 S14-2 时代 .vec 的同型隐患。
    [[nodiscard]] bool save_qc_payload(std::string_view path) const;
    [[nodiscard]] bool load_qc_payload(std::string_view path);
    // v3 反序列化后是否还欠 qc8 载入（v2 内嵌码字 / 无码字配置 → false）。
    [[nodiscard]] bool qc_payload_pending() const noexcept {
        return qc_pending_;
    }

    // serialize:V2 header → buf(供 search.ckpt kHnsw 段嵌入)。
    // 含 qcodes/qscales/qsums 直存 + has_payload 标志 + payload 基本元信息
    // (dim/count/watermark 供 load_vec_payload 交叉校验)。不含 vecs_ 字节。
    [[nodiscard]] bool serialize(std::vector<std::uint8_t>& out) const;
    // deserialize:V2 header buf → 内存结构。设置 qcodes/adj/entry/count/
    // max_inserted_ord。不 mmap payload;调用方随后 load_vec_payload()。
    [[nodiscard]] bool deserialize(std::span<const std::uint8_t> bytes);

    // save/load:独立文件便捷 wrapper(测试用)。save = save_vec_payload +
    //   serialize → fwrite;load = fread → deserialize → load_vec_payload。
    //   base_path.ckpt = header,base_path.vec = payload。
    [[nodiscard]] bool save(std::string_view base_path) const;
    [[nodiscard]] bool load(std::string_view base_path);

private:
    static constexpr std::uint32_t kChunkBits = 16;
    static constexpr std::uint32_t kChunkSize = 1u << kChunkBits;
    static constexpr std::uint32_t kChunkMask = kChunkSize - 1;
    static constexpr std::size_t   kMaxChunks = 1024;  // 上限 64M 节点
    // ⑥ 邻接 arena slab 大小(uint32 个数)。单节点块最大 (1+2M)+31*(1+M),
    // M=16 时 = 560，远小于 slab；大 M 仍 << 此值(alloc_adj 有 assert 兜底)。
    static constexpr std::size_t   kAdjSlabWords = 1u << 18;  // 256K u32 = 1 MB

    // 节点分段存储:全部成员构造时定容,生命周期内地址稳定。
    struct NodeChunk {
        // V7:vecs_ 仅 hot chunk(insert 新建)分配,deserialize 路径传
        // needs_vecs=false → 容量 0(数据在 mmap)。定容预分配,地址永不变,
        // 恢复并发读者协议依赖的地址稳定不变量(hot_vecs_ 单 vector 方案
        // 因 reallocation 破坏此不变量,segfault)。
        std::vector<float>          vecs;    // needs_vecs ? kChunkSize*dim : 0
        std::vector<std::uint64_t>  ords;
        std::vector<std::uint8_t>   levels;
        // ⑥ 每节点邻接块首指针,指向 per-chunk bump-slab arena(adj_slabs)。
        // 替代旧的 vector<vector> 每节点 malloc：消除 ~93.75% 的 L0-only 节点
        // malloc，且分配序=插入序 → 邻接块在 slab 内连续(copy_neighbors 局部
        // 性)。slab 永不搬迁，地址稳定不变量与旧 vector.data() 一致。
        std::vector<std::uint32_t*> adj;
        // bump arena：固定大小 slab，满了追加新 slab(旧 slab 永不移动)。仅
        // 单写者(insert)/deserialize 触碰；读者只跟 adj[slot] 指针、不读
        // adj_slabs，故 slab 增长对并发读者安全。
        std::vector<std::unique_ptr<std::uint32_t[]>> adj_slabs;
        std::size_t adj_slab_used = 0;
        // S13-P7：per-node seqlock 序号（偶=稳定，奇=写者更新中）。此前是
        // 自旋锁——读者 copy_neighbors 对锁字节做 exchange（写操作），HNSW
        // 流量高度偏向 hub 节点 → 并发查询时锁缓存行核间乒乓。写者是单
        // 线程（reducer），无需互斥，只需发布协议：写者 ±1 包住 adj 更新，
        // 读者双读序号一致才采信（torn 读被重试丢弃；数据字全走 atomic_ref
        // relaxed，UB-free 且 TSan 干净）。u32 防 ABA 回绕。
        std::unique_ptr<std::atomic<std::uint32_t>[]> locks;

        // V4.2:int8 量化副本(对称量化,scale = max |v[i]|)。codes 是紧
        // 排 int8,scale/sum_codes 是每向量一个标量;load/insert 时写入,
        // 读者两阶段检索的粗筛直接读这段,4× 带宽缩减 + VNNI 指令提速。
        // V7:BCVS v2 盘上直存(省启动量化 pass);V1 是启动时从 f32 重算。
        std::vector<std::int8_t>    qcodes;  // kChunkSize * dim
        std::vector<float>          qscales; // kChunkSize
        std::vector<std::int32_t>   qsums;   // kChunkSize(VNNI 偏置补偿)

        // 从 arena bump 分配 n 个零初始化 uint32(slab 构造即全零，bump 区
        // 从不复用，故无需再清零——与旧 resize(slots) 的零填充语义一致)。
        // 仅单写者/deserialize 调用，非线程安全(协议保证不并发)。
        std::uint32_t* alloc_adj(std::size_t n) {
            assert(n <= kAdjSlabWords && "adj block exceeds slab");
            if (adj_slabs.empty() || adj_slab_used + n > kAdjSlabWords) {
                adj_slabs.push_back(
                    std::make_unique<std::uint32_t[]>(kAdjSlabWords));
                adj_slab_used = 0;
            }
            std::uint32_t* p = adj_slabs.back().get() + adj_slab_used;
            adj_slab_used += n;
            return p;
        }

        NodeChunk(std::size_t dim, bool needs_vecs, bool needs_qcodes);
        ~NodeChunk() = default;
        NodeChunk(const NodeChunk&) = delete;
        NodeChunk& operator=(const NodeChunk&) = delete;
    };

    using DistFn = float (*)(const float*, const float*, std::size_t);

    [[nodiscard]] NodeChunk* chunk_of(std::uint32_t id) const {
        return chunks_[id >> kChunkBits].load(std::memory_order_acquire);
    }
    [[nodiscard]] const float* vec_of(std::uint32_t id) const {
        if (id < checkpoint_count_) {
            return vecs_mmap_base_ +
                   static_cast<std::size_t>(id) * cfg_.dim;
        }
        const NodeChunk* c = chunk_of(id);
        return c->vecs.data() +
               static_cast<std::size_t>(id & kChunkMask) * cfg_.dim;
    }
    // V4.2:量化副本访问器。两阶段检索的 int8 粗筛用,与 f32 路径并行
    // 而不互相干扰;QVector 的 sum_codes 由 quantize() 预算好供 VNNI 偏置
    // 补偿。
    // S32-M2:与 vec_of 同型路由——id < qc_checkpoint_count_ 走 .qc8 mmap
    // (BCQ8 记录 [codes dim | scale f32 | sum i32] 定长 stride,页缓存可
    // 回收,堆零驻留),≥ 走 hot chunk 堆数组。scale/sum 在记录尾部,偏移
    // 任意对齐 → memcpy 取(int8 codes 指针无对齐要求,内核全 loadu)。
    [[nodiscard]] std::size_t qc_stride() const noexcept {
        return static_cast<std::size_t>(cfg_.dim) + sizeof(float) +
               sizeof(std::int32_t);
    }
    [[nodiscard]] const std::int8_t* qcodes_of(std::uint32_t id) const {
        if (id < qc_checkpoint_count_) {
            return reinterpret_cast<const std::int8_t*>(
                qc_mmap_recs_ + static_cast<std::size_t>(id) * qc_stride());
        }
        const NodeChunk* c = chunk_of(id);
        return c->qcodes.data() +
               static_cast<std::size_t>(id & kChunkMask) * cfg_.dim;
    }
    [[nodiscard]] float qscale_of(std::uint32_t id) const {
        if (id < qc_checkpoint_count_) {
            float s;
            std::memcpy(&s,
                        qc_mmap_recs_ +
                            static_cast<std::size_t>(id) * qc_stride() +
                            cfg_.dim,
                        sizeof(float));
            return s;
        }
        return chunk_of(id)->qscales[id & kChunkMask];
    }
    [[nodiscard]] std::int32_t qsum_of(std::uint32_t id) const {
        if (id < qc_checkpoint_count_) {
            std::int32_t z;
            std::memcpy(&z,
                        qc_mmap_recs_ +
                            static_cast<std::size_t>(id) * qc_stride() +
                            cfg_.dim + sizeof(float),
                        sizeof(std::int32_t));
            return z;
        }
        return chunk_of(id)->qsums[id & kChunkMask];
    }
    [[nodiscard]] std::uint64_t ord_of(std::uint32_t id) const {
        return chunk_of(id)->ords[id & kChunkMask];
    }
    [[nodiscard]] std::uint32_t layer_cap(std::uint32_t layer) const {
        return layer == 0 ? cfg_.M * 2 : cfg_.M;
    }
    // 邻接块内某层的起始偏移(L0: 1+2M 槽,上层各 1+M 槽)。
    [[nodiscard]] std::size_t layer_off(std::uint32_t layer) const {
        return layer == 0
                   ? 0
                   : (1 + cfg_.M * 2) +
                         static_cast<std::size_t>(layer - 1) * (1 + cfg_.M);
    }

    [[nodiscard]] float dist_id(const float* q, std::uint32_t id) const {
        return dist_(q, vec_of(id), cfg_.dim);
    }

    // V4.2:int8 粗筛距离。与 f32 路径同"越小越近"约定(VNNI 内核返回
    // 正的重建内积,kDot 语义下取负)。查询侧 codes/scale/sum 由调用方
    // 预算好(quantize() 一次性),db 侧三个标量由 qcodes_of/qscale_of/
    // qsum_of 读取。
    [[nodiscard]] float dist_id_int8(const std::int8_t* query_codes,
                                     float query_scale,
                                     [[maybe_unused]] std::int32_t query_sum,
                                     std::uint32_t id) const {
        const float d = int8_dot_(
            query_codes, qcodes_of(id),
            qsum_of(id), query_scale, qscale_of(id), cfg_.dim);
        return -d;
    }

    // P5:两个已存节点间的 int8 距离(int8-only 建图选边/收缩用,无 f32)。
    // 与 dist_id_int8 同约定"越小越近"(kDot:取重建内积的负)。
    [[nodiscard]] float dist_id_int8_node(std::uint32_t a,
                                          std::uint32_t b) const {
        const float d = int8_dot_(qcodes_of(a), qcodes_of(b), qsum_of(b),
                                  qscale_of(a), qscale_of(b), cfg_.dim);
        return -d;
    }

    // 读者协议:持 id 的自旋锁把 layer 层邻居拷入 out(容量 ≥ 2M),
    // 返回个数。
    std::uint32_t copy_neighbors(std::uint32_t id, std::uint32_t layer,
                                 std::uint32_t* out) const;

    // 贪心单步:在 layer 上从 start 走到局部最优。n = 调用方的 count 快照
    // (邻接里 >= n 的 id 一律跳过);scratch 为邻居拷贝缓冲(容量 ≥ 2M)。
    [[nodiscard]] std::uint32_t greedy_closest(const float* q,
                                               std::uint32_t start,
                                               std::uint32_t layer,
                                               std::uint32_t n,
                                               std::uint32_t* scratch) const;

    // V4.2:int8 粗筛版贪心下降,供两阶段 search() 在上层调用。
    [[nodiscard]] std::uint32_t greedy_closest_int8(
        const std::int8_t* query_codes, float query_scale,
        std::int32_t query_sum, std::uint32_t start, std::uint32_t layer,
        std::uint32_t n, std::uint32_t* scratch) const;

    // 标准 search-layer:返回 ≤ef 个 (dist,id),按 dist 升序。
    void search_layer(const float* q, std::uint32_t entry, std::size_t ef,
                      std::uint32_t layer, std::uint32_t n,
                      std::uint32_t* scratch,
                      std::vector<std::pair<float, std::uint32_t>>& out) const;

    // V4.2:int8 粗筛版 search-layer,与 f32 版同结构,只换距离函数。
    void search_layer_int8(const std::int8_t* query_codes, float query_scale,
                           std::int32_t query_sum, std::uint32_t entry,
                           std::size_t ef, std::uint32_t layer, std::uint32_t n,
                           std::uint32_t* scratch,
                           std::vector<std::pair<float, std::uint32_t>>& out) const;

    // 邻居选择启发式(HNSW 论文 Algorithm 4):候选若离 query 比离任一
    // 已选邻居更近才保留——避免聚簇数据上邻居全挤在同一方向。
    void select_neighbors(
        const float* q,
        std::vector<std::pair<float, std::uint32_t>>& cands,
        std::uint32_t m) const;

    // P5:int8-only 版选边启发式。候选 dist(到 query)已预算在 pair 里;
    // 候选-已选比较走 dist_id_int8_node(两节点皆有量化副本)。
    void select_neighbors_int8(
        std::vector<std::pair<float, std::uint32_t>>& cands,
        std::uint32_t m) const;

    HnswConfig cfg_;
    DistFn dist_;                       // 构造时按 metric+ISA 分发一次
    int8::Int8DotFn int8_dot_;          // V4.2:int8 粗筛内核;无 VNNI 时 nullptr
    // 量化副本是否需要(写/读/序列化)。= inmem_int8 || (int8_dot_ && kDot)。
    // kL2 或无 VNNI 时为 false:qcodes/qscales/qsums 不分配(省 ~D 字节/节点),
    // serialize 写零占位、deserialize 跳过,盘上格式定长不变(跨配置兼容)。
    bool needs_qcodes_;
    double inv_log_m_;                  // mL = 1/ln(M)
    std::uint64_t instance_id_;         // thread_local visited 的实例区分键

    // chunk 目录:定容,写者 store-release 安装、读者 load-acquire 读取。
    // 必须保持裸指针 + atomic<NodeChunk*>:这是无锁发布协议的核心——
    // shared_ptr 的原子引用计数开销不可接受(每次 search 都 load)。
    // 析构由 ~HnswIndex() 单线程 delete,此时无并发读者。
    std::array<std::atomic<NodeChunk*>, kMaxChunks> chunks_{};
    std::atomic<std::uint32_t> count_{0};        // 发布水位(节点数)
    // 高 32 位 = max_level+1(0 表示空图),低 32 位 = entry id。
    std::atomic<std::uint64_t> entry_meta_{0};
    std::atomic<std::uint64_t> max_inserted_ord_{
        static_cast<std::uint64_t>(-1)};
    std::mt19937_64 rng_;               // 仅写者使用

    // 单写者声明用守卫(assert 仅 debug 生效;成员无条件存在,避免
    // NDEBUG 不一致的 TU 间布局分歧)。
    std::atomic<bool> writer_active_{false};

    // V7:BCVS v2 vecs_ 外存化。checkpoint 加载的 vecs_[0..checkpoint_count_)
    // 由 mmap 只读覆盖;checkpoint 后的新插入由 NodeChunk::vecs(定容,仅 hot
    // chunk 分配)覆盖。vec_of() 按 id 分支: < checkpoint_count_ 走 mmap,
    // ≥ 走 chunk->vecs。下次 checkpoint 时 save_vec_payload() 合并两者写新
    // payload。inmem_int8 模式下 mmap 不建立(chunk 容量 0)。
    const float*       vecs_mmap_base_  = nullptr;  // = vecs_map_.data()+off
    io::MappedFile     vecs_map_;         // B3：RAII 归并（析构 munmap）
    int                vecs_payload_fd_ = -1;
    std::uint32_t      checkpoint_count_ = 0;

    // S32-M2:.qc8 mmap 化（设计 doc/vector-dual-engine-selection-zh.md
    // §1.2 P1）。此前 load_qc_payload 是 fread+memcpy 进堆——int8 码字
    // （D+8 B/节点，高维下占堆的 ~95%）硬驻留。改 mmap 后前
    // qc_checkpoint_count_ 条记录由页缓存管理（可回收，内存不足退化成慢
    // 而非 OOM），堆上只剩 hot chunk 的新插入码字。deserialize v3 +
    // qc_pending 路径的 chunk 以 needs_qcodes=false 创建（容量 0），
    // 首次热插入懒分配（与 vecs 懒分配同协议——boundary chunk 内已发布
    // 节点全部 < qc_checkpoint_count_ 走 mmap，assign 无并发读者）。
    const std::uint8_t* qc_mmap_recs_ = nullptr;  // 记录区基址（含 stride）
    io::MappedFile      qc_map_;          // B3：RAII 归并（析构 munmap）
    int                 qc_payload_fd_ = -1;
    std::uint32_t       qc_checkpoint_count_ = 0;

    // S14-2:.vec 追加状态——与 mmap 解耦（追加读内存 vec_of、写文件，不需要
    // 目标文件被 mmap；全量重写换 inode 后也能收养新文件继续追加）。
    // valid=false ⇒ 无已知目标（新建/rebuild 后的索引）→ save 走全量重写。
    // count = 文件已持有的向量数（追加起点；load 时取 ckpt 的 n 而非
    // header.count——.prev 回退后文件尾部可能是新代垃圾，从 n 起覆盖）。
    // 并发：只在 save/load 上下文访问（reducer RunFn / close / open 均已
    // 串行化），无并发读者——mutable 仅为让 const 的 save 路径更新状态。
    struct VecFileState {
        bool          valid = false;
        dev_t         dev   = 0;
        ino_t         ino   = 0;
        std::uint64_t data_off = 0;   // 数据区起始偏移（header.vecs_off）
        std::uint32_t count    = 0;   // 已在文件中的向量数
    };
    mutable VecFileState vec_file_{};
    // S14-8:qc8 追加状态（同 VecFileState 语义）。
    mutable VecFileState qc_file_{};
    // S14-8:payload 代号（v3 段头 + .vec/.qc8 头配对校验；0 = legacy 不校验）。
    // 全量重写 payload / 首次 serialize 时惰性分配；append 继承。mutable：
    // save 路径 const。
    mutable std::uint64_t payload_gen_ = 0;
    // v3 反序列化后、load_qc_payload 前为 true（码字尚未就位）。
    bool qc_pending_ = false;

    void ensure_payload_gen() const;
    [[nodiscard]] bool try_append_qc_payload(std::string_view path,
                                             std::uint32_t n) const;

    // S14-2:追加路径本体。前置条件由 caller（save_vec_payload）检查
    // （vec_file_.valid 且 count_ ≥ vec_file_.count）；本函数再做文件身份
    // （dev/ino）与前缀完整性校验，不符/IO 失败返回 false（caller 退全量
    // 重写兜底）。成功时推进 vec_file_.count。
    [[nodiscard]] bool try_append_vec_payload(std::string_view path,
                                              std::uint32_t n) const;
};

}  // namespace bitcask::vec
