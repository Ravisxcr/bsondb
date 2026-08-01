/*
 * test_storage.c -- compile-smoke test for storage.c/scanner.c (plain
 * assert(), no CMocka/Unity). Not part of `pip install -e .`; built
 * only via CMakeLists.txt's BUILD_TESTING option.
 */
#include "custom_bson/storage.h"
#include "custom_bson/bson_engine.h"
#include "test_common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *make_doc(const char *key, int32_t val, size_t *out_len) {
    bson_writer_t w;
    bson_writer_init(&w, 64);
    size_t patch;
    bson_writer_begin_document(&w, &patch);
    bson_writer_append_element_header(&w, BSON_TYPE_INT32, key, strlen(key));
    bson_writer_append_int32(&w, val);
    bson_writer_end_document(&w, patch);
    return bson_writer_release(&w, out_len);
}

static size_t count_live(bson_mmap_file_t *file) {
    bson_error_t err;
    bson_scanner_t sc;
    bson_scanner_open(&sc, bson_mmap_data(file), bson_storage_data_end(file), &err);
    size_t n = 0;
    size_t roff, doff, dlen;
    while (bson_scanner_next(&sc, &roff, &doff, &dlen, &err)) n++;
    assert(err.code == BSON_OK);
    return n;
}

int main(void) {
    char TEST_PATH[512];
    test_tmp_path(TEST_PATH, sizeof(TEST_PATH), "custom_bson_test_storage.bin");
    remove(TEST_PATH);
    bson_error_t err;

    bson_mmap_file_t *file = NULL;
    bson_status_t st = bson_mmap_open(TEST_PATH, BSON_MMAP_READ_WRITE, 0, &file, &err);
    assert(st == BSON_OK);

    st = bson_storage_open_header(file, BSON_MMAP_READ_WRITE, &err);
    assert(st == BSON_OK);
    assert(bson_storage_data_end(file) == BSON_STORAGE_HEADER_LEN);
    assert(bson_storage_live_count(file) == 0);

    /* Append N records, forcing at least one growth (small initial
     * capacity, plenty of records). */
    enum { N = 500 };
    size_t offs[N];
    for (int i = 0; i < N; i++) {
        size_t len;
        uint8_t *doc = make_doc("v", i, &len);
        st = bson_storage_append(file, doc, len, &offs[i], &err);
        assert(st == BSON_OK);
        free(doc);
    }
    assert(bson_storage_live_count(file) == N);
    assert(count_live(file) == N);

    /* Tombstone every third record. */
    size_t tombstoned = 0;
    for (int i = 0; i < N; i += 3) {
        st = bson_storage_tombstone(file, offs[i], &err);
        assert(st == BSON_OK);
        tombstoned++;
    }
    assert(bson_storage_live_count(file) == (uint64_t)(N - tombstoned));
    assert(count_live(file) == (size_t)(N - tombstoned));

    /* Idempotent re-tombstone. */
    st = bson_storage_tombstone(file, offs[0], &err);
    assert(st == BSON_OK);
    assert(bson_storage_live_count(file) == (uint64_t)(N - tombstoned));

    /* Tombstoning a bogus offset fails cleanly, not a crash. */
    st = bson_storage_tombstone(file, bson_storage_data_end(file) + 1000, &err);
    assert(st == BSON_ERR_RECORD_NOT_FOUND);

    /* Read back a still-live record directly by offset. */
    uint8_t status;
    size_t doc_off, doc_len;
    st = bson_storage_read(file, offs[1], &status, &doc_off, &doc_len, &err);
    assert(st == BSON_OK);
    assert(status == BSON_RECORD_LIVE);
    bson_iter_t it;
    bson_iter_init_document(&it, bson_mmap_data(file) + doc_off, doc_len, &err);
    assert(bson_iter_next(&it, &err));
    int32_t v;
    bson_iter_value_int32(&it, &v);
    assert(v == 1);

    bson_storage_mark_clean(file, &err);
    bson_mmap_close(file, &err);

    printf("test_storage: all assertions passed\n");
    remove(TEST_PATH);
    return 0;
}
