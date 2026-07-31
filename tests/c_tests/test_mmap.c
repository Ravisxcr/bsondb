/*
 * test_mmap.c -- compile-smoke test for platform_mmap.c (plain
 * assert(), no CMocka/Unity wiring -- consistent with this project's
 * existing C test convention). Not part of `pip install -e .`; built
 * only via CMakeLists.txt's BUILD_TESTING option.
 */
#include "custom_bson/platform_mmap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TEST_PATH = "/tmp/custom_bson_test_mmap.bin";

int main(void) {
    unlink(TEST_PATH);

    bson_error_t err;

    /* Opening a nonexistent file in READ_ONLY mode must fail cleanly. */
    bson_mmap_file_t *ro = NULL;
    bson_status_t st = bson_mmap_open(TEST_PATH, BSON_MMAP_READ_ONLY, 0, &ro, &err);
    assert(st != BSON_OK);
    assert(ro == NULL);

    /* READ_WRITE creates the file and grows it to initial_size. */
    bson_mmap_file_t *file = NULL;
    st = bson_mmap_open(TEST_PATH, BSON_MMAP_READ_WRITE, 4096, &file, &err);
    assert(st == BSON_OK);
    assert(file != NULL);
    assert(bson_mmap_size(file) == 4096);
    assert(bson_mmap_data(file) != NULL);

    /* Write through the mapping, flush, and read it back via a fresh
     * read-only mapping to prove the bytes actually reached disk. */
    memcpy((void *)bson_mmap_data(file), "hello, mmap", 11);
    st = bson_mmap_flush(file, &err);
    assert(st == BSON_OK);

    bson_mmap_file_t *reader = NULL;
    st = bson_mmap_open(TEST_PATH, BSON_MMAP_READ_ONLY, 0, &reader, &err);
    assert(st == BSON_OK);
    assert(memcmp(bson_mmap_data(reader), "hello, mmap", 11) == 0);

    st = bson_mmap_resize(reader, 16384, &err);
    assert(st == BSON_ERR_READ_ONLY_VIOLATION);
    st = bson_mmap_flush(reader, &err);
    assert(st == BSON_ERR_READ_ONLY_VIOLATION);

    bson_mmap_close(reader, &err);

    /* Resize (grow) the writable mapping; old data survives, new
     * region is available. */
    st = bson_mmap_resize(file, 8192, &err);
    assert(st == BSON_OK);
    assert(bson_mmap_size(file) == 8192);
    assert(memcmp(bson_mmap_data(file), "hello, mmap", 11) == 0);

    /* close() is safe on NULL, and idempotent-safe to call once. */
    bson_mmap_close(NULL, &err);
    bson_mmap_close(file, &err);

    unlink(TEST_PATH);
    printf("test_mmap: all assertions passed\n");
    return 0;
}
