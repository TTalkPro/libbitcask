// 测试辅助：模拟「上一个写者没有正常关闭」。
//
// 很多崩溃用例是「正常 close 之后再手工改坏 data 文件」来模拟掉电。S67 起恢复按
// 写锁文件判断上一个写者是否崩溃：正常 close 会先 fsync 活跃文件、再删锁，所以
// 「没有陈旧写锁」= 全部文件已封口，文件中间 / 末尾读不出来的字节一律按**损坏**
// 处理（跳过、不截断）；只有留下了陈旧写锁，锁里记的活跃文件（和 id 最大的文件）
// 才按「没写完的尾巴」截断。要模拟掉电，就得把那把陈旧的锁也留下。
#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace bitcask::test {

// 在 dir 里放一把属于「已经不存在的进程」的写锁，记着 active_data_file。
inline void plant_stale_writer_lock(const std::filesystem::path& dir,
                                    const std::filesystem::path& active_data_file) {
    std::ofstream out(dir / "bitcask.write.lock", std::ios::binary | std::ios::trunc);
    // pid 远超任何平台的 pid 上限 ⇒ 判活必然为假；第二行是进程实例令牌（0 = 无）。
    out << "999999999 " << active_data_file.string() << "\n0\n";
}

}  // namespace bitcask::test
