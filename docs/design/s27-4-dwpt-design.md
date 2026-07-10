# S27-4 实现设计:DWPT 并行 builder

> 状态:**设计(2026-07-10),分 3 相实施**。骨架来自
> `doc/segment-index-design-zh.md` §5.2(不共享可变态 / 免跨线程 ord 定序 /
> refresh 可见性);本文补实现级决策。前置已全部就位:S27-3 五步(段唯一
> 真相源 + Building 并发读 + 段生命周期 pin/锁)+ S29-9(reorder 环形)。

## 1. 架构

```
queue → N map worker(analyze,既有) → reorder ring → reducer(路由器)
                                                      ├─ docmap 行/meta(宿主,单线程,不变)
                                                      ├─ VectorPlugin(HNSW 单写者,不变)
                                                      └─ TextPlugin::on_put → round-robin 派发
                                                              ↓
                                                   Builder×B(各自线程 + MPSC 队列)
                                                     每个持自己的 building 段
                                                     analyze(单文本路径)/add/封口
```

- reducer 退化为轻路由:文本 job **move 进 builder 队列**即返回(背压 = 队满阻塞)。
- 每 builder 一个 building 段(对象即 SealedSegment,S27-3 的并发读契约:
  deque + count_pub_ 发布 + live_ 原子——查询可直接并发读任意 building)。
- 查询视图 = [SegmentSet 快照 + B 个 building load],既有 pin 机制原样适用。

## 2. 正确性核心:LSN 守卫 upsert(任意分派序安全)

round-robin 下同 key 的 v1(ord5)/v2(ord9) 可能落不同 builder、乱序 apply。
靠 key_to_location_ 的 **ord 比较**仲裁,而非到达序:

```
KeyLocation { shared_ptr<SealedSegment> seg;  // building 与 sealed 统一为对象指针
              DocId docid; Lsn ord; bool tomb; }
upsert(key, my_ord, add 动作):
  key_loc_mu_ shared: prior = find(key)
    if (prior && prior.ord > my_ord) return SKIP   // 纯优化:已见更高版本免 add
  docid = my_building->add(...)
  key_loc_mu_ unique(终检登记 = **唯一 mark_dead 责任点**):
    e = entry(key)
    if (e.ord <= my_ord): displaced = e; e = {my_building, docid, my_ord}
    else: lost = true
  锁外: lost → my_building->mark_dead(docid)          // 败者自标
       else displaced 非墓碑 → displaced.seg->mark_dead(displaced.docid)
on_delete(key, tomb_ord)(reducer 直做):
  同款终检:胜出 → 锁内捕获 displaced + entry 置 {tomb, tomb_ord},锁外
  mark_dead displaced(墓碑**保留**——erase 会让更旧的 put 复活成幽灵;
  墓碑条目重启时随 rebuild_key_locations 消失,运行期增长 = 删除的
  distinct key 数,可接受)
```

> **P3 教训(早标死协议的幽灵窗口)**:P1 原稿是「读 prior → 标死 prior →
> add → 终检登记」。B>1 时三步间隙可被并发 put/delete 插入:终检**败者**的
> 行无人再指向(登记被拒但行已 add)、**胜者**读 prior 时对手尚未登记(双方
> 都 have_prior=false,谁也不标谁)——两种幽灵存活,B=4 压力测试 +2 计数
> 坐实。修正后 mark_dead 全部收敛到终检临界区裁决(胜→顶替者/败→自身),
> 每行恰好一个归宿,unique 锁串行化链条。

关键简化(源自对象指针统一):**封口不再改 key_to_location_**——building
对象封口后即 sealed 段对象本身(shared_ptr 移入段集,身份不变),O(map) 的
seal 清扫消失。shared_ptr 钉住段对象:全死段被 compact drop 时,其 key 的
location 必已被覆盖/删除改写 → 无残留引用,内存即释。

## 3. 可见性与屏障

- 常态查询:refresh 语义(设计 §5.2)——builder 在途 job 不可见,微秒级。
- **read-your-writes 保障点**(既有测试/API 依赖):`Cask::flush_index`/
  `prepare_search` 在 index_pool flush(reducer 排干)后追加
  `TextPlugin::drain_builders()`(各 builder 队列空 + 在途 apply 完成,
  cv 等待)。插件 API 增 `virtual void drain() {}`。
- checkpoint flush(reducer RunFn):drain_builders → 逐 builder 封口 →
  resave/commit/kSegManifest(既有步骤 1/4 流程)。

## 4. 共享态并发面(全部已有锁或原子)

| 共享态 | 保护 | 备注 |
|---|---|---|
| key_to_location_ | key_loc_mu_(unique 短临界区) | N 写者争用点,首版接受 |
| SegmentSet 列表 | list_mu_ | 多 builder 并发 add_pending ✓ |
| sealed/building mark_dead | live_ 原子 | dead_dirty_ 需转原子 |
| doc_texts_ / cache_ / intern | 自带锁 | ✓ |
| set_doc_len(ord) | 行由宿主 reducer 先建,builder 写既有槽 | 需核 SoA 无增长 |
| seg_dirty_ | 转原子 | |

## 5. 分相

- **P1(本会话)**:KeyLocation 对象指针化 + LSN 守卫 upsert + 墓碑保留,
  在**现单 reducer**上落地(全局序仍在 → 行为等价,守卫成为冗余保险),
  全量回归验证语义。
- **P2(完成 2026-07-10)**:BuilderPool(线程 + MPSC 队列 + drain)+
  on_put/apply_text 派发 + 插件 drain 钩子接 Cask;默认 **B=0**(内联,
  偏离原稿 B=1——安全灰度)。
- **P3(完成 2026-07-10)**:每 builder 一个 building 段(apply/封口按
  「目标槽」参数化)+ B>1 开放 + upsert 协议修正(见 §2 教训)+ 唤醒协议
  (waiting 标记 + 背压半满迟滞,稳态零 futex)+ 三 sanitizer + bench:
  20k 篇 30 词单文本 put_doc→flush_index 端到端,B=0 基线 ~95-103k docs/s,
  B=1 ~105k(+10~15%),B=2 ~180k(1.9×),B=4 ~198k(2.1×,put 前端成新
  瓶颈——WAL 写 + 组提交,加 B 不再涨)。

## 6. 风险

- key_loc_mu_ 成为新争用点 → 若 bench 显示瓶颈,分片(按 key hash)二期。
- HNSW 仍单写者(设计 §6.3 既定):向量重负载吞吐不受益。
- 墓碑条目运行期驻留:删除密集型长进程的 map 增长,重启归零;必要时挂
  周期清扫(墓碑 ord < 全段集 min hi_lsn 即可清)。
