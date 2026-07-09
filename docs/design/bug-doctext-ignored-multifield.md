# Bug：多字段模式下 doc.text 未被索引为默认字段

> 发现于 2026-07-09，来源：wiser-cpp 移植时实测 `search_text` 无法命中正文。
> 严重级别：**Medium**（功能正确性缺口 + API 契约违背）。

## 现象

`put_doc` 同时设置 `DocInput.text`（正文）与 `DocInput.fields`（命名字段，如 `title`）时，
`doc.text` **完全不进倒排索引**。`search_text` / `search_phrase` / `search_near`
（只查 `kDefaultField`）无法通过正文内容命中文档。`get_owned` 可正常取回正文（纯 KV 存储路径不受影响）。

## 契约违背

`DocInput` 的字段注释（`include/bitcask/cask.hpp:233`）明确写道：

```cpp
std::span<const std::byte> text;    // required（多字段时可空，作默认字段）
```

"作默认字段"暗示 `text` 在多字段模式下也应被索引为默认字段。但实现未兑现该契约。

## 根因

### 第一层：prepare 路由丢弃 doc.text

`TextPlugin::prepare()`（`include/bitcask/text_plugin.hpp:97-116`）的多字段路径只把
`e.doc->fields` 传给 `map_analyze`，**不传 `e.doc->text`**：

```cpp
if (!e.doc || e.doc->fields.empty()) {
    // 单文本路径：doc.text → kDefaultField（正确）
    const std::string_view t = e.doc ? e.doc->text : e.value;
    const std::pair<std::string_view, std::string_view> f{kDefaultField, t};
    p->job = map_analyze(e.key, e.ord, {&f, 1}, ...);
    return p;
}
// 多字段路径：只传 fields，doc.text 被丢弃
p->job = map_analyze(e.key, e.ord, e.doc->fields, ...);
return p;
```

### 第二层：水位幂等保护阻止简单绕过

即使把 `doc.text` 手动包装成 `kDefaultField` 追加到 fields 列表，也会撞上
`InvertedIndex::add_doc`（`src/bm25/inverted.cpp:357-360`）的水位幂等保护：

```cpp
if (ord <= max_indexed_ord_) return;  // 同 (field, ord) 第二次 add_doc 静默丢弃
```

kDefaultField 的 InvertedIndex 对同一文档只接受**一次** `add_doc`。catch-all 机制
（把命名字段词项合并进 kDefaultField，见 `apply_job_impl` line 234-238）本身也是一次
`add_doc`。如果先直接写 doc.text 为 kDefaultField、再 catch-all 合并命名字段词项，
第二次调用被水位丢弃 → 命名字段词项丢失。

### 第三层：wrote_default 互斥

`map_analyze`（`src/search/text_plugin.cpp:160-161`）对 kDefaultField 字段设置
`job.wrote_default = true`，导致 `apply_job_impl`（line 234）跳过 catch-all 合并：

```cpp
if (!job.wrote_default && !job.ca_data.empty()) {
    field_index(kDefaultField).add_doc(job.ord, job.ca_data);
}
```

即使不撞水位，catch-all 的结果也因 `wrote_default` 被跳过 → kDefaultField 只有
doc.text 的词项，命名字段词项不在默认字段中。

## 影响

| 内容来源 | 存入 doc.text | 存入 fields | 进倒排索引？ | search_text 命中？ |
|---------|:---:|:---:|:---:|:---:|
| 正文（body） | **是** | — | **否** | **否** |
| 标题（title） | — | **是** | 是（"title" 字段 + catch-all → kDefaultField） | 是（经 catch-all） |

用户被迫把同一份正文**同时存入 `doc.text`（供 `get_owned` 取回）和 `fields`（供索引）**
——违反 DRY，且 fields 存一份 text 段又存一份 = DocValue 膨胀。

## 复现

```cpp
DocInput doc;
doc.text  = as_bytes("正文内容含有特殊关键词独家秘方"sv);
doc.fields.emplace_back("title", as_bytes("文档标题"sv));
cask->put_doc(key, doc);

cask->search_text("独家秘方", 10);  // 期望命中，实际：空结果
cask->search_text("标题", 10);      // 命中（catch-all 把 title 词项合并进默认字段）
```

## 修复方向

kDefaultField 的所有词元（doc.text 正文词元 + 命名字段 catch-all 词元）必须在
**单次 `add_doc`** 中写入（水位幂等保护约束）。

### 推荐方案：map_analyze 中 kDefaultField 也纳入 catch-all

在 `map_analyze`（`text_plugin.cpp:154-182`）中，当 `index_catch_all` 开启时，
kDefaultField 的词项（来自 doc.text）也累积进 `ca_data`，而非走 `wrote_default` 直接写入：

- catch-all 开启时：kDefaultField 词项 + 命名字段词项合并进 `ca_data` →
  `apply_job_impl` 单次 `add_doc(kDefaultField, ca_data)` 写入全部词元。
  `wrote_default` 不置位 → catch-all 合并不被跳过。
- catch-all 关闭时：kDefaultField 词项走直接写入（保持现有 `wrote_default` 语义）。

### prepare 路由层：前置 doc.text 为 kDefaultField

`prepare()` 多字段路径中，当 `doc.text` 非空时前置 `{kDefaultField, doc.text}` 到
传入 `map_analyze` 的 fields 列表，使 doc.text 经 catch-all 统一处理。

### on_put 兜底路径同步

`on_put`（`text_plugin.hpp:118-134`）的单文本兜底分支（prepare 返回 nullptr 时）
也需要同步处理：当 fields 非空且 doc.text 非空时，不能只调 `apply_text(doc.text)`
（会与 fields 的 `apply_job` 撞水位）。

## 测试建议

```cpp
// 多字段 + doc.text：search_text 能同时命中 title 和 body
DocInput doc;
doc.text = as_bytes("正文有独特关键词"sv);
doc.fields.emplace_back("title", as_bytes("标题词"sv));
cask->put_doc(key, doc);
EXPECT_EQ(cask->search_text("独特关键词", 10).size(), 1);  // body 命中
EXPECT_EQ(cask->search_text("标题词", 10).size(), 1);      // title 经 catch-all 命中
```

## 关联

- S26-2 `index_catch_all` 配置开关（TASK.md S26 梯队）——本 bug 的修复与该开关交互：
  catch-all 关闭时 doc.text 仍需能进 kDefaultField（走直接写入路径）。
- `DocInput` 注释（`cask.hpp:233`）——修复后注释语义才真正兑现。
