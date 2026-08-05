# S34：TxnCask 多键事务 helper 实现设计（方案 B，提交路径已被 S35 取代）

> **状态（S35）**：`TxnCask::commit` 已改走引擎原生原子批
> （[`atomic-batch-design-zh.md`](atomic-batch-design-zh.md)），本文的
> 意图日志提交路径（§2.2/§3/§4）不再是热路径；`recover()` 仍按本文
> §5 重放旧目录遗留的 `_txn:` 意图（blob v1 解码保留）。

> 模式原理见 [`multikey-txn-zh.md`](multikey-txn-zh.md)(意图日志 + 前滚重放)。
> 本文是**库内参考实现**(方案 B)的设计定稿:建在 `Cask` 公共 API
> (`put` / `put_batch` / `remove` / `sync` / `make_range_iter`)之上,
> **零盘上格式改动、零 flag-day 风险**。
> 参考:[`pg-xid-mvcc-zh.md`](pg-xid-mvcc-zh.md)(为何无需 XID 式回收)。

---

## 1. 目标 / 非目标

| | |
|---|---|
| ✅ 目标 | 多 key 写操作的**崩溃原子性(A)与持久性(D)**;启动前滚恢复;悬挂事务巡检 |
| ❌ 非目标 | 隔离性(I)——事务中间态对并发读者可见;CAS;跨进程并发控制;`put_doc`/索引模式事务(意图重放走 KV 路径) |

引擎原生原子批(WAL commit marker,可省 2-3× 写放大)另行评估,
需 record 格式 flag-day,不在本期。

## 2. 公共 API(`include/bitcask/txn.hpp`)

```cpp
namespace bitcask {

struct TxnOp {
    enum class Type : std::uint8_t { kPut = 0, kRemove = 1 };
    Type type = Type::kPut;
    std::span<const std::byte> key;
    std::span<const std::byte> value{};   // kRemove 忽略
};

enum class TxnSyncPolicy : std::uint8_t {
    kSyncOnCommit = 0,  // 意图 put 后显式 sync()——防掉电(默认)
    kNone = 1,          // 依赖 o_sync / sync_every_n,或只防进程崩溃
};

struct PendingTxn {
    std::string txn_key;
    std::uint64_t created_at_us;
    std::size_t op_count;
};

class TxnCask {
public:
    explicit TxnCask(Cask* cask,
                     TxnSyncPolicy sync = TxnSyncPolicy::kSyncOnCommit);

    // 启动恢复:前滚重放全部 pending 事务,返回重放条数。
    // 契约:open 后、任何业务写之前调用;不得与 commit 并发。
    [[nodiscard]] std::expected<std::size_t, CaskFault> recover();

    // 原子提交。校验失败 → kInvalidOption,零副作用。
    [[nodiscard]] std::expected<void, CaskFault>
    commit(std::span<const TxnOp> ops);

    // 运维巡检:枚举 pending 事务(不重放)。
    [[nodiscard]] std::expected<std::vector<PendingTxn>, CaskFault>
    pending_txns();

    static constexpr std::string_view kTxnPrefix = "_txn:";
};

}  // namespace bitcask
```

`TxnCask` 非拥有地包装 `Cask*`,自身无状态(序号分配器为进程级静态),
可随建随用——C API 每次调用栈上构造即可。

### 2.1 commit 校验规则(违反 → `kInvalidOption`,零副作用)

1. `ops` 非空;
2. 每个 key 非空;
3. key 互不重复——PUT/REMOVE 拆分重放后,重复 key 的次序不可定义;
4. key 不得以 `_txn:` 开头(业务/意图命名空间隔离)。

### 2.2 提交流程(同模式文档 §2.1)

```
① put(txn_key, encode(ops))          意图日志,单条记录原子
② [sync()]                           TxnSyncPolicy::kSyncOnCommit 时
③ apply(ops)                         PUT 集一次 put_batch + REMOVE 逐条 remove
④ remove(txn_key)                    提交完成
```

③ 失败 → commit 返回错误,**意图记录保留**,下次 recover() 重试前滚。
④ 失败同理(数据已生效,重放幂等无副作用)。

**注意**:`Cask::BatchItem` 只有 `{key, value}`,`put_batch` 不支持墓碑
——含 REMOVE 的事务必须拆分 apply。这是模式文档 §5.2 未提及的实现约束,
由「key 互不重复」规则消除拆分引入的次序问题。

## 3. 意图日志编码(v1,全小端)

```
[u8 ver = 1][u64 created_at_us][u32 count]
count × ( [u8 type][u32 klen][u32 vlen][key][value] )    // kRemove: vlen=0
```

- 完整性由 record CRC32 覆盖,blob 不自带校验;
- 解码严格边界检查,任何越界/尾部余字节/ver 不识别/count=0
  → `kBadCrc`("txn intent blob malformed")——意图记录 CRC 已通过,
  解不开即逻辑损坏,**报错停止恢复**而非跳过;
- 该布局是**稳定格式**(盘上 value 段内容),测试用独立手写编码器对拍钉死。

## 4. txn key 与重放顺序

```
_txn:{seq:016x}-{rand:08x}
range = ["_txn:", "_txn;")        // ';' = ':' + 1
```

- **seq**:进程级静态分配器 `seq = max(now_us, last+1)`(CAS)——
  进程内严格单调;跨进程由 `bitcask.write.lock` 排他 + 墙钟保证实用单调;
- **rand**:进程随机 session id,防墙钟异常下跨进程碰撞;
- 定宽 hex ⇒ **字典序 = seq 序 = 提交序**。

这修正了模式文档用无序 uuid 的缺口:多个 pending 事务若触碰同一 key,
uuid 序重放的终值可能与崩溃前不一致;seq 序保证重放收敛到「最后提交者胜」,
与 keydir last-write-wins 一致。(键集重叠的**并发** commit 本就无隔离保证,
需应用层串行化——模式文档 §4。)

## 5. recover 流程

```
1. make_range_iter({lo="_txn:", hi="_txn;"})     // 默认惰性取值,不开预取
2. 全量收集 (txn_key, blob) 到内存               // O(pending),正常为 0
3. 按序逐个:decode → apply(ops) → remove(txn_key)
4. 返回重放条数
```

- **先收集再 apply**,不在弱一致迭代器活跃期间写库(比模式文档骨架的
  边迭代边写更稳,也避开预取交互);
- recover 自身任意点崩溃安全:同一幂等模式,下次重来;
- 依赖 OKI:read-write 打开时 OKI 必在(缺失自动重建);只读打开
  `make_range_iter` 返回 `kNoIndex`——recover 本就需要写权限,直接透传;
- `Cask::remove` 对任意 key 都只写墓碑不报错(`cask.cpp:1895`),
  重放 REMOVE 与清理 txn key 均幂等安全。

## 6. 并发契约

| 场景 | 结论 |
|---|---|
| 多线程并发 commit(键集不相交) | ✅ 安全——各事务独立意图 key,写路径 `write_mu_` 串行 |
| 键集重叠的并发 commit | ⚠️ 原子性仍成立,但无隔离/无定序——应用层串行化 |
| recover 与 commit 并发 | ❌ 禁止——recover 必须在业务写启动前完成 |
| 与并发读(get/iter/scan) | 事务中间态可见(无 I);`_txn:` key 会出现在全表扫描中,业务侧按前缀过滤 |
| 与 merge | 无交互——意图记录/墓碑是普通记录,死记录照常回收 |

## 7. 空间回收

无新增回收机制(论证见 [`pg-xid-mvcc-zh.md`](pg-xid-mvcc-zh.md) §4):

- 已完成事务的意图记录 + 墓碑 = 普通死记录,**merge** 回收;
- 悬挂事务由 **recover()** 前滚清除;长期滞留(进程死后不重启)用
  `pending_txns()` 巡检发现;
- 写放大约 2-3×(意图含全部 value 副本)⇒ 事务流量会加速 dead-bytes
  阈值触发 merge——文档明示预期,非缺陷。

## 8. C API(`bitcask_kv.h`,纯增量)

```c
typedef struct {
    uint8_t         op;      // 0 = put, 1 = remove(忽略 value)
    bitcask_slice_t key;
    bitcask_slice_t value;
} bitcask_txn_op_t;

BITCASK_API bitcask_error_t bitcask_txn_commit(bitcask_t*,
    const bitcask_txn_op_t* ops, size_t n_ops,
    int sync_on_commit, bitcask_fault_t*);
BITCASK_API bitcask_error_t bitcask_txn_recover(bitcask_t*,
    size_t* out_replayed, bitcask_fault_t*);
BITCASK_API bitcask_error_t bitcask_txn_pending_count(bitcask_t*,
    size_t* out_count, bitcask_fault_t*);
```

实现每调用栈上构造 `TxnCask`(无状态,见 §2)。`guarded` 异常隔离 +
`to_c_error` 翻译,同既有 KV 接口。

## 9. 测试计划(`tests/txn_test.cpp`)

| 用例 | 覆盖 |
|---|---|
| CommitBasic | 正常提交(PUT+REMOVE 混合),数据生效、`_txn:` 清空 |
| ValidationRejects | 空批/空 key/重复 key/`_txn:` 前缀 → `kInvalidOption` 零副作用 |
| RecoverReplaysPendingIntent | **手写编码器**造意图记录(格式对拍钉死)→ reopen → recover 前滚 + 清理 |
| RecoverAppliesRemoveOps | 意图含 REMOVE → 重放后 key 消失 |
| ReplayOrderIsSeqOrder | 两条 pending 触碰同一 key → seq 大者终值胜 |
| CrashMidApply(fork) | 子进程写意图 + 部分数据后 `_exit`(crash_recovery_test 同款)→ 父进程 recover 收敛 |
| PendingTxnsInspection | 巡检枚举 created_at/op_count;recover 后为空 |
| RecoverOnCleanDirIsZero | 干净目录 recover O(0) 返回 0 |

## 10. 落点清单

| 文件 | 改动 |
|---|---|
| `include/bitcask/txn.hpp` + `src/cask/txn.cpp` | 新增(挂 `bitcask_cask` target) |
| `tests/txn_test.cpp` | 新增,注册 ctest |
| `c_api/bitcask_kv.h` / `bitcask_kv.cpp` | txn 三接口 |
| `doc/multikey-txn-zh.md` | 修 §2.3 `.prefetch=true` 实为关闭的 bug;§5.2 补 put_batch 无墓碑;§5.1 换单调 txn key;§6 骨架改指向本实现 |
| `README.md` / `doc/api-cpp.md` / `doc/api-c.md` | 索引与 API 文档增量 |

版本:C API 纯增量,无盘上格式改动——随 **5.1.0**(未发布)出货,CHANGELOG
并入该版本条目;若 5.1.0 已先行发布则为 5.2.0。`SOVERSION` 保持 5。
