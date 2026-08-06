// DiskANN 向量段（S32-M5 v1；设计 doc/vector-dual-engine-selection-zh.md §5.2）。
//
// === 定位 ===
// 磁盘档向量引擎的**不可变段**（与 IvfSegment 平级的另一实现）：Vamana
// 单层图 + 盘上节点块共置布局——「一次块读同时取到算距离所需与下一跳
// 所需」（DiskANN 的灵魂，NeurIPS'19）。build 一次性产文件（建图 +
// tmp+rename），open 后 mmap 服务；可变性在 DiskannPlugin 层（窗口 +
// 链，与 IvfPlugin 同构）。
//
// === v1 范围（明示裁剪，与 M3 同款风格）===
// - RAM 导航码 = **int8 三件套**（v1;est ≡ 精评精度,beam 排序理论最优。
//   1-bit sign est 曾试——紧簇语料簇内零区分度,beam 逐步决策误差复合,
//   l=64 召回崩至 ~0.55,废弃）。RAM 成本 dim+8 B/向量（1024d@10M ≈
//   10GB——**尚未兑现 DiskANN「RAM ≪ N·dim」**,压到 PQ32(320MB) 是
//   M5.5 的核心目标;届时 nav 区换 PQ 码 + 块内 int8 恢复精评职责）。
// - 盘上向量 = int8 码字（非 f32；「精排精度统一 int8」S32 既定取舍）。
// - 建图 = 增量 Vamana（beam search 候选 + RobustPrune α，单趟 + 反向
//   边回剪；论文的两趟 α 调度与分片大建留 M5.5）。建图期全部 int8 码
//   驻 RAM——O(N·(dim+8))，10M×1024d ≈ 10GB：**大库建图在大内存机做**
//   （引擎切换 fold 重建同理），设计 §5.2 既定。
// - 度量仅 kDot（cosine 上游归一化）。
//
// === BDA1 文件格式（LE）===
//   header 96B:
//     [0..3]  magic "BDA1"       [4]  ver u32 = 1     [8] flags u32 = 0
//     [12]    dim u16 | pad u16  [16] R u32（邻接容量）
//     [20]    medoid u32（beam 起点）
//     [24]    count u64          [32] max_ord u64     [40] gen u64
//     [48]    nav_off u64        [56] blocks_off u64  [64] file_len u64
//     [72]    nav_crc u32        [76] blocks_crc u32
//     [80..91] reserved          [92] hcrc u32（覆盖 [0,92)）
//   nav 区:    count × { int8 codes dim | scale f32 | sum i32 } —— open 时
//              整段拷入 RAM（驻留导航码,堆账 dim+8 B/向量;v1 与块内码字
//              冗余——块自足性为 M5.5 撤 RAM 副本预留）
//   blocks 区: count × block_stride 节点块（mmap 随机读）:
//     { ord u64 | codes int8[dim] | scale f32 | sum i32 |
//       ncnt u32 | nbrs u32×R（不足补零，ncnt 定界）}
//     block_stride = 8 + dim + 4 + 4 + 4 + 4·R
//
// === 查询（beam search，标准 DiskANN 形态）===
// 候选池 L（= ef 语义；0 → max(2k, 64)）按 **RAM 导航 est** 排序；每次
// 展开池中最优未访节点：读其盘上块 → int8 精确分入结果堆（live 过滤在
// 结果侧）→ 邻居以 est 入池。池全访问完终止。L = count 时图连通则全图
// 可达——精确退化路径（对拍用，S32 原则：有损层必留退化路径）。
//
// === 线程模型 ===
// build 静态（内部并行仅限只读源取数）；open 后只读 → search 并发安全
// （visited 为查询局部状态）。

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "bitcask/ivf_rq.hpp"  // IvfBuildSource（两段引擎共用数据源契约）

#include "bitcask/io.hpp"  // B3：MappedFile（sealed 段只读映射）

namespace bitcask::vec {

class DiskannSegment {
public:
    DiskannSegment() = default;
    ~DiskannSegment();
    DiskannSegment(const DiskannSegment&) = delete;
    DiskannSegment& operator=(const DiskannSegment&) = delete;

    // Vamana 建图 + 落盘（tmp+rename 原子）。r = 邻接容量（0 → 32）；
    // l_build = 建图 beam 宽（0 → max(64, 2r)）。count=0 合法（空段）。
    // 建图期驻 RAM：N×(dim+8) 码字 + N×4R 邻接。
    [[nodiscard]] static bool build(std::string_view path, std::uint16_t dim,
                                    const IvfBuildSource& src,
                                    std::uint32_t r, std::uint32_t l_build,
                                    std::uint64_t gen,
                                    std::uint64_t seed = 0x5EEDF00D);

    // mmap 打开 + nav 区拷入 RAM。gen/CRC 守卫同 IvfSegment（expected_gen
    // 非 0 须配对；verify_crc 全验 nav+blocks，S30 默认开）。
    [[nodiscard]] bool open(std::string_view path, std::uint16_t dim,
                            std::uint64_t expected_gen, bool verify_crc = true);
    void close();

    struct Hit {
        std::uint64_t ord;
        float score;  // int8 重建内积（与 HNSW/IVF kDot 分数同语义可归并）
    };
    // beam search top-k。l = 候选池宽（ef 语义；0 → max(2k,64)；≥ count
    // 时全图精确退化）。线程安全。
    [[nodiscard]] std::vector<Hit> search(
        std::span<const float> query, std::size_t k, std::uint32_t l,
        const std::function<bool(std::uint64_t)>* live = nullptr) const;

    // 线性记录访问（rebuild 数据源用，语义同 IvfSegment::record_at）。
    struct RecordView {
        std::uint64_t       ord;
        const std::int8_t*  codes;
        float               scale;
        std::int32_t        sum;
    };
    [[nodiscard]] RecordView record_at(std::uint64_t i) const;

    [[nodiscard]] bool opened() const noexcept { return base_ != nullptr; }
    [[nodiscard]] std::uint64_t size() const noexcept { return count_; }
    [[nodiscard]] std::uint16_t dim() const noexcept { return dim_; }
    [[nodiscard]] std::uint32_t r() const noexcept { return r_; }
    [[nodiscard]] std::uint64_t gen() const noexcept { return gen_; }
    [[nodiscard]] std::uint64_t max_ord() const noexcept { return max_ord_; }

private:
    [[nodiscard]] std::size_t block_stride() const noexcept {
        return 8 + static_cast<std::size_t>(dim_) + 4 + 4 + 4 +
               4 * static_cast<std::size_t>(r_);
    }
    [[nodiscard]] std::size_t nav_stride() const noexcept {
        // v1:int8 三件套（codes | scale | sum）。M5.5 换 PQ 时 bump ver。
        return static_cast<std::size_t>(dim_) + 8;
    }

    const std::uint8_t* base_ = nullptr;  // mmap 基址（= map_.data()）
    io::MappedFile      map_;             // B3：RAII 归并（析构 munmap）
    int                 fd_   = -1;

    std::uint16_t dim_ = 0;
    std::uint32_t r_ = 0;
    std::uint32_t medoid_ = 0;
    std::uint64_t count_ = 0;
    std::uint64_t max_ord_ = 0;
    std::uint64_t gen_ = 0;
    std::uint64_t blocks_off_ = 0;
    std::vector<std::uint8_t> nav_;  // RAM 驻留导航码（count × nav_stride）
};

}  // namespace bitcask::vec
