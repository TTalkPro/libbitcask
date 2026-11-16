// 「只放行已提交记录」的 data 文件 fold（S35 原子批语义的唯一实现）。
//
// 批头声明区间内的成员先暂存，扫到区间末端（批已完整落盘 = 已提交）才
// 依序交给 on_record；区间不完整时 DataFile::fold 在批头处 break，暂存
// 随作用域丢弃。原先这段逻辑在 rebuild_from_data / data-only 恢复 /
// 搜索全量 fold 三处各复制一份。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"

namespace bitcask::fileops {

struct CommittedFoldOptions {
    bool tolerate_crc_errors = true;
    std::uint64_t* out_last_valid_end = nullptr;
    std::uint64_t start_offset = 0;
    // false：暂存时不拷贝 value（只需 key/位置的调用方，如索引重建），
    // 交给 on_record 的 view.value 为空。非批记录不受影响（原样透传）。
    bool keep_batch_values = true;
};

using CommittedRecordFn = std::function<void(
    const codec::DataRecordView& view, std::uint64_t offset,
    std::uint32_t total_size)>;
// 批头回调（可空）：批头本身不是数据记录，但其 ord 需推进分配水位。
using BatchHeaderFn = std::function<void(const codec::DataRecordView& view)>;

[[nodiscard]] std::expected<void, DataFileFault>
fold_committed(DataFile& df, const CommittedRecordFn& on_record,
               const BatchHeaderFn& on_batch_header = {},
               const CommittedFoldOptions& opts = {});

}  // namespace bitcask::fileops
