# S30 设计:封口段 mmap 化 + 写路径 RAM 预算封口(倒排索引出内存)

> **批次收官(2026-07-11)**:P1(格式/reader/TermIndex 接线,WAND 块游标
> 留作优化挂账)+ P2(封口即 mmap 出内存 + 预算封口,默认开启)+ P3
> (tiered merge,S27-3 consolidation 了结)+ P4(RSS 实测 60k/120k 文档
> **-49%/-55%**,bench 零回归,S21-A6 落地,三 sanitizer 全量)。

> 状态:**P1 Slice 1 已落地(2026-07-11)**——共享评分实现抽取(bm25_search_impl.hpp)
> + v2 格式/流式 writer/MmapSegment reader 核心(segment_v2.hpp/.cpp),round-trip
> 与内存段**逐位一致**(200 轮随机 WAND 对拍),clang/ASan 全量 589/589。
> **P1(Slice 1/3/4)+ P2 已落地(2026-07-11)**:v2 格式/reader 全查询面/
> TermIndex 接线/SealedSegment mmap 背衬/**写路径换入 + 预算封口 + ckpt 收窄
> (v2 封口默认开启)**。既有 crash/checkpoint/builder 全量测试群默认跑在新
> 写路径上,clang/ASan 604/604。**P3 tiered merge 亦已落地(2026-07-11)**:
> k-way 流式合并(docid 重编+死行物理回收+统计自愈,金标位级等价)+
> fan_in 策略 flush 静止点换入,S27-3 consolidation 挂账就此了结,v1 段随
> merge 自然迁移 v2(clang/ASan 607/607)。剩余:WAND 块游标(interim,
> 非阻塞)、P4 RSS bench 收尾。进度详见 TASK.md S30。
> 对标 Lucene/ES 段式索引的标准内存模型。
>
> **动机**:倒排索引当前全量驻留内存——SoA 后仍 ≈16B/posting(+positions
> ~4-5B/词位),1M 文档 × 500 唯一词 ≈ 8-10GB;wiser-cpp 实测 checkpoint 单次
> flush 1.7GB(S26-⑥)。两个内存尖峰:① 封口段常驻(载入时把盘上压缩格式
> 全量解码回 vector);② **写入期间** building 段无大小预算(只在 ckpt 封口)
> + save() 先把全段序列化进内存字节缓冲再落盘(双份驻留)。
>
> **目标内存模型**(Lucene 形态):
> - 封口段 = 不可变盘文件,查询经 **mmap 按需解码**,冷数据由 OS page cache
>   自动换出 → 常驻 RSS = 热工作集;
> - building 段 = **RAM 预算封顶**(超预算即封口落盘换 mmap,不等 ckpt);
> - 落盘 = **流式写**(增量 CRC,无全段内存缓冲);
> - checkpoint = 只提交清单(段文件早已在盘上)→ 1.7GB flush 停顿消失。
>
> **常驻内存清单(改造后)**:building(≤ 预算 × builder 数)+ 每段:live
> 位图(1B/doc)+ 词典块索引(~KB)+ 文件句柄;每库:key→(seg,docid)
> resolver(O(keys),独立轴,本批不动)+ TermSnapshotCache(S29-6B,
> 线程级 ~MB,自动变身「解码缓存」)。

## 0. 前提盘点(全部已具备)

| 前提 | 出处 |
|---|---|
| 封口段不可变(唯 live 位可变) | S27 核心不变量,`segment.hpp` 头注释 |
| 盘格式已是列式压缩块(ord FOR 128/块、tf/dl VByte、positions gap-varint) | v6 格式,`inverted.cpp` save/deserialize |
| BOW 查询本就拷快照(`snapshot_flat`→`FlatPostings`) | P1/S23-M3,改「从 mmap 解码进同一 thread_local」即天然替换 |
| 解码结果缓存 | S29-6B `TermSnapshotCache`——封口段 gen 恒 0 → 热词解码一次永久命中 |
| 段生命周期 vs 查询并发(shared_ptr pin,drop 只摘清单) | S27-3 步骤 5——munmap 延后到引用归零同款模式 |
| 清单单一提交点 + 孤儿段文件 open 忽略 | S27-3 步骤 1/4——**提前封口天然崩溃安全** |
| 流式硬件 CRC(16-63B 小块内核) | S29-10 `hw_crc32.hpp` |
| 封口按槽参数化(`flush_building_slot`) | S27-4 P3 |

## 1. 盘格式 v2(mmap-native,每段一文件)

现格式(SearchCheckpoint 容器内嵌 `InvertedIndex::serialize` 字节流)是
**顺序流**——词典无目录,必须整体解码。v2 改自描述分节 + 可点查:

```
[Header]     magic/version/seg_id/doc_count/字段数/节目录(各节 off/len/CRC)
[FieldDir]   每字段:{field_name, TermDict/Postings/Blocks/Positions 节引用}
每字段:
  [TermDict]   排序词条:term blob(len 前缀连续存)+ 定长目录记录
               {term_off u32, term_len u16, df u32, max_tf u32,
                postings_off u64, blocks_off u64, blocks_cnt u32,
                pos_off u64}——mmap 上直接二分(先比 blob,S24-M9 词典
               双份常驻对封口段就此消灭)
  [Postings]   per term:docid 列 FOR 128/块(**u32 段内 docid**,盘上减半;
               解码时拓宽 u64 喂现有内核)+ tf/dl 列 VByte
  [Blocks]     per term 跳表:{base_docid u32, end_docid u32, max_tf u32,
               min_dl u32, packed_off u32} × ⌈N/128⌉——WAND 块游标随机跳块
  [Positions]  gap-varint(index_positions=false 时节缺席)
[DocStore]   平坦定长行(docid→{key_off, lsn, DocSlot})+ key blob——现
             kSegDocStore 本就定长,直接 mmap;lsn 列只读映射
[Footer]     总 CRC + 节 CRC 清单(S21-A6:载入验一次,opt-in 跳过)
```

- **live 位图不进段文件**:独立小 sidecar(`<seg>.live`,doc_count/8 字节
  + CRC,ckpt 时 tmp+rename 重写)。替代现 `resave_dead_dirty` 整段重存
  ——mark_dead 只改 RAM 位图 + 脏标,ckpt 落 sidecar,open 时段文件叠加
  sidecar。段文件严格一次写永不改。
- u32 docid 上限 42 亿/段:预算封口(§3)保证段大小,恒安全(断言兜底)。

## 2. 读路径:`MmapSegmentReader`

实现 SealedSegment 的查询面(SegmentView/MultiFieldSegmentView 契约不变,
`multi_segment_search`/`multi_field_segment_search` 零改动):

- **doc_freq(term)**:词典 mmap 二分 → 目录记录的 df。O(log T) 纯读。
- **BOW search**:二分词典 → FOR 块解码进 thread_local `FlatPostings`
  (u32→u64 拓宽,blocks 从跳表节直读)→ 现 `score_bow_topk` 内核零改动。
  解码结果落 **TermSnapshotCache**(S29-6B):封口段 gen 恒 0 → 热词
  **解码一次、永久命中**——缓存从「省 4 次 RMW」升格为「省整趟解码」。
- **WAND search**:新 **BlockCursor**——按跳表逐块解码(128 条/scratch,
  thread_local 复用),上界/跳块逻辑复用 `upper_bound_from`/`block_for_ord`
  形态。这是本批工程量最大的一块(现 `search_wand` 裸读 PL 数组)。
- **phrase/near**:positions 节按 (term, 行) 偏移 gap 解码,候选交集后
  按需取——现路径本就先交集后读位置,访问量小。
- **explain/df_live/wildcard/fuzzy**:wildcard/fuzzy 需词表区间扫——排序
  词典 mmap 上前缀二分 + 顺序扫,**优于**现 vocab_ 侧表(顺带消灭封口段的
  ensure_vocab/vocab_ 全套,S13-F6/S24-M9 机制只剩 building 需要)。
- 段对象持 mmap 区 + RAM 侧:live 位图、词典块索引;`shared_ptr` pin 延后
  munmap(析构时解映射,drop 只摘清单——现成模式)。

## 3. 写路径:RAM 预算封口 + 流式落盘(用户硬需求)

- **预算封口**:`TextPluginConfig::seal_ram_budget`(默认建议 64MB/builder;
  0=沿用现行为仅 ckpt 封口)。building 段增量记账(posting 行数 × 行宽 +
  pos_data + 词典近似);apply 路径检测超预算 → **就地封口**
  (`flush_building_slot` 现成,builder 模式下由该 builder 对自己的槽执行,
  单写者不变量保持)→ 流式落盘 → `add_pending` 登记 → **换入
  MmapSegmentReader、释放内存副本**。稳态写入 RSS = 预算 × B + 在途一段。
- **流式 save**:v2 writer 逐节写文件(S29-10 流式 CRC 增量算,节目录/
  footer 最后补写),**无全段字节缓冲**——现 `serialize(vector<byte>&)` +
  SearchCheckpoint owned bufs 的双份驻留消失。峰值额外内存 = 单块编码
  scratch(KB 级)。
- **checkpoint 语义收窄**:段文件在封口时已落盘,ckpt 只做 ① 各段 live
  sidecar 落盘 ② 封口当前 building(尾段)③ 提交清单(kSegManifest,
  单一提交点不变)。S26-⑥「1.7GB flush 停顿」自然消解。
- **崩溃安全**(不变量沿用):封口早于 ckpt 的段文件 = 未提交孤儿 → open
  忽略/GC;WAL(data file)从 ckpt 水位重放重建 → 无数据丢失。「文件序 ==
  ord 序」不受影响(封口只动索引侧)。
- **读己之写**:换入 mmap reader 前段已可查(building 查询路径);换入是
  原子指针替换(查询快照式收集段列表,现成)。

## 4. 段数收敛:tiered merge(与跨段 consolidation 挂账合并)

预算封口 ⇒ 段数随写入量线性增长 ⇒ 查询扇出涨。本批必须带段合并
(S27-3/4 挂账「跨段 consolidation + legacy 段化迁移」正好在此落地):

- **size-tiered 策略**(Lucene TieredMergePolicy 简化版):同量级 ≥K 段
  触发合并,死点占比高的段优先;
- **流式 k-way merge**:输入段词典有序 → k 路归并逐 term 流式写输出段
  (docid 重编:旧 (seg,docid) → 新段稠密 docid;doc_store 行拷贝 + 死行
  跳过 = 物理回收),**有界内存**(每输入段一个块游标 + 输出编码 scratch);
- resolver(key→(seg,docid))批量改指新段;清单一次提交换入/摘除;
- **legacy 段化迁移**顺带:老格式 ckpt → 首次 open 时流式转写 v2 段
  (替代现「退全量 fold 重建」)。
- 词表遍历原语:mmap 词典天然有序可遍历——挂账里「需 InvertedIndex 词表
  遍历原语」的前置条件被 v2 格式免费满足。

## 5. 分阶段实现(每相独立可验收)

- **P1 格式 + 只读 reader**(~2-3 会话):v2 writer(流式)+
  MmapSegmentReader 全查询面(BOW/WAND 块游标/phrase/doc_freq/explain/
  wildcard/fuzzy)。隔离验收:内存段 → v2 落盘 → mmap 载入,全查询与内存
  段**逐位一致**(镜像 S27-2 SealedSegmentRoundTrip 方法论);WAND 剪枝
  等价性对拍;CRC 篡改拒载。SealedSegment 双实现并存,live 路径不动。
- **P2 写路径接线**(~1-2 会话):预算封口 + 封口→流式落盘→mmap 换入→
  释放内存副本;live sidecar;ckpt 清单化。验收:写入压力下 RSS 曲线
  封顶于预算(新 bench 记录 max RSS);crash 矩阵(封口后-ckpt 前 kill →
  孤儿清理 + WAL 重放等价);TSan 换入 vs 并发查询。
- **P3 tiered merge + legacy 迁移**(~2 会话):§4 全部。验收:合并前后
  查询逐位一致、死点物理回收计量、段数收敛曲线;老 ckpt 升级路径。
- **P4 收尾**(~1 会话):三 sanitizer 全量;`BM_Inverted_*`/
  `BM_Cask_PutDocTextIndex`/SearchHybrid 无回归 + 新增冷/热查询延迟与
  RSS bench;S21-A6 opt-in 跳 CRC 顺带;TASK.md/设计文档归档。

## 6. 风险与权衡

- **冷查询长尾**:page cache miss → 毫秒级 IO(Lucene 同款权衡)。缓解:
  词典/跳表节 `madvise(WILLNEED)` 或 mlock(小);TermSnapshotCache 兜热词。
- **WAND 块游标复杂度**:最大单项;先对拍内存实现保逐位一致再切换。
- **mmap 生命周期**:SIGBUS(文件被截断/盘错)——段文件一次写永不改 +
  打开时 CRC 验过,残余风险接受(与读 data file 同级);munmap 延后由
  shared_ptr pin 保证(现成模式)。
- **building 仍内存态**:预算封顶后这是有意保留(在建段需可变结构);
  预算调小可换更低 RSS(更多段 → merge 压力,tiered 策略消化)。
- **不动的轴**(明确非目标):KV 主路径(WAL/keydir/hint)零变化;HNSW
  向量常驻另立项;key→location resolver 的 O(keys) RAM 不在本批。

## 7. 收编的既有挂账

S26-⑤(position delta-varint——v2 沿用 gap-varint 落盘且内存不再常驻,
自然了结)/ S26-⑥(ckpt 流式/停顿——§3 消解)/ S21-A6(跳 CRC opt-in)/
S24-M9 后半(封口段词典双份——§2 消灭,building 侧保留)/ S27-3 挂账
(跨段 consolidation + legacy 迁移——§4 落地)。
