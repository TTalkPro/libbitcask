# `GetResultView` 零拷贝设计

`Cask::get()` 返回的 `GetResultView` 是零拷贝视图：`value` / `meta` /
`vector` 是 `std::span`，借用底层 ReadRecord 缓冲或 sealed mmap，**无
堆分配**。`Cask::get_owned()` 是拷贝语义版本（benchmark / 需要持久化
数据的场景）。

本文档基于当前 `include/bitcask/cask.hpp` 的 `GetResultView` /
`GetResult` 定义，逐项说明字段、来源、生命周期、移动语义与 `to_owned()`
开销。

---

## 1. 完整结构定义

符号：`include/bitcask/cask.hpp::GetResultView` / `GetResult`

### 1.1 `GetResultView`

```cpp
// cask.hpp
struct GetResult {
    std::vector<std::byte> value;  // DocValue 解码后的 text 段（纯 binary）
    std::vector<std::byte> meta;   // DocValue 解码后的 meta 段（可为空）
    std::vector<float>      vector; // 向量段（空 = 该文档无向量）
    std::uint32_t tstamp = 0;
    std::uint64_t ord    = 0;
};
```

```cpp
// cask.hpp
struct GetResultView {
private:
    friend class Cask;

    // ① owned(pread) 路径——持 pread 数据的所有权。
    fileops::ReadRecord storage_;

    // ② mmap 路径——持 sealed DataFile 的 shared_ptr 锚定映射；
    //    view 生命内映射不撤（即便期间 merge unlink 文件）。
    //    owned 路径下为空。
    std::shared_ptr<fileops::DataFile> map_holder_;

    // DocValue 原始字节来源：
    //   owned 路径：借 storage_.value
    //   mmap 路径：指向映射（map_holder_ 必须存活）
    std::span<const std::byte> value_bytes_{};

    format::RecordType rec_type_ = format::RecordType::kDoc;

    // P3b：量化文档落盘是 int8，无法零拷贝成 f32 span——dequant 进此拥有
    // 缓冲，vector span 指向它。未量化时为空。
    std::vector<float> vector_dequant_;

public:
    std::span<const std::byte> value{};     // text 段（指向底层字节内部）
    std::span<const std::byte> meta{};      // meta 段（可为空）
    std::span<const float>    vector{};    // 向量段（空 = 无向量）
    std::uint32_t tstamp    = 0;
    std::uint64_t ord       = 0;
    std::uint32_t expiry_at = 0;            // S13-D5：per-key TTL（0 = 永不）

    /// 拷贝为 owned 版本
    GetResult to_owned() const;

    // 可移动（std::expected 要求），不可拷贝
    GetResultView(GetResultView&& other) noexcept;
    GetResultView(const GetResultView&)            = delete;
    GetResultView& operator=(const GetResultView&) = delete;

private:
    // owned(pread) 路径——从 DataFile::read() 转入。
    explicit GetResultView(fileops::ReadRecord&& rec);

    // P6：mmap 命中——holder 锚定映射，value_bytes 指向映射内的 DocValue 字节。
    GetResultView(std::shared_ptr<fileops::DataFile> holder,
                  std::span<const std::byte> value_bytes,
                  format::RecordType type,
                  std::uint32_t tstamp, std::uint64_t ord);

    // 从 value_bytes_ 解出 value / meta / vector span（量化则 dequant 进
    // vector_dequant_）。三个 ctor 共用，避免漂移。
    void derive_from_storage();
};
```

### 1.2 字段总览

| 字段 | 类型 | 公开 | 来源 / 语义 |
|------|------|------|-------------|
| `storage_` | `fileops::ReadRecord` | 私有 | owned 路径下 DataFile::read 返回；持 key / value 字节 vector |
| `map_holder_` | `shared_ptr<DataFile>` | 私有 | mmap 路径下锚定 sealed DataFile 映射（防 merge unlink） |
| `value_bytes_` | `span<const std::byte>` | 私有 | DocValue 原始字节来源（owned 借 storage_，mmap 借映射） |
| `rec_type_` | `format::RecordType` | 私有 | `kDoc` / `kTombstone`（墓碑在 get 中早返 kNotFound；这里记录以保完整） |
| `vector_dequant_` | `vector<float>` | 私有 | 量化文档的 f32 拥有缓冲；未量化时为空 |
| `value` | `span<const std::byte>` | 公开 | DocValue text 段（指向底层字节内部） |
| `meta` | `span<const std::byte>` | 公开 | DocValue meta 段（可为空） |
| `vector` | `span<const float>` | 公开 | 向量段（空 = 无向量；量化时指向 `vector_dequant_`） |
| `tstamp` | `uint32_t` | 公开 | record 时间戳 |
| `ord` | `uint64_t` | 公开 | 单调递增写入序号 |
| `expiry_at` | `uint32_t` | 公开 | S13-D5 per-key 过期时刻（绝对 unix 秒，0 = 永不过期） |

### 1.3 `GetResult`（owned 版本）

| 字段 | 类型 | 来源 |
|------|------|------|
| `value` | `vector<byte>` | `GetResultView::to_owned()` 从 `value` span 拷贝构造 |
| `meta` | `vector<byte>` | 同上 |
| `vector` | `vector<float>` | 同上（仅当 `vector` 非空时 assign） |
| `tstamp` | `uint32_t` | 直拷 |
| `ord` | `uint64_t` | 直拷 |

**注**：`GetResult` 不带 `expiry_at` —— 该字段仅供读路径过滤用，不进 owned
输出。需要 TTL 信息的 caller 用 `GetResultView` 读。

---

## 2. zero-copy span 的来源

### 2.1 owned（pread）路径

热路径：`Cask::get(key)` → keydir `get` → `DataFile::read(offset, sz)` →
`ReadRecord{type, tstamp, ord, total_size, key, value}`（owned vector）
→ `GetResultView(ReadRecord&&)` 构造 → `value_bytes_ = span{storage_.value}`
→ `derive_from_storage()` 解码 DocValue → `value` / `meta` / `vector`
span 指进 `storage_.value` 内部。

```
DataFile::read(pread)
  → ReadRecord { vector<byte> value }     ← 拷贝 1（pread→owned vector）
  → GetResultView ctor（move）
       value_bytes_ = span{storage_.value}     ← 指针运算
       derive_from_storage()
         decode_doc_value(value_bytes_)         ← O(1) 指针运算（无分配）
         value/meta/vector = DocValueView 内的 spans  ← 指进 storage_ 内部
  → 返回 GetResultView（持有 storage_）
```

`ReadRecord::value` 是 owned vector（`std::vector<std::byte>`，`DataFile::read`
返回值），离开 `DataFile::read` 后即脱离 pread buffer 的生命周期 → 必须
由 `GetResultView::storage_` 接住。spans 在 `derive_from_storage()` 内
从 `storage_.value` 派生，指向 vector 内部存储。

### 2.2 mmap 路径（P6）

热路径：`Cask::get(key)` 检测到 sealed 文件已 mmap（`df->mmapped() == true`）
→ `df->read_mmap(offset, sz)` 返回 `DataRecordView`（span 指向映射）
→ 构造 `GetResultView(shared_ptr<DataFile>, value_bytes, type, tstamp, ord)`
→ `map_holder_` 锚定映射，`value_bytes_` 指向映射内 DocValue 字节 →
`derive_from_storage()` 解出 spans，span 全部指向映射内。

```
DataFile::read_mmap
  → DataRecordView { span<byte> value }    ← 零拷贝（直读映射）
  → GetResultView ctor（mmap）
       map_holder_ = shared_ptr<DataFile>  ← 锚定映射，防 merge unlink
       value_bytes_ = value span          ← 指向映射内
       derive_from_storage()              ← O(1) 指针运算
  → 返回 GetResultView
```

`map_holder_` 是关键：mmap 的生命周期归 `DataFile` 析构管，析构即
munmap → 所有指向映射的 span 悬垂。`shared_ptr` 在 view 生命内保
持 `DataFile` 存活；view 析构 → 最后一份 shared_ptr 释放 → `DataFile`
析构 → munmap。即使期间并发 merge unlink 文件，mmap 也已被引用计数
保活。

### 2.3 量化文档的特殊路径（P3b）

量化文档（`opts_.vector_quantized`）落盘是 int8 codes，无法直接零拷
贝成 `span<const float>`。`derive_from_storage()` 检测到
`dv->vec_quantized` 时：

```cpp
vector_dequant_ = codec::doc_vector_f32(*dv);   // 一次 vector 分配
vector = span<const float>(vector_dequant_.data(), vector_dequant_.size());
```

`vector_dequant_` 是 `GetResultView` 私有字段（owning vector），持有
期间有效；view 析构时一并释放。量化文档的 `value` / `meta` 仍是 zero
copy（指进底层字节内部），仅 `vector` 多一次堆分配。

未量化但 4 字节未对齐的文档（S24 补）：vector 字节偏移不保证 4 对齐，
misaligned `float*` 解引是 UB。`derive_from_storage()` 检测未对齐时
复用 `vector_dequant_` 路径做对齐拷贝（一次性 vector 分配 + memcpy
对齐化），仍避免直接 deref 未对齐指针。

---

## 3. 所有权模型与生命周期

```
Cask::get(key)
  │
  ├─ keydir_->get(key) → Entry{offset, file_id, total_sz, tstamp, ord}
  ├─ df = read_file(entry->file_id)
  │     // df 是 shared_ptr<DataFile>，O10 UAF 修复：merge unlink 期间句柄不析构
  │
  ├─ if df->mmapped():
  │     rv = df->read_mmap(offset, sz)        ← 零拷贝（直读 page cache）
  │     return GetResultView(df, rv.value, rv.type, rv.tstamp, rv.ord)
  │            // map_holder_ 锚定映射
  │
  └─ else (pread 路径):
        rec = df->read(offset, sz)            ← owned vector（pread→vector）
        return GetResultView(std::move(rec))  // storage_ 接管 vector
                // value/meta/vector span 指进 storage_.value 内部

调用方持有 GetResultView 期间：
  - owned 路径：storage_ 存活 → spans 有效
  - mmap 路径：map_holder_ 引用计数 ≥ 1 → 映射不撤 → spans 有效
  - 量化路径：vector_dequant_ 存活 → vector span 有效

GetResultView 析构：
  - storage_ 析构 → owned vector 释放 → spans 失效（owned 路径）
  - map_holder_ 最后引用释放 → DataFile 析构 → munmap → spans 失效
    （mmap 路径）
  - vector_dequant_ 析构 → vector span 失效（量化 / 未对齐文档）
```

**约定**：spans 的有效期与 `GetResultView` 对象生命周期严格绑定。
move 后原对象的 spans 立即悬垂，新对象的 spans 在新对象的整个生命
期内有效。

---

## 4. 移动构造：span 重推导

符号：`src/cask/cask.cpp::GetResultView::GetResultView(GetResultView&&)`

`std::expected<GetResultView, CaskFault>` 要求 `T` 可移动构造。移动
后原对象的 spans 立即悬垂（指进了已被 move 走的 `storage_` 缓冲），
必须在 move 构造函数中从新 `storage_` / `map_holder_` 重新推导。

```cpp
GetResultView::GetResultView(GetResultView&& other) noexcept
    : storage_(std::move(other.storage_))
    , map_holder_(std::move(other.map_holder_))
    , rec_type_(other.rec_type_)
    , tstamp(other.tstamp)
    , ord(other.ord)
{
    // owned 路径：value_bytes_ 重指向自己的 storage_（other 的已悬垂）；
    // mmap 路径：映射地址稳定，沿用 other 的字节区（map_holder_ 已移交本对象）。
    value_bytes_ = map_holder_ ? other.value_bytes_
                               : std::span<const std::byte>(storage_.value);
    derive_from_storage();
}
```

`decode_doc_value` 仅做指针运算（解析 DocValue 头部偏移量），O(1)
无分配，重推导代价可忽略。`vector_dequant_` 由 `derive_from_storage()`
按需填充；mmap 路径下 `other.vector_dequant_` 已被 move 走（自有 owned
vector）——重推导会重新 `doc_vector_f32()` 一次。这是为何量化 mmap
路径的 `GetResultView` move 比 owned 路径稍贵；非量化路径为 O(1)。

### 4.1 为什么删除拷贝

```cpp
GetResultView(const GetResultView&)            = delete;
GetResultView& operator=(const GetResultView&) = delete;
```

两个独立原因：

1. **spans 的语义不允许「重指向」**：`value` / `meta` / `vector` 是
   `std::span<const T>`，底层指向 `storage_.value` / `vector_dequant_`
   内部。拷贝构造会让两个 view 的 spans 指进同一个 owning 容器——
   容器 move 后两边 spans 同时悬垂，类型系统无法表达这个「逻辑所有
   权唯一但指针共享」的语义。move-only 是 `std::span` 与 owning 容
   器组合时的标准做法。
2. **不期望 caller 拷贝**：`GetResultView` 的设计目标是「即取即用」
   （C API handler 拿到 spans → 拷到 `bitcask_get_result_t` → 释放
   view）；需要持久化的场景用 `to_owned()` 显式得到 `GetResult`（独立
   owning，复制无歧义）。

### 4.2 为什么 move ctor 必须 `derive_from_storage()` 而不是直接 move spans

`std::span` move 等价拷贝（指针 + 长度）。但 owned 路径下 move 后
`other.storage_.value.data()` 已指向被 move 走的 vector（被接管后
`other.storage_.value` 空了）——不能直接拿 `other.value` / `other.meta`
当新 spans。`map_holder_` 路径下映射地址稳定，可以保留 `other.value_bytes_`
不动（因为 `map_holder_` 移交到本对象，spans 仍指向被保活的映射）。

---

## 5. `to_owned()` 语义与开销

符号：`src/cask/cask.cpp::GetResultView::to_owned`

```cpp
GetResult GetResultView::to_owned() const {
    GetResult out{
        std::vector<std::byte>(value.begin(), value.end()),
        std::vector<std::byte>(meta.begin(),  meta.end()),
        {},
        tstamp,
        ord
    };
    if (!vector.empty()) {
        out.vector.assign(vector.begin(), vector.end());
    }
    return out;
}
```

### 5.1 开销

| 段 | 操作 | 典型开销 |
|----|------|---------|
| `value` | `vector<byte>(span.begin(), span.end())` —— 1 次堆分配 + memcpy | O(value.size) |
| `meta` | 同上 | O(meta.size) |
| `vector` | 空时跳过；非空时 `assign` —— 1 次堆分配 + memcpy | O(dim × 4 B) |
| `tstamp` / `ord` | 直拷 | 0 |
| **总计** | **2-3 次堆分配 + 2-3 次堆释放 + 2-3 次 memcpy** | 决定于 value / meta / vector 长度 |

`Cask::get_owned(key)` 内部即调 `get(key)` → `to_owned()`：

```cpp
// src/cask/cask.cpp
std::expected<GetResult, CaskFault>
Cask::get_owned(std::span<const std::byte> key) {
    auto v = get(key);
    if (!v) return std::unexpected(v.error());
    return v->to_owned();
}
```

### 5.2 与 zero-copy 路径的对比

| 指标 | `get`（zero-copy） | `get_owned`（owned） |
|------|--------------------|----------------------|
| 堆分配次数 | 0（非量化）；1（量化 / 未对齐） | 2-3 |
| memcpy 次数 | 0（非量化）；1（量化） | 2-3 |
| 适用场景 | C API 即取即用、查询回调内消费 | benchmark、测试、跨线程传递、长期持有 |

C API handler 直接从 view 的 spans 填充 `bitcask_get_result_t`，无中间
owned 构造：

```cpp
// c_api/bitcask_c.cpp — 改造后
auto r = h->cask->get(as_bytes(key));    // GetResultView
if (!r) return translate_fault(r.error(), fault);

out->value    = {r->value.data(), r->value.size()};    // 零拷贝借用
out->meta     = {r->meta.data(),  r->meta.size()};
out->vector   = r->vector.data();
out->vector_len = r->vector.size();
out->tstamp   = r->tstamp;
out->ord      = r->ord;
// GetResultView 保持存活直到 caller 调 bitcask_get_result_free
```

---

## 6. `derive_from_storage()` —— 解码与 span 派生

符号：`src/cask/cask.cpp::GetResultView::derive_from_storage`

```cpp
void GetResultView::derive_from_storage() {
    if (rec_type_ != format::RecordType::kDoc) return;  // tombstone 不解码
    if (value_bytes_.empty()) return;
    auto dv = codec::decode_doc_value(value_bytes_);
    if (!dv) return;  // corrupt DocValue → empty spans
    value     = dv->text;
    meta      = dv->meta;
    expiry_at = dv->expiry_at;     // S13-D5
    if (dv->vec_quantized) {
        vector_dequant_ = codec::doc_vector_f32(*dv);
        vector = std::span<const float>(vector_dequant_.data(),
                                        vector_dequant_.size());
    } else if (dv->has_vector && dv->dim > 0 &&
               dv->vector_raw.size() == dv->dim * sizeof(float)) {
        const auto* p = dv->vector_raw.data();
        if (reinterpret_cast<std::uintptr_t>(p) % alignof(float) == 0) {
            // 对齐 → 零拷贝 view（指 dv->vector_raw 内部；dv 又指 value_bytes_）
            vector = std::span<const float>(
                reinterpret_cast<const float*>(p), dv->dim);
        } else {
            // 未对齐 → 拷进 vector_dequant_ 对齐化（防 UB）
            vector_dequant_.resize(dv->dim);
            std::memcpy(vector_dequant_.data(), p,
                        static_cast<std::size_t>(dv->dim) * sizeof(float));
            vector = std::span<const float>(vector_dequant_.data(), dv->dim);
        }
    }
}
```

要点：

- 仅对 `kDoc` 解码；`kTombstone` 早返（get 路径上墓碑已早返 kNotFound，
  此处为防御）。
- `decode_doc_value` 是 O(1) 纯函数，只做指针运算（解析 DocValue 头
  Ver / Flags / 各段偏移），无堆分配。
- 量化 → dequant 进 `vector_dequant_` 拥有缓冲；非量化 + 对齐 → 直接
  span 借底层字节（**真零拷贝**）；非量化 + 未对齐 → `memcpy` 进
  `vector_dequant_` 对齐化（防 misaligned `float*` 解引 UB）。
- `expiry_at` 从 `dv->expiry_at` 读出（S13-D5 per-key TTL），由调用方
  与 `now_sec` 比对判过期。

---

## 7. 前置条件与约束

- **`std::expected<T, E>` 要求 `T` 可移动构造** —— `GetResultView` 满
  足（定义了 move ctor，noexcept）。
- **`decode_doc_value` 必须是 O(1) 无分配** —— 当前实现仅做指针运算，
  满足。
- **`std::vector` move 是 O(1) pointer steal**（非 SSO 场景）—— 标准
  保证，满足。
- **`map_holder_` shared_ptr 引用计数开销** —— mmap 路径每次 view 多
  一次 `fetch_add` / `fetch_sub`；与一次 syscall（pread）相比可忽略。
- **view 不可比支撑对象活得更久**：
  - owned 路径：`ReadRecord` 数据来自 pread，不依赖 `DataFile` 生命
    周期；view 析构 → `storage_` 析构 → vector 释放。
  - mmap 路径：`map_holder_` 保活 `DataFile` → 保活映射；view 析构
    → 最后 shared_ptr 释放 → `DataFile` 析构 → munmap。

---

## 8. C API 适配

| 步骤 | 动作 |
|------|------|
| 1 | C API handler 调 `Cask::get(key)` → `expected<GetResultView, CaskFault>` |
| 2 | 错误路径：`translate_fault` 翻成 `bitcask_error_t` + `bitcask_fault_t` |
| 3 | 成功路径：从 `r->value` / `r->meta` / `r->vector` span 填充 `bitcask_get_result_t` 的 slice / pointer 字段 |
| 4 | view 保持存活直到 caller 调 `bitcask_get_result_free` |
| 5 | `bitcask_get_result_free` 触发 view 析构 → spans 失效 |

**消除的中间拷贝**：原 `GetResult{vector<byte> value/meta, vector<float> vector}`
构造取消；view 的 span 直接被 C API slice 借用。

---

## 9. 两条路径共存（owned vs mmap）

| 维度 | owned（pread） | mmap（P6） |
|------|----------------|------------|
| 触发条件 | `df->mmapped() == false` | `df->mmapped() == true`（sealed + 64-bit + `mmap_enabled`） |
| 数据来源 | `pread(2)` 到 ReadRecord 内部 vector | `mmap(2)` 整文件映射（PROT_READ / MAP_SHARED） |
| 锚定对象 | `storage_`（`ReadRecord`） | `map_holder_`（`shared_ptr<DataFile>`） |
| syscall | `pread` 每次 | 无（直读 page cache） |
| 内存占用 | 1 fd + 临时缓冲 | 1 fd + 1 mmap（cask `max_read_handles` 统一管） |
| merge unlink 安全性 | pread 后即脱离文件生命周期 | `map_holder_` 保活映射 |
| 构造代价 | 1 次堆分配 + 1 次 pwrite-sized memcpy | 0 分配 + 0 memcpy（直读映射） |

两条路径通过 `map_holder_` 的有 / 无区分构造函数，spans 由共用
`derive_from_storage()` 派生 —— 三处构造（owned ctor / mmap ctor /
move ctor）逻辑共用，避免漂移。

---

## 10. 不做的事

- **不引入 `enif_make_resource_binary`** —— Erlang NIF 已移除，C API
  通过 `bitcask_get_result_free` 显式释放，无需 GC 交互。
- **不改变 `DataFile::read()` 签名** —— `ReadRecord` 返回 by value 已
  满足需求，move 进 `GetResultView` 是 O(1) 零开销。
- **不做 shared_ptr 默认开启** —— owned 路径下 `map_holder_` 为空
  shared_ptr（构造 / 析构零开销）；mmap 路径才付一次 `fetch_add` /
  `fetch_sub`。
- **不在 view 内缓存完整 DocValueView** —— `DocValueView` 是临时解
  码结果（多 `vector<DocField>` 等 owning 字段），缓存会让 view 不再
  零分配；view 只持有最终需要的 spans。

---

## 11. 源码导航

| 符号 | 角色 |
|------|------|
| `include/bitcask/cask.hpp::GetResultView` | 主结构定义 |
| `include/bitcask/cask.hpp::GetResult` | owned 版本 |
| `include/bitcask/cask.hpp::GetResultView::GetResultView(ReadRecord&&)` | owned 路径构造 |
| `include/bitcask/cask.hpp::GetResultView::GetResultView(shared_ptr<DataFile>, ...)` | mmap 路径构造 |
| `include/bitcask/cask.hpp::GetResultView::derive_from_storage` | span 派生 |
| `include/bitcask/cask.hpp::GetResultView::to_owned` | owned 拷贝 |
| `src/cask/cask.cpp::GetResultView::*` | 实现（解码 / 派生 / move / to_owned） |
| `src/cask/cask.cpp::Cask::get` | 零拷贝读主路径（owned + mmap 分派） |
| `src/cask/cask.cpp::Cask::get_owned` | owned 拷贝读 |
| `src/fileops/data_file.cpp::DataFile::read` | pread → `ReadRecord` |
| `src/fileops/data_file.cpp::DataFile::read_mmap` | mmap → `DataRecordView` |
| `include/bitcask/data_file.hpp::ReadRecord` | owned 缓冲结构 |
| `include/bitcask/codec.hpp::decode_doc_value` | O(1) 指针运算 DocValue 解码 |
| `include/bitcask/codec.hpp::doc_vector_f32` | 量化 → f32 dequant |
| `include/bitcask/format.hpp::RecordType` / `kDocValueVersion` | record 类型与 DocValue 版本 |
| `c_api/bitcask_c.cpp::bitcask_get` | C API 直接消费 view spans |