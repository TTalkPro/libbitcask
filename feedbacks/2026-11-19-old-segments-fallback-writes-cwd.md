# 打开旧版封存的检索库：段集静默退成「无目录」空集 ⇒ 新段写进进程当前目录

**报的人**：观澜（chartroom，经 Keelson 1.4.0 / clj-bitcask，pin 9.0.0 `d164744`，Linux x64）。
**严重度**：🟠 写错位置 + 不报错不记日志（检索结果本身是对的）。

## 症状

观澜从 Keelson 1.2.8（libbitcask 7.0.1）升到 1.4.0（9.0.0）后，打开 7.0.1 建的检索库、flush 一次：

- 进程的**当前工作目录**里多出 `seg-0.seg` 与 `segments.manifest`（文件头 `BCSC` / `SGMF`）；
- 库目录的 `bm25_segments/` 不变；
- 检索结果正确；`log_fn` 一条都没收到（warning / error 都没有）。

7.0.1 打开自己建的库不会这样；9.0.0 新建的库反复打开也不会这样。观澜的单测就是因此在仓库根目录留下了这两个文件才被发现。

## 位置

- `src/search/text_plugin.cpp:1358` `TextPlugin::init_segment_set`：`open_from_payload` 与 `SegmentSet::open(segs_dir)` 都回 nullptr 时，
  走 `segment_set_ = std::make_unique<search::SegmentSet>();` —— 默认构造的段集 `dir_` 是**空串**；
- `include/bitcask/segment_set.hpp:94` `add_pending` / `:314` `manifest_path`：`join(dir_, "seg-N.seg")`、`join(dir_, "segments.manifest")`，
  `dir_` 为空 ⇒ 相对路径 ⇒ 落进进程当前目录；
- 同函数里 `ok = opened != nullptr || !expect_segments`：清单 payload 为空（没内嵌段清单的旧 ckpt）时，两路都打不开也回 `true`，上层看不到失败。

旧段为什么载不进来，下游没定位（`SegmentSet::open` 在清单 CRC 坏或任一段 `load_any` 失败时回 nullptr，两种都可能）。

## 怎么撞上的

1. 用 libbitcask 7.0.1（Keelson 1.2.8）建一个检索库（jieba、4 / 8 维 cosine hnsw，写几篇文档、flush、close）——`bm25_segments/` 里是 `seg-0.seg` + `segments.manifest`；
2. 换 9.0.0 打开同一目录，`cd` 到一个空目录再跑，flush、close；
3. 空目录里出现 `seg-0.seg` / `segments.manifest`。

观澜仓库里有一份现成的旧库夹具：`modules/chartroom-index/test/golden/ak07-cs-index/`（当年 C# 经 Keelson.Bitcask 建，`.keyspace` = 2，4 维），
用 clj-bitcask 1.4.0 `open-index` 打开即复现。

## 影响面

- 所有拿旧版库升级到 9.0.0 的下游：每次打开 + flush 都往**进程当前目录**写段文件（桌面应用的当前目录常是家目录 / 启动目录），不报错；
- 新封的段没进库目录，下次打开大概率又载不到段（推测每次都从数据重建 BM25，大库启动慢；未在下游量化）；
- 同一进程里开多个旧库时，它们的段文件写到同一个当前目录、同名覆盖（推测）。

观澜侧已绕过（不改库）：把自己的索引键空间版本从 2 升到 3，旧库首次打开即整棵删掉重建，之后是 9.0.0 自己的格式，不再走这条回退；
判据 `ak07-cross-impl-test/old-keyspace-index-is-rebuilt-without-touching-cwd` 钉住「进程当前目录不多出段文件」。

## 建议（下游不替 libbitcask 拍板）

1. 回退出来的空段集也要带上 `segs_dir`（或 `SegmentSet` 不允许无目录构造），⛔ 落到相对路径；
2. 两路都打不开时至少 `log_fn` 记一条 warning（说明丢掉了哪些旧段、要从数据重建），非空 `expect_segments` 之外的失败也上抛或标记；
3. 若 9.0.0 确实不再能读 7.x 封的段，CHANGELOG / 升级说明写明「旧检索库需要重建」，或打开时显式返回一个可识别的错误码，让下游决定重建。
