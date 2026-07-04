// libbitcask C API — 提供 C 语言 ABI 接口，用于 .so 动态库跨语言绑定。
//
// 设计原则：
//   - 不透明句柄 (opaque handle)：C 侧持有指针，不知道内部布局
//   - 显式内存管理：每个 alloc 的结果有配对的 free 函数
//   - 错误码 + out-param：函数返回错误码，详情经 bitcask_fault_t* 传出
//   - 二进制安全：使用 {ptr, len} 切片，不依赖 NUL 结尾
//
// 线程安全（与 C++ 核心一致；S11：通用库，**同一 handle 多线程安全**。
// C API 透明包装 Cask，无 C 层共享可变态，故完全继承其契约。详见
// doc/api-c.md §14 / docs/design/thread-safety.md）：
//   - open/close/factory：线程安全（产生/销毁独立对象）
//   - 读（get / 全部 search_text/phrase/fields/near/fuzzy/wildcard/bool /
//     search_vector / search_hybrid）：**线程安全**（并发读，无锁/共享锁）
//   - 写（put / delete / sync / put_doc / close_write_file）：**线程安全**
//     （S11-W1：内部 write_mu_ 串行化；同一 handle 多线程写安全。写在文件层本就
//     串行 → 锁不损吞吐；更高写并发 → 按目录分片多个实例横向扩展）
//   - 读写并发：安全（搜索可见性遵循 near-real-time 契约）；merge 与读写并发
//     （经 keydir shared_mutex 协调，不阻塞写）
//   - iter_*：同一 iter 不可并发使用（每线程一个迭代器，同 std 容器约定）；
//     不同迭代器之间并发安全
//
// 用法示例：
//   bitcask_options_t opts;
//   bitcask_options_init(&opts);
//   opts.read_write = 1;
//
//   bitcask_t* cask = NULL;
//   bitcask_fault_t fault;
//   if (bitcask_open("/tmp/db", &opts, &cask, &fault) != BITCASK_OK) {
//       fprintf(stderr, "open failed: %s\n", fault.detail);
//       return 1;
//   }
//   bitcask_slice_t key = {"hello", 5};
//   bitcask_slice_t val = {"world", 5};
//   bitcask_put(cask, key, val, 0, NULL);
//
//   bitcask_get_result_t* result = NULL;
//   if (bitcask_get(cask, key, &result, NULL) == BITCASK_OK) {
//       printf("value: %.*s\n", (int)result->value.size, (char*)result->value.data);
//       bitcask_get_result_free(result);
//   }
//   bitcask_close(cask);

#ifndef BITCASK_C_H
#define BITCASK_C_H

// S19-5：本头拆分为三个分域头（组织性变化，符号/签名/ABI 不变）；
// 继续 include 本头获得全量 API（既有用户零改动）。
#include "bitcask_kv.h"
#include "bitcask_text.h"
#include "bitcask_vec.h"

#endif // BITCASK_C_H
