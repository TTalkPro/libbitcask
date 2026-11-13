# 反馈 2026-11-13：文档能力四项缺口 —— fields 读回 / 部分更新 / 多值 meta / 字段命中归属

来源：锦书（clj，经 clj-bitcask）存储再重构设计 §Q-21「bitcask 从可删除的索引升为
真相源」（全文见上游 keelson 仓库 `feedbacks/2026-11-13-bitcask-doc-fields-gaps.md`）。
设定卡走「顶点 = 文档 = 真相」：整卡 md 是 value，H2 节名→节正文是 fields，
坐标标量是 meta，出边是图边。核实过程中确认四项文档能力缺口，逐项给出现状与请求。

## 1. search-fields 命中不带「哪个字段命中」（优先级：高——决定一处派生存储能否退役）

**症状**：命中统一 `{:key :ord :score}`，`search-fields` 查 `field:term^boost`
但回包不说命中的是哪个字段。

**怎么撞上的**：锦书设定集节级搜索要回答「哪张卡的第几节」，只能维持派生的
`codex_section` chunk（一节一块、单独同步、单独占存储）。

**建议**：字段检索的命中附命中的字段名列表，最好再带逐字段 snippet。

**回没回**：**已修（6.8.0，归属部分）**：`SearchFieldHit{key, ord, score, fields}` +
`TextPlugin::search_fields_attributed` / `Cask::search_fields_attributed`；
C API 纯加法 `bitcask_search_fields_ex` / `bitcask_search_fields_ex_filtered` /
`bitcask_search_fields_result_free`。`fields` 去重、首次命中序；默认字段
（text / catch-all）记空串。逐字段 snippet 未做。详见 CHANGELOG。

## 2. fields 只进索引、不可读回（优先级：中）

**症状**：`->doc` 把 fields 编码进 `DocValue` v2 写入（已核实盘上有存），但读侧
只有 `Kv.get` / `Kv.read`（text + meta + vector），没有任何 get-doc / read-doc——
fields 成了纯写侧投影，读侧享受不到结构化，「按节读回」只能重新解析整卡 md。

**建议**：文档读回 API——get-doc / read-record 带 fields（至少可选返回）。

**回没回**：**已修（6.8.0）**：C++ 侧 `Cask::get_doc_view()` /
`Cask::get_doc_owned()`，`GetResultView` / `GetResult` 末位加 `fields`；C API 纯加法
`bitcask_get_doc` / `bitcask_get_doc_result_free`。盘上 field id 经 `field.schema`
翻译回原名；schema 缺名 / DocValue 解码失败按 `kIo` 显式失败。普通
`get()` / `bitcask_get()` 不解码 fields，热路径与布局零改动。逐字段写侧 patch 更新
未做——覆盖写仍是全量 fields 替换。

## 3. 文档无部分更新（优先级：中）

**症状**：`put_document!` 是整文档覆盖写；没有 field 级 / meta 级更新原语。
改一个坐标标量要把整卡 md + 全部节 fields 重写重索引——append-only 引擎上纯写放大。

**建议**：`update-doc!`（或 put-doc 的 merge 语义）：只改 fields/meta 的若干键，
不动 value/text，引擎内原子完成读-改-写（写锁内）。

**回没回**：未回。

## 4. meta 值只能是标量（优先级：低）

**症状**：meta 值类型只收 nil / 布尔 / 整数 / 浮点 / 字符串；设定卡的**别名**天然
多值，拼串只能 substring 碰运气、挪 fields 又不可过滤。

**建议**：字符串数组（或任何多值）meta 类型，filter DSL 相应支持「包含」语义。

**回没回**：未回。

## 对等与兼容提醒

- 以上若动原生（`bitcask_put_doc_ex` / meta 编码 / 检索回包），clj 与 C# 两侧要同步
  （锦书 K-22 字节级对等；盘上格式互通是既定性质，新 meta 类型 / 新回包字段都要考虑
  旧库打开）。
- 锦书侧不阻塞：§Q-21 分刀按「今天的绕法」开工，各项落地后逐项收割。
