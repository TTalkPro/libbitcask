// crash_child — 崩溃注入测试的子进程骨架（S37-2）。
//
// === 为什么不是 fork ===
// 崩溃恢复测试（WAL / torn tail / 墓碑复活门 / 原子批区间提交 / 退休队列）
// 靠「子进程干到一半直接 _exit，不 close、不释放写锁」制造真实的进程崩溃态。
// 原实现用 fork()——**Windows 没有 fork**，且这批测试恰是全套里最有价值的
// 一部分，不能在移植中丢掉。
//
// 改为 exec-self：子进程 = 重新启动测试二进制自身 + `--bitcask-crash-child=<场景>`
// 参数分发。父进程 posix_spawn（Windows 侧换 CreateProcess）+ 等待退出码。
//
// === 与 fork 的语义差异（改造时逐个 case 核对过）===
// exec-self 下子进程**不再继承父进程的内存状态**（已建好的 keydir、已打开
// 的句柄、fixture 成员）。本仓库的 6 个崩溃场景全部形如
//   「Cask::open(dir) → 干活 → _exit(N)」
// ——唯一输入是目录路径，无一依赖 fork 时刻的内存快照，故改造是等价的。
// **新增崩溃场景若需要父进程的内存状态，本骨架不适用**，须另想办法
// （通常意味着该状态应当先落盘，那本身就是更好的测试设计）。
//
// === ⚠️ 场景函数必须用 crash_exit()，不能 return ===
// 这是改造中唯一真正咬人的地方，值得写清楚。
//
// 原 fork 版在作用域**内**调 `_exit(N)`：进程立刻消失，栈不展开，局部
// `Cask` 的析构**从不执行**——这正是崩溃态的来源（不 close ⇒ 不 flush OKI、
// 不写 keydir 快照、不释放 bitcask.write.lock）。
//
// 而场景函数里的 `return N` 会正常展开栈，局部 `Cask` 析构 → close() →
// **OKI flush + 写锁释放**，注入的崩溃态当场消失。
//
// 后果不是「测试失败」而是「测试静默失效」：改造初版用 return 时，6 个场景
// 里有 4 个照样 PASS——它们的断言在「干净关闭」下同样成立，于是不声不响地
// 不再测崩溃路径了。只有 2 个（OKI tail 重放 / B1 不变量）因为断言了「必须
// 存在未固化的尾巴」才暴露出来。**这类沉默失效比红色失败危险得多。**
//
// 故：场景函数返回类型为 void，退出一律经 crash_exit()；若函数体正常走到
// 结尾（忘了调），骨架会带诊断 abort，而不是当作成功。
//
// === 用法 ===
//   BITCASK_CRASH_SCENARIO(my_scenario) {
//       auto c = Cask::open(dir, opts, &test_registry());
//       if (!c) crash_exit(1);
//       ...
//       crash_exit(0);   // 崩溃点：不 close，句柄/写锁随进程消失
//   }
//
//   TEST_F(MyTest, Foo) {
//       ASSERT_EQ(bitcask::test::spawn_crash_child("my_scenario", dir_.string()),
//                 0);
//       // …父进程重开目录做断言…
//   }
//
// 场景函数体在**子进程**里跑，不得用 gtest 断言宏（ASSERT_*/EXPECT_* 在非
// 测试上下文无意义，且失败不会传回父进程）——一律用退出码表达失败，父进程
// 侧断言退出码。诊断信息 fprintf(stderr) 即可（子进程共用父进程的 stderr）。

#ifndef BITCASK_TESTS_SUPPORT_CRASH_CHILD_HPP
#define BITCASK_TESTS_SUPPORT_CRASH_CHILD_HPP

#include <string>
#include <string_view>

namespace bitcask::test {

// 崩溃点：立刻终止进程，**不展开栈、不跑任何析构**——与原 fork 版作用域内
// 的 _exit(N) 逐字等价。见本文件顶部的告警。
[[noreturn]] void crash_exit(int code);

// 场景函数：接目录路径。**不返回**（经 crash_exit 退出）。
using CrashScenarioFn = void (*)(const std::string& dir);

// 注册一个场景。由 BITCASK_CRASH_SCENARIO 宏在静态初始化期调用。
// 重名注册会 abort（拼写错误比静默覆盖更好发现）。
void register_crash_scenario(std::string_view name, CrashScenarioFn fn);

// 由 crash_main.cpp 的 main() 在 InitGoogleTest **之前**调用。
// 若本进程是崩溃场景子进程，则跑完场景后经 crash_exit 直接退出（**不返回**）；
// 否则返回 false，调用方继续走正常 gtest 流程。
bool run_as_crash_child(int argc, char** argv);

// 父进程侧：spawn 本测试二进制自身跑指定场景，阻塞至其退出。
// 返回值：
//   >= 0  子进程退出码
//   -1    spawn 或 wait 失败（测试应 ASSERT 住）
//   -2    子进程被信号杀死 / 异常终止
[[nodiscard]] int spawn_crash_child(std::string_view scenario,
                                    const std::string& dir);

// main() 启动时记录可执行文件路径（spawn 需要）。必须在任何 chdir 之前调用。
void record_executable_path(const char* argv0);

}  // namespace bitcask::test

// 定义并注册一个崩溃场景。函数体内可用形参 `dir`（const std::string&）。
#define BITCASK_CRASH_SCENARIO(name)                                          \
    static void bitcask_crash_scenario_##name(const std::string& dir);       \
    namespace {                                                               \
    struct BitcaskCrashReg_##name {                                           \
        BitcaskCrashReg_##name() {                                            \
            ::bitcask::test::register_crash_scenario(                         \
                #name, &bitcask_crash_scenario_##name);                       \
        }                                                                     \
    };                                                                        \
    const BitcaskCrashReg_##name bitcask_crash_reg_##name{};                  \
    }                                                                         \
    static void bitcask_crash_scenario_##name([[maybe_unused]]                \
                                              const std::string& dir)

#endif  // BITCASK_TESTS_SUPPORT_CRASH_CHILD_HPP
