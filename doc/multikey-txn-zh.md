# 在 libbitcask 上实现多键事务（应用层模式）

> 本文是**应用层指南**，不描述库内实现。libbitcask 本身不提供跨 key 事务；
> 本文给出一个基于「意图日志 + 前滚重放」的模式，用库已有的三项保证补齐
> **原子性（A）与持久性（D）**。
>
> 相关：[`put-flow-zh.md`](put-flow-zh.md)（`put_batch` 语义）、
> [`format-zh.md`](format-zh.md)（record CRC）、
> [`recovery-unified-checkpoint-design-zh.md`](recovery-unified-checkpoint-design-zh.md)（torn-write 恢复）、
> [`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md)（OKI range 扫描）、
> [`pg-xid-mvcc-zh.md`](pg-xid-mvcc-zh.md)（参考：为何无需 PostgreSQL 式 XID 回收）

---

## 1. 问题：AOF ≠ 事务

libbitcask 是 append-only 引擎，data file 本身就是 WAL。一个常见的直觉是
「既然 data 就是 WAL，事务是不是白送的」——**不是**。

### 1.1 已经具备的

| 能力 | 依据 |
|---|---|
| **单条记录原子性** | 每条 record 带 zlib CRC32；恢复时 torn-write 检测（`last_valid_end`）丢弃尾部半条记录 |
| **可重放恢复** | fold 逐条重放 data file 到 keydir |
| **可控持久化** | `CaskOptions::o_sync`（每条 durable）/ `sync_every_n`（组提交） |
| **写幂等** | 同一 key 写入相同 value 多次，结果一致 |

### 1.2 缺的那一块

**fold 不认识「批」这个概念。** 它逐条 apply 记录，没有任何机制表达
「这 N 条属于同一个逻辑操作，缺一条就全部作废」。

这正是 `put_batch` 的语义边界（见 [`put-flow-zh.md`](put-flow-zh.md)）：

> 失败返回 ⟹ 整批在本进程内不可见（keydir 未动）。
> **磁盘上可能残留批前缀**——每条记录独立自洽，崩溃重启 fold 后可见。

即：`put_batch` 提供**进程内** all-or-nothing，但**不提供崩溃后的** all-or-nothing。

### 1.3 根因：状态日志 vs 意图日志

| | libbitcask data file | 事务需要的 |
|---|---|---|
| 日志类型 | **状态日志**：`k = 最终值` | **意图日志**：「本次操作要写 k1,k2,k3」 |
| 重放语义 | 幂等 ✅ | 幂等 ✅ |
| 崩在操作中间 | 只能恢复已落盘的那部分 | **能补齐剩下的部分** |

> **AOF 给了幂等性，没给原子性。**
> 原子性的本质是「知道哪些记录该被丢弃 / 该被补上」——状态日志里没有这个信息。

---

## 2. 方案：意图日志 + 前滚重放

### 2.1 流程

```
1. txn_id = uuid()
2. put("_txn:" + txn_id, encode(ops))     ← 意图日志：单条记录，原子
   [sync()]                                ← 见 §2.4
3. put_batch(ops)                          ← 实际写入全部数据 key + 二级索引
4. remove("_txn:" + txn_id)                ← 提交完成
```

`ops` 是完整的操作意图——**必须包含所有 key 与其目标 value 的副本**，
这样重放时无需依赖任何其他状态。

### 2.2 崩溃点分析

| 崩溃位置 | 磁盘状态 | 恢复处理 |
|---|---|---|
| 2 之前 | 无痕迹 | 无需处理 |
| **2 的中间** | 半条记录 | **CRC / torn tail 检测丢弃**，等价于未写 |
| 2 后、3 前 | 仅意图日志 | 启动时重做 `ops` |
| **3 的中间** | 批前缀 + 意图日志 | 启动时**重做全部 `ops`**（幂等，无副作用） |
| 3 后、4 前 | 数据完整 + 意图日志 | 重做一遍，仍无副作用 |
| 4 之后 | 干净 | 无需处理 |

**任意点崩溃都收敛到一致状态。**

### 2.3 启动恢复

```cpp
// 用 OKI range 扫描枚举未完成事务 —— O(pending) 而非 O(全表)
bitcask::RangeOptions ro{
    .lo = as_bytes("_txn:"),
    .hi = as_bytes("_txn;"),   // ';' == ':' + 1，半开区间上界
    .prefetch = true,
};
auto it = c->make_range_iter(ro);
for (; it->valid(); it->next()) {
    auto ops = decode(it->value());
    c->put_batch(ops);              // 幂等重放
    c->remove(it->key());           // 标记完成
}
```

正常关闭的库里 `_txn:` 前缀为空，**恢复扫描的开销为零**。

> 这一步依赖 OKI（[`ordered-key-index-design-zh.md`](ordered-key-index-design-zh.md)）。
> 若无有序 range 扫描，就只能全表 fold 才能找出 pending 事务，该模式将不实用。

### 2.4 ⚠️ 持久化要求

第 2 步之后**意图日志必须真正落盘**，否则掉电时它还在 page cache 里，
整个方案失效：

| 需要防御的故障 | 要求 |
|---|---|
| **仅进程崩溃**（page cache 存活） | 无需 fsync |
| **掉电 / 内核 panic** | **必须 fsync** |

```cpp
// 方式一：全局组提交
auto c = Cask::open(dir, CaskOptions{.read_write = true, .sync_every_n = 1}, &reg);

// 方式二：只在事务提交点显式 fsync
c->put(txn_key, ops_blob);
c->sync();                  // 持 write_mu_，与 put/remove 互斥
c->put_batch(ops);
c->remove(txn_key);
```

写频率低的场景（人工触发的业务操作）直接开 `sync_every_n = 1` 即可，
fsync 代价可忽略。高吞吐场景用方式二，只在提交点付出 fsync。

### 2.5 成立的三个前提

该模式能成立，完全依赖库已有的三项保证：

| 前提 | 由什么保证 |
|---|---|
| 单条记录写入原子 | record CRC32 + torn tail 检测 |
| 操作幂等 | put 同 key/value 多次结果一致 |
| 能高效枚举 pending 事务 | OKI 有序 range 扫描，O(range) |

---

## 3. 代价

| 项 | 影响 |
|---|---|
| 写放大 | 约 2-3×（意图日志 + 数据 + 墓碑） |
| 空间 | 意图日志需存所有 value 的副本；`remove` 后由 merge 回收 |
| 启动扫描 | 正常情况 O(0)；崩溃后 O(pending) |
| fsync | 见 §2.4，取决于故障模型 |

对写频率低、一致性要求高的场景（配置管理、标注/编辑类工具、元数据服务），
这个代价基本无感。对高吞吐写入场景需要重新权衡。

---

## 4. 边界：本方案不提供什么

| ACID | 状态 | 说明 |
|---|---|---|
| **A** 原子性 | ✅ 提供 | 崩溃后不留半截操作 |
| **C** 一致性 | ✅ 提供 | 二级索引可与主数据放进同一 `ops` 批，一起原子提交 |
| **I** 隔离性 | ❌ **不提供** | 事务中间状态对并发读者可见；无写事务快照 |
| **D** 持久性 | ✅ 提供 | 前提是按 §2.4 配置 fsync |

另外**不提供 CAS**（compare-and-swap）：

- 库内 `write_mu_` 串行化写序列，但应用层拿不到「读到的版本 == 写入时的版本」的保证
- 单进程场景可在应用层加锁解决（写路径本就串行）
- **跨进程场景无解**——`bitcask.write.lock` 是排他锁，不是 CAS 原语

> **若业务存在并发修改同一 key 的场景（多用户协同编辑等），本方案不足以保证正确性，
> 需要额外的并发控制层。**

---

## 5. 实现要点

### 5.1 key 命名空间

用一个不会与业务 key 冲突的前缀，且前缀的字典序后继要便于构造：

```
_txn:{uuid}          业务约定：所有业务 key 不以 '_' 开头
range = ["_txn:", "_txn;")
```

### 5.2 意图日志的编码

```
ops := [ (op_type, key, value?) ... ]
op_type ∈ { PUT, REMOVE }
```

`REMOVE` 也必须记进意图日志，否则重放会漏掉删除动作。
建议带上 `created_at` 便于排查悬挂事务。

### 5.3 幂等性的隐含要求

重放依赖「重复执行同样的 ops 结果不变」。因此 **ops 内不能出现依赖当前状态的操作**：

```
✗ incr("counter:x")           非幂等，重放会多加一次
✓ put("counter:x", 42)        幂等，值由调用方算好
```

计数器一类的需求，必须在写意图日志**之前**读出旧值算好新值，把最终值写进 ops。
（注意这本身存在竞态——参见 §4 关于 CAS 的说明。）

### 5.4 悬挂事务的运维

正常情况 `_txn:` 前缀应为空。可加一个巡检：

```cpp
// 存在超过 N 分钟仍未完成的事务 → 说明有进程异常退出且未重启恢复
```

---

## 6. 最小实现骨架

```cpp
class TxnCask {
public:
    // 启动时必须先调用，否则可能读到半截状态
    [[nodiscard]] std::expected<void, CaskFault> recover() {
        bitcask::RangeOptions ro{.lo = k_txn_lo, .hi = k_txn_hi, .prefetch = true};
        auto it = cask_->make_range_iter(ro);
        for (; it->valid(); it->next()) {
            auto ops = decode_ops(it->value());
            if (auto r = apply(ops); !r) return r;
            if (auto r = cask_->remove(it->key()); !r) return r;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, CaskFault> commit(std::span<const Op> ops) {
        const auto txn_key = make_txn_key();          // "_txn:{uuid}"
        std::vector<std::byte> blob = encode_ops(ops);

        if (auto r = cask_->put(txn_key, blob); !r) return r;   // ① 意图
        if (auto r = cask_->sync(); !r) return r;               // ② 落盘（见 §2.4）
        if (auto r = apply(ops); !r) return r;                  // ③ 数据
        return cask_->remove(txn_key);                          // ④ 提交
    }

private:
    // ③ 与 recover() 共用：保证重放路径与正常路径完全一致
    [[nodiscard]] std::expected<void, CaskFault> apply(std::span<const Op> ops);

    bitcask::Cask* cask_;
};
```

> **关键**：`apply()` 必须被正常提交路径和恢复路径**共用**。
> 两条路径若各写一份，迟早会出现「正常写了 4 个 key，重放只写 3 个」这类偏差。

---

## 7. 一句话

> **libbitcask 的 AOF 省掉了「自己实现持久化与崩溃恢复」，
> 没省掉「批边界标记」——而后者用一条意图日志记录 + 前滚重放即可补齐，
> 因为单条记录的写入本身已经是原子的。**
