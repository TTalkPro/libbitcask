#include "bitcask/merger.hpp"

#include <cstring>
#include <filesystem>

#include "bitcask/data_file.hpp"
#include "bitcask/format.hpp"
#include "bitcask/hint_file.hpp"
#include "bitcask/search_layer.hpp"

namespace bitcask::merge {

namespace {

MergeFault io_fault(MergeError kind, int errnum, std::string detail = {}) {
    return MergeFault{kind, errnum, std::move(detail)};
}

// 延迟应用项：fold 时只写新文件 + 记录变更，不立即 CAS 更新 keydir。
// fsync 成功后才统一 apply——保证 merge 失败时 keydir 完全未动，所有
// key 仍可立即可见（无需重启恢复）。
struct PendingUpdate {
    std::string    key;
    std::uint32_t  old_file_id;
    std::uint64_t  old_offset;
    std::uint32_t  new_total_size;
    std::uint64_t  new_offset;
    std::uint32_t  tstamp;
    std::uint64_t  ord;
};

}  // namespace

std::expected<MergeStats, MergeFault>
run_merge(std::span<const std::string> input_data_paths,
          std::string_view output_dir,
          keydir::KeyDir& keydir,
          bool sync_output,
          search::SearchLayer* search_layer) {
    MergeStats stats;
    // 给输出文件分配新 file_id；这一步必须在 open 前完成，
    // 文件名直接拼成 "<id>.bitcask.data" / "<id>.bitcask.hint"。
    stats.output_file_id = keydir.increment_file_id();
    stats.output_data_path =
        fileops::mk_data_filename(output_dir, stats.output_file_id);
    stats.output_hint_path = fileops::mk_hint_filename(stats.output_data_path);

    // 失败清理：避免残文件被下次 needs_merge 当输入→数据损坏。
    // 残文件指输出 data + hint，任意一个 write/finalize/sync 失败时触发。
    auto cleanup_partial_outputs = [&]() {
        std::error_code ec;
        std::filesystem::remove(stats.output_data_path, ec);
        std::filesystem::remove(stats.output_hint_path, ec);
    };

    std::vector<PendingUpdate> pending_;

    auto out_data = fileops::DataFile::open(stats.output_data_path,
                                             fileops::DataFile::Mode::kCreate,
                                             sync_output);
    if (!out_data) {
        cleanup_partial_outputs();  // 幂等：kCreate 可能已建空文件
        return std::unexpected(io_fault(MergeError::kOutputOpenFailed,
                                         out_data.error().errnum,
                                         stats.output_data_path));
    }
    auto out_hint = fileops::HintFile::open(stats.output_hint_path,
                                             fileops::HintFile::Mode::kCreate,
                                             sync_output);
    if (!out_hint) {
        cleanup_partial_outputs();  // 关键：out_data 已创建，必须清理
        return std::unexpected(io_fault(MergeError::kOutputOpenFailed,
                                         out_hint.error().errnum,
                                         stats.output_hint_path));
    }

    // 按顺序遍历每个输入文件。对每条 record 都问 keydir：「这个 key 当前
    // 最新的 entry 是不是还指向 (in_file_id, offset)？」是的话就是活的，
    // 复制到输出；否则跳过（已经被新 put 或 delete 覆盖了）。
    for (const auto& path : input_data_paths) {
        const auto in_tstamp = fileops::parse_data_tstamp(path);
        if (!in_tstamp) continue;
        const std::uint32_t in_file_id =
            static_cast<std::uint32_t>(*in_tstamp);

        // P6:merge 输入纯 fold,不 mmap(避免对大库逐文件全映射)。
        auto in_data = fileops::DataFile::open(path,
                                                fileops::DataFile::Mode::kRead,
                                                /*sync*/ false,
                                                /*mmap_enabled*/ false);
        if (!in_data) {
            cleanup_partial_outputs();
            return std::unexpected(io_fault(MergeError::kInputOpenFailed,
                                             in_data.error().errnum, path));
        }

        std::expected<void, MergeFault> error;
        auto fold_res = in_data->fold(
            [&](const codec::DataRecordView& view,
                std::uint64_t offset,
                std::uint32_t total_size) {
                if (error.has_value() == false) return;  // 之前 fold 中已出错，停止处理
                stats.records_seen += 1;

                // 墓碑：本简化版直接跳过。legacy 在 v2 模式下会回写一条
                // 「shadow file_id」标记到源文件——cask 层 (M3.4+) 自己处理
                // 这部分细节，merger 不再操心。
                if (view.type == format::RecordType::kTombstone) {
                    stats.records_tombs += 1;
                    return;
                }

                // 活性检查：keydir 当前最新指向必须正好是 (in_file_id, offset)
                // 才算这条 record 是活的。否则它已经被新写入覆盖了，merge
                // 不要再保留——直接 records_stale +1 跳过。
                std::string_view key_sv(
                    reinterpret_cast<const char*>(view.key.data()),
                    view.key.size());
                auto current = keydir.get(key_sv);
                if (!current ||
                    current->file_id != in_file_id ||
                    current->offset  != offset) {
                    stats.records_stale += 1;
                    return;
                }

                // 复制到输出：先写新 data file，再写新 hint file。
                // keydir CAS 更新延后到所有 fold 完成 + fsync 成功后，
                // 失败时 keydir 完全未动→key 仍可读，无需重启恢复。
                // S2:批量 append——累积到 1 MiB 才一次 pwrite。merge 输出末尾
                // 统一 fsync 后才被 caller 采信，符合 write_buffered 的使用前提。
                auto w = out_data->write_buffered(format::RecordType::kDoc,
                                          view.tstamp, view.ord,
                                          view.key, view.value);
                if (!w) {
                    error = std::unexpected(io_fault(
                        MergeError::kOutputWriteFailed, 0,
                        stats.output_data_path));
                    cleanup_partial_outputs();
                    return;
                }
                auto h = out_hint->write(view.tstamp, w->total_size,
                                          w->offset, /*tomb*/ false, view.key);
                if (!h) {
                    error = std::unexpected(io_fault(
                        MergeError::kOutputWriteFailed, 0,
                        stats.output_hint_path));
                    cleanup_partial_outputs();
                    return;
                }

                pending_.push_back(PendingUpdate{
                    std::string(key_sv),
                    in_file_id, offset,
                    w->total_size, w->offset,
                    view.tstamp, view.ord
                });

                stats.records_kept += 1;
                stats.bytes_written += total_size;
                (void)total_size;  // 已在 w->total_size 里记过
            },
            /*tolerate_crc_errors*/ true);
        if (!error) {
            cleanup_partial_outputs();
            return std::unexpected(error.error());
        }
        if (!fold_res) {
            cleanup_partial_outputs();
            return std::unexpected(io_fault(MergeError::kInputReadFailed,
                                             fold_res.error().errnum,
                                             path));
        }
    }

    // S2:把 data 的 batch_buf_ 残尾一次 pwrite 落盘（后续 sync() 也会兜底，
    // 这里显式 flush 让错误能走 cleanup 路径而非掩盖在 sync 里）。
    if (auto f = out_data->flush_batch(); !f) {
        cleanup_partial_outputs();
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                         f.error().errnum,
                                         stats.output_data_path));
    }

    if (auto f = out_hint->finalize(); !f) {
        cleanup_partial_outputs();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         f.error().errnum,
                                         stats.output_hint_path));
    }
    // 无条件 fsync 输出 data + hint：保证 run_merge 成功返回 = 新文件已落盘，
    // 调用方据此才能安全 unlink 原始输入文件。sync_output 仅控制写入过程是
    // 否每条 pwrite 都 O_SYNC 落盘（写入吞吐 vs 单条延迟权衡）；末尾必须至
    // 少一次 fsync 把 page cache 刷到磁盘——否则断电窗口内新文件未落盘而
    // 原始文件已被 caller 删除 → 数据永久丢失。data 是权威优先 fsync。
    if (auto s = out_data->sync(); !s) {
        cleanup_partial_outputs();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         s.error().errnum,
                                         stats.output_data_path));
    }
    if (auto s = out_hint->sync(); !s) {
        cleanup_partial_outputs();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         s.error().errnum,
                                         stats.output_hint_path));
    }

    // 批量 apply：fsync 已完成，新文件持久化。现在原子切换 keydir + search
    // 指向新文件。CAS 失败 = 并发 put 已改这个 key（合法）→ 跳过，X 中的
    // record 成 dead，下次 merge 清。on_relocate 必须与 keydir.put 配对成功
    // 才调，避免 search 已切但 keydir 未切的不一致窗口。
    for (const auto& u : pending_) {
        auto pr = keydir.put(u.key, stats.output_file_id, u.new_total_size,
                             u.new_offset, u.tstamp, /*now_sec*/ 0,
                             /*newest_put*/ true,
                             /*old_file_id*/ u.old_file_id,
                             /*old_offset*/ u.old_offset,
                             /*ord*/ u.ord);
        if (pr == keydir::PutResult::kOk && search_layer) {
            search_layer->on_relocate(u.key, u.ord, stats.output_file_id,
                                       u.new_offset, u.new_total_size);
        }
    }

    return stats;
}

}  // namespace bitcask::merge
