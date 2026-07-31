/*
 * test_btree.c -- compile-smoke test for btree.c (plain assert(), no
 * CMocka/Unity). Not part of `pip install -e .`; built only via
 * CMakeLists.txt's BUILD_TESTING option. Complements the much larger
 * ad hoc stress test used during development (50k shuffled inserts) by
 * asserting the specific behaviors that matter most: split-forcing
 * insert volume, lookup/range correctness, delete + re-lookup, and
 * unique-index duplicate rejection.
 */
#include "custom_bson/btree.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TEST_PATH = "/tmp/custom_bson_test_btree.bidx";
static const char *UNIQUE_PATH = "/tmp/custom_bson_test_btree_unique.bidx";

static void key_int(uint8_t out[BSON_BTREE_KEY_SIZE], int64_t v) {
    memset(out, 0, BSON_BTREE_KEY_SIZE);
    out[0] = BSON_BTREE_TAG_INT;
    memcpy(out + 1, &v, 8); /* test runs on a little-endian host */
}

int main(void) {
    unlink(TEST_PATH);
    unlink(UNIQUE_PATH);
    bson_error_t err;

    bson_btree_t *tree = NULL;
    bson_status_t st = bson_btree_create(TEST_PATH, "age", BSON_BTREE_TAG_INT, false, false, &tree, &err);
    assert(st == BSON_OK);
    assert(strcmp(bson_btree_field_path(tree), "age") == 0);
    assert(bson_btree_unique(tree) == false);
    assert(bson_btree_key_type_tag(tree) == BSON_BTREE_TAG_INT);

    /* Enough inserts to force multiple leaf splits and at least one
     * internal split (LEAF_MAX_ENTRIES=180, so a few thousand keys
     * guarantees several levels). */
    enum { N = 5000 };
    for (int i = 0; i < N; i++) {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, i);
        st = bson_btree_insert(tree, k, (uint64_t)(10000 + i), &err);
        assert(st == BSON_OK);
    }

    for (int i = 0; i < N; i++) {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, i);
        bson_btree_offset_list_t out;
        bson_btree_offset_list_init(&out);
        st = bson_btree_lookup(tree, k, &out, &err);
        assert(st == BSON_OK);
        assert(out.len == 1);
        assert(out.data[0] == (uint64_t)(10000 + i));
        bson_btree_offset_list_free(&out);
    }

    /* Range: [10, 20] inclusive both ends -> 11 results in order. */
    {
        uint8_t low[BSON_BTREE_KEY_SIZE], high[BSON_BTREE_KEY_SIZE];
        key_int(low, 10);
        key_int(high, 20);
        bson_btree_offset_list_t out;
        bson_btree_offset_list_init(&out);
        st = bson_btree_range(tree, low, true, high, true, &out, &err);
        assert(st == BSON_OK);
        assert(out.len == 11);
        for (size_t i = 0; i < out.len; i++) {
            assert(out.data[i] == (uint64_t)(10000 + 10 + (int)i));
        }
        bson_btree_offset_list_free(&out);
    }

    /* Delete every even key; odd keys must remain findable. */
    for (int i = 0; i < N; i += 2) {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, i);
        st = bson_btree_delete(tree, k, (uint64_t)(10000 + i), &err);
        assert(st == BSON_OK);
    }
    for (int i = 0; i < N; i++) {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, i);
        bson_btree_offset_list_t out;
        bson_btree_offset_list_init(&out);
        st = bson_btree_lookup(tree, k, &out, &err);
        assert(st == BSON_OK);
        assert(out.len == (size_t)(i % 2 == 0 ? 0 : 1));
        bson_btree_offset_list_free(&out);
    }

    /* Idempotent delete. */
    {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, 0);
        st = bson_btree_delete(tree, k, 10000, &err);
        assert(st == BSON_OK);
    }

    bson_btree_close(tree, &err);

    /* Reopen: persistence. */
    tree = NULL;
    st = bson_btree_open(TEST_PATH, &tree, &err);
    assert(st == BSON_OK);
    {
        uint8_t k[BSON_BTREE_KEY_SIZE];
        key_int(k, 1);
        bson_btree_offset_list_t out;
        bson_btree_offset_list_init(&out);
        st = bson_btree_lookup(tree, k, &out, &err);
        assert(st == BSON_OK);
        assert(out.len == 1);
        bson_btree_offset_list_free(&out);
    }
    bson_btree_close(tree, &err);
    unlink(TEST_PATH);

    /* Unique index: duplicate rejection, and re-insert after delete. */
    bson_btree_t *utree = NULL;
    st = bson_btree_create(UNIQUE_PATH, "email", BSON_BTREE_TAG_INT, true, false, &utree, &err);
    assert(st == BSON_OK);
    uint8_t k1[BSON_BTREE_KEY_SIZE];
    key_int(k1, 42);
    st = bson_btree_insert(utree, k1, 1, &err);
    assert(st == BSON_OK);
    st = bson_btree_insert(utree, k1, 2, &err);
    assert(st == BSON_ERR_DUPLICATE_KEY);
    st = bson_btree_delete(utree, k1, 1, &err);
    assert(st == BSON_OK);
    st = bson_btree_insert(utree, k1, 3, &err);
    assert(st == BSON_OK);
    bson_btree_close(utree, &err);
    unlink(UNIQUE_PATH);

    printf("test_btree: all assertions passed\n");
    return 0;
}
