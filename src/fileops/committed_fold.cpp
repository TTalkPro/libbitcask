#include "bitcask/detail/committed_fold.hpp"

#include <vector>

#include "bitcask/format.hpp"

namespace bitcask::fileops {

std::expected<void, DataFileFault>
fold_committed(DataFile& df, const CommittedRecordFn& on_record,
               const BatchHeaderFn& on_batch_header,
               const CommittedFoldOptions& opts) {
    struct Staged {
        format::RecordType type;
        std::uint64_t tstamp;
        std::uint64_t ord;
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        std::uint64_t offset;
        std::uint32_t total_size;
    };
    std::vector<Staged> staged;
    std::uint64_t batch_end = 0;  // 0 = 未处于批区间
    return df.fold(
        [&](const codec::DataRecordView& view, std::uint64_t offset,
            std::uint32_t total_size) {
            if (view.type == format::RecordType::kBatchHeader) {
                // fold 层已验证区间在 EOF 内；value 畸形时防御性忽略。
                auto bh = codec::decode_batch_header_value(view.value);
                if (!bh) return;
                batch_end = offset + total_size + bh->span_bytes;
                staged.clear();
                if (on_batch_header) on_batch_header(view);
                return;
            }
            if (batch_end == 0) {
                on_record(view, offset, total_size);
                return;
            }
            Staged s{view.type, view.tstamp, view.ord,
                     {view.key.begin(), view.key.end()}, {}, offset,
                     total_size};
            if (opts.keep_batch_values) {
                s.value.assign(view.value.begin(), view.value.end());
            }
            staged.push_back(std::move(s));
            if (offset + total_size >= batch_end) {
                // 区间收口 = 批已提交：依序放行。
                for (const auto& r : staged) {
                    const codec::DataRecordView v{
                        .crc = 0,
                        .type = r.type,
                        .tstamp = r.tstamp,
                        .ord = r.ord,
                        .key = r.key,
                        .value = r.value,
                        .total_size = r.total_size,
                    };
                    on_record(v, r.offset, r.total_size);
                }
                staged.clear();
                batch_end = 0;
            }
        },
        opts.tolerate_crc_errors, opts.out_last_valid_end, opts.start_offset);
}

}  // namespace bitcask::fileops
