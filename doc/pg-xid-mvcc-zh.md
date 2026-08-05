# PostgreSQL xmin/xmax 与 XID 回收（参考笔记）

> 本文是**外部系统参考笔记**，不描述 libbitcask 实现。整理 PostgreSQL 的
> MVCC 可见性机制（xmin/xmax）与 XID 回卷/冻结问题，用于论证
> [`multikey-txn-zh.md`](multikey-txn-zh.md) 的多键事务方案**为什么不需要**
> PostgreSQL 式的事务 ID 回收——两边的 txn 标识在语义上根本不是同一种东西。

---

## 1. xmin/xmax 是什么

PostgreSQL 的表里，每一行（准确说是每一个**行版本**，tuple）的头部都藏着
几个系统字段，其中两个是事务 ID：

| 字段 | 含义 |
|---|---|
| **xmin** | 创建这个行版本的事务 XID（哪次 INSERT/UPDATE 生出了我） |
| **xmax** | 删除这个行版本的事务 XID（哪次 DELETE/UPDATE 判了我死刑；0 = 没人删我） |

关键在于：PostgreSQL 的 UPDATE 和 DELETE **从不原地修改数据**——

- UPDATE = 写一个新版本 + 给旧版本盖 xmax 章；
- DELETE = 只给旧版本盖 xmax 章。

行版本一旦写下就不可变。这与 bitcask 的 append-only 精神很像，区别在于
PostgreSQL 把「哪个版本对谁可见」的判断信息（xmin/xmax）刻在每行上，
而 bitcask 用 keydir 全局裁决「最新即唯一」。

这两个字段可以直接查：

```sql
SELECT xmin, xmax, ctid, * FROM account;
```

---

## 2. 用一个例子走一遍

```sql
CREATE TABLE account(id int, balance int);

-- 事务 1000 执行：
INSERT INTO account VALUES (1, 100);
```

此时磁盘上有一个行版本：

```
(xmin=1000, xmax=0)    id=1, balance=100     ← 活着，没人删
```

两个会话并发：

```sql
-- 会话 A，分到 XID 1001：
BEGIN;
UPDATE account SET balance = 50 WHERE id = 1;
-- 尚未 COMMIT
```

UPDATE 之后磁盘上**两个版本并存**：

```
版本1: (xmin=1000, xmax=1001)   balance=100   ← 旧版本，被 1001 标记删除
版本2: (xmin=1001, xmax=0)      balance=50    ← 新版本，1001 创建
```

```sql
-- 会话 B 此刻查询：
SELECT balance FROM account WHERE id = 1;   -- 返回 100
```

### 2.1 可见性规则

B 对每个版本做可见性判断，口语化地说：

> 一个版本对我可见，当且仅当：
> **xmin 已提交**（且在我的快照之前）——「生它的事务真实发生过」；
> 并且 **xmax 为 0、或未提交、或已回滚**——「判它死刑的事务还没生效」。

套到 B 头上：

| 版本 | 判断 | 结论 |
|---|---|---|
| 版本1 | xmin=1000 已提交 ✅；xmax=1001 **未提交** → 死刑不算数 | **可见，返回 100** |
| 版本2 | xmin=1001 未提交 → 这行「还不存在」 | 不可见 |

### 2.2 提交与回滚都是零字节改动

A 提交后 B 再查返回 50：版本1 的 xmax=1001 生效（不可见），版本2 的
xmin=1001 生效（可见）。**提交这一刻磁盘上的两个版本一个字节都没改**——
变的只是「1001 提交了没有」这个事实（记录在 clog/pg_xact 里），可见性
结论随之翻转。这就是 MVCC：读写互不阻塞，代价是旧版本成了死元组，
等 VACUUM 回收。

如果 A 是 ROLLBACK？同样一个字节不改：版本1 的 xmax 指向「已回滚」
事务 → 死刑永久无效，继续活着；版本2 的 xmin 已回滚 → 永远不可见，
直接是垃圾。**回滚零成本，清理全部推给 VACUUM。**

---

## 3. 范围：32 位环形比较，以及为什么必须回收

XID 是 32 位无符号数，空间约 42.9 亿。PostgreSQL 不做线性比较，而是
**环形比较**：

> XID a 比 b 老，当且仅当 `(b - a) mod 2³² < 2³¹`。

即：站在任何一个 XID 上，往前 21.5 亿是「过去」，往后 21.5 亿是「未来」。
像钟表：站在 3 点，1 点是过去、5 点是未来——「11 点」算什么取决于绕没绕圈。

灾难场景：

```
某行 xmin = 100，一直没动它。
系统持续跑，当前 XID 推进到 100 + 2³¹。
环形比较下，xmin=100 从「遥远的过去」翻转成「未来」！
→ 可见性判断认为「生这行的事务还没发生」→ 整行凭空消失。
```

数据没坏，只是可见性算术翻车——对用户来说就是丢数据。

### 3.1 冻结（freeze）

VACUUM 把足够老的行版本**冻结**：在行头打 `HEAP_XMIN_FROZEN` 标记
（老版本 PostgreSQL 直接把 xmin 改写成保留值 2，`FrozenTransactionId`），
含义是「此行诞生于无限远的过去，对所有人永远可见」，从此退出环形比较，
原 XID 可被安全复用。

- `autovacuum_freeze_max_age`（默认 2 亿）保证在逼近 2¹·⁵ 亿悬崖前强制冻结；
- 拖到极限，数据库拒绝新事务，逼你 vacuum。

保留 XID：0 无效、1 bootstrap、2 frozen；正常事务从 3 开始分配。

---

## 4. PostgreSQL 的三类「回收」对照 libbitcask

| PostgreSQL 机制 | 解决的问题 | libbitcask 对应 |
|---|---|---|
| freeze（XID 回收） | 32 位 XID 刻在每行、终身参与环形可见性比较，会回卷 | **不存在**：txn 序号不刻在数据记录上、不参与可见性（keydir last-write-wins 裁决），u64 且生命周期毫秒级 |
| VACUUM（死元组回收） | MVCC 把旧版本留在堆表 | **merge**：append-only 的死记录回收本来就是 merge 的职责，意图日志 + 墓碑没有引入新垃圾类别 |
| clog/pg_xact 截断 | 独立维护「每个 XID 提交与否」 | **不存在**：提交状态 = `_txn:` key 的存在与否，状态与数据同生命周期 |

唯一真正需要「回收」的是**悬挂事务**（对应 orphaned prepared transaction）：
进程异常退出后 `_txn:` key 滞留（活 key，merge 不会动它）。由
`recover()` 前滚清除（正常为零开销），加运维巡检兜底——见
[`multikey-txn-zh.md`](multikey-txn-zh.md) §2.3 / §5.4。

---

## 5. 一句话

> **PostgreSQL 回收 XID，是因为 XID 被刻进每一行、终身参与环形比较的
> 可见性算术；libbitcask 的 txn 序号只给崩溃后的 pending 重放定序，
> 事务完成即删除——所以那边需要 freeze，我们只需要
> merge（收空间）+ recover（收悬挂事务）。**
