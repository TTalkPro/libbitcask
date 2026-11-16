// S44：OKI run 归并写放大 / 读放大 / 空间放大基准（OkiState 直驱，不经 Cask）。
//
// 用法：bitcask_oki_compaction_bench <dir> <N> <F> <full|tiered> [K]
//                                    [--del=<0..1>] [--then-full] [--keep]
//       bitcask_oki_compaction_bench <dir> <N> 0 measure [--then-full]
//         （measure：在全新进程里加载已有目录并测量；配合 --keep 隔离「写入阶段
//          的进程状态」与「run 布局」对读路径的影响）
//
// 模拟「均匀随机 key、每 F 行 flush 一次」的写入，直到 N 次操作（--del 为删除操作的
// 比例，其余为 put）；随后 wait_compaction 并测量（phase=main）：
//   flushed_rows / compaction_rows / write_amp（= compaction_rows / flushed_rows）
//   compactions / full / young / backpressure_waits / compaction_s / wall_s / runs
//   点查 p50 / p99（10 万次随机 key，先预热 2 万次）、每次点查块读 / 块缓存未命中
//   全量迭代吞吐（行/秒）、短区间扫描（随机起点取 100 行）p50 / p99
//   空间：run 总行数、活 key 数、视图内墓碑胜出行数、space_amp = run 总行数 / 活 key 数
// --then-full：在**同一份数据**上强制全归并成单个 run 后重测（phase=postfull）——数据集与页缓存
// 状态相同，只有 run 数不同，用于隔离 run 数对读路径的影响。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // ::sync

#include <bitcask/oki_merge_cursor.hpp>
#include <bitcask/oki_state.hpp>

namespace {

std::string key_of(std::uint64_t x) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "k%016llx",
                  static_cast<unsigned long long>(x * 0x9E3779B97F4A7C15ull));
    return buf;
}

double pct(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(static_cast<double>(v.size() - 1) * p)];
}

bool g_locate_only = false;
bool g_decompose = false;
std::string g_dir;

// 测量前把全部 run 文件顺序读一遍、使其驻留页缓存（两个阶段同样处理）。
// S44-6 分解测量发现：点查 p99 尾部几乎全是 find 段的读盘（100～250µs），来自
// 「老 run 文件在写入压力下被挤出页缓存」，与 run 结构无关；共享机器上的外部负载
// 又让驻留程度随时间漂移。预热后比较的是「索引常驻页缓存」的稳态。
void warm_page_cache() {
    namespace fs = std::filesystem;
    std::vector<char> buf(8u << 20);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_dir, ec)) {
        if (e.path().filename().string().rfind("kv.oki.seg-", 0) != 0) continue;
        std::FILE* f = std::fopen(e.path().c_str(), "rb");
        if (!f) continue;
        while (std::fread(buf.data(), 1, buf.size(), f) == buf.size()) {}
        std::fclose(f);
    }
}

// 点查分解（诊断）：用公开接口复刻 locate 的 run 段——依次查各 run 的 bloom 直到首个
// 命中（gen 降序），再在命中的 run 上 find（经视图共享块缓存）。分别给出 bloom 段、
// find 段、合计的 p50 / p99，以及平均探测 run 数 / 平均 find 次数（含假阳性）。
void decompose(const char* phase, bitcask::oki::OkiState& st, std::uint64_t n) {
    using Clock = std::chrono::steady_clock;
    using bitcask::oki::OkiRunReader;
    std::uniform_int_distribution<std::uint64_t> keyd(0, n - 1);
    auto v = st.make_read_view();
    std::vector<double> tb, tf, tt;
    tb.reserve(100000); tf.reserve(100000); tt.reserve(100000);
    std::uint64_t probes = 0, finds = 0;
    std::mt19937_64 q(7);
    for (int round = 0; round < 2; ++round) {  // 第 0 轮预热
        for (int i = 0; i < (round == 0 ? 20000 : 100000); ++i) {
            const auto key = key_of(keyd(q));
            const std::span<const std::byte> ks(
                reinterpret_cast<const std::byte*>(key.data()), key.size());
            const auto a = Clock::now();
            const auto bh = OkiRunReader::bloom_hash(ks);
            double bloom_us = 0, find_us = 0;
            auto t0 = a;
            for (std::size_t r = v->runs.size(); r-- > 0;) {
                const bool maybe = v->runs[r]->may_contain_hashed(bh);
                if (round) ++probes;
                if (!maybe) continue;
                const auto t1 = Clock::now();
                bloom_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
                auto e = v->runs[r]->find(ks, v->cache.get(), v->gens[r]);
                const auto t2 = Clock::now();
                find_us += std::chrono::duration<double, std::micro>(t2 - t1).count();
                if (round) ++finds;
                t0 = t2;
                if (e && e->has_value()) break;
            }
            const auto z = Clock::now();
            bloom_us += std::chrono::duration<double, std::micro>(z - t0).count();
            if (round) {
                tb.push_back(bloom_us);
                tf.push_back(find_us);
                tt.push_back(std::chrono::duration<double, std::micro>(z - a).count());
            }
        }
    }
    std::printf("decompose phase=%s runs=%zu bloom_p50=%.2f bloom_p99=%.2f find_p50=%.2f "
                "find_p99=%.2f total_p50=%.2f total_p99=%.2f probes_per_lookup=%.2f "
                "finds_per_lookup=%.3f\n",
                phase, v->runs.size(), pct(tb, 0.5), pct(tb, 0.99), pct(tf, 0.5),
                pct(tf, 0.99), pct(tt, 0.5), pct(tt, 0.99),
                static_cast<double>(probes) / 100000.0,
                static_cast<double>(finds) / 100000.0);
    std::fflush(stdout);
}

void measure(const char* phase, bitcask::oki::OkiState& st, std::uint64_t n) {
    using Clock = std::chrono::steady_clock;
    // 先让写盘余波（脏页回写）落定，再把 run 文件预热进页缓存。两个阶段同样处理。
    ::sync();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    warm_page_cache();
    if (g_decompose) {
        decompose(phase, st, n);
        return;
    }
    std::uniform_int_distribution<std::uint64_t> keyd(0, n - 1);

    // 点查。
    std::vector<double> lat;
    lat.reserve(100000);
    std::mt19937_64 q(7);
    for (int i = 0; i < 20000; ++i) (void)st.locate(key_of(keyd(q)));
    const auto bc0 = st.block_cache_stats();
    for (int i = 0; i < 100000; ++i) {
        const auto key = key_of(keyd(q));
        const auto a = Clock::now();
        (void)st.locate(key);
        lat.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    }
    const auto bc1 = st.block_cache_stats();
    const double blocks = static_cast<double>((bc1.hits - bc0.hits) +
                                              (bc1.misses - bc0.misses)) / 100000.0;
    const double misses = static_cast<double>(bc1.misses - bc0.misses) / 100000.0;
    const double p50 = pct(lat, 0.50), p99 = pct(lat, 0.99);
    if (g_locate_only) {
        std::printf("phase=%s runs=%zu locate_p50_us=%.2f locate_p99_us=%.2f\n", phase,
                    st.run_count(), p50, p99);
        return;
    }

    // 全量迭代 + 空间统计。
    auto v = st.make_read_view();
    std::uint64_t total_entries = 0;
    for (const auto& r : v->runs) total_entries += r->entry_count();
    for (const auto& seg : v->frozen) total_entries += seg->size();
    std::uint64_t live = 0, tomb_winners = 0;
    const auto s0 = Clock::now();
    {
        auto mc = bitcask::oki::OkiMergeCursor::open(*v);
        bitcask::oki::OkiState::DeltaRow row;
        while (true) {
            auto r = mc->next(row, /*include_tombs=*/true);
            if (!r || !*r) break;
            if (row.tomb) ++tomb_winners; else ++live;
        }
    }
    const double scan_s = std::chrono::duration<double>(Clock::now() - s0).count();
    const double scan_rps = static_cast<double>(live + tomb_winners) / scan_s;

    // 短区间扫描：随机起点取 100 个活 key。
    std::vector<double> rl;
    rl.reserve(5000);
    std::mt19937_64 rq(11);
    for (int i = 0; i < 5000; ++i) {
        const auto lo = key_of(keyd(rq));
        const auto a = Clock::now();
        auto mc = bitcask::oki::OkiMergeCursor::open(
            *v, std::span<const std::byte>(reinterpret_cast<const std::byte*>(lo.data()),
                                           lo.size()));
        bitcask::oki::OkiState::DeltaRow row;
        for (int k = 0; k < 100; ++k) {
            auto r = mc->next(row);
            if (!r || !*r) break;
        }
        rl.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    }
    const double r50 = pct(rl, 0.50), r99 = pct(rl, 0.99);

    std::printf(
        "phase=%s runs=%zu locate_p50_us=%.2f locate_p99_us=%.2f blocks_per_lookup=%.3f "
        "block_misses_per_lookup=%.3f scan_rows_per_s=%.0f range100_p50_us=%.1f "
        "range100_p99_us=%.1f run_entries=%llu live_keys=%llu tomb_winners=%llu "
        "space_amp=%.3f\n",
        phase, st.run_count(), p50, p99, blocks, misses, scan_rps, r50, r99,
        static_cast<unsigned long long>(total_entries),
        static_cast<unsigned long long>(live),
        static_cast<unsigned long long>(tomb_winners),
        live ? static_cast<double>(total_entries) / static_cast<double>(live) : 0.0);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <dir> <N> <F> <full|tiered> [K] [--del=x] [--then-full]\n",
                     argv[0]);
        return 2;
    }
    namespace fs = std::filesystem;
    const fs::path dir = argv[1];
    const std::uint64_t n = std::strtoull(argv[2], nullptr, 10);
    g_dir = dir.string();
    const std::uint64_t f = std::strtoull(argv[3], nullptr, 10);
    const std::string mode = argv[4];
    double k = 1.0, del = 0.0;
    bool then_full = false, keep = false;
    for (int i = 5; i < argc; ++i) {
        if (std::strncmp(argv[i], "--del=", 6) == 0) {
            del = std::strtod(argv[i] + 6, nullptr);
        } else if (std::strcmp(argv[i], "--then-full") == 0) {
            then_full = true;
        } else if (std::strcmp(argv[i], "--keep") == 0) {
            keep = true;
        } else if (std::strcmp(argv[i], "--locate-only") == 0) {
            g_locate_only = true;
        } else if (std::strcmp(argv[i], "--decompose") == 0) {
            g_decompose = true;
        } else {
            k = std::strtod(argv[i], nullptr);
        }
    }

    std::error_code ec;
    const auto force_full_and_measure = [&](bitcask::oki::OkiState& st,
                                            std::uint64_t trigger_ord) -> bool {
        // 同一份数据强制全归并：kFull + full_limit=1，再写一行触发一次 flush。
        bitcask::oki::CompactionPolicyOptions fp;
        fp.mode = bitcask::oki::CompactionMode::kFull;
        fp.full_limit = 1;
        st.set_compaction_policy(fp);
        const bitcask::oki::RowLoc loc{999, 64, 0, 1700000000};
        st.apply_put("zz-then-full-trigger", trigger_ord, loc);
        if (!st.flush(dir.string())) return false;
        st.wait_compaction();
        measure("postfull", st, n);
        return true;
    };
    if (mode == "measure") {
        bitcask::oki::OkiState st;
        st.load(dir.string());
        if (!st.loaded()) return 1;
        st.enable_point_query();
        measure("fresh-process", st, n);
        if (then_full && !force_full_and_measure(st, std::uint64_t{1} << 62)) return 1;
        return 0;
    }
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    bitcask::oki::CompactionPolicyOptions p;
    if (mode == "full") {
        p.mode = bitcask::oki::CompactionMode::kFull;
    } else {
        p.mode = bitcask::oki::CompactionMode::kTiered;
        p.k = k;
    }

    bitcask::oki::OkiState st;
    st.set_compaction_policy(p);
    if (!st.rebuild(dir.string(), {}, 0)) return 1;
    st.enable_point_query();

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint64_t> keyd(0, n - 1);
    std::uniform_real_distribution<double> opd(0.0, 1.0);
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t in_batch = 0, ord = 0;
    for (; ord < n; ++ord) {
        const auto key = key_of(keyd(rng));
        if (del > 0.0 && opd(rng) < del) {
            st.apply_remove(key, ord);
        } else {
            const bitcask::oki::RowLoc loc{1 + static_cast<std::uint32_t>(ord / 1000000),
                                           64, (ord % 1000000) * 64, 1700000000};
            st.apply_put(key, ord, loc);
        }
        if (++in_batch == f) {
            if (!st.flush(dir.string())) return 1;
            in_batch = 0;
        }
    }
    if (in_batch != 0 && !st.flush(dir.string())) return 1;
    st.wait_compaction();
    const double wall = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0).count();

    const auto cs = st.compaction_stats();
    std::printf(
        "mode=%s K=%.1f del=%.2f N=%llu F=%llu runs=%zu flushes=%llu flushed_rows=%llu "
        "compaction_rows=%llu write_amp=%.2f compactions=%llu full=%llu young=%llu "
        "backpressure_waits=%llu compaction_s=%.2f wall_s=%.2f\n",
        mode.c_str(), k, del, static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(f), st.run_count(),
        static_cast<unsigned long long>(cs.flushes),
        static_cast<unsigned long long>(cs.flushed_rows),
        static_cast<unsigned long long>(cs.rows_written),
        cs.flushed_rows ? static_cast<double>(cs.rows_written) /
                              static_cast<double>(cs.flushed_rows)
                        : 0.0,
        static_cast<unsigned long long>(cs.compactions),
        static_cast<unsigned long long>(cs.full_compactions),
        static_cast<unsigned long long>(cs.young_compactions),
        static_cast<unsigned long long>(cs.backpressure_waits),
        static_cast<double>(cs.compaction_ns) / 1e9, wall);
    std::fflush(stdout);
    measure("main", st, n);

    if (then_full && !force_full_and_measure(st, ord)) return 1;
    if (!keep) fs::remove_all(dir, ec);
    return 0;
}
