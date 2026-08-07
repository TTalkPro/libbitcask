// crash_child 实现（S37-2）。契约见 crash_child.hpp。

#include "crash_child.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#endif

extern char** environ;

namespace bitcask::test {

namespace {

constexpr std::string_view kScenarioFlag = "--bitcask-crash-child=";
constexpr std::string_view kDirFlag      = "--bitcask-crash-dir=";

// 函数局部静态：避免与各 TU 里 BITCASK_CRASH_SCENARIO 的静态注册器之间
// 出现静态初始化顺序问题（注册器构造时本表按需建立）。
std::map<std::string, CrashScenarioFn, std::less<>>& registry() {
    static std::map<std::string, CrashScenarioFn, std::less<>> r;
    return r;
}

std::string& executable_path() {
    static std::string p;
    return p;
}

// 取 argv 里某个 "--flag=value" 形式参数的 value；缺失返回 nullopt 语义的空。
bool find_flag(int argc, char** argv, std::string_view prefix,
               std::string* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a.substr(0, prefix.size()) == prefix) {
            *out = std::string(a.substr(prefix.size()));
            return true;
        }
    }
    return false;
}

}  // namespace

void register_crash_scenario(std::string_view name, CrashScenarioFn fn) {
    auto [it, inserted] = registry().emplace(std::string(name), fn);
    if (!inserted) {
        std::fprintf(stderr,
                     "crash_child: 场景重名 '%.*s'——静默覆盖会让测试跑错剧本\n",
                     static_cast<int>(name.size()), name.data());
        std::abort();
    }
}

void record_executable_path(const char* argv0) {
    // 立刻绝对化：ctest 可能以相对路径启动，而 spawn 发生在若干测试之后；
    // 中途一旦有 chdir，相对路径就失效了。
    std::error_code ec;
    auto abs = std::filesystem::absolute(argv0 != nullptr ? argv0 : "", ec);
    executable_path() = ec ? std::string(argv0 != nullptr ? argv0 : "")
                           : abs.string();
}

void crash_exit(int code) {
    // 只 flush 诊断输出（子进程 stderr 与父进程共用），随后 _Exit——
    // 不展开栈、不跑静态/局部析构、不跑 atexit。崩溃态由此保真。
    std::fflush(nullptr);
    std::_Exit(code);
}

bool run_as_crash_child(int argc, char** argv) {
    std::string scenario;
    if (!find_flag(argc, argv, kScenarioFlag, &scenario)) return false;

    std::string dir;
    if (!find_flag(argc, argv, kDirFlag, &dir)) {
        std::fprintf(stderr, "crash_child: 缺 %s\n", kDirFlag.data());
        crash_exit(120);
    }
    const auto it = registry().find(scenario);
    if (it == registry().end()) {
        std::fprintf(stderr, "crash_child: 未注册的场景 '%s'\n",
                     scenario.c_str());
        crash_exit(121);
    }
    it->second(dir);
    // 走到这里 = 场景函数正常返回而没调 crash_exit ⇒ 栈已展开、Cask 已析构、
    // 写锁已释放，注入的崩溃态早没了。**决不能当成功**——否则测试会静默
    // 失效（见 crash_child.hpp 顶部告警）。
    std::fprintf(stderr,
                 "crash_child: 场景 '%s' 正常返回而未调用 crash_exit()——"
                 "崩溃态已被析构清掉，测试将不再检验崩溃路径\n",
                 scenario.c_str());
    crash_exit(122);
}

int spawn_crash_child(std::string_view scenario, const std::string& dir) {
    if (executable_path().empty()) {
        std::fprintf(stderr,
                     "crash_child: 可执行文件路径未记录——main() 是否漏调"
                     " record_executable_path()？\n");
        return -1;
    }
    const std::string arg_scenario =
        std::string(kScenarioFlag) + std::string(scenario);
    const std::string arg_dir = std::string(kDirFlag) + dir;

#if defined(_WIN32)
    // S37-5 补齐：CreateProcessW + WaitForSingleObject + GetExitCodeProcess。
    // 路径与参数须转 UTF-16；命令行需按 Windows 规则加引号转义。
    (void)arg_scenario;
    (void)arg_dir;
    std::fprintf(stderr, "crash_child: Windows 后端待实现（S37-5）\n");
    return -1;
#else
    // posix_spawn 而非 fork+exec：本改造的目的就是去掉 fork，用 fork 实现
    // 会把要消除的东西又请回来（且 fork 在多线程进程里本就危险——gtest
    // 进程此刻可能已有后台线程）。
    char* const argvec[] = {
        const_cast<char*>(executable_path().c_str()),
        const_cast<char*>(arg_scenario.c_str()),
        const_cast<char*>(arg_dir.c_str()),
        nullptr,
    };
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, executable_path().c_str(), nullptr,
                                 nullptr, argvec, environ);
    if (rc != 0) {
        std::fprintf(stderr, "crash_child: posix_spawn 失败: %s\n",
                     std::strerror(rc));
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) == -1) return -1;
    if (!WIFEXITED(status)) return -2;
    return WEXITSTATUS(status);
#endif
}

}  // namespace bitcask::test
