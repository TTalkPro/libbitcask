// bitcask/seq_shard_table.hpp — seqlock 原生开放寻址哈希表（S29-6 P3）。
//
// 为什么不用 ankerl::unordered_dense:乐观读者需要「逐跳 copy → seq 验证 →
// 使用」配方保证任何交错下 deref 恒 in-bounds 且落在存活内存(设计
// docs/design/s29-6-keydir-lockfree-read.md §6.2)——这要求桶数组指针与掩码
// 天然一致(self-describing 块)+ 全部内部释放走 epoch limbo,ankerl 内部
// private 且是 submodule,无法健全实现(§6.3,评审选 A)。
//
// === 结构 ===
//   values_  : 稠密 pair<std::string, T> 数组(std::vector,LimboAllocator
//              ——grow 的旧数组 retire 进 limbo,不立即 free)。
//   buckets_ : 原子指针 → self-describing 桶块 {mask; Bucket[]},Bucket =
//              {frag(hash 低 32 位), idx1(values 下标+1,0=空)}。线性探测 +
//              backward-shift 删除(无墓碑桶)。块替换时旧块 retire 进 limbo。
//   seq_     : 表级 seqlock。**所有**结构/值变更(含 caller 经 find 指针的
//              就地改写——用 write_section() RAII 包住)在 odd/even 窗口内。
//   limbo    : 表自持的延迟回收池(raw 块 / key 遗骸 / 值遗骸)。erase 恒零
//              free:key/值先 move 进 limbo,swap-with-last 只搬指针。
//
// === 并发契约 ===
//   写者:外部互斥(KeyDir 分片锁),表内所有变更方法自动 bump seq。
//   乐观读者:try_get_optimistic()——不持锁;进入前必须已在
//     epoch::Registry 注册槽位(存活性依据:注册后 retire 的 stamp ≥ 读者
//     epoch ⇒ limbo 不回收 ⇒ 已 copy 的指针 deref 恒安全)。
//   内存序:配方的正确性论证基于 x86-TSO(写者 seq++ 先于其后变更可见;
//     读者按程序序 load)。seq 全用 acquire/release 原子,数据面是普通
//     load/store——按评审决议 ①,读函数标 no_sanitize("thread")(seqlock
//     业界标准;写者侧不豁免,TSan 照常查)。
//   加锁读者(回退路径/迭代/快照):与写者同锁,普通语义。
//
// 线程安全:见上;单写者多读者。

#pragma once

#include "bitcask/epoch_reclaim.hpp"
#include "bitcask/detail/cpu_features.hpp"  // S37-3.b：BITCASK_NO_SANITIZE
#include "bitcask/string_hash.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>  // zero_buckets 的 is_trivially_copyable_v 断言
#include <utility>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>   // S37-4：__iso_volatile_load64（见 opt_copy_bytes）
#endif

namespace bitcask::detail {

template <class T>
class SeqShardTable {
public:
    using Pair = std::pair<std::string, T>;
    using iterator = Pair*;
    using const_iterator = const Pair*;

    enum class OptResult { kHit, kMiss, kRetry };

    SeqShardTable() : values_(ValueAlloc{&limbo_raw_}) {}
    ~SeqShardTable() {
        destroy_buckets();
        limbo_drain();
    }
    SeqShardTable(const SeqShardTable&) = delete;
    SeqShardTable& operator=(const SeqShardTable&) = delete;

    // ---- 容量/迭代(caller 持锁) ----
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    // S33-1 诊断:稠密数组容量与桶块槽数(keydir 内存估算探针用)。
    [[nodiscard]] std::size_t values_capacity() const noexcept {
        return values_.capacity();
    }
    [[nodiscard]] std::size_t bucket_count() const noexcept {
        const BucketBlock* bb = buckets_.load(std::memory_order_relaxed);
        return bb != nullptr ? bb->count() : 0;
    }
    [[nodiscard]] iterator begin() noexcept { return values_.data(); }
    [[nodiscard]] iterator end() noexcept { return values_.data() + values_.size(); }
    [[nodiscard]] const_iterator begin() const noexcept { return values_.data(); }
    [[nodiscard]] const_iterator end() const noexcept {
        return values_.data() + values_.size();
    }

    void reserve(std::size_t n) {
        WriteSection ws(*this);
        values_.reserve(n);
        grow_buckets_to(bucket_need(n));
    }

    void clear() {
        WriteSection ws(*this);
        // 遗骸化整表(与 erase 同款零 free 语义;调用方是 load 失败复位等
        // 冷路径,批量 retire 可接受)。
        for (auto& kv : values_) {
            limbo_keys_.emplace_back(stamp(), std::move(kv.first));
            limbo_vals_.emplace_back(stamp(), std::move(kv.second));
        }
        values_.clear();
        if (BucketBlock* bb = buckets_.load(std::memory_order_relaxed)) {
            zero_buckets(bb->b, bb->count());
        }
    }

    // ---- 查找(caller 持锁;写者/回退读者共用) ----
    [[nodiscard]] iterator find(std::string_view key) noexcept {
        const std::uint64_t h = StringHash{}(key);
        BucketBlock* bb = buckets_.load(std::memory_order_relaxed);
        if (bb == nullptr) return end();
        const std::uint64_t mask = bb->mask;
        const auto frag = static_cast<std::uint32_t>(h);
        std::uint64_t i = h & mask;
        while (true) {
            const Bucket& bk = bb->b[i];
            if (bk.idx1 == 0) return end();
            if (bk.frag == frag) {
                Pair& p = values_[bk.idx1 - 1];
                if (p.first == key) return &p;
            }
            i = (i + 1) & mask;
        }
    }
    [[nodiscard]] const_iterator find(std::string_view key) const noexcept {
        return const_cast<SeqShardTable*>(this)->find(key);
    }

    // ---- 写入(caller 持锁) ----
    // 语义对齐 std::unordered_map::insert_or_assign / emplace(存在则不动)。
    template <class K, class V>
    void insert_or_assign(K&& key, V&& value) {
        WriteSection ws(*this);
        if (iterator it = find(key); it != end()) {
            it->second = std::forward<V>(value);
            return;
        }
        insert_new(std::forward<K>(key), std::forward<V>(value));
    }
    template <class K, class V>
    void emplace(K&& key, V&& value) {
        WriteSection ws(*this);
        if (find(key) != end()) return;
        insert_new(std::forward<K>(key), std::forward<V>(value));
    }

    // 删除:key/值遗骸 move 进 limbo(erase 全程零 free——swap-with-last 的
    // move 只搬指针,被摧毁的都是 moved-from 空壳),桶走 backward-shift。
    // 返回原位置(swap-with-last 后换入的新元素,匹配 `it = erase(it)` 用法)。
    iterator erase(iterator pos) {
        WriteSection ws(*this);
        assert(pos >= begin() && pos < end());
        const auto idx = static_cast<std::size_t>(pos - values_.data());
        const std::uint64_t h = StringHash{}(pos->first);

        limbo_keys_.emplace_back(stamp(), std::move(pos->first));
        limbo_vals_.emplace_back(stamp(), std::move(pos->second));

        remove_bucket(h, static_cast<std::uint32_t>(idx + 1));

        const std::size_t last = values_.size() - 1;
        if (idx != last) {
            values_[idx] = std::move(values_[last]);
            // 修正被搬元素的桶指向:new idx。
            rewire_bucket(StringHash{}(values_[idx].first),
                          static_cast<std::uint32_t>(last + 1),
                          static_cast<std::uint32_t>(idx + 1));
        }
        values_.pop_back();
        return pos;
    }

    // caller 经 find 指针就地改写值(put_overwrite/墓碑覆写/链升级)时,
    // 必须用本 RAII 包住——否则乐观读者读到撕裂数据却校验通过。
    //
    // ⚠️ 深度感知,只有最外层 bump:嵌套 section(caller 包住表方法,表方法
    // 再自开 section)若各自 +1,内层期间 seq = 外+内 = **偶**——读者校验
    // 形同虚设,撕裂读被当权威结果(压力测试实测:0.03% 窗口伪 miss)。
    // depth 为普通成员——写者被外部互斥锁串行,无并发。
    // 内存序:odd-bump 后置 release fence(阻止变更写上浮到标记可见之前——
    // release RMW 是单向屏障,挡不住后面的写前移);even-bump 前置 release
    // fence(变更写全部落定才摘标记)。
    class [[nodiscard]] WriteSection {
    public:
        explicit WriteSection(SeqShardTable& t) noexcept : t_(t) {
            if (t_.write_depth_++ == 0) {
                t_.seq_.fetch_add(1, std::memory_order_relaxed);  // → odd
                std::atomic_thread_fence(std::memory_order_release);
            }
        }
        ~WriteSection() {
            if (--t_.write_depth_ == 0) {
                std::atomic_thread_fence(std::memory_order_release);
                t_.seq_.fetch_add(1, std::memory_order_relaxed);  // → even
            }
        }
        WriteSection(const WriteSection&) = delete;
        WriteSection& operator=(const WriteSection&) = delete;

    private:
        SeqShardTable& t_;
    };
    [[nodiscard]] WriteSection write_section() noexcept {
        return WriteSection(*this);
    }

    // ---- 乐观读(不持锁;评审决议 ① TSan 豁免——论证见文件头/设计 §6.2) ----
    // 前置:caller 已在 epoch::Registry 注册活跃槽位。命中时把 T 的原始字节
    // 拷入 out(caller 负责按可乐观消费的判别解释——KeyDir 只认 Single
    // POD,其余回退加锁);kMiss 为权威不存在;kRetry = 撕裂/写者活跃。
#if defined(__clang__) || defined(__GNUC__)
    BITCASK_NO_SANITIZE("thread")
#endif
    OptResult try_get_optimistic(std::string_view key,
                                 std::byte* out) const noexcept {
        const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) return OptResult::kRetry;

        // 跳 1:桶块 + values 快照。bb->mask 与块自洽(self-describing);
        // (vdata, vsize) 经 seq 验证后与 bb 同世代。
        const BucketBlock* bb = buckets_.load(std::memory_order_acquire);
        const Pair* vdata = values_.data();
        const std::size_t vsize = values_.size();
        if (seq_changed(s1)) return OptResult::kRetry;
        if (bb == nullptr) return OptResult::kMiss;  // 空表(见上验证)
        const std::uint64_t mask = bb->mask;

        const std::uint64_t h = StringHash{}(key);
        const auto frag = static_cast<std::uint32_t>(h);
        std::uint64_t i = h & mask;
        for (std::uint64_t probes = 0; probes <= mask; ++probes) {
            // 跳 2:桶项拷贝 + 验证(idx1 与快照同世代 → in-bounds)。
            Bucket bk;  // 恒 in-bounds:i ≤ bb->mask
            static_assert(sizeof(Bucket) == 8);
            opt_copy_bytes(&bk, &bb->b[i], sizeof(Bucket));
            if (seq_changed(s1)) {
                return OptResult::kRetry;
            }
            if (bk.idx1 == 0) return OptResult::kMiss;
            if (bk.frag == frag) {
                const std::uint64_t idx = bk.idx1 - 1;
                if (idx >= vsize) return OptResult::kRetry;  // 混代兜底
                const Pair& p = vdata[idx];

                // 跳 3:key 对象字节拷贝 + 验证 → 拷贝未撕裂 → 才可按其
                // 内容 deref(缓冲存活:limbo + 读者活跃槽)。
                alignas(std::string) std::byte kraw[sizeof(std::string)];
                static_assert(sizeof(std::string) % 8 == 0);
                opt_copy_bytes(kraw, &p.first, sizeof(std::string));
                if (seq_changed(s1)) {
                    return OptResult::kRetry;
                }
                const auto* kobj = reinterpret_cast<const std::string*>(kraw);
                const bool key_eq =
                    kobj->size() == key.size() &&
                    opt_bytes_equal(kobj->data(), key.data(), key.size());
                // ⚠️ SSO 陷阱:拷贝对象字节后 data() 对短串仍指向**原位**
                // 内部缓冲(拷贝的是指针语义,不是内容)——比较输入可能被
                // 写者的 swap-move 弄脏。**mismatch 也必须验证**:否则把
                // 撕裂误判为「不同 key」→ continue 到空桶 → 伪 miss
                // (压力测试 ConcurrentGetPutRemoveGrowStress 实测复现)。
                if (seq_changed(s1)) {
                    return OptResult::kRetry;
                }
                if (key_eq) {
                    // 跳 4:值字节拷贝 + 终验。
                    static_assert(sizeof(T) % 8 == 0 && alignof(T) >= 8);
                    opt_copy_bytes(out, &p.second, sizeof(T));
                    if (seq_changed(s1)) {
                        return OptResult::kRetry;
                    }
                    return OptResult::kHit;
                }
                // 已验证的真 mismatch(frag 碰撞)→ 继续探测。
            }
            i = (i + 1) & mask;
        }
        return OptResult::kMiss;  // 全表扫完(理论不可达:恒有空桶)
    }

    // ---- limbo(caller 持锁;stamp 全局单调 → 前缀回收) ----
    [[nodiscard]] std::size_t limbo_items() const noexcept {
        return limbo_raw_.items.size() + limbo_keys_.size() + limbo_vals_.size();
    }
    void limbo_reclaim(std::uint64_t safe) noexcept {
        auto prefix = [safe](const auto& v) {
            std::size_t n = 0;
            while (n < v.size() && v[n].first < safe) ++n;
            return n;
        };
        auto& raw = limbo_raw_.items;
        if (const auto n = prefix(raw); n > 0) {
            for (std::size_t i = 0; i < n; ++i) ::operator delete(raw[i].second);
            raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (const auto n = prefix(limbo_keys_); n > 0) {
            limbo_keys_.erase(limbo_keys_.begin(),
                              limbo_keys_.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (const auto n = prefix(limbo_vals_); n > 0) {
            limbo_vals_.erase(limbo_vals_.begin(),
                              limbo_vals_.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    void limbo_drain() noexcept {
        limbo_reclaim(std::numeric_limits<std::uint64_t>::max());
    }

private:
    struct Bucket {
        std::uint32_t frag = 0;  // hash 低 32 位(home = frag & mask)
        std::uint32_t idx1 = 0;  // values 下标 + 1;0 = 空
    };

    // 批量清零桶数组。桶块是手工 ::operator new 的变长块,从不逐个默认构造,
    // 所以清零走 memset(热路径:grow 每次都要清整块)。
    //
    // 直接 `std::memset(b, 0, n * sizeof(Bucket))` 会触发 GCC 的
    // `-Wclass-memaccess`(CI 的 werror-lib 带 -Werror):Bucket 因为带**默认成员
    // 初始化器**(`= 0`)而不是「trivially default constructible」,编译器据此提醒
    // 「别拿 memset 清非平凡类型」。但这里真正需要的性质是**平凡可复制**——
    // 下面的 static_assert 把它钉死,不成立时编译期就炸,而不是靠这段注释。
    // 满足该性质时全零字节就是合法的 Bucket 表示(frag=0/idx1=0 = 空桶),
    // memset 正确。转 void* 是消除该告警的标准写法,不改任何生成代码。
    static void zero_buckets(Bucket* b, std::size_t n) noexcept {
        static_assert(std::is_trivially_copyable_v<Bucket>,
                      "zero_buckets 依赖 Bucket 平凡可复制(全零字节 = 空桶)");
        std::memset(static_cast<void*>(b), 0, n * sizeof(Bucket));
    }
    // self-describing 桶块:mask 内嵌于块——乐观读者拿到块指针即拿到与之
    // 恒一致的界,探测 deref 无混代可能。
    struct BucketBlock {
        std::uint64_t mask;
        Bucket b[1];  // 变长(手工分配)
        [[nodiscard]] std::size_t count() const noexcept { return mask + 1; }
    };

    // raw 块 limbo(供 LimboAllocator 与桶块替换共用)。
    // 自带析构释放:values_ 的**最终**数组在其 vector 析构时才 retire 进来
    // (晚于 ~SeqShardTable 函数体)——靠声明序(limbo_raw_ 先声明 → 最后
    // 析构)兜底,否则泄漏(ASan/LSan 实测抓到)。
    struct RawLimbo {
        std::vector<std::pair<std::uint64_t, void*>> items;
        ~RawLimbo() {
            for (auto& [st, p] : items) ::operator delete(p);
        }
    };
    template <class U>
    struct LimboAllocator {
        using value_type = U;
        RawLimbo* limbo = nullptr;
        LimboAllocator() noexcept = default;
        explicit LimboAllocator(RawLimbo* l) noexcept : limbo(l) {}
        template <class W>
        LimboAllocator(const LimboAllocator<W>& o) noexcept : limbo(o.limbo) {}
        [[nodiscard]] U* allocate(std::size_t n) {
            return static_cast<U*>(::operator new(n * sizeof(U)));
        }
        void deallocate(U* p, std::size_t) noexcept {
            if (limbo != nullptr) {
                limbo->items.emplace_back(
                    epoch::Registry::instance().advance(), p);
            } else {
                ::operator delete(p);
            }
        }
        template <class W>
        bool operator==(const LimboAllocator<W>& o) const noexcept {
            return limbo == o.limbo;
        }
    };
    using ValueAlloc = LimboAllocator<Pair>;

    [[nodiscard]] static std::uint64_t stamp() noexcept {
        return epoch::Registry::instance().advance();
    }

    // seqlock 读者校验。⚠️ 必须 acquire **fence**——acquire load 只挡「后面
    // 的访问前移」,不挡「前面的普通载入下沉到校验之后」(编译器可合法把
    // 数据读移到 seq 重读后,校验即形同虚设——压力测试实测复现伪 miss)。
    // fence 建立 此前全部载入 ≺ 此后 seq 载入 的顺序(Boehm seqlock 论文
    // 的标准读者形态)。
    [[nodiscard]] bool seq_changed(std::uint64_t s1) const noexcept {
        std::atomic_thread_fence(std::memory_order_acquire);
        return seq_.load(std::memory_order_relaxed) != s1;
    }

    // 乐观路径专用拷贝/比较。**不得用 std::memcpy/memcmp**:它们是真实
    // libc 调用,被 TSan 拦截器记录访问——函数级 no_sanitize 只压制编译器
    // 插桩,压不住拦截器(TSan 树实测报 race)。volatile 逐字装载同时阻止
    // 编译器把循环聚合回 memcpy libcall(loop-idiom 识别)。
#if defined(__clang__) || defined(__GNUC__)
    BITCASK_NO_SANITIZE("thread")  // 独立函数,须自带豁免(非内联时
                                            // 不继承 caller 的豁免——TSan 实测)
#endif
    // 逐 8 字节 __atomic_load_n(relaxed):① TBAA 豁免——曾用 uint64_t*
    // 直读,严格别名违规,GCC -O2 判定与 Bucket/string 的写不别名读到陈旧
    // 零值(单线程 100% 伪 miss,clang 恰好宽容);② 编译为普通 mov,无
    // libcall/无拦截器;③ TSan 原生理解原子。前置:两侧 8 对齐、bytes%8==0
    // (调用点 static_assert)。relaxed 足够——序由 seq_changed 的 fence 背书。
    // (曾用 volatile 逐字节:正确但 Get 热路径 +30ns、长 key 比较 3×。)
    static inline void opt_copy_bytes(void* dst, const void* src,
                                      std::size_t bytes) noexcept {
        auto* d = static_cast<std::uint64_t*>(dst);
        const auto* s = static_cast<const std::uint64_t*>(src);
        for (std::size_t i = 0; i < bytes / 8; ++i) {
#if defined(_MSC_VER) && !defined(__clang__)
            // S37-4：`__iso_volatile_load64` 是 MSVC 侧与
            // `__atomic_load_n(..., __ATOMIC_RELAXED)` 逐条对应的等价物：
            // 单条 mov、无 libcall、**且不参与 loop-idiom 识别**（后者会把
            // 本循环聚合回 memcpy，正是上面注释要避免的）。
            // 用它而非普通 `volatile`：x64 默认 /volatile:ms 会给 volatile
            // 读加上 acquire 语义，比这里需要的 relaxed 更强；`__iso_` 前缀
            // 的这一族恰恰是「ISO volatile 语义、不附加序」。
            // TBAA 在此不是问题——MSVC 不做基于类型的别名分析（GCC 侧的
            // 严格别名违规才是 __atomic_load_n 的首要理由，见上）。
            // TSan 一项不适用：MSVC 无 TSan（设计稿 §5.3）。
            static_assert(sizeof(long long) == sizeof(std::uint64_t));
            d[i] = static_cast<std::uint64_t>(__iso_volatile_load64(
                reinterpret_cast<const volatile long long*>(s + i)));
#else
            d[i] = __atomic_load_n(s + i, __ATOMIC_RELAXED);
#endif
        }
    }
    [[nodiscard]]
#if defined(__clang__) || defined(__GNUC__)
    BITCASK_NO_SANITIZE("thread")
#endif
    static inline bool opt_bytes_equal(const void* shared,
                                       const void* own,
                                       std::size_t n) noexcept {
        // 8 字块比较:定长 8 的 memcpy → 单条(非对齐)mov,别名豁免、无 libcall
        // (std::mem**cmp** 是真实 libc 调用,会被 TSan 拦截器记录,函数级
        // no_sanitize 压不住——实测报 race)。shared 侧可能是脏数据,
        // 结果由 caller 的 seq 校验背书。
        //
        // S37-3.b 把这里的 `__builtin_memcpy` 换成了 `std::memcpy`(MSVC 无
        // `__builtin_memcpy`)。看着像是违反了上一段的禁令,**实测不是**:
        // 禁的是 `memcmp`/变长 `memcpy` 那种真会落成 libcall 的形态,而
        // **定长 8** 的 `std::memcpy` 两个编译器都直接内联。实测(2026-08-08)
        // g++ 14.2 / clang 均在 -O0 与 -O2 下 libcall 数为 0,且与
        // `__builtin_memcpy` 版的指令序列逐条一致(仅标签名与一处调度差异)。
        // 换言之此处**不是** TSan 拦截器的暴露点——别改回去,也别顺手把下面
        // 的循环"简化"成 `std::memcmp`,那才会真的触发上一段说的问题。
        const auto* x = static_cast<const unsigned char*>(shared);
        const auto* y = static_cast<const unsigned char*>(own);
        std::uint64_t acc = 0;
        std::size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            std::uint64_t a;
            std::uint64_t b;
            std::memcpy(&a, x + i, 8);
            std::memcpy(&b, y + i, 8);
            acc |= a ^ b;
        }
        for (; i < n; ++i) {
            acc |= static_cast<std::uint64_t>(x[i] ^ y[i]);
        }
        return acc == 0;
    }

    [[nodiscard]] static std::size_t bucket_need(std::size_t n) noexcept {
        // 负载因子 0.75:need ≥ n/0.75,取 ≥64 的 pow2。
        std::size_t cap = 64;
        while (cap * 3 < n * 4) cap <<= 1;
        return cap;
    }

    void destroy_buckets() noexcept {
        if (BucketBlock* bb = buckets_.load(std::memory_order_relaxed)) {
            ::operator delete(bb);
            buckets_.store(nullptr, std::memory_order_relaxed);
        }
    }

    // 扩桶到 cap(pow2)并按 values_ 重建。旧块 retire 进 limbo。
    void grow_buckets_to(std::size_t cap) {
        BucketBlock* old = buckets_.load(std::memory_order_relaxed);
        if (old != nullptr && old->count() >= cap) return;
        const std::size_t bytes =
            sizeof(BucketBlock) + (cap - 1) * sizeof(Bucket);
        auto* nb = static_cast<BucketBlock*>(::operator new(bytes));
        nb->mask = cap - 1;
        zero_buckets(nb->b, cap);
        for (std::size_t v = 0; v < values_.size(); ++v) {
            place_bucket(nb, StringHash{}(values_[v].first),
                         static_cast<std::uint32_t>(v + 1));
        }
        buckets_.store(nb, std::memory_order_release);
        if (old != nullptr) {
            limbo_raw_.items.emplace_back(stamp(), old);
        }
    }

    static void place_bucket(BucketBlock* bb, std::uint64_t h,
                             std::uint32_t idx1) noexcept {
        const std::uint64_t mask = bb->mask;
        std::uint64_t i = h & mask;
        while (bb->b[i].idx1 != 0) i = (i + 1) & mask;
        bb->b[i] = Bucket{static_cast<std::uint32_t>(h), idx1};
    }

    template <class K, class V>
    void insert_new(K&& key, V&& value) {
        grow_buckets_to(bucket_need(values_.size() + 1));
        values_.emplace_back(std::string(std::forward<K>(key)),
                             std::forward<V>(value));
        place_bucket(buckets_.load(std::memory_order_relaxed),
                     StringHash{}(values_.back().first),
                     static_cast<std::uint32_t>(values_.size()));
    }

    // home ∈ (hole, j] (循环意义)?——backward-shift 的「不可回填」判据。
    [[nodiscard]] static bool in_cyclic_range(std::uint64_t home,
                                              std::uint64_t hole,
                                              std::uint64_t j) noexcept {
        if (hole < j) return home > hole && home <= j;
        return home > hole || home <= j;
    }

    // 删除指向 idx1 的桶项 + backward-shift 补洞(无墓碑桶)。
    void remove_bucket(std::uint64_t h, std::uint32_t idx1) noexcept {
        BucketBlock* bb = buckets_.load(std::memory_order_relaxed);
        const std::uint64_t mask = bb->mask;
        std::uint64_t i = h & mask;
        while (bb->b[i].idx1 != idx1) i = (i + 1) & mask;
        std::uint64_t hole = i;
        std::uint64_t j = (i + 1) & mask;
        while (bb->b[j].idx1 != 0) {
            const std::uint64_t home = bb->b[j].frag & mask;
            if (!in_cyclic_range(home, hole, j)) {
                bb->b[hole] = bb->b[j];
                hole = j;
            }
            j = (j + 1) & mask;
        }
        bb->b[hole] = Bucket{};
    }

    // 把指向 old_idx1 的桶项改指 new_idx1(swap-with-last 修正)。
    void rewire_bucket(std::uint64_t h, std::uint32_t old_idx1,
                       std::uint32_t new_idx1) noexcept {
        BucketBlock* bb = buckets_.load(std::memory_order_relaxed);
        const std::uint64_t mask = bb->mask;
        std::uint64_t i = h & mask;
        while (bb->b[i].idx1 != old_idx1) i = (i + 1) & mask;
        bb->b[i].idx1 = new_idx1;
    }

    // ---- 成员(limbo 声明先于 values_:map 析构的 deallocate 仍安全) ----
    RawLimbo limbo_raw_;
    std::vector<std::pair<std::uint64_t, std::string>> limbo_keys_;
    std::vector<std::pair<std::uint64_t, T>> limbo_vals_;
    std::vector<Pair, ValueAlloc> values_;
    std::atomic<BucketBlock*> buckets_{nullptr};
    std::atomic<std::uint64_t> seq_{0};
    std::size_t write_depth_ = 0;  // WriteSection 嵌套深度(写者串行,普通成员)
};

}  // namespace bitcask::detail
