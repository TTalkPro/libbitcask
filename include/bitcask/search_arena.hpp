// 进程级共享「有界 Search 池」的 inter-query 并发入口（S19-1 自
// search_layer 抽出——shim 降级为测试夹具后本设施仍是生产件，Cask 批量
// 查询与 Searcher 门面共用）。

#pragma once

#include <cstddef>
#include <functional>

namespace bitcask::search {

// S7-4: 把 [0, n) 并发跑在进程级共享「有界 Search 池」上（inter-query 并发）。
// body(i) 执行第 i 条**独立**查询、写各自结果槽（槽间不重叠 → 无需锁）；并发发生在
// 查询**之间**，每条查询内部仍串行。n<=1 直跑（零池开销）。
// 要求：body 之间不共享可变态（查询纯读各索引 shared_lock，安全）。
void parallel_for_queries(std::size_t n,
                          const std::function<void(std::size_t)>& body);

}  // namespace bitcask::search
