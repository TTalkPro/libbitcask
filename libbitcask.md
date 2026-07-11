# libbitcask 缺陷反馈：超长 term 导致整个 segment 静默检索失效

- **提交日期**：2026-07-11
- **libbitcask 版本**：`main` @ `3d492e5`（S29-6B）
- **严重级别**：高 —— 单条脏数据使整段索引对**所有**查询静默返回 0 条，无任何错误上报
- **发现场景**：wiser-cpp 用 libbitcask 索引 zhwiki（`doc.text = 正文`，`doc.fields = {title}`），2000 篇后任何查询都搜不到东西

---

## 一、现象

用真实 zhwiki 数据索引 2000 篇后，任意查询（哪怕是几乎每篇都出现的 `的`）都返回：

```
Total 0 documents found.
```

- 没有报错、没有 `kAnalyzerMismatch`、没有 CRC 失败提示。
- KV 数据完好：`get_owned` / `parallel_scan` 能正常取回全部正文，`analyze` 统计正常。
- 盘上文件结构、`bitcask.meta`、`field.schema`、`bm25.ckpt`、`segments.manifest`、`index.manifest` 全部字节级正常，`bm25_segments/seg-0.seg`（约 200 MB）也在。

即：**数据和索引都写下去了，但检索一条都命中不了**。

---

## 二、二分定位

用当前 wiser 二进制，从同一份 XML **全新索引**，逐步缩小规模：

| 索引篇数 | `的` 命中 | seg-0.seg 大小 |
|---------|----------|---------------|
| 200 | 10 | 41 MB |
| 400 | 10 | 67 MB |
| 495 | 10 | 80,238,274 B |
| **499** | **10** | 80,439,450 B |
| **500** | **0** | 81,608,390 B |
| 700 / 1000 / 2000 | 0 | 96 MB … 213 MB |

**精确断点：第 500 篇。** 前 499 篇检索完全正常，第 500 篇一进去，整段索引对所有词全部失效。且**可稳定复现**（与二进制、与是否重建 checkpoint 无关）。

第 500 篇 = **清华大学**（正文 354,989 字符）。

---

## 三、根因

### 3.1 写端不限长，读端限长 1024，两端不对称

**写端**（`src/bm25/inverted.cpp` `InvertedIndex::serialize`，约第 2371 行）：term 字节长以 u32 写入，**无任何上限校验**：

```cpp
auto tlen = static_cast<std::uint32_t>(term.size());
write_u32(tlen);
put(term.data(), tlen);
```

**读端**（`src/bm25/inverted.cpp` `InvertedIndex::deserialize`，约第 2536 行）：term 长度 **> 1024 即判定为损坏，返回 false**：

```cpp
auto tlen = read_u32();
if (tlen == kReadFail32 || tlen > 1024) { return false; }
```

### 3.2 一个坏 term → 整段报废 → 全量静默 0 结果

`deserialize` 的 `false` 逐层上抛，最终使整个 segment 无法加载：

```
InvertedIndex::deserialize  → false
  └─ SealedSegment::decode_fields (include/bitcask/segment.hpp)  → false
       └─ 该 SealedSegment 加载失败
            └─ segment_set_ 没有可查询的段
                 └─ search_text / bool_search / … 对所有词返回空
```

**两个致命点**：

1. **炸整段**：只要**一个** term 超过 1024 字节，`decode_fields` 就丢弃**整个** segment（含其余全部正常 term），而不是跳过这一个坏 term。
2. **静默**：失败没有任何上报路径。调用方拿到的是「成功返回、0 命中」，与「真的没匹配」完全无法区分——排障极其困难。

### 3.3 触发数据

第 500 篇「清华大学」的原始 wikitext 里，有 **38 个不含空白、字节数 > 1024 的连续串**（`{{#invoke:Cite ...}}` 引用模板、长 URL、`<ref>` 块等）。Jieba 分析器把其中一个切成了**单个 1477 字节的 token**。

用打桩版 `deserialize` 确认（在各 `return false` 处加 `WISER_DBG` 日志后重建 libbitcask）：

```
[invdeser] tlen=1477 bad (t=1058/14519)
```

即默认字段第 1058 个 term（共 14519 个）长 1477 字节 > 1024，触发返回 false。

> 逻辑闭环：500 篇规模下，`kMaxPostingsPerTerm`(1<<24≈16M) 和 `kMaxBlocksPerTerm`(1<<17≈131k) 都**不可能**达到（每 term postings ≤ 500，blocks ≤ 4）。唯一能触碰的上限就是 **term 长度**。所以断点必然由超长 term 引起——数据也印证了这一点。

---

## 四、复现步骤

```bash
# 用 doc.text=正文 + doc.fields={title} 的方式索引 zhwiki 前 500 篇
wiser index -x zhwiki-latest-pages-articles.xml -o /tmp/db500 -m 500
wiser query /tmp/db500 的      # → Total 0 documents found.

# 前 499 篇则正常
wiser index -x zhwiki-latest-pages-articles.xml -o /tmp/db499 -m 499
wiser query /tmp/db499 的      # → 10 hits
```

打桩确认（可选）：在 `InvertedIndex::deserialize` 各 `return false` 前加
`std::getenv("WISER_DBG")` 守卫的 `fprintf`，重建后 `WISER_DBG=1 wiser query /tmp/db500 的`
即可看到 `tlen=1477 bad`。

---

## 五、解决方案（方案 A：在 libbitcask 侧修复）

核心目标：**让 serialize / deserialize 对称**，且**任何单条脏数据都不能让整段静默报废**。三个子方案：

### A1 —— 写端跳过超长 term（改动最小，推荐）

在索引写入路径（`add_doc` / analyze 产出 term，或 `serialize` 落盘前）丢弃字节长 > 上限（如 1024）的 term。超长 token 本质是噪声（URL / 模板块 / base64 串），对检索无价值，不该进倒排。

- 优点：源头对称，`deserialize` 那条 1024 上限从此永不触发；改动集中、风险低。
- 注意：若在 `serialize` 里跳过，需同步修正先写的 `term_count`（否则数量对不上）。更干净的做法是在 term **进入索引之前**就过滤，使内存态本身不含超长 term。

### A2 —— 抬高 / 变长化读端上限

把 `deserialize` 的 `tlen > 1024` 上限调大，或改用变长编码承载更长 term。

- 优点：不丢数据。
- 缺点：1024 本是防御性 sanity 上限，单纯抬高是治标；且超长噪声 term 仍会进倒排、白占内存与磁盘。建议只作为兜底，不单独使用。

### A3 —— 读端容错 + 失败可见（关键，务必配合）

改变「一个坏 term 炸整段 + 静默」这一最危险的行为：

- `deserialize` 遇到超长 / 异常 term 时**跳过该 term 继续**，而不是让整个 segment 失败；
- segment 加载失败或发生跳过时，**必须上报**（返回带原因的错误 / 计数器 / 日志），杜绝「成功返回、0 命中」这种无法区分真空结果与加载失败的静默失效。

---

### 推荐组合：**A1 + A3**

- **A1** 保证写端不再产生坏 term —— 正常流程下 `deserialize` 永远不会遇到超长 term；
- **A3** 保证即使遇到任意脏数据（历史库、外部写入、格式演进），也只丢一个词、不炸整段，且失败可见。

二者叠加后，libbitcask 对任意输入都健壮，`search_text` 不会再出现「静默全量 0 结果」。

---

## 六、附注

- libbitcask 目前 `AnalyzerConfig` 只有 `min_token_length`、**没有 `max_token_length`**，所以纯配置层无法规避，必须改代码。可考虑顺带补一个 `max_token_length`（与 A1 配套，把「多长算超长」变成可配置）。
- 修复后，已有的坏库（如本例 2000 篇的 `wiki` 目录）**必须重新索引**——坏 segment 已落盘，改代码不会自动修复旧数据。
- 本反馈已剔除全部诊断打桩，libbitcask 子模块保持干净。

---

## 七、处理回执（libbitcask，2026-07-11，S31 批次）

已按 **A1 + A3 组合**修复并合入 `main`（详见 TASK.md S31 批次）：

1. **A1 已实现**：`AnalyzerConfig` 新增 `max_token_bytes`（默认 **1024**，`0` = 不限）。
   Ngram / Whitespace / Jieba 三个分析器在 token 发射点统一丢弃超限 token
   （pos 仍递增，位置语义与 `min_token_length` 一致）。正常流程不再产生超长 term。
2. **A3 已实现（两层）**：
   - **读端容错**：v1 `deserialize` 遇超长 term **完整解析后跳过该词**，不再整段
     `return false`；跳过计数经 `InvertedIndex::load_skipped_oversized_terms()`
     暴露（>0 即提示历史坏库）。
   - **失败可见**：段集装载失败（清单声明了段但载不出——任何成因）不再静默落
     空集，而是 **watermark 归 0 触发宿主全量重放重建**。「成功返回、0 命中」的
     静默失效模式已从根上消灭。
3. **对你们现存坏库的影响**：升级 libbitcask 后**无需手动重建**——
   - 原 2000 篇 wiki 目录重开即可查（读端跳过那 1 个 1477B 噪声词，其余
     14518 个 term 完好）；
   - 若希望物理清除盘上超长 term，可重建索引，或等段 merge 自然重写。
4. **附注**：S30 批次（同日）已将默认封口格式切换为 v2（mmap），其读端本就无
   term 长度上限，对本类问题天然免疫；v1 仍完整支持读取。

复现测试已入库：分析器过滤 ×3、v1 跳词容错、wiser 形态 e2e（v1/v2 双模）、
段损坏响亮降级，共 5 例（`analyzer_test` / `jieba_analyzer_test` /
`inverted_test` / `text_plugin_test`）。

## 八、跟进（2026-07-11，S31.5）：自动 checkpoint 锚点修正

下游进一步指出：ckpt 若只锚在数据文件 roll / close，大文档语料崩溃后需重放
巨量**分词**（恢复的主成本）。已修正：

- 自动 checkpoint 的锚点从「roll（字节）」改为 **ord 增量本身**——每写评估，
  自上次 ckpt 起增量 ≥ `auto_checkpoint_min_docs` 即异步提交（reducer 内
  fire-and-forget，不阻塞写者）；
- `auto_checkpoint_min_docs` **默认从 0（关）改为 65536**，与 building 段
  封口阈值对齐——段封口后至多一个阈值周期，清单必然提交；
- 效果：崩溃恢复的重放（重分词）窗口恒 ≤ 64K 文档，与语料单篇大小无关。
  wiser 场景下可按需调小（如 8192）进一步收紧窗口。
