# 迭代器用过之后再写一次，`bitcask_flush_index` 永远不返回（6.9.0 起；7.0.0 仍在）

**报的人**：锦书（clj，经 clj-bitcask / Keelson 1.2.6、1.2.7 调 libbitcask，Linux x64）。起因：锦书升级 Keelson 1.2.7 后，
后端全量单测停在 `jinshu.rag.store-test/delete-isolation`，四十多分钟不动；锦书的检索库删块路径是「带 meta 过滤扫一遍键
→ 删 → `flush_index`」。
**严重度**：🔴 调用方线程永久阻塞，不报错、不超时（进程收到 SIGTERM 也退不出，只能 SIGKILL）。

## 症状

`bitcask_flush_index(cask)` 不返回。调用线程停在原生调用里几乎不占 CPU；JVM 侧所有线程都在 futex 上等。
没有错误码、没有日志回调。

## 结论：libbitcask 这一侧，不是绑定层

下面的纯 C 程序（不经 JVM、不经 Keelson 绑定，直接链各版本 jar 里内嵌的 `libbitcask.so`）给出：

| libbitcask | 不迭代 | 迭代后 `delete` | 迭代后 `put_doc` | 迭代后不写 |
|---|---|---|---|---|
| 6.8.0（Keelson 0.1.1316） | OK | OK | OK | OK |
| 6.9.1（Keelson 1.2.6） | OK | **卡死在 flush_index** | **卡死** | OK |
| 7.0.0（Keelson 1.2.7，`677cfca`） | OK | **卡死在 flush_index** | **卡死** | OK |

与有没有向量（`vector_dim` 2 或 0）、写入是逐条 `bitcask_put_doc` 还是 `bitcask_put_batch_atomic_ex` 无关。
Keelson 1.2.6→1.2.7 之间绑定层（`clj-bitcask`）的源码没有改动，只换了原生库。

## 位置（疑似病因，未改代码验证）

- `src/cask/cask_iter.cpp:89`：`CaskIter::start` 末尾调 `parent_->authority_->advance_fold_epoch();`
  （6.9.0 `7af29da` 随 S43「OKI-only 权威」引入）。
- `include/bitcask/index_authority.hpp:64`：`advance_fold_epoch()` = `advance_ord(peek_next_ord())`。
- `src/fileops/index_authority.cpp:21`：`advance_ord(x)` 把 `next_ord_` 推到 `x + 1`
  ⇒ **每开一次迭代器，ord 序列凭空跳过一个号**（号 `x` 永远不会被 `alloc_ord` 发出去，也没有任何任务带着它进索引通道）。
- `include/bitcask/thread_pool.hpp:28`：`flush(lane)` 的契约是「等 `lane.in_flight == 0` 且 `applied_ord ≥ submitted_ord_hwm`」，
  索引任务按 ord 经重排缓冲顺序 apply。
- 对照：其它「发了号但不进索引」的路径都会补一个 `IndexOp::Skip` 填空洞——
  `src/cask/cask.cpp:1389`、`:1490`、`:2153`、`:2547`，`include/bitcask/cask.hpp:1299`。
  `advance_fold_epoch()` 跳号时**没有**补 Skip。

推断的链条：迭代器之后的下一次写拿到 `x + 1` 并提交，`submitted_ord_hwm` 涨到 `x + 1`；重排缓冲在等 `x`，`x` 永远不来
⇒ `applied_ord` 停在 `x - 1` ⇒ `flush` 的谓词永远不成立。迭代后不写时 hwm 不涨，所以不卡——与上表完全一致。
`CaskIter::release()`（`cask_iter.cpp:290`）只清本地状态，不碰这件事。

## 怎么撞上的（最短复现）

1. `enable_search = 1` 打开一个库（ngram；有无向量都行）。
2. 写几篇文档（`bitcask_put_doc` 或 `bitcask_put_batch_atomic_ex`），`bitcask_flush_index` —— 正常返回。
3. `bitcask_iter_start_ex(cask, -1, -1, 0, {NULL,0}, 0, NULL, &it, &f)`，`bitcask_iter_next_ex` 走到底，`bitcask_iter_release(it)`。
4. `bitcask_delete`（或 `bitcask_put_doc`）任意一个 key。
5. `bitcask_flush_index` —— 不返回。

完整程序（编译：`gcc -O1 -o flush_after_iter flush_after_iter.c -I c_api -L <so 目录> -lbitcask`；
运行：`./flush_after_iter <空目录> each|batch|noiter [dim=2] [after=delete|put|none]`，外面套 `timeout -s KILL 20`）：

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bitcask_kv.h"

static void step(const char* s) { printf("step %s\n", s); fflush(stdout); }
static bitcask_slice_t sl(const char* s) { bitcask_slice_t x = { s, strlen(s) }; return x; }
static void check(bitcask_error_t rc, const char* what) {
  if (rc != BITCASK_OK) { printf("FAIL %s rc=%d\n", what, (int)rc); exit(2); }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <dir> each|batch|noiter [dim=2] [after=delete|put|none]\n", argv[0]); return 1; }
  const char* dir = argv[1];
  const char* variant = argv[2];
  int dim = argc > 3 ? atoi(argv[3]) : 2;
  const char* after = argc > 4 ? argv[4] : "delete";
  printf("libbitcask %s, variant=%s dim=%d after=%s\n", bitcask_version_string(), variant, dim, after); fflush(stdout);

  bitcask_options_t o;
  bitcask_options_init(&o);
  o.read_write = 1;
  o.enable_search = 1;
  o.analyzer_type = BITCASK_ANALYZER_NGRAM;
  o.vector_dim = (uint16_t)dim;
  if (dim > 0) { o.vector_metric = BITCASK_VECTOR_METRIC_COSINE; o.vector_engine = BITCASK_VECTOR_ENGINE_HNSW; }

  bitcask_fault_t f; memset(&f, 0, sizeof f);
  bitcask_t* cask = NULL;
  step("open");
  check(bitcask_open(dir, &o, &cask, &f), "open");

  const char* keys[3] = { "a", "b", "c" };
  const char* texts[3] = { "北境的雪原", "狼群在夜里嚎叫", "既无霜也无焰" };
  float vecs[3][2] = { {1.0f, 0.0f}, {0.0f, 1.0f}, {0.7071f, 0.7071f} };

  step("write");
  if (strcmp(variant, "each") == 0) {
    for (int i = 0; i < 3; i++) {
      bitcask_doc_input_t d; memset(&d, 0, sizeof d);
      d.text = sl(texts[i]); if (dim > 0) { d.vector = vecs[i]; d.vector_len = 2; }
      check(bitcask_put_doc(cask, sl(keys[i]), &d, 0, &f), "put_doc");
    }
  } else {
    bitcask_doc_input_ex_t d[3]; bitcask_txn_op_ex_t ops[3];
    memset(d, 0, sizeof d); memset(ops, 0, sizeof ops);
    for (int i = 0; i < 3; i++) {
      d[i].text = sl(texts[i]); if (dim > 0) { d[i].vector = vecs[i]; d[i].vector_len = 2; }
      ops[i].op = 2; ops[i].key = sl(keys[i]); ops[i].doc = &d[i];
    }
    check(bitcask_put_batch_atomic_ex(cask, ops, 3, &f), "put_batch_atomic_ex");
  }

  step("flush1");
  bitcask_flush_index(cask);

  if (strcmp(variant, "noiter") != 0) {
    step("iter");
    bitcask_iter_t* it = NULL;
    bitcask_slice_t empty = { NULL, 0 };
    check(bitcask_iter_start_ex(cask, -1, -1, 0, empty, 0, NULL, &it, &f), "iter_start_ex");
    int n = 0;
    for (;;) {
      bitcask_iter_entry_ex_t e; memset(&e, 0, sizeof e);
      int rc = bitcask_iter_next_ex(it, &e, &f);
      if (rc < 0) { printf("FAIL iter_next_ex rc=%d\n", rc); return 2; }
      if (rc == 0) break;
      n++;
      bitcask_iter_entry_ex_free(&e);
    }
    printf("  iterated %d\n", n); fflush(stdout);
    bitcask_iter_release(it);
  }

  if (strcmp(after, "delete") == 0) {
    step("delete");
    check(bitcask_delete(cask, sl("a"), 0, &f), "delete");
  } else if (strcmp(after, "put") == 0) {
    step("put_doc");
    bitcask_doc_input_t d; memset(&d, 0, sizeof d);
    d.text = sl("天亮了"); if (dim > 0) { d.vector = vecs[0]; d.vector_len = 2; }
    check(bitcask_put_doc(cask, sl("x"), &d, 0, &f), "put_doc");
  } else {
    step("no-op");
  }

  step("flush2");          /* 6.9.1 / 7.0.0：迭代后写过一次 ⇒ 这里不返回 */
  bitcask_flush_index(cask);

  step("close");
  bitcask_close(cask);
  printf("RESULT OK\n");
  return 0;
}
```

实测输出（Linux x64，三个 `.so` 取自 Keelson 分类器 jar `META-INF/keelson-native/linux-x64/bitcask/libbitcask.so`）：

```
0.1.1316  libbitcask 6.8.0  each 2 delete  -> RESULT OK
1.2.6     libbitcask 6.9.1  each 2 delete  -> HANG after: step flush2
1.2.7     libbitcask 7.0.0  each 2 delete  -> HANG after: step flush2
1.2.7     libbitcask 7.0.0  each 0 delete  -> HANG after: step flush2
1.2.7     libbitcask 7.0.0  each 2 put     -> HANG after: step flush2
1.2.7     libbitcask 7.0.0  each 2 none    -> RESULT OK
1.2.7     libbitcask 7.0.0  noiter         -> RESULT OK
```

## 影响面

- 任何「迭代过一次之后还会写、并且会 `flush_index`」的检索库调用方都会撞；撞了就是调用线程永久挂住，
  上层无从感知（没有错误、没有超时、日志回调也不响）。
- 锦书：检索库的删块路径就是「带 meta 过滤扫键（`bitcask_iter_start_ex`）→ 删 → flush」，升 Keelson 1.2.6 / 1.2.7 后
  第一次按节删除就卡死；Keelson 1.2.7 的升级因此受阻（之前的 Keelson 0.1.1316 / libbitcask 6.8.0 正常）。
- 推断：`bitcask_range_iter_start*` 若也走 `advance_fold_epoch` 同病（未测）；`bitcask_iter_start`（无 `_ex`）同一实现，应同病（未测）。
- 每开一次迭代器还会永久吃掉一个 ord（即使不卡，ord 空间也有空洞）——这条本身无害，但和 flush 的连续性假设冲突。

## 建议（下游不替 libbitcask 拍板）

- `advance_fold_epoch()` 跳过的那个号补一个 `IndexOp::Skip` 进索引通道（与 `cask.cpp:1389` 等处同一做法）；或者
- fold epoch 不借用 ord 序列推进，换一个独立的代数计数器，ord 只给真正的写；
- 回归：上面的最短复现做成 C / C++ 单测（「迭代 → 释放 → 写 → flush 在限时内返回」），覆盖 `iter_start`、`iter_start_ex`、
  `range_iter_start_ex` 三个入口。

## 回没回

**已修（7.0.1）。** 报文的病因推断完全成立，按建议第二条改：fold 时代不再借 ord。

### 病因（已实测证实）

修前在 `7.0.0` 上加回归用例直接看到：库里 3 篇文档（`next_ord = 3`），开两次 fold 迭代器后 `next_ord = 5`——
没有任何写，ord 序列被推了两格；随后一次 `remove` + `flush_index` 20 秒不返回。

时序（单线程即可撞，不是竞态）：

| 步 | 动作 | `next_ord` | 索引通道 `next_apply` / `applied` | `submitted_hwm` |
|---|---|---|---|---|
| 1 | 写 3 篇（ord 0,1,2）+ flush | 3 | 等 3 / 已 apply 到 2 | 2 |
| 2 | `iter_start`：`advance_ord(peek_next_ord())` = `advance_ord(3)` | **4** | 等 3 | 2 |
| 3 | `delete`：`alloc_ord()` 拿到 **4**，提交 `Delete{ord=4}` | 5 | 4 进重排缓冲暂存，仍等 **3** | **4** |
| 4 | `flush_index`：等 `in_flight == 0 && applied ≥ hwm` | 5 | 3 号永远不来 ⇒ `applied` 停在 2，`in_flight` 停在 1 | 4 |

第 2 步之后不写就不卡（hwm 不涨，谓词仍成立），与报文的表一致。3 号既不在 data 里也不在通道里，
没有任何路径会补它；`close` 的有界 flush 超时后能退出，但 `flush_index` 本身无超时。

为什么会借 ord：S43（6.9.0）把 KeyDir 的 fold epoch 并进了 `IndexAuthority`，图省事让「时代」复用 `next_ord` 高水位
（`pending_delete = expiration_epoch < next_ord`），而「启动一次 fold 推进一个时代」就成了「跳一个 ord」。
ord 序列的契约是**每个发出去的号恰有一个任务进索引通道**（Add / Delete / Skip / RunFn），这一跳破坏了它。
只有 `CaskIter::start` 调 `advance_fold_epoch`——`bitcask_iter_start` / `_prefix` / `_ex` 同病；
`bitcask_range_iter_start*`（`CaskRangeIter`）不走这条，**不受影响**（已加用例确认）。
与并发无关、与向量 / 批写无关；不开 `enable_search` 的库没有索引通道，只是白白空一个 ord。

### 改法

- `IndexAuthority` 加独立的 fold 时代下界 `fold_floor_`；时代时钟 `fold_epoch() = max(next_ord, fold_floor_)`
  （每次写自然推进，开 fold 只抬下界）。`advance_fold_epoch()` 只动 `fold_floor_`，**不再碰 `next_ord_`**。
- `set_pending_delete` 与四处 `pending_delete` 派生（`adopt_manifest` / 两处提交快照 / `fstats()`）改用 `fold_epoch()`。
- 下界不落盘（盘上格式零改动）：`adopt_manifest` 由 fstats 里已发布的 `pending_delete` + `expiration_epoch` 还原，
  已发布的 pending 跨重启不回退。
- 没选「补 `IndexOp::Skip`」：迭代器在 `write_mu_` 之外，补 Skip 要么让只读操作进写锁，要么让 Skip 与并发写乱序提交；
  而且读操作消耗 ord 本身就是错的。

C API 签名零改动、盘上格式零改动。被 6.9.0 ～ 7.0.0 卡过的库不需要修复：空洞只存在于进程内的通道状态，
重开后通道起点取恢复出的 `next_ord`，自然对齐。

### 回归

- `tests/flush_after_iter_test.cpp`（`FlushAfterIterTest.*`，7 条）：开迭代器不动 `peek_next_ord`；
  fold / range 迭代后 delete、put_doc、混合写再 flush；8 轮「扫 → 写 → flush」；迭代器未释放时写 + flush；迭代后重开。
  flush 放旁路线程限时 20s，超时 abort（修前：第 1 条断言红、第 2 条超时）。
- `tests/c_api_test.c` `test_flush_after_iter`：`bitcask_iter_start` / `bitcask_iter_start_ex` /
  `bitcask_range_iter_start_ex` 三个入口各走一轮「迭代 → 释放 → delete + put_doc → flush_index」
  （POSIX 下 `alarm(60)`；已对照验证：还原旧实现时被 SIGALRM 打断）。
- `OkiOnlyContractTest.MergeAndTtlFstatsMatchManifestAfterTrim`：时代推进不动 ord；pending 跨重启保持。
- GCC Release 全量 ctest 860/860；ASan、TSan 下相关子集（FlushAfterIter / OkiOnlyContract / CApi / MetaFilterQuery / OkiRange）全绿。

### 下游要做的

- 升到 7.0.1（`SOVERSION` 仍是 7）即可，调用方式不用改。
- 在那之前的绕法（6.9.0 ～ 7.0.0）：删块路径的键扫描改用 `bitcask_range_iter_start_ex`（不触发本缺陷）。
