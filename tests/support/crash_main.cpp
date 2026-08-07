// 崩溃注入测试二进制的 main（S37-2）。
//
// 替代 GTest::gtest_main：多两件事——
//   ① 记录可执行文件路径（spawn_crash_child 要用它启动自身）；
//   ② 若 argv 带 --bitcask-crash-child=<场景>，则**不进 gtest**，直接跑场景
//      并以其返回码退出。
//
// ② 必须在 InitGoogleTest 之前：子进程一旦进了 gtest 就会打印测试输出、
// 注册全局环境，污染父进程解析的退出码语义。

#include <gtest/gtest.h>

#include "crash_child.hpp"

int main(int argc, char** argv) {
    bitcask::test::record_executable_path(argc > 0 ? argv[0] : nullptr);

    // 子进程分支不会返回——场景经 crash_exit() 直接 _Exit（见 crash_child.hpp）。
    (void)bitcask::test::run_as_crash_child(argc, argv);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
