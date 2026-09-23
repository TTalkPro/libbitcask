# S40 设计:检索 key 内存单实例化(四份驻留 → 两份) + 开库去 key 字符串分配

> 状态:**已落地(2026-09-23,随 6.6.0 发布;落地记录见 §13)**,设计 r2(6.6.0
> 尚在开发期,本批并入同一版本,不另起 MINOR)。r2 修订点见文末「修订记录」。
> 来源:下游 keel 转 coxswain 的
> `feedbacks/2026-09-23-search-open-resident-memory-no-c-api-knob.md`——6.6.0
> 修掉了该 feedback 的前两条(段 CRC 扫描、ckpt 整读),第三条「同一批 key
> 住多份」被标注为**结构性,未动**,即本设计。
>
> **动机(复核后比 feedback 记的更多一份)**:enable_search 的库,同一批 key
> 字节在堆上驻留 **四** 份,不止 feedback 所记三份——
> ① KeyDir(权威,KV 层);② `Index::ext2ord_` 节点(`index.hpp:292`);
> ③ `Index::Chunk::ord2ext` 每 ord 一个 `std::string`(`index.hpp:86`);
> ④ `TextPlugin::key_to_location_` 节点(`text_plugin.hpp:512`)。
> (另:building 段的 `keys_` 对未封口文档也持一份,受封口阈值约束,有界;
> 开 Level B `keydir_cache_entries` 时 ① 本就不全驻留。)
> 另有开库 `rebuild_key_locations()` 每 live doc 一次 key string 构造
> (`text_plugin.cpp:1363`)与写路径每次 upsert/delete 的临时 string
> (`text_plugin.cpp:122/144/303/327/398/907`)。
>
> **目标内存模型**:live key 只保留**两份 owned**(KeyDir 权威 + ext2ord_
> 权威),③④ 降级为 `string_view`(16B/份,零堆分配);开库重建与
> 写路径热循环不再构造 key 字符串。收益估算见 §1.1(公式),以 RSS probe
> 实测为准(沿 S30-P4 手段)。
>
> 对标:S30 设计稿 §常驻内存清单里,key→(seg,docid) resolver 被明确记为
> 「O(keys),独立轴,本批不动」——本篇即该轴的收账。

---

## 0. 前提盘点(全部已具备,逐一核实)

| 前提 | 出处 |
|---|---|
| 透明 hash 已就位:`find(string_view)` 免拷贝,insert 才需 owned | `string_hash.hpp` 头注释;合并段路径已在用(`text_plugin.cpp:1308/1450` 直传 view)——写路径 `find(std::string(key))` 纯属未消费既有能力 |
| `key_at()` 契约即本设计所需:view「持有期 ≤ 段 pin 生命周期」,mmap 段直指映射区,内存段指 `keys_` RowChunks(元素稳定) | `segment.hpp:276-281`;`keys_` 为 `RowChunks<std::string>`(`:715`),SSO 短串内联于稳定元素内,view 同样稳定 |
| `KeyLocation.seg` 是 `shared_ptr`,条目天然 pin 住 view 的背衬存储 | `text_plugin.hpp:436-441` |
| 封口换对象时已有「批量重指本段 key 定位」循环(`key_loc_mu_` 独占下逐 doc 重写) | `text_plugin.cpp:1443-1455` |
| 段 drop 前条目必被改写的不变量已存在 | `text_plugin.hpp:429-435`;本设计把「定位」层面的不变量扩展到「键字节」层面(§5.0) |
| `rebuild_key_locations` 单线程(open 尾部)、段序 = LSN 序 | `text_plugin.cpp:1347-1367` |
| ext2ord_ 是 node-based `unordered_map` → 节点 key 地址在 rehash 下稳定;`Index` 禁拷贝/移动,全库无 `ext2ord_.clear()`;ckpt 载入经 `put_doc`(`index.cpp:413`) | `index.hpp:291-292` |
| docmap ckpt 只持久化 (ord → ext_id) 行,**不持久化** key_to_location_——③④ 的存储形态属纯内存内部实现,盘上格式零牵连 | `index.hpp:270-276`;`text_plugin.cpp:1347` |

---

## 1. 现状:四份驻留的精确账

以一条 live key(长度 L)在 enable_search 库内的堆驻留计(libstdc++):

| # | 宿主 | 形态 | 固定开销 | key 字节 | 角色 |
|---|---|---|---|---|---|
| ① | KeyDir | `SeqShardTable` 桶内存储(权威) | Entry 内联 | L(1 份) | KV 层点查/迭代 |
| ② | `Index::ext2ord_` | node `std::string` + u64 | 32B(SSO 头)+ ~16B(node)+ ~8B(bucket) | L(>15 时另付堆) | docmap:ext_id→ord 权威 |
| ③ | `Index::Chunk::ord2ext` | 平坦 `std::array<std::string,64K>`/chunk | 32B/**ord 槽**(含死槽) | L(>15 时另付堆;死 ord 的份留到 chunk compact) | ord→ext 反查 |
| ④ | `key_to_location_` | node `std::string` + KeyLocation(40B) | 32B + ~16B + ~8B | L(>15 时另付堆) | 段级删除定位 / explain / merge 重指 |

> feedback 记「三份」系漏计 ③(或并入 ② 口径)。本设计按四份立账,
> feedback 表补勘误注记。

### 1.1 收益估算(公式,r2 替换原「≈130MB」口径)

记 `chunk(n)` = glibc 为 n 字节请求实际占用的块(最小 32B,16B 对齐)。

- ③:每 **ord 槽** 省 16B(32B `std::string` → 16B view;chunk 头 2MB → 1MB,
  与槽是否存活无关);每 **live key** 另省 `chunk(L+1)`(仅 L>15)。
  死 ord 槽的残留拷贝也一并消失(r2:覆盖写即置空,§6)。
- ④:每 live key 省 16B(节点键 32B → 16B);另省 `chunk(L+1)`(仅 L>15)。

| 场景(1M live key,无覆盖) | 估算节省 |
|---|---|
| L = 20 | 2 × (16 + 32) ≈ **96 MB** |
| L ≤ 15(SSO) | 2 × 16 ≈ **32 MB** |

覆盖写多的库 ③ 另有死槽收益(按 ord 槽数 × 16B + 死槽残留拷贝)。

## 2. 目标与非目标

**目标**
- T1:live key 的 owned 份 4 → 2(① ② 保留;③ ④ 变 view);
- T2:开库 `rebuild_key_locations` **不再构造 key 字符串**,并预 `reserve`
  消掉 rehash(r2 更正:map 节点本身每条一次分配不可免——
  view 16B + KeyLocation 40B + 链指针/缓存 hash ≈ 72B/节点;原稿「零分配」
  不成立。L>15 时每条分配从 2 次降到 1 次,L≤15 时次数不变,省在 rehash 与字节);
- T3:写/删热路径零临时 key string(纯消费既有透明 hash);
- T4:盘上格式、C API 零改动;C++ 公开头仅 `for_each_live*` 回调参数改
  `string_view`(§6.1)。
  **随 6.6.0 发布**(6.6.0 未发版,不单独 bump),SOVERSION 保持 6。

**非目标**
- KeyDir(①)的存储改造:那是 KV 层权威,服务全部模式(含纯 KV),
  且其 `SeqShardTable` rehash 会搬桶,view 不可指——独立轴,不碰;
- 查询路径(`search_*`)改动:`key_to_location_` 本就不在 search 面上
  (`text_plugin.hpp:509-511` 注释明示),explain 照旧;
- 墓碑的跨重启持久化:维持现状(重启随 rebuild 消失)。

## 3. 方案总览

| 子项 | 内容 | 风险 | 收益 |
|---|---|---|---|
| D3 | 写路径异构查找(去临时 string) | 极低(机械替换) | 热路径零临时分配 |
| D2 | `ord2ext` → `string_view`(指向 ext2ord_ 节点) | 低(Index 内部,读者全在 live 门内) | -1 份 owned |
| D1 | `key_to_location_` → `string_view`(指向段存储 / 墓碑存储) | 中(生命周期论证,§5) | -1 份 owned + 开库去 string 构造 |

三项同批落地(6.6.0)。**D3 的「命中就地赋值 value」只在 D1 之前成立**;
D1 落地后所有改 `seg` 的写点一律走 §5.0 的换键(rebind),D3 在写点上
被 D1 吸收,只剩只读查找(早读 / explain)的去临时化。

## 4. D3:写路径异构查找

- 只读查找:`text_plugin.cpp:122/303`(早读)、`:907`(explain)
  `find(std::string(key))` → `find(key)`;
- 写点 `:144/327/398`:`operator[](std::string(key))` → `find(key)` +
  按结果分支(D1 下经 `k2l_assign_locked` 换键,§5.0);
- `index.cpp:60-70` 已是 find-先行,miss 路径 emplace 一次构造不可免(② 是权威)。

验收:bench(put_doc 热循环)±2%;既有 LSN 仲裁测试全绿。

## 5. D1:key_to_location_ view 化(核心)

### 5.0 硬规则:改 seg 必换键(r2 新增,评审 P0)

**不变量 K**:`key_to_location_` 中每个条目 `(k, loc)`,
- `loc.seg != nullptr` ⇒ `k` 的字节 ∈ `*loc.seg` 的存储(`loc.seg->key_at(loc.docid)`);
- `loc.tomb` ⇒ `k` 的字节 ∈ `tomb_keys_`。

`loc.seg` 的 shared_ptr pin 因此恒覆盖 `k` 的背衬。**任何一处只改 value
不改键,都会让键指向一个已不被 pin 的旧段**——旧段随后被 merge / 全死
drop 时,该键悬垂;之后同桶的任意 `find` 比较都读已释放 / 已 munmap 内存。

因此所有改 `seg`/`tomb` 的写点**只允许**经唯一辅助函数:

```cpp
// key_loc_mu_ 独占下调用。it == end → emplace(backing, loc);
// 否则 extract 节点 → nh.key() = backing → nh.mapped() = loc → insert。
// node handle 换键:不释放/不重分配节点,不触发 rehash(hash 由字节决定,
// 新旧键字节相等)。旧键若在 tomb_keys_ 且新 loc 非墓碑 → 回收该墓碑串。
void k2l_assign_locked(K2LMap::iterator it, std::string_view backing,
                       const KeyLocation& loc);
```

`backing` 必须取自 `seg->key_at(docid)` 或 `tomb_keys_` 元素,**绝不能**是
入参 `key`(调用方的临时对象)。

**全量触点清单**(`grep key_to_location_ src/`,11 处,实施逐条对账):

| 触点 | 性质 | 处理 |
|---|---|---|
| `:122` apply_text_in 早读 | 只读 | `find(key)` |
| `:144` apply_text_in 终检 | 改 seg | `k2l_assign_locked(it, bld->key_at(docid), …)` |
| `:303` apply_job_impl_in 早读 | 只读 | `find(job.key)` |
| `:327` apply_job_impl_in 终检 | 改 seg | 同 `:144` |
| `:398` on_delete | 改为墓碑 | §5.3 |
| `:430` 全量重建(rebuild_index)清表 | 清空 | 同时 `tomb_keys_.clear()` |
| `:458` 全量重建登记 | 新插 | `bld->key_at(docid)`(原 `*key` 是 `ord_to_ext` 返回的临时串,view 化后当场悬垂) |
| `:907` explain | 只读(shared) | `find(key)` |
| `:1308` merge 重指 | 改 seg | `k2l_assign_locked(it, merged->key_at(nd), …)` |
| `:1352/1363` rebuild_key_locations | 清空 + 登记 | 清 `tomb_keys_`;同 key 多段 live 时**必须换键**(`operator[]` 保留首次键,value 却换段——r2 评审 P0) |
| `:1450` 封口重指 | 改 seg | `k2l_assign_locked(it, pending->key_at(d), …)` |

实施上把 map 与辅助函数收拢,新写点只能经辅助函数进入,不靠 code review 把关。

### 5.1 数据结构

```cpp
using K2LMap = std::unordered_map<std::string_view, KeyLocation,
                                  StringHash, std::equal_to<>>;
K2LMap key_to_location_;
std::unordered_set<std::string, StringHash, std::equal_to<>> tomb_keys_;
```

view 的三类背衬:

| 背衬 | 何时成为 view 目标 | 存活保证 |
|---|---|---|
| mmap 段的 key 区 | rebuild(v2 段)/ seal 重指 / merge 重指后 | `KeyLocation.seg` pin 住段对象 → 映射区活到段 drop |
| 内存段的 `keys_` RowChunks | apply 路径登记(building 段) | 同上 pin;RowChunks 元素稳定、并发读者安全 |
| `tomb_keys_` 元素 | on_delete | node-based set,元素地址稳定;条目离开墓碑态时回收(§5.3) |

### 5.2 封口重指:既有循环原地升级

v2 封口换入 mmap 背衬对象时,`flush_building_slot` 在 `key_loc_mu_` 独占下
逐 doc 重指(`sealed` 持**旧**内存对象,`pending` 持**新** mmap 对象):

```
旧:find(pending->key_at(d)) → 命中 → e.seg = pending
新:find(pending->key_at(d)) → 命中(守卫 seg==sealed && docid==d)
   → k2l_assign_locked(it, pending->key_at(d), {pending, d, e.ord, false})
```

- docid 在 v1→v2 换对象时保持身份(今日代码即依赖此);
- 换键用 node handle,摊销成本 O(段文档数),不分配;
- find 比较中读到的旧键字节属 `sealed`,由局部 shared_ptr 钉到循环结束;
  守卫穷尽 seg==sealed 的条目 ⇒ 循环后无键再指旧 `keys_`,旧 RowChunks
  照旧随局部引用释放;
- `key_loc_mu_` 独占 ⇒ 重指期间无并发读者,无半改写观察。

v1 段(`pending == sealed`)零成本。

### 5.3 墓碑:唯一的 owned 例外

墓碑条目 `seg == nullptr`,没有段背衬;而墓碑必须可被后续同 key 到达
(否则旧 put 复活,LSN 仲裁失效)——**键字节不可丢**。

r2 方案(替换原 `std::deque<std::string>`):`tomb_keys_` 用 node-based
`unordered_set<std::string>`。

- on_delete:find(key);已有更新版本 → return(同今日);条目已是墓碑 →
  **只改 value**(键本就在 tomb_keys_,满足不变量 K);否则
  `tomb_keys_.emplace(key)` 取元素 view,经 `k2l_assign_locked` 换键;
- 墓碑条目被后续 put 覆盖:`k2l_assign_locked` 换键到段存储后,从
  `tomb_keys_` 回收该串(原 deque 方案无法回收,删后再写的 key 会留孤儿
  串直到 rebuild——增长上限是「开库以来删除总数」,长跑进程不可接受);
- 清空点:`rebuild_key_locations` 与全量重建(`:430`),与 k2l 同步清。

⇒ `tomb_keys_` 的占用 **恰等于存活墓碑数**,不随时间单调增长。

### 5.4 merge 重指与 rebuild

- merge 折叠路径(`:1308`):`find(merged->key_at(nd))` 命中且守卫通过
  → `k2l_assign_locked(it, merged->key_at(nd), …)`;
- `rebuild_key_locations`:先 `reserve(Σ doc_count)`;每 live doc
  `find(seg.key_at(docid))` → `k2l_assign_locked(it, seg.key_at(docid), …)`
  (同 key 后段覆盖前段时换键,§5.0);清 `tomb_keys_`。

### 5.5 读侧纪律

view 的合法消费窗口 = 持有 `key_loc_mu_` 期间;**禁止**把键 view 带出锁外
(今日代码无人这么做——消费的只是 value,键只在 find 比较时用)。在
`key_loc_mu_` 声明处补注此契约,ASan/TSan 兜底。

## 6. D2:ord2ext view 化(Index 内部)

```cpp
std::array<std::string_view, kChunkOrds> ord2ext;   // 2MB → 1MB/chunk
```

**不变量 O(r2 新增)**:`ord2ext[ord]` 非空 ⟺ `live_[ord]`,且非空时指向
`ext2ord_` 中该 key 的节点键。

- **写**(`index.cpp:60-75`):命中路径先**置空旧 ord 槽**(r2 评审 P1:原稿
  只在 remove 时置空当前 ord——覆盖写留下的历史死槽仍 view 同一节点,节点
  erase 后全部悬垂),再令新槽 = `it->first`;miss 路径 `emplace` 返回节点,
  新槽 = `it->first`;
- **删**(`index.cpp:96-108`):`ext2ord_.erase(it)` 之前置空 cur_ord 槽;
  由不变量 O,此时该 key 只剩这一个非空槽;
- **读者门禁**:

| 读者 | 位置 | live 门禁 |
|---|---|---|
| `for_each_live` / `for_each_live_in` | `index.hpp:222-250` | ✅ 遍历前查 `live_[ord]` |
| `serialize_docmap` | `index.cpp:331`(经 for_each_live) | ✅ |
| docmap delta 行提取(S14-4) | `docmap_ckpt.cpp:356`(经 for_each_live_in) | ✅ |
| **`ord_to_ext`** | `index.cpp:131-142` | ❌ **无门禁 → 补** |

  ⚠️ **`ord_to_ext` 契约与实现不符(独立发现,随 D2 修)**:`doc_table.hpp:28`
  契约写明「越界/**已删**返回 nullopt」,实现不查 `live_`——今日死 ord 返回其
  残留 ext 拷贝。调用方:`text_plugin.cpp:444`(全量重建,遍历 live)、
  `:561/1010`(调用前已 `is_live`)、`:1056`(高亮路径,**无** is_live)、
  `vector_plugin.cpp:169`、`sealed_segment_vector_plugin.hpp:377`(HNSW 搜索时
  live 过滤,物化时无)。后三处在「搜索判 live → 物化」之间的竞态窗口内,
  今日返回过期 key,修后丢弃该命中——属契约修复,CHANGELOG 注明;
- 置空而非悬垂是纵深防御:门禁再失守,读到空而非 UB;
- `compact_chunks`(`index.cpp:265-277`)按 `live_count==0` 整块释放,
  由不变量 O 该 chunk 无非空槽,无需改动。

### 6.1 公开头签名变更(r2 新增)

`index::Index` 经 `Cask::docmap()` 公开(`doc/api-cpp.md:736`),
`for_each_live`/`for_each_live_in` 是头文件模板,回调第二参由
`const std::string&` **直接改为 `std::string_view`**(评审拍板:不做旧签名
兼容垫片)。view 指向 `ext2ord_` 节点键,只在回调内有效。

- 源码影响:写成 `const std::string&` 的外部回调编译失败,改为
  `std::string_view`(或 `const auto&`)即可;库内/测试调用点已全部迁移;
- ABI:`Chunk` 布局随之变化,内联了 `for_each_live` 的下游须随库重编
  (与项目惯例一致:内部布局变化按 MINOR,6.6.0 本身即改动了 `index.hpp`
  内部布局);C API 用户不受影响;
- CHANGELOG 6.6.0 的 Changed 节注明此签名变化。

## 7. 否决的替代方案(留档防重提)

| 方案 | 否决理由 |
|---|---|
| hash 键(`u64` key)+ 碰撞回读校验 | 墓碑无段可回读,碰撞正确性无法闭环(§5.3 根因);且仍需 tomb 键字节 |
| ord 键(`k2l` 改 ord→location,经 ext2ord_ 反查) | 插件持不到 ext2ord_(S16-3 查询面只读纪律 / 窄接口),需扩 PluginHost 反向通道 |
| 插件内私有 key arena | 与 D1 同收益但多一套机制;段存储本就稳定且已被 pin |
| ext2ord_ 也 view 到 KeyDir | KeyDir `SeqShardTable` rehash 搬桶,view 不稳定;跨层 view 破坏封装 |
| 墓碑键用 `std::deque<std::string>`(r1 方案) | 无法按条回收,删后再写留孤儿串,长跑单调增长(§5.3) |
| 换键用 erase + emplace(r1 方案) | 每次释放+重分配节点;node handle `extract`/`insert` 零分配 |

## 8. 正确性论证

1. **并发序不变**:k2l 全部读写仍在 `key_loc_mu_` 下;view 化只改键的表示,
   不改锁协议。`StringHash` 对 string_view 与 string 同为字节 hash。
2. **LSN 仲裁不变**:早读跳过与终检裁决的 `e.ord` 比较逻辑一字不动。
3. **悬垂闭环**:不变量 K(§5.0)+ 全量触点清单 + 唯一写入口
   `k2l_assign_locked` + tomb store(§5.3)+ 读侧纪律(§5.5);
   Index 侧不变量 O(§6)。ASan/TSan 兜底。
4. **崩溃恢复**:k2l/tomb 均为纯内存态,rebuild 从段集全量重建;盘上格式
   零改动 ⇒ 新旧二进制可互开同一目录。

## 9. 兼容性

- 盘上格式:零改动;
- C API:零新符号、零签名变更;
- C++ 公开头:`Index::for_each_live*` 回调 ext 参数改 `std::string_view`
  (源码级变更,§6.1);`Chunk`/`KeyLocation` 布局属内部;
- 版本:**并入 6.6.0**(开发中),SOVERSION 6 不变。

## 10. 测试与验收

| 项 | 手段 |
|---|---|
| 行为等价 | 既有全量套件 Rel + ASan + TSan;LSN 仲裁/墓碑/merge 重指/崩溃恢复既有测试零改动通过 |
| 契约回归 | `ord_to_ext` 死 ord → nullopt;覆盖写后旧 ord → nullopt;tomb 键存活:删 → 同 key 旧 ord put 被拒、新 ord put 生效 |
| 悬垂回归 | ASan:put → 覆盖(新段)→ 封口 → merge drop 旧段 → 同桶 find;删 → 再写 → tomb 回收后 find |
| 内存 | RSS probe 60k/120k doc 两档,报 key 材料净变化 |
| 开库 | rebuild 路径不构造 key 字符串(代码审阅 + 分配计数:每 live doc ≤ 1 次节点分配) |
| bench | put_doc/BOW/intersect ±2% |
| 墓碑有界 | 删/再写循环后 `tomb_keys_.size()` == 存活墓碑数 |

## 11. 工作量与执行序

| 步 | 内容 | 工作量 |
|---|---|---|
| 1 | D2(§6)+ §6.1 签名迁移 + ord_to_ext 门禁 + 单测 | 1 天 |
| 2 | D1+D3(§4/§5):map 键型迁移 → `k2l_assign_locked` → 11 触点 → tomb store | 2-3 天 |
| 3 | ASan/TSan + RSS + 长跑 + feedback 表勘误注记 + CHANGELOG(6.6.0) | 1 天 |

## 12. 风险与缓解

| 风险 | 缓解 |
|---|---|
| view 悬垂(改 seg 不换键) | 不变量 K + 唯一写入口 + 触点清单;ASan 回归用例 |
| 往 map 塞临时串的 view | 写入只经 `k2l_assign_locked`,backing 限定来源;声明处注释 |
| ord2ext 读点遗漏 | 不变量 O + 门禁;置空防御使失守表现为空串而非 UB |
| 公开模板回调签名变化 | §6.1 直接改 `string_view`,CHANGELOG 注明迁移方式 |
| 墓碑增长 | node-based set + 离开墓碑态即回收,占用 = 存活墓碑数 |

## 13. 落地记录(2026-09-23)

- D1/D2/D3 同批落地:`index.hpp`/`index.cpp`(ord2ext view + 不变量 O +
  `ord_to_ext` 门禁)、`text_plugin.hpp`/`text_plugin.cpp`(`K2LMap` +
  `tomb_keys_` + 唯一写入口 `k2l_assign_locked` / `k2l_clear_locked`,11 触点全改);
  `for_each_live*` 回调直接改 `string_view`,库内/测试调用点已迁移;
- 新增内省 `TextPlugin::key_location_stats()`(entries / tombs / tomb_keys);
- 新增测试:`Index.OrdToExtDeadOrdReturnsNullopt`、`TextPlugin.S40TombKeyLifecycle`、
  `TextPlugin.S40RekeyAcrossSealMergeNoDangling`;
- 验证:Release 全量 776/776;ASan(`-E CApi`)全绿;TSan 全绿,仅既有豁免
  `KeyDirOptimisticRead.ConcurrentGetPutRemoveGrowStress`(TASK.md 预存);
- **变异验证**:把 `k2l_assign_locked` 改为只改 value 不换键,ASan 下两条 S40 用例
  分别报断言失败与 heap-use-after-free——证明用例确实守住不变量 K;
- 未做:§10 的 RSS probe 实测与 bench ±2% 对比(`BITCASK_BUILD_BENCHMARKS=OFF`),
  收益目前只有 §1.1 的估算。

---

## 修订记录

**r2(2026-09-23,评审后)**
- P0:新增 §5.0「改 seg 必换键」硬规则、唯一写入口 `k2l_assign_locked`
  (node handle 换键)与 11 处全量触点清单;D3 的「就地赋值」与 D1 冲突,
  改为 D1 下统一 rebind;
- P0:补漏写点——全量重建 `:430/458`(原 `*key` 临时串)、explain `:907`;
  `rebuild_key_locations` 同 key 多段时必须换键;
- P1:「开库零分配」更正为「不构造 key 字符串 + reserve」(节点分配不可免);
- P1:D2 覆盖写同时置空旧 ord 槽,立不变量 O;
- P1:130MB 估算改为公式(§1.1),L=20 约 96MB、L≤15 约 32MB/1M key;
- P2:`tomb_keys_` 由 deque 改 node-based set,可回收;
- P2:版本并入 6.6.0(开发中);修正「MINOR +1(7.0.0)」笔误;
- 新增 §6.1:`Index` 经 `Cask::docmap()` 公开,`for_each_live*` 回调直接改
  `string_view`(评审拍板不做旧签名兼容);
- CHANGELOG 注明 `ord_to_ext` 门禁在高亮/向量物化竞态窗口的行为变化。

---

## 附:本批不顺车携带项(各具独立立账,见 TASK.md,不进本设计)

- Analyzer 配置重开指纹告警;
- W1 核心路径 `kCloseOnExec`;
- S37-7 Windows CI + `BITCASK_SIMD_MAX` 矩阵 + AVX-512(SDE)首次真执行。
