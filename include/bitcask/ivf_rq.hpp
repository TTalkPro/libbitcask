// IVF 向量段（S32-M3 v1；设计 doc/vector-dual-engine-selection-zh.md §5.1）。
//
// === 定位 ===
// 磁盘档向量引擎的**不可变段**——build 一次性产文件（训练 + 分簇 + 落盘，
// tmp+rename 原子），open 后 mmap 只读服务；生命周期内文件永不改写。
// 可变性（增量窗口、重建时机、链/恢复记账）全部在 IvfPlugin 层——与
// 倒排的 building/sealed 段模型、HNSW 的 clone_live 换代同一心智。
//
// === v1 范围（明示裁剪）===
// 记录 = int8 对称量化码字（与 HNSW/落盘量化共用 int8::quantize，位级
// 同源），posting 顺序扫描粗筛=精排一遍过。RaBitQ 1-bit 码区在 format
// 里留位（flags bit0，v1 恒 0）——字节缩减 8× 的粗筛层是否引入由召回
// harness（bench/ann_recall_harness.hpp）在 M3.5 出数决定。
// 度量仅 kDot（cosine 上游归一化）；kL2 由上游 open 拒绝。
//
// === BIV 文件格式（LE；ver=2 增 1-bit 粗筛码区，ver=1 兼容读）===
//   header 96B:
//     [0..3]  magic "BIV1"        [4]  ver u32 = 1|2
//     [8]     flags u32（bit0 = 有 1-bit 码区；ver=2 且启用时置位）
//     [12]    dim u16 | pad u16   [16] nlist u32 | pad u32
//     [24]    count u64           [32] max_ord u64（插入水位）
//     [40]    gen u64（payload 代号，与 ckpt 配对——HNSW gen 守卫同语义）
//     [48]    cent_off u64        [56] cidx_off u64
//     [64]    post_off u64        [72] file_len u64
//     [80]    bits_off u64（flags bit0 时有效） [88] bits_crc u32
//     [92]    hcrc u32（覆盖 [0,92)）
//   cent 区:  nlist × dim × f32（质心，已归一化——球面 k-means）
//   cidx 区:  nlist × 16B { off u64（绝对偏移，指记录区）, count u32, crc u32 }
//   bits 区（flags bit0）: 与记录区同序的 1-bit 粗筛码，每记录
//     { sign bits ceil(dim/8)B（不足补零） | mu f32（mean|v|，est 校正）}
//     —— 簇 c 的 bits 基址由 cidx off 推导（记录区连续同序）。
//   post 区:  按簇连续记录 { ord u64 | codes int8[dim] | scale f32 | sum i32 }
//             stride = dim + 16
//
// === 两段扫（S32-M3.5-②，RaBitQ-lite）===
// 有 bits 区时：阶段 A 对 probe 簇做对称 1-bit 扫（query sign 码预算一次，
// XOR+popcount；est = μ_v·(d − 2·hamming)，μ_q/全局常数不影响排序）取
// top-C（coarse_c，0 = auto max(8k,128)）；阶段 B 仅对 C 个候选 int8 精排
// （live 过滤在 B 段）。字节量 8× 缩减——高维大库档扫描带宽是瓶颈
// （100k/384d 实测已近带宽墙，10M×1024d 投影 ~13MB/查询）。无 bits 区
// （v1 文件）退单段 int8 扫。est 是有损排序：召回由 C 冗余兜底，红线由
// ivf_rq_test 召回门 + bench recall@10_i8 守护。
//
// === 线程模型 ===
// build 静态函数（调用方单线程发起；内部并行，见 IvfBuildSource 契约）。
// open 后对象只读 → search 多线程并发安全。
//
// === 已知代价（v1 挂账）===
// build 的全量 assign 是 O(N·nlist·dim)——1M×4k×384d 约几十秒（多线程
// 自动向量化）。base rebase 频率由 rebase_min_docs 门控制；若成痛点，
// v2 用质心图/层次分配加速（挂 TASK.md）。

#pragma once

#include <cstdint>
#include <cstring>  // record_at 的 memcpy 取值
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace bitcask::vec {

// build 数据源（回调式——调用方负责从 HNSW 窗口/旧段/data file 取数，
// 本类不感知来源）。契约：get **可多线程并发调用**（i 两两不同）；返回
// 的 vec 指针仅在同线程下一次 get 调用前有效；向量已归一化、长度 = dim。
struct IvfBuildSource {
    std::uint32_t count = 0;
    std::function<void(std::uint32_t i, std::uint64_t& ord,
                       const float*& vec)>
        get;
};

class IvfSegment {
public:
    IvfSegment() = default;
    ~IvfSegment();
    IvfSegment(const IvfSegment&) = delete;
    IvfSegment& operator=(const IvfSegment&) = delete;

    // 训练（采样球面 k-means）+ 全量分簇 + 写文件（tmp+rename 原子）。
    // nlist = 0 → 自动 clamp(4·√N, 16, 65536)（再按 N 下修，保簇均 ≥ 8）。
    // gen：payload 代号（调用方与 ckpt 头配对持久化）。count=0 合法（空段）。
    // with_bits：写 1-bit 粗筛码区（ver=2；默认开）。false = v1 格式
    //（测试兼容读/对拍用）。
    [[nodiscard]] static bool build(std::string_view path, std::uint16_t dim,
                                    const IvfBuildSource& src,
                                    std::uint32_t nlist, std::uint64_t gen,
                                    std::uint64_t seed = 0x5EEDF00D,
                                    bool with_bits = true);

    // mmap 打开。校验 magic/ver/dim/header CRC/文件长度；expected_gen 非 0
    // 时须与文件 gen 配对（防 .prev 回退误配，同 HNSW payload 守卫）。
    // verify_crc：逐簇记录区 CRC 全验（S30 封口段同款默认开；可信盘可关）。
    [[nodiscard]] bool open(std::string_view path, std::uint16_t dim,
                            std::uint64_t expected_gen, bool verify_crc = true);
    void close();

    struct Hit {
        std::uint64_t ord;
        float score;  // 重建内积（越大越近；与 HNSW kDot 分数同语义可归并）
    };
    // top-k：质心暴扫取 top-nprobe 簇 → posting 顺序扫（int8 内核）→
    // 结果侧 live 过滤。nprobe = 0 → 默认 max(nlist/32, 8)。线程安全。
    // coarse_c：两段扫的粗筛候选数（仅 bits 区存在时生效；0 = auto
    // max(8·k, 128)）。v1 文件/0 候选退单段 int8 扫。
    [[nodiscard]] std::vector<Hit> search(
        std::span<const float> query, std::size_t k, std::uint32_t nprobe,
        const std::function<bool(std::uint64_t)>* live = nullptr,
        std::uint32_t coarse_c = 0) const;

    // 线性记录访问（post 区跨簇连续；顺序 = 簇序非 ord 序）。前置
    // i < size()。IvfPlugin base 重建的数据源用（dequant 后再喂 build）。
    struct RecordView {
        std::uint64_t       ord;
        const std::int8_t*  codes;
        float               scale;
        std::int32_t        sum;
    };
    [[nodiscard]] RecordView record_at(std::uint64_t i) const {
        const std::uint8_t* rec = base_ + post_off_ + i * rec_stride();
        RecordView r;
        std::memcpy(&r.ord, rec, 8);
        r.codes = reinterpret_cast<const std::int8_t*>(rec + 8);
        std::memcpy(&r.scale, rec + 8 + dim_, 4);
        std::memcpy(&r.sum, rec + 8 + dim_ + 4, 4);
        return r;
    }

    [[nodiscard]] bool opened() const noexcept { return base_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const noexcept { return count_; }
    [[nodiscard]] std::uint16_t dim() const noexcept { return dim_; }
    [[nodiscard]] std::uint32_t nlist() const noexcept { return nlist_; }
    [[nodiscard]] std::uint64_t gen() const noexcept { return gen_; }
    [[nodiscard]] std::uint64_t max_ord() const noexcept { return max_ord_; }

private:
    [[nodiscard]] std::size_t rec_stride() const noexcept {
        return static_cast<std::size_t>(dim_) + 16;
    }

    const std::uint8_t* base_ = nullptr;  // mmap 基址
    void*               raw_  = nullptr;
    std::size_t         len_  = 0;
    int                 fd_   = -1;

    std::uint16_t dim_   = 0;
    std::uint32_t nlist_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t max_ord_ = 0;
    std::uint64_t gen_   = 0;
    const float*  centroids_ = nullptr;   // cent 区
    const std::uint8_t* cidx_ = nullptr;  // cidx 区
    std::uint64_t post_off_ = 0;          // post 区起始（record_at 用）
    // S32-M3.5-②:1-bit 粗筛码区（ver=2 flags bit0;v1 文件为空 = 单段扫）。
    const std::uint8_t* bits_ = nullptr;
    [[nodiscard]] std::size_t bits_stride() const noexcept {
        return (static_cast<std::size_t>(dim_) + 7) / 8 + sizeof(float);
    }
};

}  // namespace bitcask::vec
