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

## 回没回

**已修（9.1.0 已发版，S52）。** 症状、位置、建议都对；旧段为什么载不进也查清了。

### 病因（实测）

用观澜的旧库夹具（`ak07-cs-index`）逐层载入：段清单与 `bm25.ckpt`（内嵌 kSegManifest）读得出、CRC 对；
`seg-0.seg` 是 `BSG2` v2，`MmapSegment::open` 成功，字段为 `\xfault` / `tags` / `title`。`open_v2` 末尾要求段里有
`kDefaultField`——8.1.0 把它从 `\xfa` "ult" 改成了 `\x01_bitcask:default` ⇒ 旧段一律拒载。这是 8.1.0 有意的
「旧默认字段标识不迁移、需重建」（CHANGELOG 8.1.0 写了，但没写清会怎么表现）。

毛病全在回退：`load_component` 失败 ⇒ `open()` 以 loaded=false 再调 `init_segment_set` ⇒ `SegmentSet::open`
回 nullptr ⇒ 默认构造的段集（`dir_` 为空串）⇒ 全量重放封出的段写进进程当前目录。检索结果一直是对的（全量重放
本来就在做），所以只有「当前目录多出文件」这一个外在表现。

另查出一个同类坑：回退集即便带上目录，新段从 0 编号会与旧 `seg-0.seg` 同名；全活段 `save_v2` 不写也不删 `.live`
旁车 ⇒ 旧旁车下次打开会被套到新段上，把活文档标成已删（静默少返回）。

### 怎么修的

`SegmentSet::fresh(segs_dir)` 取代默认构造：新段落在库目录；seg_id 从目录里已有段文件的最大号之后起；原有段文件与
旁车排进延迟删除，下次可写 checkpoint 清掉（只读句柄不动盘）；记一条 warning
（`bm25: N existing segment file(s) in … cannot be loaded … rebuilding the full-text index from data …`）。
建议 1、2 照做；建议 3（升级说明）写进了 CHANGELOG 9.1.0。回归
`SegmentFallbackTest.UnreadableSegmentsRebuildInsideTheIndexDir` 在 9.0.0 实现上红、本实现上绿。

### 下游要知道的

- 修了之后，旧库首次打开仍要从数据全量重建全文索引一次（8.1.0 起就如此，只是现在看得见、写对地方）；之后正常载入。
- 观澜的绕法（键空间 2 → 3 强制整棵重建）与本修复兼容，升级后可以保留，也可以去掉。
- 纯新增、不破 ABI；盘上格式不变。

