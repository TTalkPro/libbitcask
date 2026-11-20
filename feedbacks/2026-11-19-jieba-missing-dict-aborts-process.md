# jieba 词典路径不对时整个进程被 abort：`open` 来不及返回错误；下游 `bitcask:merge` 带 `{analyzer, jieba}` 必撞

**报的人**：bosun（Erlang 任务管理，经 bitcask 10.0.0 撞到，11.0.0 `862d735` 复现；Linux x64）。
**严重度**：🔴 宿主进程直接没（BEAM 节点整个退出，不是一个可捕获的错误）。①在 libbitcask：词典缺失应当是 `open` 失败，不是 abort；
②在下游 bitcask（Erlang 封装）：`merge/2,3` 漏了 `open` 会做的缺省词典目录，带 jieba 的合并一调就踩①。

## 症状

bosun 的定时 merge 第一次跑，节点直接退出，日志最后一行：

```text
.../third_party/cppjieba/include/cppjieba/DictTrie.hpp:243 FATAL exp: [ifs.is_open()] false. open /jieba.dict.utf8 failed.
```

没有 Erlang 侧的异常、没有 `{error, _}`、没有 crash dump——cppjieba 的 `XCHECK` 失败后 abort。两条路径都能触发：

```erlang
%% ① 直接给一个不存在的词典目录
bitcask:open(Dir, [read_write, {analyzer, jieba}, {dict_path, <<"/nonexistent">>}]).
%%   ⇒ FATAL ... open /nonexistent/jieba.dict.utf8 failed.   进程 abort

%% ② 什么都没写错：库是 bitcask:open(Dir, [read_write, {analyzer, jieba}]) 建的，照着同样的选项合并
bitcask:merge(Dir, [{analyzer, jieba}]).
%%   ⇒ FATAL ... open /jieba.dict.utf8 failed.               进程 abort（dict_path 是空串）
```

另外 `dict_path` 给成 Erlang 字符串（list）而不是 binary 时被静默当成没给，同样走到②——
`{dict_path, filename:join(code:priv_dir(bitcask), "dict")}` 这种最自然的写法就是 list。

## 位置

- ① libbitcask `src/text/analyzer.cpp:84`：Jieba 的 creator 直接 `std::make_unique<JiebaAnalyzer>(c.dict_path, ...)`，
  词典文件存不存在不检查，交给 cppjieba 构造函数；后者 `DictTrie.hpp:243` 的 `XCHECK(ifs.is_open())` 失败即 abort。
  `include/bitcask/analyzer.hpp:70` 的注释把责任给了调用方（「必须有效，由调用方保证」），
  但 `src/cask/cask.cpp:725` 明明留了「analyzer creation failed (check analyzer type / dict_path)」这条报错路径——只是到不了。
- ② 下游 bitcask `src/bitcask.erl`：`open_1` 在组装选项后会过一遍 `maybe_default_dict_path/1`（jieba 且没给 `dict_path` ⇒ 补 `priv/dict`），
  `cask_open_opts/1`（`merge/2,3` 的临时 merge_only 打开用的那份，约 `:836`）没有这一步，于是 `dict_path` 为空。
  `cpp/nif/nif_options.cpp:106` 对 `dict_path` 只认 binary，别的类型不报 `badarg` / `invalid_option`，直接忽略。

## 怎么撞上的

见上面两段代码。② 是任何「jieba 检索库 + 用 `bitcask:merge/2,3` 合并」的组合：合并要另开一个 merge_only 句柄，
分词选项得和建库时一致（否则指纹不符），所以调用方一定会带 `{analyzer, jieba}`，而没人会想到这里还得自己补词典目录。

## 影响面

- 所有用 jieba 分词的检索库的宿主：BEAM 节点 / JVM / .NET 进程，词典目录配错一次（部署漏拷、路径拼错、权限）就是整个进程退出，
  而且是在 `open` 里——崩溃重启循环，日志只有 cppjieba 的那一行。
- 下游 bitcask 用户：带 jieba 的库调 `bitcask:merge/2,3`（或经 `bitcask_merge_worker`）必崩。
- bosun 已绕开：merge 时自己给 binary 的 `dict_path`，并且先确认 `jieba.dict.utf8` 存在，不存在就跳过这次合并。

## 建议（下游不替 libbitcask 拍板）

1. libbitcask：建 JiebaAnalyzer 之前先确认词典文件都在（`jieba.dict.utf8` / `hmm_model.utf8` / `user.dict.utf8` / `idf.utf8` / `stop_words.utf8`），
   缺了就让 creator 返回空，走 `cask.cpp:725` 已有的 `INVALID_OPTION`，报文带上找不到的那个路径。库不该替宿主决定退出。
2. 下游 bitcask：`cask_open_opts/1` 也过 `maybe_default_dict_path/1`（最好 `open` 与 `merge` 共用同一段选项组装）；
   `dict_path` 收到 list 时转成 binary（`synonym_file` 已经这么做了），收到别的类型报 `invalid_option`，不要静默忽略。
3. 回归用例：`open` 给不存在的 `dict_path` ⇒ 返回错误、进程还活着；jieba 库 `merge/2` 不带 `dict_path` ⇒ 正常合并。

## 回没回

已修 ①（**11.1.0 已发版**，S57）：Jieba 工厂建分词器前先查 `jieba.dict.utf8` / `hmm_model.utf8` / `user.dict.utf8` / `idf.utf8` /
`stop_words.utf8`（存在、普通文件、可读），缺任一 ⇒ 不建 ⇒ `Cask::open` 返回 `kInvalidOption`，报文
`jieba dictionary not found or unreadable: <缺的完整路径> (check dict_path)`。**不降级**：词典缺失 jieba 无法工作，库直接打不开，
只是不再杀宿主进程。新增 `text::jieba_missing_dict_file(dict_dir)` 供自行构造 `JiebaAnalyzer` 的调用方预检（直接构造仍不检查）。
回归 `S57Test.JiebaMissingDictFileIsReported` / `FactoryReturnsNullInsteadOfAbort` / `OpenWithBadDictPathFailsCleanly`
（11.0.0 上工厂用例进程直接死在 `DictTrie.hpp:243`）。
② 在下游 bitcask（`cask_open_opts/1` 补缺省 `dict_path`、`dict_path` 收 list）——未改，不在本仓库。
