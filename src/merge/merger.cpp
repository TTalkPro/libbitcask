#include "bitcask/merger.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>

#include "bitcask/codec.hpp"
#include "bitcask/data_file.hpp"
#include "bitcask/format.hpp"
#include "bitcask/hint_file.hpp"
#include "bitcask/plugin_api.hpp"  // S18-7：merge 参与协议（设计 §3.9）

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
    std::uint64_t  tstamp;
    std::uint64_t  ord;
};

// MergeRunner：把原 run_merge 的内联 fold lambda（85 行）与 apply_pending
// lambda（37 行）提取为命名方法——核心 merge 逻辑可独立测试。
// run_merge 退化为薄包装（构造 runner + 调 run）。
class MergeRunner {
public:
    MergeRunner(keydir::KeyDir& keydir, bool sync_output,
                std::span<plugin::CaskPlugin* const> plugins,
                std::uint64_t now_sec)
        : keydir_(keydir), sync_output_(sync_output),
          plugins_(plugins), now_sec_(now_sec) {}

    std::expected<MergeStats, MergeFault>
    run(std::span<const std::string> input_data_paths,
        std::string_view output_dir);

private:
    // S13-P8：pending 分批 apply——不再全量驻留（原大库 merge 内存峰值
    // O(活 key 总字节)）。批边界：每个输入文件 fold 完成后，或批内累计达
    // kApplyBatch 条（兜住巨型单文件）。每批 apply 前 flush+fsync 输出
    //（fsync-before-apply 契约逐批成立）。
    static constexpr std::size_t kApplyBatch = 1u << 18;  // 256K 条/批

    // 一批 apply：flush 输出尾批 → fsync data（fsync-before-apply）→ CAS
    // 切 keydir + on_relocate → 清批。CAS 语义与失败处理见下方原注释
    //（S13-F1：newest_put=false + stuck 复查）。
    std::expected<void, MergeFault> apply_pending();

    // fold 单条记录：活性检查 → TTL 过滤 → 复制到输出 → pending 累积。
    // 原 in_data->fold() 内联 lambda 体提取。
    void fold_record(const codec::DataRecordView& view,
                     std::uint64_t offset,
                     std::uint32_t total_size,
                     std::uint32_t in_file_id,
                     std::expected<void, MergeFault>& error);

    void cleanup_partial_outputs() {
        std::error_code ec;
        std::filesystem::remove(stats_.output_data_path, ec);
        std::filesystem::remove(stats_.output_hint_path, ec);
    }

    // 失败清理（分批版）：任何批 apply 过之后，输出已被 keydir 引用——
    // 绝不能删（删 = S13-F1 同型数据丢失）；只在零 apply 时清残件。
    void fail_cleanup() {
        if (!any_applied_) cleanup_partial_outputs();
    }

    keydir::KeyDir&            keydir_;
    bool                       sync_output_;
    // S18-7：merge 参与插件（设计 §3.9）。首位约定为宿主的 DocmapRelocator
    //（docmap 恒先于插件收到 relocate），其余按注册序。事件在 merge 线程
    // 直接派发——实现者自保线程安全（docmap CAS/原子更新即满足）。
    std::span<plugin::CaskPlugin* const> plugins_;
    std::uint64_t              now_sec_;

    MergeStats                                 stats_;
    std::optional<fileops::DataFile>           out_data_;
    std::optional<fileops::HintFile>           out_hint_;
    std::vector<PendingUpdate>                 pending_;
    bool                                       any_applied_ = false;
};

std::expected<void, MergeFault> MergeRunner::apply_pending() {
    if (pending_.empty()) return {};
    if (auto f = out_data_->flush_batch(); !f) {
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                        f.error().errnum,
                                        stats_.output_data_path));
    }
    if (auto sy = out_data_->sync(); !sy) {
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                        sy.error().errnum,
                                        stats_.output_data_path));
    }
    for (const auto& u : pending_) {
        auto pr = keydir_.put(u.key, stats_.output_file_id, u.new_total_size,
                              u.new_offset, u.tstamp, /*now_sec*/ 0,
                              /*newest_put*/ false,
                              /*old_file_id*/ u.old_file_id,
                              /*old_offset*/ u.old_offset,
                              /*ord*/ u.ord);
        if (pr == keydir::PutResult::kOk) {
            // S18-7：搬迁事件广播（原 search_layer_->on_relocate 直调）。
            // value 视图：分批 apply 时记录缓冲已不在手（S13-P8），传空——
            // RelocateEvent.value 本就是可选便利（设计 §3.9-5）。
            const plugin::RelocateEvent ev{
                u.ord, u.key,
                plugin::RecordLoc{stats_.output_file_id, u.new_offset,
                                  u.new_total_size},
                std::string_view{}};
            for (auto* p : plugins_) p->on_relocate(ev);
            continue;
        }
        auto cur = keydir_.get(u.key);
        if (cur && cur->file_id == u.old_file_id &&
            cur->offset == u.old_offset) {
            stats_.relocations_stuck += 1;
            stats_.stuck_file_ids.push_back(u.old_file_id);
        }
    }
    pending_.clear();
    any_applied_ = true;
    return {};
}

void MergeRunner::fold_record(const codec::DataRecordView& view,
                              std::uint64_t offset,
                              std::uint32_t total_size,
                              std::uint32_t in_file_id,
                              std::expected<void, MergeFault>& error) {
    if (!error.has_value()) return;  // 之前 fold 中已出错，停止处理
    stats_.records_seen += 1;

    // 墓碑：本简化版直接跳过。legacy 在 v2 模式下会回写一条
    // 「shadow file_id」标记到源文件——cask 层 (M3.4+) 自己处理
    // 这部分细节，merger 不再操心。
    if (view.type == format::RecordType::kTombstone) {
        stats_.records_tombs += 1;
        return;
    }

    // 活性检查：keydir 当前最新指向必须正好是 (in_file_id, offset)
    // 才算这条 record 是活的。否则它已经被新写入覆盖了，merge
    // 不要再保留——直接 records_stale +1 跳过。
    std::string_view key_sv(
        reinterpret_cast<const char*>(view.key.data()),
        view.key.size());
    auto current = keydir_.get(key_sv);
    if (!current ||
        current->file_id != in_file_id ||
        current->offset  != offset) {
        stats_.records_stale += 1;
        return;
    }

    // S13-D5：per-key TTL——过期记录不搬运，并 CAS 清 keydir（位置
    // 匹配才删，与并发 put 无冲突）。keydir 删除后输入 unlink 即
    // 完成空间回收；搜索侧死文档由 is_live 过滤 + compact 回收。
    if (now_sec_ != 0 &&
        view.type == format::RecordType::kDoc) {
        auto dv = codec::decode_doc_value(view.value);
        if (dv && dv->expiry_at != 0 && dv->expiry_at <= now_sec_) {
            stats_.records_expired += 1;
            (void)keydir_.conditional_remove(
                key_sv, view.tstamp, in_file_id, offset, now_sec_);
            return;
        }
    }

    // 复制到输出：先写新 data file，再写新 hint file。
    // keydir CAS 更新延后到所有 fold 完成 + fsync 成功后，
    // 失败时 keydir 完全未动→key 仍可读，无需重启恢复。
    // S2:批量 append——累积到 1 MiB 才一次 pwrite。merge 输出末尾
    // 统一 fsync 后才被 caller 采信，符合 write_buffered 的使用前提。
    auto w = out_data_->write_buffered(format::RecordType::kDoc,
                                      view.tstamp, view.ord,
                                      view.key, view.value);
    if (!w) {
        error = std::unexpected(io_fault(
            MergeError::kOutputWriteFailed, 0,
            stats_.output_data_path));
        fail_cleanup();  // S13-P8
        return;
    }
    auto h = out_hint_->write(view.tstamp, w->total_size,
                              w->offset, /*tomb*/ false, view.key);
    if (!h) {
        error = std::unexpected(io_fault(
            MergeError::kOutputWriteFailed, 0,
            stats_.output_hint_path));
        fail_cleanup();  // S13-P8
        return;
    }

    pending_.push_back(PendingUpdate{
        std::string(key_sv),
        in_file_id, offset,
        w->total_size, w->offset,
        view.tstamp, view.ord
    });

    stats_.records_kept += 1;
    stats_.bytes_written += total_size;
    (void)total_size;  // 已在 w->total_size 里记过

    // S13-P8：巨型单文件兜底——批内达阈值即 apply（fsync 已含）。
    if (pending_.size() >= kApplyBatch) {
        if (auto ap = apply_pending(); !ap) {
            error = std::unexpected(ap.error());
        }
    }
}

std::expected<MergeStats, MergeFault>
MergeRunner::run(std::span<const std::string> input_data_paths,
                 std::string_view output_dir) {
    // 给输出文件分配新 file_id；这一步必须在 open 前，
    // 文件名直接拼成 "<id>.bitcask.data" / "<id>.bitcask.hint"。
    stats_.output_file_id = keydir_.increment_file_id();
    stats_.output_data_path =
        fileops::mk_data_filename(output_dir, stats_.output_file_id);
    stats_.output_hint_path =
        fileops::mk_hint_filename(stats_.output_data_path);

    // B6:预 reserve 避免几何增长 realloc；分批后按批上限封顶。
    {
        std::uint64_t est_records = 0;
        for (const auto& path : input_data_paths) {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(path, ec);
            if (!ec) est_records += sz / 64;
        }
        if (est_records > 0) {
            pending_.reserve(static_cast<std::size_t>(
                std::min<std::uint64_t>(est_records, kApplyBatch)));
        }
    }

    auto od = fileops::DataFile::open(stats_.output_data_path,
                                       fileops::DataFile::Mode::kCreate,
                                       sync_output_);
    if (!od) {
        cleanup_partial_outputs();  // 幂等：kCreate 可能已建空文件
        return std::unexpected(io_fault(MergeError::kOutputOpenFailed,
                                         od.error().errnum,
                                         stats_.output_data_path));
    }
    out_data_.emplace(std::move(*od));

    auto oh = fileops::HintFile::open(stats_.output_hint_path,
                                       fileops::HintFile::Mode::kCreate,
                                       sync_output_);
    if (!oh) {
        cleanup_partial_outputs();  // 关键：out_data 已创建，必须清理
        return std::unexpected(io_fault(MergeError::kOutputOpenFailed,
                                         oh.error().errnum,
                                         stats_.output_hint_path));
    }
    out_hint_.emplace(std::move(*oh));

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
            fail_cleanup();  // S13-P8：前序文件批可能已 apply → 输出不可删
            return std::unexpected(io_fault(MergeError::kInputOpenFailed,
                                             in_data.error().errnum, path));
        }

        std::expected<void, MergeFault> error;
        auto fold_res = in_data->fold(
            [this, in_file_id, &error](const codec::DataRecordView& view,
                                        std::uint64_t offset,
                                        std::uint32_t total_size) {
                fold_record(view, offset, total_size, in_file_id, error);
            },
            /*tolerate_crc_errors*/ true);
        if (!error) {
            fail_cleanup();
            return std::unexpected(error.error());
        }
        if (!fold_res) {
            fail_cleanup();
            return std::unexpected(io_fault(MergeError::kInputReadFailed,
                                             fold_res.error().errnum,
                                             path));
        }
        // S13-P8：每输入文件一批（典型批边界；内存峰值 ≈ 单文件活记录）。
        if (auto ap = apply_pending(); !ap) {
            fail_cleanup();
            return std::unexpected(ap.error());
        }
    }

    // S2:把 data 的 batch_buf_ 残尾一次 pwrite 落盘（后续 sync() 也会兜底，
    // 这里显式 flush 让错误能走 cleanup 路径而非掩盖在 sync 里）。
    // S13-P8：分批后此处通常已空（每批 apply 前已 flush+sync），保留兜底。
    if (auto f = out_data_->flush_batch(); !f) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kOutputWriteFailed,
                                         f.error().errnum,
                                         stats_.output_data_path));
    }

    if (auto f = out_hint_->finalize(); !f) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         f.error().errnum,
                                         stats_.output_hint_path));
    }
    // 无条件 fsync 输出 data + hint：保证 run_merge 成功返回 = 新文件已落盘，
    // 调用方据此才能安全 unlink 原始输入文件。sync_output 仅控制写入过程是
    // 否每条 pwrite 都 O_SYNC 落盘（写入吞吐 vs 单条延迟权衡）；末尾必须至
    // 少一次 fsync 把 page cache 刷到磁盘——否则断电窗口内新文件未落盘而
    // 原始文件已被 caller 删除 → 数据永久丢失。data 是权威优先 fsync。
    if (auto s = out_data_->sync(); !s) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         s.error().errnum,
                                         stats_.output_data_path));
    }
    if (auto s = out_hint_->sync(); !s) {
        fail_cleanup();
        return std::unexpected(io_fault(MergeError::kFinalizeFailed,
                                         s.error().errnum,
                                         stats_.output_hint_path));
    }

    // 收尾 apply：分批后 pending 通常已空（各文件批已在 fold 循环内 apply）。
    // 语义注释（S13-F1，适用于 apply_pending 内的 CAS 循环）：
    //   - newest_put=false 条件 CAS：CAS 门要求精确匹配 (old_file_id,
    //     old_offset)，accept 走 cur.file_id < file_id（输入 id 恒小于输出
    //     id）——不受并发 roll 影响。CAS 被拒 = 并发 put/remove 已改该 key
    //     （合法，输入中该 record 已 dead）；复查仍指旧位置 = stuck（当前
    //     实现不可达，防回归），caller 跳过该输入的 unlink。
    //   - on_relocate 与 keydir.put 配对成功才调，避免 search 已切但
    //     keydir 未切的不一致窗口。
    if (auto ap = apply_pending(); !ap) {
        fail_cleanup();
        return std::unexpected(ap.error());
    }
    if (!stats_.stuck_file_ids.empty()) {
        std::sort(stats_.stuck_file_ids.begin(), stats_.stuck_file_ids.end());
        stats_.stuck_file_ids.erase(
            std::unique(stats_.stuck_file_ids.begin(),
                        stats_.stuck_file_ids.end()),
            stats_.stuck_file_ids.end());
    }

    return stats_;
}

}  // namespace

std::expected<MergeStats, MergeFault>
run_merge(std::span<const std::string> input_data_paths,
          std::string_view output_dir,
          keydir::KeyDir& keydir,
          bool sync_output,
          std::span<plugin::CaskPlugin* const> plugins,
          std::uint64_t now_sec) {
    MergeRunner runner(keydir, sync_output, plugins, now_sec);
    // S18-7：merge 生命周期事件（设计 §3.9）——begin 在 fold 前、commit 在
    // keydir 批量切换完成后、abort 在任何失败路径。插件收尾（GC/rebase）经
    // host->run_serialized 投递 reducer 静止点，先于宿主随后的成对保存点
    // RunFn（同队列 FIFO 顺序涌现 = 旧硬编码序）。
    {
        const plugin::MergeBeginEvent ev{{}, keydir.peek_next_ord()};
        for (auto* p : plugins) p->on_merge_begin(ev);
    }
    auto result = runner.run(input_data_paths, output_dir);
    if (result) {
        const std::uint32_t out_id = result->output_file_id;
        const plugin::MergeCommitEvent ev{{&out_id, 1}, 0.0};
        for (auto* p : plugins) p->on_merge_commit(ev);
    } else {
        for (auto* p : plugins) p->on_merge_abort();
    }
    return result;
}

}  // namespace bitcask::merge
