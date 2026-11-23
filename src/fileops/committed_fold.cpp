#include "bitcask/detail/committed_fold.hpp"

#include <vector>

#include "bitcask/format.hpp"

namespace bitcask::fileops {

namespace {

std::expected<void, DataFileFault>
fold_committed_once(DataFile& df, const CommittedRecordFn& on_record,
                    const BatchHeaderFn& on_batch_header,
                    const CommittedFoldOptions& opts, std::uint64_t start_offset,
                    std::uint64_t* out_last_valid_end) {
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
        opts.tolerate_crc_errors, out_last_valid_end, start_offset);
}

}  // namespace


std::expected<void, DataFileFault>
fold_committed(DataFile& df, const CommittedRecordFn& on_record,
               const BatchHeaderFn& on_batch_header,
               const CommittedFoldOptions& opts) {
    std::uint64_t lve = opts.start_offset;
    std::uint64_t start = opts.start_offset;
    for (int gaps = 0;;) {
        auto r = fold_committed_once(df, on_record, on_batch_header, opts, start, &lve);
        if (opts.out_last_valid_end) *opts.out_last_valid_end = lve;
        const bool bad_crc = !r && r.error().kind == DataFileError::kBadCrc;
        if (!r && !bad_crc) return r;                       // IO 等错误原样上抛
        if (r && lve >= df.size()) return r;                // 干净读到文件尾
        if (!opts.heal_gaps) return r;                      // 旧语义
        // 已封口的文件在中间停下了：找下一处完好的记录。
        // 停在一个批头上 ⇒ 这个批不完整（区间越过文件尾，或区间内有坏记录）。
        // 整批作废：区间越过文件尾则后面没有别的东西了；否则从区间末尾之后找起——
        // 绝不把不完整的批的成员单独放出来（原子批：要么全有要么全无）。
        std::uint64_t search_from = lve + 1;
        auto batch_end = df.batch_end_at(lve);
        if (!batch_end) return std::unexpected(batch_end.error());
        if (*batch_end) {
            if (**batch_end >= df.size()) return {};
            search_from = **batch_end;
        }
        auto next = df.find_next_record(search_from);
        if (!next) return std::unexpected(next.error());
        if (!*next) return {};                              // 直到文件尾都没有：尾部垃圾
        if (++gaps > CommittedFoldOptions::kMaxHealedGaps) {
            return std::unexpected(DataFileFault{DataFileError::kBadCrc});
        }
        if (opts.on_gap) opts.on_gap(lve, **next);
        start = **next;
        lve = start;
    }
}

}  // namespace bitcask::fileops
