# C6 设计:Roaring 位图元数据索引(filter / bool 加速)

> 状态:**预研,未立项(2026-07-11)**。本文是 C6(S10 审计 C 梯队)的重估与
> 完整设计——结论:当下无瓶颈信号,**触发条件成立前不做**(§1.3);届时按
> 本文形态实施,勿退化成「只换 bool_search 数据结构」的小改(§1.2 论证其
> 收益撑不起改动)。
>
> 关联:S30 段模型([`docs/design/s30-mmap-segments.md`](../docs/design/s30-mmap-segments.md),
> 不可变段 + mmap 是本设计的架构前提)、C4 MaxScore(top-k 主路径已提
> 21-46%,布尔/过滤是下一个可能的热点)。

---

## 1. 背景与立项判据

### 1.1 现状:filter 与 bool 的执行形态

- **元数据过滤**(`meta_filter.hpp`):查询带 `MetaFilter` 时走「先取打分候
  选(overfetch ~4k)→ 逐 doc `Index::eval_meta(ord, filter)` 在 shared_lock
  内对 meta blob 求值」(S13-P8 已把锁内拷贝优化成锁内求值)。**每查询
  O(候选数) 次逐条解码求值**;过滤选择率越低(命中越少),overfetch 放大
  越狠。
- **布尔查询**(`bool_search`/`bool_search_tree`):MUST 交集走 K1 k-way
  leapfrog + SIMD pairwise(`intersect_u64`,k=2 特化),must-only 有 BMW
  块跳跃;MUST_NOT 用排序数组 + binary_search 差集;树形求值用
  `set_intersection/union/difference` 物化中间集。
- **live 过滤**:每段一张平坦位图(S30 后 RAM 常驻 + sidecar 持久化),
  已是位图形态,**不在本设计范围**。

### 1.2 为什么「只换 bool_search 的集合结构」不值得

Roaring 对纯集合运算的优势在**稠密段按字批量位运算**(一条指令 64 个
docid)。但 bool_search 的端到端成本 = 集合运算 + **BM25 打分**(候选
平行分数数组 + 每词双指针归并)——打分不因集合结构变化而变。现有 SIMD
pairwise 在护栏基准(`BM_Inverted_SearchLockedScalar` 系)下健康,交集
本身不是可测瓶颈。把数组换成位图:

- 稀疏词(常态)下 roaring 退化为数组归并,无收益;
- 稠密热词下交集提速被打分稀释(Amdahl);
- 代价是双表示(posting 数组仍需——tf/positions 平行列)或转换成本。

**结论:C6 的真实价值载体是「元数据位图索引」——一个新功能,而非数据
结构替换。**

### 1.3 立项触发条件(满足其一才开工)

- 下游出现**分面/结构化过滤叠加全文**的真实负载(`status=x AND tag IN(…)`
  + search_text),且 profile 显示 `eval_meta` 逐条求值进入热点;
- bool_search 在真实负载下成为瓶颈且 MUST 词恒稠密(df/N > 1/64 量级);
- 出现「纯过滤计数/枚举」类查询需求(不打分,纯集合代数——位图的主场)。

无信号时本文封存。下文为触发后的实施设计。

---

## 2. Roaring 结构速览(实现所需的最小集)

docid 空间按高 16 位分桶(每桶覆盖 64K docid),桶内按密度选容器:

| 容器 | 条件 | 存储 | 集合运算 |
|---|---|---|---|
| array | ≤4096 个 | 排序 u16 数组 | 归并/galloping |
| bitmap | >4096 个 | 8KB 位图 | 按 u64 字批量 AND/OR/ANDNOT |
| run | 长连续段 | (start,len) 对 | 区间代数 |

**本设计只需 array + bitmap 两种容器**(run 是可选优化;段内 docid 稠密
连续的场景主要来自「几乎全命中的值」,bitmap 已近最优)。序列化采用
**frozen 布局**(容器目录 + 顺序容器体,均对齐)——mmap 后零解析直读,
与 S30 段文件一次写永不改的契约完全一致。

依赖选型:
- **方案 A(推荐):自实现 array/bitmap 两容器子集**。所需运算仅
  AND/OR/ANDNOT/求交计数/迭代,~500 行 + 对拍测试;仓库保持零新第三方
  依赖(现状仅 TBB/zlib/cppjieba),frozen 格式自定义进 v2 段节。
- 方案 B:引入 CRoaring(成熟、含 frozen_view)。代价:新 submodule、
  格式与库版本耦合。仅当后续需要 run 容器/SIMD galloping 等高级优化时升级。

---

## 3. 核心形态:per-segment 元数据位图索引

### 3.1 数据模型

对**声明为可过滤**的元数据字段(opt-in,见 §3.5 配置),在段内建:

```
(field_id, value) → RoaringBitmap<段内 docid>
```

- value 域:字符串/整型枚举值直接作 key;数值范围查询(§3.6)首期不做,
  仅等值与 IN(与现 MetaFilter 的主用法一致)。
- 位图内容 = 该段内 meta[field]==value 的全部 docid(**不含 live 语义**
  ——删除经查询期 `AND live位图` 叠加,与 posting 的 live 过滤同哲学:
  段文件不可变,删除不改索引)。

### 3.2 段格式(v2 新节)

```
[kMetaBitmap 节](per segment,可缺席 = 该段无位图索引)
  u32 magic 'BMBI' | u32 version
  u32 entry_count
  目录(定长,按 (field_id, value) 升序,mmap 二分):
    { u32 field_id, u64 value_off, u32 value_len,   // value blob 内偏移
      u64 bitmap_off, u32 bitmap_len, u32 cardinality }
  [value blob]
  [frozen bitmaps](对齐;array 容器 = u16 数组,bitmap 容器 = 8KB 字对齐)
```

- 复用 v2 的节表/CRC/footer 机制(`segment_v2.hpp` Section 枚举扩一项);
- **写路径**:封口时(`write_segment_v2_streams` 增一个可选 source)从
  doc_store 的 meta blob 逐 doc 提取声明字段 → 内存态构建(段有预算上界,
  瞬态可控)→ frozen 序列化流式写;
- **merge**(S30-P3):docid 重编后直接**重建**(遍历输出段 doc_store 一
  趟)——比位图 remap 简单且顺带物理清除死 doc 的位;
- v1 段:不支持(查询时该段回退逐条 eval_meta;v1 随 merge 自然消亡)。

### 3.3 读路径

1. `MetaFilter` 表达式编译为位图代数:叶子 `(field,value)` → 目录二分取
   frozen 位图(缺席 → 空位图/该字段未索引 → **整段回退逐条求值**,
   语义恒正确);AND/OR/NOT 逐层求值,NOT 实现为 `全段位图 ANDNOT x`
   (全段 = [0, doc_count) 的 run 语义,用 bitmap 容器物化或惰性表示);
2. 结果 `AND live位图` → 段内匹配集 M;
3. 与打分融合(两档,按选择率自适应):
   - **M 小**(≤ ~k×常数):直接对 M 内 docid 逐个取 posting 打分
     (filter-first,免 overfetch);
   - **M 大**:照常 BOW/MaxScore 打分,`LiveChecker` 换成
     `live AND M` 的组合视图(位图 contains 测试 O(1),替代逐条
     eval_meta)——现有 `fill_is_live` 批量接口直接受益。
4. bool_search 次级应用:MUST_NOT 排除集若来自元数据条件,直接用位图;
   纯词项交并保持现状(§1.2)。

### 3.4 一致性与失效

- 段不可变 ⇒ 位图不可变;删除经 live 叠加;覆盖写 = 旧版本 doc 死亡
  (live 位翻转),位图无需维护——**零失效逻辑**,这是段模型给本设计的
  最大红利;
- building 段(内存态,持续写):**不建位图**,查询对 building 回退逐条
  eval_meta(building 有预算上界,量小);
- 字段声明变更(新增可过滤字段):只影响新封口的段;旧段该字段缺席 →
  回退路径,merge 重写后补齐。

### 3.5 配置

```cpp
// SearchConfig / TextPluginConfig(镜像现有开关形态)
std::vector<std::string> meta_bitmap_fields;  // 空(默认)= 关闭本特性
```

opt-in 字段白名单——高基数字段(如 URL、时间戳原值)不适合建位图
(entry 爆炸),由使用方声明;可加护栏:单段 entry 数上限,超限该字段
本段降级回退并计数上报。

### 3.6 明确不做(首期)

- 数值范围查询(需 BSI/分桶位图,另立项);
- posting 本体位图化(tf/positions 平行列不适配,Lucene 亦不做);
- building 段实时位图(失效复杂度不值,见 §3.4);
- run 容器与 SIMD galloping(方案 A 子集先行,数据说话再升级)。

---

## 4. 验收标准(立项后)

1. **正确性**:随机语料 × 随机 filter 表达式,位图路径 vs 现逐条
   eval_meta 路径**结果集逐一致**(含删除/覆盖/多段/merge 后);
   段 CRC/回退路径(未索引字段、v1 段、building)全覆盖;
2. **性能**:新 bench——N=10 万 doc × 选择率 {0.1%, 1%, 10%, 50%} ×
   filter 复杂度 {单值, AND×2, OR×4}:
   - 目标:低选择率(≤1%)过滤查询端到端 **≥5×**(消 overfetch +
     逐条求值);高选择率不劣化(自适应第 3.3-3 档);
   - RSS:位图节走 mmap,常驻仅目录(KB 级/段);
3. **三 sanitizer 全量 + 双树构建**(仓库既定门槛)。

## 5. 实施计划(触发后,预估 2-3 会话)

- **P1** 位图子集(array/bitmap 容器 + frozen 格式 + 代数运算)+ 对拍
  测试(vs std::set 参照,随机 10⁵ 轮);
- **P2** v2 段节(writer source + reader 目录二分)+ 封口/merge 构建 +
  round-trip 测试;
- **P3** MetaFilter 编译器 + 查询融合(两档自适应)+ 回退矩阵 + bench
  + 默认灰度(白名单空 = 关闭,行为零变化)。

## 6. 风险

- **entry 基数失控**:高基数字段白名单误配 → 段文件膨胀。护栏:per-段
  entry 上限 + 降级计数;
- **回退路径分叉**:位图路径与逐条求值路径语义漂移。对策:等价性随机
  对拍是常驻测试(同 C4 三方对拍模式);
- **提前优化**:再次强调 §1.3——无触发信号不开工。
