#include "bitcask_c.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* S37-5：原先用固定 "/tmp/..." 路径 + system("rm -rf ...") 清理，两者在
 * Windows 上都不成立（无 /tmp，也没有 rm 命令——实测报「'rm' 不是内部或
 * 外部命令」，随后陈旧目录污染 reopen，用例断在一个与真因无关的地方）。
 *
 * 本文件是 C，用不了 <filesystem>：改成平台条件的目录前缀 + rmrf() 外壳调用。
 * Windows 侧用相对当前工作目录的名字——ctest 给每个测试设了工作目录，
 * 天然隔离，不必去解析 %TEMP%。 */
#if defined(_WIN32)
#  define BITCASK_TEST_DIR_PREFIX "bitcask_c_test_"
#else
#  define BITCASK_TEST_DIR_PREFIX "/tmp/bitcask_c_test_"
#endif
#define TDIR(name) (BITCASK_TEST_DIR_PREFIX name)

static void rmrf(const char* dir) {
    char cmd[512];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "rd /s /q \"%s\" >nul 2>&1", dir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" >/dev/null 2>&1", dir);
#endif
    /* C 下 `(void)` 压不住 warn_unused_result（与 C++ 的 [[nodiscard]] 不同），
       须真的接住返回值。清理是尽力而为，失败不影响用例。 */
    const int rc = system(cmd);
    (void)rc;
}

/* 运行前统一清空：跨运行/跨二进制版本累积的 checkpoint 残留会污染 reopen
 * （尤以向量批量用例敏感，陈旧 vec.ckpt → 搜索命中 0）。
 * 通配符交给 shell 在 POSIX 上可行，Windows 的 rd 不认通配符，故逐个列举。 */
static const char* const kAllTestDirs[] = {
    TDIR("batch"),
    TDIR("filter"),
    TDIR("iter"),
    TDIR("kv"),
    TDIR("levelb"),
    TDIR("pscan"),
    TDIR("putbatch"),
    TDIR("range"),
    TDIR("status"),
    TDIR("statusex"),
    TDIR("txn"),
    TDIR("veng"),
    TDIR("vhbatch"),
};

static void cleanup_all_test_dirs(void) {
    for (size_t i = 0; i < sizeof(kAllTestDirs) / sizeof(kAllTestDirs[0]); ++i) {
        rmrf(kAllTestDirs[i]);
    }
}

static int test_kv_basic(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("kv"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_kv_basic: open failed: %s\n", fault.detail);
        return 1;
    }

    bitcask_slice_t key = {"hello", 5};
    bitcask_slice_t val = {"world", 5};
    err = bitcask_put(cask, key, val, 0, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_kv_basic: put failed\n");
        bitcask_close(cask);
        return 1;
    }

    bitcask_get_result_t* result = NULL;
    err = bitcask_get(cask, key, &result, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_kv_basic: get failed\n");
        bitcask_close(cask);
        return 1;
    }
    assert(result->value.size == 5);
    assert(memcmp(result->value.data, "world", 5) == 0);
    bitcask_get_result_free(result);

    bitcask_slice_t missing_key = {"nokey", 5};
    err = bitcask_get(cask, missing_key, &result, &fault);
    assert(err == BITCASK_ERR_NOT_FOUND);
    assert(result == NULL);

    err = bitcask_delete(cask, key, 0, &fault);
    assert(err == BITCASK_OK);
    err = bitcask_get(cask, key, &result, &fault);
    assert(err == BITCASK_ERR_NOT_FOUND);

    bitcask_close(cask);
    printf("PASS test_kv_basic\n");
    return 0;
}

static int test_status_and_merge(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("status"), &opts, &cask, &fault);
    assert(err == BITCASK_OK);

    for (int i = 0; i < 100; i++) {
        char kbuf[32], vbuf[64];
        int klen = snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        int vlen = snprintf(vbuf, sizeof(vbuf), "value_%d", i);
        bitcask_slice_t k = {kbuf, (size_t)klen};
        bitcask_slice_t v = {vbuf, (size_t)vlen};
        err = bitcask_put(cask, k, v, 0, &fault);
        assert(err == BITCASK_OK);
    }

    bitcask_status_t st;
    err = bitcask_status(cask, &st, &fault);
    assert(err == BITCASK_OK);
    assert(st.key_count == 100);
    assert(st.key_bytes > 0);
    printf("  status: %llu keys, %llu bytes\n",
           (unsigned long long)st.key_count,
           (unsigned long long)st.key_bytes);

    bitcask_needs_merge_t nm;
    err = bitcask_needs_merge(cask, &nm, &fault);
    assert(err == BITCASK_OK);
    bitcask_needs_merge_free(&nm);

    assert(bitcask_is_empty(cask) == 0);

    bitcask_close(cask);
    printf("PASS test_status_and_merge\n");
    return 0;
}

static int test_iteration(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("iter"), &opts, &cask, &fault);
    assert(err == BITCASK_OK);

    for (int i = 0; i < 10; i++) {
        char kbuf[16], vbuf[32];
        int klen = snprintf(kbuf, sizeof(kbuf), "k%d", i);
        int vlen = snprintf(vbuf, sizeof(vbuf), "val%d", i);
        bitcask_slice_t k = {kbuf, (size_t)klen};
        bitcask_slice_t v = {vbuf, (size_t)vlen};
        bitcask_put(cask, k, v, 0, &fault);
    }

    bitcask_iter_t* iter = NULL;
    err = bitcask_iter_start(cask, -1, -1, 0, &iter, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_iteration: iter_start failed: %d\n", err);
        bitcask_close(cask);
        return 1;
    }

    int count = 0;
    bitcask_iter_entry_t entry;
    while (bitcask_iter_next(iter, &entry, &fault) == 1) {
        count++;
        bitcask_iter_entry_free(&entry);
    }
    assert(count == 10);

    bitcask_iter_release(iter);
    bitcask_close(cask);
    printf("PASS test_iteration\n");
    return 0;
}

/* S33-6：有序 range 迭代（OKI）+ 前缀迭代/前缀并行扫描的 C 侧覆盖。
 * 数据形态 "gNNN:kMMM" —— 前缀组即天然的 range 窗口。 */
static int test_range_iter(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err =
        bitcask_open(TDIR("range"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_range_iter: open: %s\n", fault.detail);
        return 1;
    }

    for (int g = 0; g < 4; g++) {
        for (int i = 0; i < 25; i++) {
            char kbuf[24], vbuf[32];
            int klen = snprintf(kbuf, sizeof(kbuf), "g%03d:k%03d", g, i);
            int vlen = snprintf(vbuf, sizeof(vbuf), "v%d-%d", g, i);
            bitcask_slice_t k = {kbuf, (size_t)klen};
            bitcask_slice_t v = {vbuf, (size_t)vlen};
            assert(bitcask_put(cask, k, v, 0, &fault) == BITCASK_OK);
        }
    }
    /* 删两条：range 只产出活 key。 */
    bitcask_slice_t dead1 = {"g001:k000", 9};
    bitcask_slice_t dead2 = {"g001:k024", 9};
    assert(bitcask_delete(cask, dead1, 0, &fault) == BITCASK_OK);
    assert(bitcask_delete(cask, dead2, 0, &fault) == BITCASK_OK);

    /* 窗口 [g001:, g002:) —— 组 1 剩 23 条，升序。 */
    bitcask_range_options_t ro;
    bitcask_range_options_init(&ro);
    ro.lo.data = "g001:";
    ro.lo.size = 5;
    ro.hi.data = "g002:";
    ro.hi.size = 5;

    bitcask_range_iter_t* rit = NULL;
    err = bitcask_range_iter_start(cask, &ro, &rit, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_range_iter: range_iter_start: %s\n",
                fault.detail);
        bitcask_close(cask);
        return 1;
    }
    int count = 0;
    char prev[32];
    size_t prev_len = 0;
    bitcask_range_entry_t rentry;
    while (bitcask_range_iter_next(rit, &rentry, &fault) == 1) {
        assert(rentry.key.size == 9);
        assert(memcmp(rentry.key.data, "g001:", 5) == 0);
        if (prev_len > 0) {  /* 严格升序 */
            assert(memcmp(prev, rentry.key.data, 9) < 0);
        }
        memcpy(prev, rentry.key.data, rentry.key.size);
        prev_len = rentry.key.size;
        assert(rentry.value.size > 0);
        count++;
        bitcask_range_entry_free(&rentry);
    }
    bitcask_range_iter_release(rit);
    assert(count == 23);

    /* 同窗口 + 值预取：条数必须一致（预取只改取值时机）。 */
    ro.prefetch = 8;
    ro.prefetch_threads = 3;
    rit = NULL;
    assert(bitcask_range_iter_start(cask, &ro, &rit, &fault) == BITCASK_OK);
    int pcount = 0;
    while (bitcask_range_iter_next(rit, &rentry, &fault) == 1) {
        pcount++;
        bitcask_range_entry_free(&rentry);
    }
    bitcask_range_iter_release(rit);
    assert(pcount == count);

    /* 批量取：一次要 100 条，先得 23 条再得 0（EOI）。 */
    bitcask_range_entry_t batch[100];
    rit = NULL;
    assert(bitcask_range_iter_start(cask, &ro, &rit, &fault) == BITCASK_OK);
    int n = bitcask_range_iter_next_batch(rit, batch, 100, &fault);
    assert(n == 23);
    for (int i = 0; i < n; i++) bitcask_range_entry_free(&batch[i]);
    assert(bitcask_range_iter_next_batch(rit, batch, 100, &fault) == 0);
    bitcask_range_iter_release(rit);

    /* opts == NULL → 全域（100 - 2 删除 = 98）。 */
    rit = NULL;
    assert(bitcask_range_iter_start(cask, NULL, &rit, &fault) == BITCASK_OK);
    int total = 0;
    while (bitcask_range_iter_next(rit, &rentry, &fault) == 1) {
        total++;
        bitcask_range_entry_free(&rentry);
    }
    bitcask_range_iter_release(rit);
    assert(total == 98);

    /* 参数校验：NULL 句柄/out。 */
    assert(bitcask_range_iter_start(NULL, &ro, &rit, &fault) ==
           BITCASK_ERR_INVALID_OPTION);
    assert(bitcask_range_iter_start(cask, &ro, NULL, &fault) ==
           BITCASK_ERR_INVALID_OPTION);
    assert(bitcask_range_iter_next(NULL, &rentry, &fault) == -1);
    bitcask_range_iter_release(NULL);  /* no-op */
    bitcask_range_entry_free(NULL);    /* no-op */

    /* S33-6：前缀迭代（O(全表) 过滤，语义对照 range）。 */
    bitcask_slice_t prefix = {"g001:", 5};
    bitcask_iter_t* it = NULL;
    assert(bitcask_iter_start_prefix(cask, -1, -1, 0, prefix, &it, &fault) ==
           BITCASK_OK);
    int pref_count = 0;
    bitcask_iter_entry_t entry;
    while (bitcask_iter_next(it, &entry, &fault) == 1) {
        assert(entry.key.size == 9);
        assert(memcmp(entry.key.data, "g001:", 5) == 0);
        pref_count++;
        bitcask_iter_entry_free(&entry);
    }
    bitcask_iter_release(it);
    assert(pref_count == 23);

    /* 空前缀 = 全表（等价 bitcask_iter_start）。 */
    bitcask_slice_t empty_prefix = {NULL, 0};
    it = NULL;
    assert(bitcask_iter_start_prefix(cask, -1, -1, 0, empty_prefix, &it,
                                     &fault) == BITCASK_OK);
    int all_count = 0;
    while (bitcask_iter_next(it, &entry, &fault) == 1) {
        all_count++;
        bitcask_iter_entry_free(&entry);
    }
    bitcask_iter_release(it);
    assert(all_count == 98);

    bitcask_close(cask);
    printf("PASS test_range_iter\n");
    return 0;
}

static int test_version(void) {
    assert(bitcask_version_major() > 0);
    assert(bitcask_version_string() != NULL);
    printf("  version: %s\n", bitcask_version_string());
    printf("PASS test_version\n");
    return 0;
}

static int test_search_text_batch(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("batch"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_search_text_batch: open failed: %s\n", fault.detail);
        return 1;
    }

    const char* docs[3] = {"hello world", "hello there", "foo bar"};
    const char* keys[3] = {"d0", "d1", "d2"};
    for (int i = 0; i < 3; i++) {
        bitcask_doc_input_t doc;
        memset(&doc, 0, sizeof(doc));
        doc.text.data = docs[i];
        doc.text.size = strlen(docs[i]);
        bitcask_slice_t key = {keys[i], strlen(keys[i])};
        err = bitcask_put_doc(cask, key, &doc, 0, &fault);
        assert(err == BITCASK_OK);
    }
    bitcask_flush_index(cask);  // 等异步索引管线消费完，再搜索

    // 批量：hello→{d0,d1}、foo→{d2}、zzz→{}（无命中：非 NULL 结果、count==0）。
    const char* queries[3] = {"hello", "foo", "zzz"};
    bitcask_search_result_t** results = NULL;
    err = bitcask_search_text_batch(cask, queries, 3, 10, &results, &fault);
    assert(err == BITCASK_OK);
    assert(results != NULL);
    assert(results[0] != NULL && results[0]->count == 2);
    assert(results[1] != NULL && results[1]->count == 1);
    assert(results[2] != NULL && results[2]->count == 0);
    bitcask_search_result_batch_free(results, 3);

    // n==0 边界：*out_results 置 NULL、返回 OK。
    results = NULL;
    err = bitcask_search_text_batch(cask, queries, 0, 10, &results, &fault);
    assert(err == BITCASK_OK);
    assert(results == NULL);

    // 参数校验：out_results 为空 → INVALID_OPTION。
    err = bitcask_search_text_batch(cask, queries, 3, 10, NULL, &fault);
    assert(err == BITCASK_ERR_INVALID_OPTION);

    bitcask_close(cask);
    printf("PASS test_search_text_batch\n");
    return 0;
}

/* S32：C API 向量引擎选择——IVFRQ 建库/写查/重开一致性/引擎不符拒开。 */
static int test_vector_engine_ivfrq(void) {
    const char* dir = TDIR("veng");
    rmrf(dir);

    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;
    opts.vector_dim = 4;
    opts.vector_metric = BITCASK_VECTOR_METRIC_COSINE;
    opts.vector_engine = BITCASK_VECTOR_ENGINE_IVFRQ;  /* S32 */

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(dir, &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_vector_engine_ivfrq: open: %s\n",
                fault.detail);
        return 1;
    }
    float vecs[3][4] = {{1, 0.1f, 0, 0}, {0, 1, 0.1f, 0}, {0, 0.1f, 1, 0}};
    const char* keys[3] = {"e0", "e1", "e2"};
    const char* texts[3] = {"one", "two", "three"};
    for (int i = 0; i < 3; i++) {
        bitcask_doc_input_t doc;
        memset(&doc, 0, sizeof(doc));
        doc.text.data = texts[i];
        doc.text.size = strlen(texts[i]);
        doc.vector = vecs[i];
        doc.vector_len = 4;
        bitcask_slice_t key = {keys[i], strlen(keys[i])};
        err = bitcask_put_doc(cask, key, &doc, 0, &fault);
        assert(err == BITCASK_OK);
    }
    bitcask_flush_index(cask);
    {
        bitcask_search_result_t* res = NULL;
        err = bitcask_search_vector(cask, vecs[1], 4, 1, 0, &res, &fault);
        assert(err == BITCASK_OK && res && res->count >= 1);
        assert(strcmp(res->hits[0].key, "e1") == 0);
        bitcask_search_result_free(res);
    }
    bitcask_close(cask);

    /* 引擎不符（默认 HNSW）重开 → MODE_MISMATCH（meta 持久化生效）。 */
    {
        bitcask_options_t bad = opts;
        bad.vector_engine = BITCASK_VECTOR_ENGINE_HNSW;
        bitcask_t* c2 = NULL;
        err = bitcask_open(dir, &bad, &c2, &fault);
        if (err != BITCASK_ERR_MODE_MISMATCH) {
            fprintf(stderr,
                    "FAIL test_vector_engine_ivfrq: mismatch err=%d\n", err);
            if (c2) bitcask_close(c2);
            return 1;
        }
    }
    /* 一致引擎重开 → 组件恢复 + 检索可用。 */
    {
        bitcask_t* c2 = NULL;
        err = bitcask_open(dir, &opts, &c2, &fault);
        assert(err == BITCASK_OK);
        bitcask_search_result_t* res = NULL;
        err = bitcask_search_vector(c2, vecs[2], 4, 1, 0, &res, &fault);
        assert(err == BITCASK_OK && res && res->count >= 1);
        assert(strcmp(res->hits[0].key, "e2") == 0);
        bitcask_search_result_free(res);
        bitcask_close(c2);
    }
    printf("PASS test_vector_engine_ivfrq\n");
    return 0;
}

static int test_search_vector_hybrid_batch(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;
    opts.vector_dim = 4;
    opts.vector_metric = BITCASK_VECTOR_METRIC_L2;  // L2 无需归一化

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("vhbatch"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_search_vector_hybrid_batch: open: %s\n", fault.detail);
        return 1;
    }

    float vecs[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const char* texts[3] = {"alpha", "beta", "gamma"};
    const char* keys[3] = {"v0", "v1", "v2"};
    for (int i = 0; i < 3; i++) {
        bitcask_doc_input_t doc;
        memset(&doc, 0, sizeof(doc));
        doc.text.data = texts[i];
        doc.text.size = strlen(texts[i]);
        doc.vector = vecs[i];
        doc.vector_len = 4;
        bitcask_slice_t key = {keys[i], strlen(keys[i])};
        err = bitcask_put_doc(cask, key, &doc, 0, &fault);
        assert(err == BITCASK_OK);
    }
    bitcask_flush_index(cask);

    // 向量批量：query 精确等于 v0 / v2 → L2 距离 0 → top-1 确定。
    const float q0[4] = {1, 0, 0, 0};
    const float q2[4] = {0, 0, 1, 0};
    const float* vqueries[2] = {q0, q2};
    bitcask_search_result_t** vres = NULL;
    err = bitcask_search_vector_batch(cask, vqueries, 2, 4, 3, 0, &vres, &fault);
    assert(err == BITCASK_OK && vres != NULL);
    assert(vres[0] != NULL && vres[0]->count >= 1);
    assert(strcmp(vres[0]->hits[0].key, "v0") == 0);
    assert(vres[1] != NULL && vres[1]->count >= 1);
    assert(strcmp(vres[1]->hits[0].key, "v2") == 0);
    bitcask_search_result_batch_free(vres, 2);

    // 混合批量：(text "alpha" + vec q0) / (text "gamma" + vec q2)——两路都指向同一文档。
    bitcask_hybrid_query_t hq[2];
    memset(hq, 0, sizeof(hq));
    hq[0].text = "alpha"; hq[0].vector = q0; hq[0].vector_len = 4;
    hq[1].text = "gamma"; hq[1].vector = q2; hq[1].vector_len = 4;
    bitcask_search_result_t** hres = NULL;
    err = bitcask_search_hybrid_batch(cask, hq, 2, 5, &hres, &fault);
    assert(err == BITCASK_OK && hres != NULL);
    assert(hres[0] != NULL && hres[0]->count >= 1);
    assert(hres[1] != NULL && hres[1]->count >= 1);
    bitcask_search_result_batch_free(hres, 2);

    // n==0 边界。
    vres = NULL;
    err = bitcask_search_vector_batch(cask, vqueries, 0, 4, 3, 0, &vres, &fault);
    assert(err == BITCASK_OK && vres == NULL);

    bitcask_close(cask);
    printf("PASS test_search_vector_hybrid_batch\n");
    return 0;
}

typedef struct {
    atomic_int      count;
    atomic_uint     checksum;  // 所有 value 字节和（校验每 value 恰读一次、内容正确）
} scan_ctx_t;

// parallel_scan 回调：多工作线程并发调用 → 用 atomic 累加。
static void scan_cb(void* ctx, bitcask_slice_t key, bitcask_slice_t value) {
    scan_ctx_t* c = (scan_ctx_t*)ctx;
    atomic_fetch_add(&c->count, 1);
    unsigned sum = 0;
    const unsigned char* p = (const unsigned char*)value.data;
    for (size_t i = 0; i < value.size; i++) sum += p[i];
    atomic_fetch_add(&c->checksum, sum);
    (void)key;
}

static int test_parallel_scan(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open(TDIR("pscan"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_parallel_scan: open: %s\n", fault.detail);
        return 1;
    }

    const int N = 500;
    unsigned expected_checksum = 0;
    for (int i = 0; i < N; i++) {
        char kbuf[16], vbuf[32];
        int klen = snprintf(kbuf, sizeof(kbuf), "pk_%d", i);
        int vlen = snprintf(vbuf, sizeof(vbuf), "pv_%d", i);
        bitcask_slice_t k = {kbuf, (size_t)klen};
        bitcask_slice_t v = {vbuf, (size_t)vlen};
        err = bitcask_put(cask, k, v, 0, &fault);
        assert(err == BITCASK_OK);
        for (int j = 0; j < vlen; j++) expected_checksum += (unsigned char)vbuf[j];
    }

    scan_ctx_t sc;
    atomic_init(&sc.count, 0);
    atomic_init(&sc.checksum, 0u);
    size_t visited = 0;
    err = bitcask_parallel_scan(cask, 4, scan_cb, &sc, &visited, &fault);
    assert(err == BITCASK_OK);
    assert(visited == (size_t)N);
    assert(atomic_load(&sc.count) == N);                 // 每 key 恰访问一次
    assert(atomic_load(&sc.checksum) == expected_checksum);  // value 内容正确

    // n_threads==0 → hardware_concurrency，仍遍历全部。
    atomic_init(&sc.count, 0);
    visited = 0;
    err = bitcask_parallel_scan(cask, 0, scan_cb, &sc, &visited, &fault);
    assert(err == BITCASK_OK && visited == (size_t)N && atomic_load(&sc.count) == N);

    // 参数校验：fn 为 NULL → INVALID_OPTION。
    err = bitcask_parallel_scan(cask, 4, NULL, &sc, &visited, &fault);
    assert(err == BITCASK_ERR_INVALID_OPTION);

    // S33-6：前缀版——"pk_1" 前缀命中 pk_1 / pk_1x / pk_1xx
    // （1, 10-19, 100-199 = 111 条）。
    bitcask_slice_t prefix = {"pk_1", 4};
    atomic_init(&sc.count, 0);
    visited = 0;
    err = bitcask_parallel_scan_prefix(cask, 4, prefix, scan_cb, &sc, &visited,
                                       &fault);
    assert(err == BITCASK_OK);
    assert(visited == 111 && atomic_load(&sc.count) == 111);

    // 空前缀 = 全表（等价 bitcask_parallel_scan）。
    bitcask_slice_t empty_prefix = {NULL, 0};
    atomic_init(&sc.count, 0);
    visited = 0;
    err = bitcask_parallel_scan_prefix(cask, 4, empty_prefix, scan_cb, &sc,
                                       &visited, &fault);
    assert(err == BITCASK_OK && visited == (size_t)N &&
           atomic_load(&sc.count) == N);

    bitcask_close(cask);
    printf("PASS test_parallel_scan\n");
    return 0;
}

// S13-D1：批量写。基础语义 + n==0 边界 + 参数校验。
static int test_put_batch(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err =
        bitcask_open(TDIR("putbatch"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_put_batch: open failed: %s\n", fault.detail);
        return 1;
    }

    bitcask_kv_pair_t items[3] = {
        {{"bk0", 3}, {"v0", 2}},
        {{"bk1", 3}, {"v1", 2}},
        {{"bk2", 3}, {"v2", 2}},
    };
    err = bitcask_put_batch(cask, items, 3, 0, &fault);
    assert(err == BITCASK_OK);

    for (int i = 0; i < 3; i++) {
        bitcask_get_result_t* r = NULL;
        err = bitcask_get(cask, items[i].key, &r, &fault);
        assert(err == BITCASK_OK);
        assert(r->value.size == 2);
        assert(memcmp(r->value.data, items[i].value.data, 2) == 0);
        bitcask_get_result_free(r);
    }

    // n==0 → OK；items NULL + n>0 → INVALID_OPTION。
    assert(bitcask_put_batch(cask, NULL, 0, 0, &fault) == BITCASK_OK);
    assert(bitcask_put_batch(cask, NULL, 2, 0, &fault) ==
           BITCASK_ERR_INVALID_OPTION);

    bitcask_close(cask);
    printf("PASS test_put_batch\n");
    return 0;
}

// S34：多键事务。提交语义 + 校验拒绝 + recover/pending 冒烟。
static int test_txn(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err =
        bitcask_open(TDIR("txn"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_txn: open failed: %s\n", fault.detail);
        return 1;
    }

    // 启动恢复：干净目录 0 条。
    size_t replayed = 99;
    assert(bitcask_txn_recover(cask, &replayed, &fault) == BITCASK_OK);
    assert(replayed == 0);

    // 预置一个 key 供事务内 remove。
    bitcask_slice_t vk = {"txn_victim", 10};
    assert(bitcask_put(cask, vk, (bitcask_slice_t){"old", 3}, 0, &fault) ==
           BITCASK_OK);

    bitcask_txn_op_t ops[3] = {
        {0, {"txn_a", 5}, {"va", 2}},
        {0, {"txn_b", 5}, {"vb", 2}},
        {1, {"txn_victim", 10}, {NULL, 0}},
    };
    assert(bitcask_txn_commit(cask, ops, 3, /*sync_on_commit=*/1, &fault) ==
           BITCASK_OK);

    bitcask_get_result_t* r = NULL;
    assert(bitcask_get(cask, ops[0].key, &r, &fault) == BITCASK_OK);
    assert(r->value.size == 2 && memcmp(r->value.data, "va", 2) == 0);
    bitcask_get_result_free(r);
    r = NULL;
    assert(bitcask_get(cask, vk, &r, &fault) == BITCASK_ERR_NOT_FOUND);

    // 提交完成 → 无 pending。
    size_t n_pending = 99;
    assert(bitcask_txn_pending_count(cask, &n_pending, &fault) == BITCASK_OK);
    assert(n_pending == 0);

    // 校验拒绝：空批 / 重复 key / "_txn:" 前缀。
    assert(bitcask_txn_commit(cask, NULL, 0, 1, &fault) ==
           BITCASK_ERR_INVALID_OPTION);
    bitcask_txn_op_t dup[2] = {
        {0, {"same", 4}, {"1", 1}},
        {1, {"same", 4}, {NULL, 0}},
    };
    assert(bitcask_txn_commit(cask, dup, 2, 1, &fault) ==
           BITCASK_ERR_INVALID_OPTION);
    bitcask_txn_op_t reserved[1] = {{0, {"_txn:x", 6}, {"v", 1}}};
    assert(bitcask_txn_commit(cask, reserved, 1, 1, &fault) ==
           BITCASK_ERR_INVALID_OPTION);

    // S35：引擎原子批直通（同 key 批内 LWW 合法）。
    bitcask_txn_op_t raw[2] = {
        {0, {"raw", 3}, {"first", 5}},
        {0, {"raw", 3}, {"second", 6}},
    };
    assert(bitcask_put_batch_atomic(cask, raw, 2, &fault) == BITCASK_OK);
    r = NULL;
    assert(bitcask_get(cask, raw[0].key, &r, &fault) == BITCASK_OK);
    assert(r->value.size == 6 && memcmp(r->value.data, "second", 6) == 0);
    bitcask_get_result_free(r);
    assert(bitcask_put_batch_atomic(cask, NULL, 0, &fault) == BITCASK_OK);

    bitcask_close(cask);
    printf("PASS test_txn\n");
    return 0;
}

// S13-D7/D8/D11：options 新字段默认值 + status_ex + log 回调冒烟。
static void test_log_cb(int level, const char* msg, void* ctx) {
    (void)level; (void)msg;
    atomic_fetch_add((atomic_int*)ctx, 1);
}
static int test_status_ex_and_log(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    assert(opts.log_fn == NULL && opts.log_ctx == NULL);
    assert(opts.hnsw_m == 0 && opts.hnsw_ef_construction == 0);

    static atomic_int log_calls;
    atomic_store(&log_calls, 0);
    opts.read_write = 1;
    opts.log_fn = test_log_cb;   // 冒烟：挂上回调不影响正常路径
    opts.log_ctx = &log_calls;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err =
        bitcask_open(TDIR("statusex"), &opts, &cask, &fault);
    assert(err == BITCASK_OK);
    bitcask_slice_t k = {"sk", 2}, v = {"sv", 2};
    assert(bitcask_put(cask, k, v, 0, &fault) == BITCASK_OK);

    bitcask_status_ex_t st;
    err = bitcask_status_ex(cask, &st, &fault);
    assert(err == BITCASK_OK);
    assert(st.key_count == 1);
    assert(st.hnsw_nodes == 0);           // 无向量索引
    assert(st.search_cache_entries == 0); // 无搜索
    assert(bitcask_status_ex(cask, NULL, &fault) == BITCASK_ERR_INVALID_OPTION);

    bitcask_close(cask);
    printf("PASS test_status_ex_and_log\n");
    return 0;
}

// S13-D2：meta 过滤检索变体。文档不带 meta（C 侧暂无 meta 编码 API），
// 引擎语义：**filter 非空时无 meta 的文档一律不通过**（materialize_hits 的
// 「空 blob 不通过」约定）——故任何 filter 下命中皆 0；NULL filter 退化为
// 无过滤；非法 filter（NULL key）→ INVALID_OPTION；嵌套子树正常转换。
static int test_search_filtered(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.enable_search = 1;
    opts.analyzer_type = BITCASK_ANALYZER_WHITESPACE;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err =
        bitcask_open(TDIR("filter"), &opts, &cask, &fault);
    if (err != BITCASK_OK) {
        fprintf(stderr, "FAIL test_search_filtered: open failed: %s\n", fault.detail);
        return 1;
    }

    const char* docs[3] = {"hello world", "hello there", "foo bar"};
    const char* keys[3] = {"f0", "f1", "f2"};
    for (int i = 0; i < 3; i++) {
        bitcask_doc_input_t doc;
        memset(&doc, 0, sizeof(doc));
        doc.text.data = docs[i];
        doc.text.size = strlen(docs[i]);
        bitcask_slice_t key = {keys[i], strlen(keys[i])};
        err = bitcask_put_doc(cask, key, &doc, 0, &fault);
        assert(err == BITCASK_OK);
    }
    bitcask_flush_index(cask);

    // Exists("tag")：无 doc 带 meta → 0 命中。
    bitcask_meta_condition_t cond_exists;
    memset(&cond_exists, 0, sizeof(cond_exists));
    cond_exists.key = "tag";
    cond_exists.op = BITCASK_META_OP_EXISTS;
    bitcask_meta_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.conditions = &cond_exists;
    filter.conditions_count = 1;

    bitcask_search_result_t* r = NULL;
    err = bitcask_search_text_filtered(cask, "hello", 10, &filter, &r, &fault);
    assert(err == BITCASK_OK);
    assert(r != NULL && r->count == 0);
    bitcask_search_result_free(r);

    // Neq("tag", int64 1)：无 meta 文档同样被「空 blob 不通过」滤掉 → 0。
    bitcask_meta_condition_t cond_neq;
    memset(&cond_neq, 0, sizeof(cond_neq));
    cond_neq.key = "tag";
    cond_neq.op = BITCASK_META_OP_NEQ;
    cond_neq.value.type = BITCASK_META_VALUE_INT64;
    cond_neq.value.i64 = 1;
    filter.conditions = &cond_neq;

    r = NULL;
    err = bitcask_search_text_filtered(cask, "hello", 10, &filter, &r, &fault);
    assert(err == BITCASK_OK);
    assert(r != NULL && r->count == 0);
    bitcask_search_result_free(r);

    // 嵌套：root{AND, children=[ OR{ Exists(tag), Neq(tag,1) } ]} → 转换正常，
    // 无 meta 文档仍 0 命中。
    bitcask_meta_condition_t or_conds[2];
    memset(or_conds, 0, sizeof(or_conds));
    or_conds[0] = cond_exists;
    or_conds[1] = cond_neq;
    bitcask_meta_filter_t child;
    memset(&child, 0, sizeof(child));
    child.logic_or = 1;
    child.conditions = or_conds;
    child.conditions_count = 2;
    bitcask_meta_filter_t root;
    memset(&root, 0, sizeof(root));
    root.children = &child;
    root.children_count = 1;

    r = NULL;
    err = bitcask_search_text_filtered(cask, "hello", 10, &root, &r, &fault);
    assert(err == BITCASK_OK);
    assert(r != NULL && r->count == 0);
    bitcask_search_result_free(r);

    // NULL filter 退化为无过滤。
    r = NULL;
    err = bitcask_search_text_filtered(cask, "hello", 10, NULL, &r, &fault);
    assert(err == BITCASK_OK);
    assert(r != NULL && r->count == 2);
    bitcask_search_result_free(r);

    // 非法：condition.key == NULL → INVALID_OPTION。
    bitcask_meta_condition_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.op = BITCASK_META_OP_EQ;
    bitcask_meta_filter_t bad_filter;
    memset(&bad_filter, 0, sizeof(bad_filter));
    bad_filter.conditions = &bad;
    bad_filter.conditions_count = 1;
    r = NULL;
    err = bitcask_search_text_filtered(cask, "hello", 10, &bad_filter, &r, &fault);
    assert(err == BITCASK_ERR_INVALID_OPTION);
    assert(r == NULL);

    bitcask_close(cask);
    printf("PASS test_search_filtered\n");
    return 0;
}

// 注：C API 的 bitcask_close **销毁句柄**（adopt+delete），故纯 C 无「已关闭但存活」
// 状态——close 后再用是 use-after-free（caller bug），非 BITCASK_ERR_CLOSED 场景。
// kClosed 的实际受益方是 C++ 消费方（Cask::close 保留对象 + fail-fast），其覆盖见
// C++ 测试 CrashRecoveryTest.OperationsAfterCloseReturnErrorNotUb。


/* S36：Level B（keydir 磁盘驻留）冒烟——预算开启下写入远超预算、全部可
   读（冷路径）、重开（v4 子集快照 + BCOM v3 戳）数据无损。 */
static int test_levelb(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;
    opts.keydir_cache_entries = 512; /* 2000 键 ≫ 512 预算 → 深度逐出 */

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    if (bitcask_open(TDIR("levelb"), &opts, &cask, &fault) !=
        BITCASK_OK) {
        fprintf(stderr, "FAIL test_levelb: open failed: %s\n", fault.detail);
        return 1;
    }
    char kb[32], vb[32];
    for (int i = 0; i < 2000; i++) {
        int kl = snprintf(kb, sizeof kb, "lb%d", i);
        int vl = snprintf(vb, sizeof vb, "val-%d", i);
        bitcask_slice_t k = {kb, (size_t)kl}, v = {vb, (size_t)vl};
        assert(bitcask_put(cask, k, v, 0, &fault) == BITCASK_OK);
    }
    for (int i = 0; i < 2000; i += 111) { /* 抽查（多数走冷路径） */
        int kl = snprintf(kb, sizeof kb, "lb%d", i);
        int vl = snprintf(vb, sizeof vb, "val-%d", i);
        bitcask_slice_t k = {kb, (size_t)kl};
        bitcask_get_result_t* res = NULL;
        assert(bitcask_get(cask, k, &res, &fault) == BITCASK_OK);
        assert(res->value.size == (size_t)vl &&
               memcmp(res->value.data, vb, (size_t)vl) == 0);
        bitcask_get_result_free(res);
    }
    bitcask_close(cask);

    /* 重开：子集快照 + 模式戳快路径；数据无损。 */
    if (bitcask_open(TDIR("levelb"), &opts, &cask, &fault) !=
        BITCASK_OK) {
        fprintf(stderr, "FAIL test_levelb: reopen failed: %s\n", fault.detail);
        return 1;
    }
    for (int i = 1; i < 2000; i += 97) {
        int kl = snprintf(kb, sizeof kb, "lb%d", i);
        int vl = snprintf(vb, sizeof vb, "val-%d", i);
        bitcask_slice_t k = {kb, (size_t)kl};
        bitcask_get_result_t* res = NULL;
        assert(bitcask_get(cask, k, &res, &fault) == BITCASK_OK);
        assert(res->value.size == (size_t)vl &&
               memcmp(res->value.data, vb, (size_t)vl) == 0);
        bitcask_get_result_free(res);
    }
    bitcask_close(cask);
    printf("PASS test_levelb\n");
    return 0;
}

int main(void) {
    // 各用例使用固定 /tmp 路径且原先不清理——跨运行/跨二进制版本累积的
    // checkpoint 残留会污染 reopen（尤以向量批量用例敏感，陈旧 vec.ckpt →
    // 搜索命中 0）。运行前统一清空，保证 hermetic。
    cleanup_all_test_dirs();

    int failures = 0;
    failures += test_version();
    failures += test_kv_basic();
    failures += test_status_and_merge();
    failures += test_iteration();
    failures += test_range_iter();
    failures += test_search_text_batch();
    failures += test_search_vector_hybrid_batch();
    failures += test_vector_engine_ivfrq();
    failures += test_parallel_scan();
    failures += test_put_batch();
    failures += test_txn();
    failures += test_levelb();
    failures += test_status_ex_and_log();
    failures += test_search_filtered();

    if (failures == 0) {
        printf("\n=== All C API tests passed ===\n");
    } else {
        printf("\n=== %d test(s) FAILED ===\n", failures);
    }
    return failures;
}
