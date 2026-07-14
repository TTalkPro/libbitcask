# Google C++ Style 规范化修复任务清单

> 来源：`RISK_REPORT.md`（2026-07-14）
> 范围：5 项行动建议，按 ROI 排序
> 基线测试：641/641 ctest 通过（1 个 S30RssProbe 预存 Disabled）
> 验收标准：每项改动后 ctest 全绿 + 编译无新告警

---

## ✅ Phase 1：低风险快速修复（已完成）

### T1 — CRC32 入口统一（MED-3） ✅

把 3 个文件直接调 `hw::crc32*` 的 10 处改为 `codec::crc32*`，让硬件派发藏在 codec 内部。

- ✅ `src/bm25/segment_v2.cpp`（6 处）
- ✅ `src/cask/meta_file.cpp`（1 处）
- ✅ `include/bitcask/field_schema.hpp`（3 处）
- ✅ 641/641 ctest 通过

### T2 — vbyte 带检查版本归并（MED-2） ✅

三份带边界检查的 vbyte 解码归并到 `vbyte.hpp::vbyte_read_checked`
（`std::optional<pair<u64, size_t>>` 返回，无歧义）。无检查版 `vbyte_decode`
保持独立（热路径契约，S21-2 A3 既定）。

- ✅ `vbyte.hpp` 增加 `vbyte_read_checked` 权威实现
- ✅ `codec.cpp` 删本地 vbyte_read，5 处调用迁移
- ✅ `meta_codec.hpp` 删 detail::vbyte_read + 内联 lambda，6 处调用迁移
- ✅ 641/641 ctest 通过

---

## ✅ Phase 2：基础设施（已完成）

### T3 — `.clang-tidy` 配置 ✅

- ✅ `.clang-tidy`（google-* + modernize/bugprone/performance/readability/cert/cppcoreguidelines 基线，关闭与项目风格冲突的 ~15 项）
- ✅ `.github/workflows/ci.yml` 加 `clang-tidy` job（continue-on-error，待存量清零改门控）
- ✅ 本地 clang-tidy 21 验证：codec.cpp 从数百告警降到 7 条真问题

### T4 — `MmapRegion` RAII 基础设施 ✅（渐进式应用）

- ✅ `include/bitcask/detail/mmap_handle.hpp` 创建（MmapRegion 类 + 析构序安全文档）
- ⏩ 全量应用到 8 处 mmap 站点改为**渐进式**——每处需成员布局重写（30+ 访问点/文件），
  风险/收益不划算。后续随其他重构（如 T5、引擎迭代）自然推广。

---

## ⏳ Phase 3：高风险高价值（进行中）

### T5 — `SealedSegmentVectorPlugin` 抽取（HIGH-1） ⏳

IVF/DiskANN 插件 ~950 行近乎完全重复。抽 Template Method 中间基类，
子类只 override 差异点（search_sealed / ckpt_name / blob_size 等钩子）。

- ⏳ `include/bitcask/detail/sealed_segment_vector_plugin.hpp` 创建
- ⏳ `ivf_plugin.cpp` / `diskann_plugin.cpp` 各收缩到 ~100 行
- ⏳ 删除 LOW-3（comp_path/biv_path_of/bda_path_of 一并模板化）
- ⏳ IVF + DiskANN 测试套件全过
- **委派给 deep agent 执行**（task: bg_92e8ae3a）

---

## 当前状态快照（T5 进行中）

| 项 | 状态 | 验证 |
|---|---|---|
| T1 CRC32 入口统一 | ✅ done | 641/641 ctest |
| T2 vbyte 归并 | ✅ done | 641/641 ctest |
| T3 .clang-tidy | ✅ done | 本地 clang-tidy 21 验证 |
| T4 MmapRegion | ✅ 基础设施 done | 类已创建，渐进式应用 |
| T5 PluginBase | ⏳ deep agent 执行中 | 待验证 |

