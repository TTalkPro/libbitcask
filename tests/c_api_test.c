#include "bitcask_c.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_kv_basic(void) {
    bitcask_options_t opts;
    bitcask_options_init(&opts);
    opts.read_write = 1;

    bitcask_t* cask = NULL;
    bitcask_fault_t fault;
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_kv", &opts, &cask, &fault);
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
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_status", &opts, &cask, &fault);
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
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_iter", &opts, &cask, &fault);
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
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_batch", &opts, &cask, &fault);
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
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_vhbatch", &opts, &cask, &fault);
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
    bitcask_error_t err = bitcask_open("/tmp/bitcask_c_test_pscan", &opts, &cask, &fault);
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

    bitcask_close(cask);
    printf("PASS test_parallel_scan\n");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_version();
    failures += test_kv_basic();
    failures += test_status_and_merge();
    failures += test_iteration();
    failures += test_search_text_batch();
    failures += test_search_vector_hybrid_batch();
    failures += test_parallel_scan();

    if (failures == 0) {
        printf("\n=== All C API tests passed ===\n");
    } else {
        printf("\n=== %d test(s) FAILED ===\n", failures);
    }
    return failures;
}
