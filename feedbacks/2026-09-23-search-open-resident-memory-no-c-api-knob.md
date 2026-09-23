# `enable_search` 的库一打开就常驻 ≈ 盘上 1.8 倍 —— 大头都没有 C API 旋钮（BM25 段开库整读校验、ckpt 整份读缓冲、同一批 key 住三份）

**报的人**：keel（下游消费者，pin 6.5.0 `f618e82`），转的是 coxswain（keel 的宿主，代码编辑器，
代码符号索引建在 bitcask 上）2026-09-22 的账：keel `feedbacks/2026-09-22-bitcask-index-resident-cost.md`，
keel 的回信 `feedbacks/2026-09-23-reply-to-coxswain-bitcask-resident.md`。
**严重度**：🟠 不报错，内存预算上的——「开一个大项目的符号库多 550 MB」，一开库就付、不随查询走；
下游**没有旋钮可调**（keel 这一侧能接的 `keydir_cache_entries` 已经接了，见「keel 做了什么」）。

## 症状

Windows 11 / MSVC 14.44，`enable_search = 1`，BM25 only（`vector_dim = 0`）：

| 库 | 盘上 | 打开它的峰值 RSS |
|---|---|---|
| 15900 个文件的整仓 | 336 MB | **610 MB** |
| 731 个文件 | 2.2 MB | 58 MB |

盘上构成：`8.bitcask.data` 115 MB、`8.bitcask.hint` 37 MB、`bm25_segments/` 104 MB、`docmap.ckpt*` 60 MB、
`kv.keydir.ckpt` 7 MB。

## 位置（keel 按代码读的，⚠️ 没在本仓复现那个 336 MB 的库）

| 那一份 | 形态 | 位置 |
|---|---|---|
| **BM25 段开库整读** | mmap，但 `mmap_verify_crc` 缺省 `true` ⇒ 每节 CRC ⇒ 104 MB 全进工作集 | `src/bm25/segment_v2.cpp:438`（映射）/ `:485`（逐节 CRC）；缺省在 `include/bitcask/text_plugin_config.hpp:55` / `include/bitcask/search_config.hpp:101`；⚠️ `bitcask_options_t` 里**没有**这一格 |
| **docmap ckpt 整份读进缓冲** | 堆，瞬时 ≈ 文件大小（60 MB），再解成常驻的 `ext2ord_` | `include/bitcask/search_checkpoint.hpp:330`（`read_file_bytes`）→ `src/keydir/docmap_ckpt.cpp:454` → `include/bitcask/index.hpp:284` |
| **keydir 快照整份读进缓冲** | 堆，瞬时 ≈ 7 MB | `src/keydir/keydir.cpp:2382` |
| **BM25 的 `key_to_location_`** | 堆，开库时对每条活文档重建一遍 `std::string` 键 | `src/search/text_plugin.cpp:1350-1366` |

⇒ 同一批 key 在内存里住**三份**：keydir 哈希、docmap 的 `ext2ord_`、TextPlugin 的 `key_to_location_`。
Level B（`keydir_cache_entries`）只拆得掉第一份。
⇒ 峰值里另有两块整份读缓冲（这个库 ≈ 67 MB）。
（hint 在 `search_on` 时不读，`cask_recovery.cpp:367`；data 文件惰性 mmap —— 这两份不是账。）

## 怎么撞上的

任何走 C API、`enable_search = 1`、库在百 MB 级的宿主。桌面程序最疼：库是「开着备用」，多数时候没人查，
内存却从开库那一刻起就占着。

## 影响面

- 下游只能调 `keydir_cache_entries`（对 key 多、文档短的代码索引，估计十几 MB 级）；其余三份**没有口子**。
- BM25 段那份是文件页（OS 收得回、不算 Private Bytes），但它算进工作集 / 峰值 —— 用户在任务管理器里看到的就是它。
- 报不报错：**不报**。

## 建议（下游不替 libbitcask 拍板）

1. **`mmap_verify_crc` 透到 C API**（`bitcask_open_ex` 旁的独立结构体，照 6.4.0 `bitcask_merge_policy_t` 的先例，不改 `bitcask_options_t` 布局）；
   或者更好：**开库只校验 footer / 目录，节 CRC 推迟到第一次读该节**（缺省安全不变，只是不再开库即全读）。
2. **ckpt 读改成流式 / mmap**（`read_file_bytes` 整份进 `vector` 的两处），削掉开库峰值那 ≈ 67 MB。
3. （长线，只是提出来）同一批 key 住三份：`ext2ord_` 与 `key_to_location_` 能不能共享一份 key 存储，或者其中一份按需从段里取 `key_at`。
   这条动的是结构，排在前两条后面。

## keel 做了什么

keel 阶段 219：`bitcask-open` 收 `'keydir-cache-entries`，透到 `bitcask_options_t::keydir_cache_entries`
（C API 6.0.0 起就有，是 keel 从前没接 —— 不算这里的账）。1 / 2 落地后 keel 那边照样一格转一手。

## 回

**6.6.0 收前两条，第三条不动**。本仓没复现那个 336 MB 的库，下面是按机制说的，不是实测数字。

1. **BM25 段**：两条建议之间取了第三条路。缺省仍校验每节 CRC，但 `MmapSegment::open`
   改走分块定位读（1 MiB 缓冲，数据经 OS 文件缓存），不再扫映射 ⇒ 校验不再把整个段目录
   拉进工作集；映射只给查询按需触页。开库那一遍 I/O 还在，所以旋钮也照样透出去了：
   `bitcask_open_ex2` + `bitcask_open_tuning_t.segment_verify_crc`（0 = 只验页脚 / 目录，
   信任盘）。结构体带 `struct_size`，以后加格不再开新入口。C API 文档 §8.1c。
2. **ckpt 整读**：报账低估了一半。`SearchCheckpoint::read` 是整文件读进缓冲、**再**
   把每段拷一份 payload，峰值 2 × 文件大小。现在：`read` 按目录逐段定位读（全部组件
   ckpt 受益，峰值 1 ×）；docmap 行段与 keydir 快照完全流式（分块校验 CRC + 分块解析，
   堆上只剩一块 1 MiB 缓冲）。这一条不用旋钮，keel 那侧不用接。
3. **同一批 key 住三份**：结构性改动，本版不动。补一条读代码时看到的：
   `key_to_location_` 的条目带着段的 `shared_ptr`，把 key 改成指向段内 key blob 的
   `string_view` 有生命期上的坑（覆盖到新段后旧段可能被 merge 释放），不是小改。

