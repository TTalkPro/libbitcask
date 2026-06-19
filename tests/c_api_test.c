#include "bitcask_c.h"
#include <assert.h>
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

int main(void) {
    int failures = 0;
    failures += test_version();
    failures += test_kv_basic();
    failures += test_status_and_merge();
    failures += test_iteration();

    if (failures == 0) {
        printf("\n=== All C API tests passed ===\n");
    } else {
        printf("\n=== %d test(s) FAILED ===\n", failures);
    }
    return failures;
}
