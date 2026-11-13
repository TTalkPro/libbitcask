# S41 · fields 读回设计（zh-CN）

## 1. 背景与问题

`put_doc` 自 S8.6/S39 起已把命名字段编码进 `DocValue` v2 并写入盘上；检索侧也能用
`field:term` 命中。但读接口此前只交出 `text / meta / vector`：字段成了写侧投影，
宿主要取“某文档有哪些字段、字段原值”只能重新解析业务文档。该缺口来自
`feedbacks/2026-11-13-doc-capability-gaps.md` 第 2 条。

## 2. 目标 / 非目标

**目标**

- 显式文档读 API 能读回命名字段，并把盘上 field id 翻译回字段名。
- 普通 KV 点查 `get / get_owned` 的热路径与行为零变化。
- C++ 与 C API 同步补齐；C API 只走 additive 新结构 + 新函数，`SOVERSION` 不变。
- 盘上格式零变化，新旧目录互通。

**非目标**

- 不做 field 级 / meta 级部分更新。append-only 的写入单位仍是整条文档；覆盖写就是
  全量替换 fields。
- 不让 fields 变成通用二级存储。字段值保存在 DocValue 中，检索索引仍然只是派生投影。

## 3. API 契约

### C++

```cpp
struct DocFieldView { std::string name; std::span<const std::byte> value; };
struct DocField     { std::string name; std::vector<std::byte> value; };

Cask::get_doc_view(key);   // GetResultView.fields，值借用读缓冲
Cask::get_doc_owned(key);  // GetResult.fields，值独立拷贝
```

- `GetResultView` / `GetResult` 的 `fields` 追加在结构体末位，旧成员不前移。
- `get()` 继续不解析 fields；`fields` 恒空，避免 KV 点查热路径多付分配。
- `get_doc_view()` 共用 `get()` 的定位、TTL、CRC 与 merge-retry 路径，只是
  `want_fields=true` 时额外解码 fields。
- field id 无法翻译回名字（`field.schema` 缺项 / 代际不一致）返回 `kIo`。
  对文档读 API，DocValue 解码失败同样返回 `kIo`；不得静默给空结果。

### C

```c
bitcask_get_doc(cask, key, &doc, &fault);
bitcask_get_doc_result_free(doc);
```

`bitcask_get_doc_result_t` 携带 `value / meta / vector / fields / tstamp / ord`。
每个字段名与字段值独立分配，由配对的 free 释放。既有 `bitcask_get` 结果布局与
语义零改动。

## 4. 存储与生命周期

盘上仍使用 DocValue v2 fields 段：字段名经 `field.schema` intern 为小整数 id，
字段值原样保存。读回时不重扫索引，只在目标记录的 DocValue 字节上解码。

`get_doc_view()` 的字段值借用底层 `pread` 缓冲或 mmap 映射，生命周期与
`GetResultView` 绑定；`to_owned()` 深拷贝字段名与字段值。C API 直接使用
C++ owning 读结果物化，调用方释放前始终有效。

## 5. 失败与兼容

| 场景 | 行为 |
|---|---|
| key 不存在 / 墓碑 / 已过期 | `kNotFound` |
| data record CRC 失败 | `kBadCrc`（与 `get()` 一致）|
| DocValue 解码失败 | `get()` 保持空 span 旧契约；`get_doc*` 返回 `kIo` |
| field id 缺 schema 名 | `kIo`，不静默丢弃 |
| 文档覆盖写 | fields 全量替换；旧版本由 merge 回收 |

C API 的新符号是纯加法；既有 C 结构体、函数签名与调用序列不变，SOVERSION 保持 6。

## 6. 验收

- C++：fields 写入读回、owned 深拷贝、`get()` fields 恒空、覆盖写全量替换、
  重开后 schema 翻译成功、`search_fields` 原语义不回归。
- C：`put_doc_ex` 后 `get_doc` 读回 text/meta/fields，并验证 free 所有权路径。
- 无盘上格式版本变化，普通 `get` / C `bitcask_get` 零行为变化。
