# 向量数据库 ANN 算法全景（2024-2026）

各向量数据库的索引算法选型对比，以及 HNSW 在行业中的定位。配合 [`hnsw-overview-zh.md`](hnsw-overview-zh.md)（HNSW 算法原理）与 [`hnsw-design-zh.md`](hnsw-design-zh.md)（本实现设计）阅读。

---

## 一、各数据库用什么算法

| 数据库 | 主算法 | 次算法 | 语言 | 关键特点 |
|--------|--------|--------|------|----------|
| **Qdrant** | HNSW | Plain(精确) | Rust | Filterable HNSW（过滤在图遍历中做，非后过滤） |
| **Weaviate** | HNSW | Flat | Go | 原生 BM25+向量混合检索（RRF 融合） |
| **Milvus** | HNSW | IVF-PQ、DiskANN、ScaNN、GPU-CAGRA | Go+C++ | 索引类型最全；分布式分层存储（RAM→NVMe→S3） |
| **pgvector** | HNSW | IVFFlat、DiskANN(0.8+) | C | PostgreSQL 扩展；HNSW 索引 0.7+ 生产可用 |
| **Elasticsearch** | HNSW (Lucene) | Flat | Java | 每段（segment）独立 HNSW；BBQ 二值量化 |
| **Pinecone** | **专有自适应**（非 HNSW） | — | C++/Rust | 按 slab 大小自动选 Ananas / PQFS / IVF；**唯一不用 HNSW 默认的主流 DB** |
| **Vespa** | HNSW | 精确 NN | Java | ACORN-1 过滤 + Adaptive Beam Search |
| **Chroma** | HNSW (hnswlib) | SPANN/SPFresh(云) | Python+Rust | 单机 HNSW；云端用 SPANN 做水平扩展 |
| **FAISS** | IVF-PQ | HNSW、Flat、CAGRA(GPU) | C++ | 金标准库；GPU 加速经 cuVS |
| **Redis** | HNSW | FLAT、SVS-VAMANA | C | Redis 8.2+ 加入 DiskANN 式 SVS-VAMANA |
| **MongoDB Atlas** | HNSW | Flat | C++ | 标量/二值量化 |
| **LanceDB** | IVF-PQ(磁盘原生) | IVF-HNSW、IVF-RQ(RaBitQ) | Rust | 磁盘优先设计；RaBitQ 32× 压缩 |

**一句话**：除了 Pinecone（专有）和 LanceDB（IVF-PQ 磁盘原生），**几乎所有主流向量数据库的默认内存 ANN 都是 HNSW**。本实现选择 HNSW 符合行业主流。

---

## 二、四大算法家族

### 谱系图

```
                        ANN 算法
            ┌──────────┬──────────┬──────────┬──────────┐
         图基          聚类/量化     树基        哈希
     ┌────┴────┐     ┌──┴──┐       │          │
   HNSW   DiskANN   IVF   ScaNN   Annoy      LSH
    │     (Vamana)   │  └─PQ/RaBitQ│       (已过时)
   CAGRA              │
  (GPU)            IVF-PQ
```

### 对比矩阵

| 维度 | HNSW | DiskANN | IVF-PQ | ScaNN | CAGRA | Annoy |
|------|------|---------|--------|-------|-------|-------|
| **家族** | 图 | 图(单层) | 聚类+量化 | 树+量化 | 图(GPU) | 树 |
| **搜索复杂度** | O(log N) | O(log N) | O(√N) | O(√N) | O(log N) | O(log N) |
| **内存/向量(1024dim)** | ~4 KB | 128B RAM+4KB SSD | **128 B** | ~400 B | ~4 KB | ~1 KB |
| **召回率@10** | **98-99%** | 95-98% | 85-95% | 95-98% | 98-99% | 85-95% |
| **延迟(p99)** | **5-15ms** | 30-80ms | 10-50ms | 8-20ms | 1-5ms(GPU) | 10-30ms |
| **构建速度** | 慢 | 很慢 | 中 | 中 | **快(GPU 12×)** | 中 |
| **增量插入** | 是 | 否(批量) | 否(需重训) | 否 | 是 | 否(重建) |
| **删除** | 软删除 | 软删除 | 重训 | 重训 | 软删除 | 重建 |
| **磁盘友好** | 否(全内存) | **是** | 是 | 否 | 否 | 是(mmap) |
| **GPU 加速** | 否(CPU) | 否 | 是 | 否 | **是** | 否 |

---

## 三、HNSW 的主要挑战者

### 3.1 DiskANN（Microsoft Research）— 磁盘之王

```
HNSW:   全图 + 全向量 → 全在 RAM
DiskANN: Vamana 图(单层) + PQ 压缩向量 → RAM
         全精度向量 → SSD
```

| vs HNSW | 优势 | 劣势 |
|---------|------|------|
| 内存 | **~12× 更少 RAM**（64GB RAM 可服务 SIFT1B 10 亿向量） | — |
| 延迟 | — | p99 30-80ms（HNSW 5-15ms），SSD 随机读瓶颈 |
| 构建 | — | 更慢；alpha-relaxed pruning 更复杂 |
| 适用 | **>1 亿向量，RAM 受限** | <5000 万向量时 HNSW 更优 |

**用 DiskANN 的 DB**：Milvus、pgvector(0.8+)、Redis(8.2+ SVS-VAMANA)

> 参考：Singh et al., *"FreshDiskANN: A Streamed and Disk-Based Index for ANN Search"*, NeurIPS 2021。DiskANN 在 SIFT1B（10 亿 128 维）上用 16 核 + 64GB RAM + 单块 SSD 达到 5000+ QPS / <3ms 均值延迟 / 95%+ recall@1。

### 3.2 ScaNN（Google Research）— 内积优化

Google 搜索 / YouTube 推荐 / Google Photos 的生产算法。

| vs HNSW | 优势 | 劣势 |
|---------|------|------|
| 精度 | 各向异性量化（anisotropic quantization）为内积排序专门优化，非通用重建误差 | 主要面向 MIPS/内积，L2/cosine 不如 HNSW |
| 内存 | ~10× 压缩（3 GB vs HNSW 12 GB，同规模） | — |
| 生态 | — | Google 生态外工具少；功能不如 FAISS/HNSW 丰富 |
| 适用 | **超大内积检索** | 通用场景 HNSW 更灵活 |

> 参考：Guo et al., *"Accelerating Large-Scale Inference with Anisotropic Vector Quantization"*, ICML 2020。

### 3.3 CAGRA（NVIDIA）— GPU 加速

| vs HNSW | 优势 | 劣势 |
|---------|------|------|
| 构建速度 | **2.2-27× 更快**（GPU 并行建图） | 必须 GPU |
| 查询吞吐 | **33-77× 更高**（大批量查询，90-95% 召回） | 单查询时 GPU 启动开销抵消优势 |
| CPU 回退 | 可将 GPU 建的图转为 HNSW 格式给 CPU 查询 | — |
| 适用 | **GPU 环境、大批量检索** | 纯 CPU 部署无优势 |

**用 CAGRA 的 DB**：Milvus(GPU_CAGRA)、FAISS(经 cuVS)

> 参考：Ootomo et al., *"CAGRA: Highly Parallel Graph Construction and Approximate Nearest Neighbor Search for GPUs"*, ICDE 2024。

### 3.4 IVF-PQ — 成本敏感的大规模方案

| vs HNSW | 优势 | 劣势 |
|---------|------|------|
| 内存 | **32× 压缩**（4096B → 128B/向量） | 召回率掉 5-10% |
| 延迟 | — | 10-50ms（HNSW 5-15ms） |
| 更新 | — | 需要重训聚类中心，不支持增量 |
| 适用 | **1 亿-10 亿向量，成本敏感** | <1000 万时 HNSW 更优 |

> 参考：Jégou et al., *"Product quantization for nearest neighbor search"*, IEEE TPAMI 2011。

---

## 四、选型决策树

```
向量数量？
├─ < 10 万 ──────────────────→ 暴力搜索（不需要索引）
├─ 10 万 - 5000 万 ─────────→ HNSW ← 本实现 & 行业默认
│   ├─ 内存充足 ─────────────→ HNSW (f32)
│   ├─ 内存紧张 ─────────────→ HNSW + 量化 (int8 / binary)
│   │                          ← 本实现已做 int8 + VNNI
│   └─ 已有 PostgreSQL ─────→ pgvector (HNSW)
├─ 5000 万 - 10 亿 ──────────→ IVF-PQ 或 HNSW + 分片
│   ├─ 有 GPU ───────────────→ CAGRA (GPU)
│   ├─ RAM 受限 ─────────────→ DiskANN
│   └─ 内积检索 ─────────────→ ScaNN
└─ > 10 亿 ──────────────────→ DiskANN 或分布式(Milvus)
```

---

## 五、本实现 vs 行业

| 维度 | 本实现 (libbitcask HNSW) | 行业主流 |
|------|--------------------------|----------|
| 算法 | HNSW | HNSW（Qdrant / Weaviate / ES / pgvector 同选） |
| 量化 | int8 对称量化 + VNNI 粗筛 | Qdrant(Rust) 同路；ES 用 BBQ 二值；Milvus 用 PQ |
| 并发 | 单写者 + 多读者无锁发布 | Qdrant 同（Rust atomic）；hnswlib 无并发 |
| 删除 | 软删除 + merge 重建 | 行业标准做法（Qdrant / Weaviate 同） |
| 磁盘 | 全内存（BCVS 快照持久化） | 同 HNSW 默认；DiskANN 是另一条路线 |
| 缺什么 | 无 DiskANN / 无 GPU / 无 PQ | Milvus 全有；Qdrant 有 on-disk HNSW |

**结论**：本实现在 <5000 万向量 + 内存型场景下与 Qdrant / Weaviate / Elasticsearch 处于同一技术路线（HNSW + int8 量化 + 无锁并发读），选型正确。若未来需要超内存规模，可加 DiskANN 路线（单层 Vamana 图 + SSD 存全精度向量 + RAM 存 PQ 压缩）。
