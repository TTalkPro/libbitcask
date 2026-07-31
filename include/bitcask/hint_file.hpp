// bitcask hint file：data file 的并行索引。
//
// S33 flag-day 起写端/读端均仅 v5（"BCH5"：文件头 magic + 变长 vbyte 记录
// （含 ord 差分）+ 8B trailer；布局见 format.hpp）。完整性靠 trailer CRC
// 一次性兜底（不像 data file 每条 record 自带 CRC），校验不过 → caller 退
// fold(data) 重建。BCH4 及更早纪元无读端：按校验失败同样退 fold(data)
// （hint 是派生缓存，陈旧格式 ≠ 数据风险）；纪元硬门禁在 bitcask.meta v5。
//
// 用途：keydir 重建加速。完整跑 fold(data_file) 重建 keydir 需要读全部
// value bytes；fold(hint_file) 只读 key + 元数据，省掉绝大部分 I/O。
// v5 起 hint 记录含 ord，hint 快路径恢复与 fold(data) 完全等价。
//
// === 线程模型 ===
// 类似 DataFile：写路径 write()/finalize() 修改 running_crc_，必须单线程
// 串行；读路径 fold()/validate_trailer() 走 pread，可在不同 HintFile 对象
// 间并发。本类无内部锁，并发由上层保证。

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "bitcask/codec.hpp"
#include "bitcask/detail/file_fault.hpp"
#include "bitcask/io.hpp"

namespace bitcask::fileops {

class HintFile {
public:
    enum class Mode { kRead, kAppend, kCreate };

    HintFile() = default;
    ~HintFile() = default;

    HintFile(const HintFile&) = delete;
    HintFile& operator=(const HintFile&) = delete;
    HintFile(HintFile&&) noexcept = default;
    HintFile& operator=(HintFile&&) noexcept = default;

    // 线程安全: 是；不需任何锁。
    [[nodiscard]] static std::expected<HintFile, DataFileFault>
    open(std::string_view path, Mode mode, bool sync = false);

    // ---- 写入 ----

    // append 一条 hint record（v5：含 ord）。同时更新 running_crc_。
    // 写入先进内存缓冲 pending_，攒到阈值（kFlushBytes）才一次 write(2)，把
    // 写路径的 per-put syscall 减半（hint 可重建：崩溃丢缓冲 → 下次 open
    // 无 trailer → validate_trailer 失败 → fold(data) 回退，安全语义不变）。
    // 线程安全: 否（修改 pending_/running_crc_ 与底层 fd 顺序写状态）；caller 串行化。
    [[nodiscard]] std::expected<void, DataFileFault>
    write(std::uint64_t tstamp, std::uint32_t total_sz,
          std::uint64_t offset, bool tombstone,
          std::span<const std::byte> key, std::uint64_t ord);

    // 写 trailer（magic + 整文件 running CRC）。
    // 不是「per-call 幂等」的——调两次会真写两条 trailer；正常使用是
    // close 前调一次。
    // 线程安全: 否（与 write() 共享 running_crc_）；caller 串行化。
    [[nodiscard]] std::expected<void, DataFileFault> finalize();

    // fsync(2) 落盘。给 merger 在删除原始输入文件前强制持久化输出文件用：
    // 断电是 caller 删除原始文件后无法回退的临界点，必须保证新文件已真正
    // 落盘才能 unlink 旧文件。线程安全: 否（与 write() 共享 fd）；caller 串行化。
    [[nodiscard]] std::expected<void, DataFileFault> sync();

    // ---- 读取 ----

    // 遍历每条 hint record。trailer CRC 不通过 / 非 v5 magic 返回 kBadCrc
    // ——caller 应该退回 fold(data_file) 重建。
    using FoldFn = std::function<void(const codec::HintRecord& rec)>;
    // 线程安全: 是（pread + 顺序读 buf）；多读者可并发 fold 同一对象；
    // fn 自身的并发安全由 caller 负责。
    [[nodiscard]] std::expected<void, DataFileFault> fold(FoldFn fn);

    // 单独验 trailer CRC，不真正解析每条 record。给 has_valid_hintfile()
    // 用——快速判断 hint 文件能不能直接 fold。返回 false：非 v5 magic /
    // trailer 缺失 / CRC 不匹配。
    // 线程安全: 是（仅 pread + 局部 CRC 累加）。
    [[nodiscard]] std::expected<bool, DataFileFault> validate_trailer();

    // S13-P8：单遍「校验 + fold」——整文件一次读入内存，先对 trailer 前
    // 全部字节算 CRC（与 validate_trailer 判定逐字节一致），通过才从内存
    // 解析逐条回调 fn（fn 语义同 fold；CRC 不过时 fn 一次都不会被调）。
    // 返回 true = 校验通过且已 fold；false = 校验不过（caller 回退
    // fold(data)）。替代「validate_trailer 全文件读一遍 + fold 再读一遍」。
    // 内存代价：hint 文件大小的瞬时缓冲（hint ≪ data，可接受）。
    [[nodiscard]] std::expected<bool, DataFileFault> fold_validated(FoldFn fn);

    // ---- 内省 ----
    [[nodiscard]] std::string_view path() const noexcept { return path_; }
    [[nodiscard]] std::uint32_t    running_crc() const noexcept { return running_crc_; }

    void close() noexcept { file_.close_quiet(); }

private:
    HintFile(io::PosixFile&& f, std::string p, std::uint32_t crc, Mode m) noexcept
        : file_(std::move(f)), path_(std::move(p)), running_crc_(crc), mode_(m) {}

    // 把 pending_ 的内容一次性 write 到 fd 并清空（pending 为空则 no-op）。
    [[nodiscard]] std::expected<void, DataFileFault> flush_pending();

    // v5 流式 fold（fold() 校验文件头 magic 后分派至此）。
    [[nodiscard]] std::expected<void, DataFileFault>
    fold_v5(std::uint64_t total, FoldFn fn);

    // 攒满多少字节就 flush 一次（hint 可重建，丢缓冲只触发 fold(data) 回退）。
    // P2:64KiB→1MiB——merge/active 写 hint 的 pwrite 次数 16×↓。hint 非 WAL，
    // 加大缓冲只增大「崩溃丢 hint → fold(data) 回退」的窗口，不影响正确性。
    static constexpr std::size_t kFlushBytes = 1024 * 1024;

    io::PosixFile file_;
    std::string   path_;
    std::uint32_t running_crc_ = 0;
    Mode          mode_        = Mode::kRead;
    // 差分编码的串联状态：prev_end_ = 上条 offset+total_sz；
    // prev_ord_（v5）= 上条 ord。
    std::uint64_t prev_end_    = 0;
    std::uint64_t prev_ord_    = 0;
    std::vector<std::byte> pending_;  // 攒批写缓冲（write 追加，flush/finalize 落盘）
};

}  // namespace bitcask::fileops
