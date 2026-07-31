/*
 * btree.c -- persisted on-disk B+Tree index implementation (see
 * btree.h for the file/page format). Pure C11, no Python.h dependency.
 *
 * _POSIX_C_SOURCE must be defined before any system header is included
 * so glibc exposes strdup under -std=c11.
 */
#define _POSIX_C_SOURCE 200809L

#include "custom_bson/btree.h"

#include <stdlib.h>
#include <string.h>

struct bson_btree {
    bson_mmap_file_t *file;
    char *field_path; /* owned, NUL-terminated cache of the header field */
};

/* ---- little-endian header/page field helpers (same pattern as storage.c) */

static uint16_t read_u16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

static void write_u16(uint8_t *p, uint16_t v) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    memcpy(p, &v, 2);
}

static void write_u32(uint8_t *p, uint32_t v) {
    uint32_t le = BSON_HOST_TO_LE32(v);
    memcpy(p, &le, 4);
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return bson_le64_to_host(v);
}

static void write_u64(uint8_t *p, uint64_t v) {
    uint64_t le = BSON_HOST_TO_LE64(v);
    memcpy(p, &le, 8);
}

static uint8_t *mutable_base(bson_mmap_file_t *file) {
    return (uint8_t *)(uintptr_t)bson_mmap_data(file);
}

/* ---- header field offsets ---- */
#define OFF_MAGIC 0
#define OFF_VERSION 4
#define OFF_FLAGS 6
#define OFF_HEADER_LEN 8
#define OFF_PAGE_SIZE 12
#define OFF_ROOT_PAGE_NO 16
#define OFF_PAGE_COUNT 24
#define OFF_FREE_PAGE_HEAD 32
#define OFF_KEY_TYPE_TAG 40
#define OFF_FIELD_PATH_LEN 41
#define OFF_FIELD_PATH 56

/* ---- page layout ---- */
#define PAGE_OFF_TYPE 0
#define PAGE_OFF_COUNT 2
#define PAGE_OFF_LINK 8
#define PAGE_HEADER_LEN 16

#define PAGE_TYPE_LEAF 1
#define PAGE_TYPE_INTERNAL 2

#define LEAF_ENTRY_LEN (BSON_BTREE_KEY_SIZE + 8 + 1) /* key + offset + flags = 22 */
#define LEAF_MAX_ENTRIES 180
#define LEAF_ENTRY_FLAG_DEAD 0x01u

#define INTERNAL_ENTRY_LEN (BSON_BTREE_KEY_SIZE + 8) /* key + child = 21 */
#define INTERNAL_MAX_KEYS 190

static uint8_t *page_ptr(bson_btree_t *tree, uint64_t page_no) {
    return mutable_base(tree->file) + page_no * BSON_BTREE_PAGE_SIZE;
}

static uint8_t *leaf_entry_ptr(uint8_t *page, uint16_t i) {
    return page + PAGE_HEADER_LEN + (size_t)i * LEAF_ENTRY_LEN;
}

static uint8_t *internal_entry_ptr(uint8_t *page, uint16_t i) {
    return page + PAGE_HEADER_LEN + (size_t)i * INTERNAL_ENTRY_LEN;
}

static void write_leaf_entry(uint8_t *slot, const uint8_t key[BSON_BTREE_KEY_SIZE], uint64_t offset,
                              uint8_t flags) {
    memcpy(slot, key, BSON_BTREE_KEY_SIZE);
    write_u64(slot + BSON_BTREE_KEY_SIZE, offset);
    slot[BSON_BTREE_KEY_SIZE + 8] = flags;
}

static uint64_t read_leaf_offset(const uint8_t *slot) {
    return read_u64(slot + BSON_BTREE_KEY_SIZE);
}

static uint8_t read_leaf_flags(const uint8_t *slot) {
    return slot[BSON_BTREE_KEY_SIZE + 8];
}

/* ======================================================================
 * Key comparison
 * ==================================================================== */

int bson_btree_key_cmp(const uint8_t a[BSON_BTREE_KEY_SIZE], const uint8_t b[BSON_BTREE_KEY_SIZE]) {
    if (a[0] != b[0]) return (int)a[0] - (int)b[0];

    switch (a[0]) {
        case BSON_BTREE_TAG_NULL:
            return 0;
        case BSON_BTREE_TAG_BOOL:
            return (int)a[1] - (int)b[1];
        case BSON_BTREE_TAG_INT:
        case BSON_BTREE_TAG_DATETIME: {
            int64_t va = (int64_t)read_u64(a + 1);
            int64_t vb = (int64_t)read_u64(b + 1);
            return va < vb ? -1 : (va > vb ? 1 : 0);
        }
        case BSON_BTREE_TAG_DOUBLE: {
            uint64_t ra = read_u64(a + 1);
            uint64_t rb = read_u64(b + 1);
            double da, db;
            memcpy(&da, &ra, 8);
            memcpy(&db, &rb, 8);
            return da < db ? -1 : (da > db ? 1 : 0);
        }
        case BSON_BTREE_TAG_OBJECTID:
            return memcmp(a + 1, b + 1, 12);
        default:
            return memcmp(a, b, BSON_BTREE_KEY_SIZE);
    }
}

/* ======================================================================
 * Lifecycle
 * ==================================================================== */

static uint64_t current_root(bson_btree_t *tree) {
    const uint8_t *base = bson_mmap_data(tree->file);
    return read_u64(base + OFF_ROOT_PAGE_NO);
}

static bson_status_t ensure_capacity_for_page(bson_btree_t *tree, uint64_t page_no, bson_error_t *err) {
    size_t needed = (size_t)(page_no + 1) * BSON_BTREE_PAGE_SIZE;
    size_t cap = bson_mmap_size(tree->file);
    if (needed <= cap) return BSON_OK;
    size_t new_cap = cap == 0 ? BSON_BTREE_PAGE_SIZE : cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    return bson_mmap_resize(tree->file, new_cap, err);
}

static bson_status_t allocate_page(bson_btree_t *tree, uint64_t *out_page_no, bson_error_t *err) {
    uint8_t *base = mutable_base(tree->file);
    uint64_t page_count = read_u64(base + OFF_PAGE_COUNT);
    uint64_t new_page_no = page_count + 1;

    bson_status_t st = ensure_capacity_for_page(tree, new_page_no, err);
    if (st != BSON_OK) return st;

    base = mutable_base(tree->file); /* re-fetch: may have just resized */
    memset(page_ptr(tree, new_page_no), 0, BSON_BTREE_PAGE_SIZE);
    write_u64(base + OFF_PAGE_COUNT, page_count + 1);
    *out_page_no = new_page_no;
    return BSON_OK;
}

bson_status_t bson_btree_create(const char *path, const char *field_path, uint8_t key_type_tag,
                                 bool unique, bool descending, bson_btree_t **out_tree,
                                 bson_error_t *err) {
    if (out_tree) *out_tree = NULL;
    size_t field_len = strlen(field_path);
    if (field_len >= BSON_BTREE_FIELD_PATH_CAP) {
        bson_error_set(err, BSON_ERR_INDEX_UNSUPPORTED, "index field path too long (max %d bytes)",
                        BSON_BTREE_FIELD_PATH_CAP - 1);
        return BSON_ERR_INDEX_UNSUPPORTED;
    }

    bson_mmap_file_t *file = NULL;
    bson_status_t st = bson_mmap_open(path, BSON_MMAP_READ_WRITE, BSON_BTREE_PAGE_SIZE * 2, &file, err);
    if (st != BSON_OK) return st;

    bson_btree_t *tree = (bson_btree_t *)malloc(sizeof(bson_btree_t));
    if (!tree) {
        bson_mmap_close(file, err);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate B-Tree handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }
    tree->file = file;
    tree->field_path = strdup(field_path);

    uint8_t *base = mutable_base(file);
    memset(base, 0, bson_mmap_size(file));
    memcpy(base + OFF_MAGIC, BSON_BTREE_MAGIC, BSON_BTREE_MAGIC_LEN);
    write_u16(base + OFF_VERSION, BSON_BTREE_VERSION);
    uint16_t flags = 0;
    if (unique) flags |= BSON_BTREE_FLAG_UNIQUE;
    if (descending) flags |= BSON_BTREE_FLAG_DESCENDING;
    write_u16(base + OFF_FLAGS, flags);
    write_u32(base + OFF_HEADER_LEN, BSON_BTREE_HEADER_LEN);
    write_u32(base + OFF_PAGE_SIZE, BSON_BTREE_PAGE_SIZE);
    write_u64(base + OFF_ROOT_PAGE_NO, 0);
    write_u64(base + OFF_PAGE_COUNT, 0);
    write_u64(base + OFF_FREE_PAGE_HEAD, 0);
    base[OFF_KEY_TYPE_TAG] = key_type_tag;
    base[OFF_FIELD_PATH_LEN] = (uint8_t)field_len;
    memcpy(base + OFF_FIELD_PATH, field_path, field_len);

    uint64_t root;
    st = allocate_page(tree, &root, err);
    if (st != BSON_OK) {
        free(tree->field_path);
        bson_mmap_close(file, err);
        free(tree);
        return st;
    }
    uint8_t *root_page = page_ptr(tree, root);
    root_page[PAGE_OFF_TYPE] = PAGE_TYPE_LEAF;
    write_u16(root_page + PAGE_OFF_COUNT, 0);
    write_u64(root_page + PAGE_OFF_LINK, 0);

    base = mutable_base(file);
    write_u64(base + OFF_ROOT_PAGE_NO, root);

    bson_mmap_flush(file, err);
    if (out_tree) *out_tree = tree;
    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_btree_open(const char *path, bson_btree_t **out_tree, bson_error_t *err) {
    if (out_tree) *out_tree = NULL;
    bson_mmap_file_t *file = NULL;
    bson_status_t st = bson_mmap_open(path, BSON_MMAP_READ_WRITE, 0, &file, err);
    if (st != BSON_OK) return st;

    size_t size = bson_mmap_size(file);
    if (size < BSON_BTREE_HEADER_LEN) {
        bson_mmap_close(file, err);
        bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER, "index file is too small to contain a header");
        return BSON_ERR_INVALID_FILE_HEADER;
    }
    const uint8_t *base = bson_mmap_data(file);
    if (memcmp(base, BSON_BTREE_MAGIC, BSON_BTREE_MAGIC_LEN) != 0) {
        bson_mmap_close(file, err);
        bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER, "bad magic in index file");
        return BSON_ERR_INVALID_FILE_HEADER;
    }
    if (read_u16(base + OFF_VERSION) != BSON_BTREE_VERSION) {
        bson_mmap_close(file, err);
        bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER, "unsupported index file version");
        return BSON_ERR_INVALID_FILE_HEADER;
    }

    bson_btree_t *tree = (bson_btree_t *)malloc(sizeof(bson_btree_t));
    if (!tree) {
        bson_mmap_close(file, err);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate B-Tree handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }
    tree->file = file;
    uint8_t path_len = base[OFF_FIELD_PATH_LEN];
    tree->field_path = (char *)malloc((size_t)path_len + 1);
    if (!tree->field_path) {
        bson_mmap_close(file, err);
        free(tree);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate B-Tree handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }
    memcpy(tree->field_path, base + OFF_FIELD_PATH, path_len);
    tree->field_path[path_len] = '\0';

    if (out_tree) *out_tree = tree;
    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_btree_close(bson_btree_t *tree, bson_error_t *err) {
    if (!tree) {
        bson_error_clear(err);
        return BSON_OK;
    }
    bson_mmap_flush(tree->file, err);
    bson_mmap_close(tree->file, err);
    free(tree->field_path);
    free(tree);
    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_btree_flush(bson_btree_t *tree, bson_error_t *err) {
    return bson_mmap_flush(tree->file, err);
}

const char *bson_btree_field_path(const bson_btree_t *tree) {
    return tree->field_path;
}

bool bson_btree_unique(const bson_btree_t *tree) {
    const uint8_t *base = bson_mmap_data(tree->file);
    return (read_u16(base + OFF_FLAGS) & BSON_BTREE_FLAG_UNIQUE) != 0;
}

bool bson_btree_descending(const bson_btree_t *tree) {
    const uint8_t *base = bson_mmap_data(tree->file);
    return (read_u16(base + OFF_FLAGS) & BSON_BTREE_FLAG_DESCENDING) != 0;
}

uint8_t bson_btree_key_type_tag(const bson_btree_t *tree) {
    const uint8_t *base = bson_mmap_data(tree->file);
    return base[OFF_KEY_TYPE_TAG];
}

/* ======================================================================
 * Offset list (lookup/range output)
 * ==================================================================== */

void bson_btree_offset_list_init(bson_btree_offset_list_t *list) {
    list->data = NULL;
    list->len = 0;
    list->cap = 0;
}

void bson_btree_offset_list_free(bson_btree_offset_list_t *list) {
    free(list->data);
    list->data = NULL;
    list->len = 0;
    list->cap = 0;
}

static bson_status_t offset_list_push(bson_btree_offset_list_t *list, uint64_t v, bson_error_t *err) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 16 : list->cap * 2;
        uint64_t *nd = (uint64_t *)realloc(list->data, new_cap * sizeof(uint64_t));
        if (!nd) {
            bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "out of memory growing offset list");
            return BSON_ERR_OUT_OF_MEMORY;
        }
        list->data = nd;
        list->cap = new_cap;
    }
    list->data[list->len++] = v;
    return BSON_OK;
}

/* ======================================================================
 * Descent, lookup, range, delete
 * ==================================================================== */

static uint64_t descend_to_leaf(bson_btree_t *tree, uint64_t page_no, const uint8_t key[BSON_BTREE_KEY_SIZE]) {
    uint8_t *page = page_ptr(tree, page_no);
    while (page[PAGE_OFF_TYPE] == PAGE_TYPE_INTERNAL) {
        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        uint64_t child = read_u64(page + PAGE_OFF_LINK);
        for (uint16_t i = 0; i < count; i++) {
            uint8_t *entry = internal_entry_ptr(page, i);
            if (bson_btree_key_cmp(key, entry) < 0) break;
            child = read_u64(entry + BSON_BTREE_KEY_SIZE);
        }
        page_no = child;
        page = page_ptr(tree, page_no);
    }
    return page_no;
}

static uint64_t leftmost_leaf(bson_btree_t *tree, uint64_t page_no) {
    uint8_t *page = page_ptr(tree, page_no);
    while (page[PAGE_OFF_TYPE] == PAGE_TYPE_INTERNAL) {
        page_no = read_u64(page + PAGE_OFF_LINK);
        page = page_ptr(tree, page_no);
    }
    return page_no;
}

static bool key_has_live_entry(bson_btree_t *tree, uint64_t leaf_no, const uint8_t key[BSON_BTREE_KEY_SIZE]) {
    while (leaf_no != 0) {
        uint8_t *page = page_ptr(tree, leaf_no);
        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        for (uint16_t i = 0; i < count; i++) {
            uint8_t *e = leaf_entry_ptr(page, i);
            int c = bson_btree_key_cmp(e, key);
            if (c > 0) return false;
            if (c == 0 && (read_leaf_flags(e) & LEAF_ENTRY_FLAG_DEAD) == 0) return true;
        }
        leaf_no = read_u64(page + PAGE_OFF_LINK);
    }
    return false;
}

bson_status_t bson_btree_lookup(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 bson_btree_offset_list_t *out, bson_error_t *err) {
    bson_error_clear(err);
    uint64_t leaf_no = descend_to_leaf(tree, current_root(tree), key);
    while (leaf_no != 0) {
        uint8_t *page = page_ptr(tree, leaf_no);
        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        for (uint16_t i = 0; i < count; i++) {
            uint8_t *e = leaf_entry_ptr(page, i);
            int c = bson_btree_key_cmp(e, key);
            if (c > 0) return BSON_OK; /* sorted order: no more matches anywhere */
            if (c == 0 && (read_leaf_flags(e) & LEAF_ENTRY_FLAG_DEAD) == 0) {
                bson_status_t st = offset_list_push(out, read_leaf_offset(e), err);
                if (st != BSON_OK) return st;
            }
        }
        leaf_no = read_u64(page + PAGE_OFF_LINK);
    }
    return BSON_OK;
}

bson_status_t bson_btree_range(bson_btree_t *tree, const uint8_t *low, bool low_inclusive,
                                const uint8_t *high, bool high_inclusive, bson_btree_offset_list_t *out,
                                bson_error_t *err) {
    bson_error_clear(err);
    uint64_t leaf_no = low ? descend_to_leaf(tree, current_root(tree), low)
                           : leftmost_leaf(tree, current_root(tree));

    while (leaf_no != 0) {
        uint8_t *page = page_ptr(tree, leaf_no);
        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        for (uint16_t i = 0; i < count; i++) {
            uint8_t *e = leaf_entry_ptr(page, i);
            if (low) {
                int c = bson_btree_key_cmp(e, low);
                if (c < 0 || (c == 0 && !low_inclusive)) continue;
            }
            if (high) {
                int c = bson_btree_key_cmp(e, high);
                if (c > 0 || (c == 0 && !high_inclusive)) return BSON_OK;
            }
            if ((read_leaf_flags(e) & LEAF_ENTRY_FLAG_DEAD) == 0) {
                bson_status_t st = offset_list_push(out, read_leaf_offset(e), err);
                if (st != BSON_OK) return st;
            }
        }
        leaf_no = read_u64(page + PAGE_OFF_LINK);
    }
    return BSON_OK;
}

bson_status_t bson_btree_delete(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 uint64_t record_offset, bson_error_t *err) {
    bson_error_clear(err);
    uint64_t leaf_no = descend_to_leaf(tree, current_root(tree), key);
    while (leaf_no != 0) {
        uint8_t *page = page_ptr(tree, leaf_no);
        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        for (uint16_t i = 0; i < count; i++) {
            uint8_t *e = leaf_entry_ptr(page, i);
            int c = bson_btree_key_cmp(e, key);
            if (c > 0) return BSON_OK;
            if (c == 0 && read_leaf_offset(e) == record_offset) {
                e[BSON_BTREE_KEY_SIZE + 8] |= LEAF_ENTRY_FLAG_DEAD;
                return BSON_OK;
            }
        }
        leaf_no = read_u64(page + PAGE_OFF_LINK);
    }
    return BSON_OK; /* not found -- not an error */
}

/* ======================================================================
 * Insert, with recursive top-down split
 * ==================================================================== */

static bson_status_t insert_recursive(bson_btree_t *tree, uint64_t page_no,
                                       const uint8_t key[BSON_BTREE_KEY_SIZE], uint64_t offset,
                                       bool unique, uint8_t split_key_out[BSON_BTREE_KEY_SIZE],
                                       uint64_t *split_page_out, bool *did_split, bson_error_t *err) {
    uint8_t *page = page_ptr(tree, page_no);

    if (page[PAGE_OFF_TYPE] == PAGE_TYPE_LEAF) {
        if (unique && key_has_live_entry(tree, page_no, key)) {
            bson_error_set(err, BSON_ERR_DUPLICATE_KEY, "duplicate key for unique index");
            *did_split = false;
            return BSON_ERR_DUPLICATE_KEY;
        }

        uint16_t count = read_u16(page + PAGE_OFF_COUNT);
        uint16_t insert_at = 0;
        while (insert_at < count && bson_btree_key_cmp(leaf_entry_ptr(page, insert_at), key) <= 0) {
            insert_at++;
        }

        if (count < LEAF_MAX_ENTRIES) {
            memmove(leaf_entry_ptr(page, insert_at + 1), leaf_entry_ptr(page, insert_at),
                    (size_t)(count - insert_at) * LEAF_ENTRY_LEN);
            write_leaf_entry(leaf_entry_ptr(page, insert_at), key, offset, 0);
            write_u16(page + PAGE_OFF_COUNT, count + 1);
            *did_split = false;
            bson_error_clear(err);
            return BSON_OK;
        }

        /* Full: split. Build a temp buffer of count+1 entries with the
         * new one inserted at insert_at (existing entries are raw-
         * copied since they're already correctly encoded on the page;
         * the new one is written via write_leaf_entry). */
        uint8_t tmp[(LEAF_MAX_ENTRIES + 1) * LEAF_ENTRY_LEN];
        memcpy(tmp, leaf_entry_ptr(page, 0), (size_t)insert_at * LEAF_ENTRY_LEN);
        write_leaf_entry(tmp + (size_t)insert_at * LEAF_ENTRY_LEN, key, offset, 0);
        memcpy(tmp + (size_t)(insert_at + 1) * LEAF_ENTRY_LEN, leaf_entry_ptr(page, insert_at),
               (size_t)(count - insert_at) * LEAF_ENTRY_LEN);

        uint16_t total = (uint16_t)(count + 1);
        uint16_t left_n = (uint16_t)((total + 1) / 2); /* ceil */
        uint16_t right_n = (uint16_t)(total - left_n);

        uint64_t new_page_no;
        bson_status_t st = allocate_page(tree, &new_page_no, err);
        if (st != BSON_OK) {
            *did_split = false;
            return st;
        }

        page = page_ptr(tree, page_no); /* re-fetch: allocate_page may have resized */
        uint64_t old_next = read_u64(page + PAGE_OFF_LINK);

        memcpy(leaf_entry_ptr(page, 0), tmp, (size_t)left_n * LEAF_ENTRY_LEN);
        write_u16(page + PAGE_OFF_COUNT, left_n);
        write_u64(page + PAGE_OFF_LINK, new_page_no);

        uint8_t *right_page = page_ptr(tree, new_page_no);
        right_page[PAGE_OFF_TYPE] = PAGE_TYPE_LEAF;
        memcpy(leaf_entry_ptr(right_page, 0), tmp + (size_t)left_n * LEAF_ENTRY_LEN,
               (size_t)right_n * LEAF_ENTRY_LEN);
        write_u16(right_page + PAGE_OFF_COUNT, right_n);
        write_u64(right_page + PAGE_OFF_LINK, old_next);

        memcpy(split_key_out, tmp + (size_t)left_n * LEAF_ENTRY_LEN, BSON_BTREE_KEY_SIZE);
        *split_page_out = new_page_no;
        *did_split = true;
        bson_error_clear(err);
        return BSON_OK;
    }

    /* Internal page: find the child to descend into. */
    uint16_t count = read_u16(page + PAGE_OFF_COUNT);
    uint64_t child = read_u64(page + PAGE_OFF_LINK);
    uint16_t i;
    for (i = 0; i < count; i++) {
        uint8_t *entry = internal_entry_ptr(page, i);
        if (bson_btree_key_cmp(key, entry) < 0) break;
        child = read_u64(entry + BSON_BTREE_KEY_SIZE);
    }

    uint8_t child_split_key[BSON_BTREE_KEY_SIZE];
    uint64_t child_split_page = 0;
    bool child_did_split = false;
    bson_status_t st = insert_recursive(tree, child, key, offset, unique, child_split_key,
                                         &child_split_page, &child_did_split, err);
    if (st != BSON_OK) {
        *did_split = false;
        return st;
    }
    if (!child_did_split) {
        *did_split = false;
        bson_error_clear(err);
        return BSON_OK;
    }

    /* Insert (child_split_key, child_split_page) at index i. Re-fetch:
     * the child's recursion may have allocated pages and resized. */
    page = page_ptr(tree, page_no);
    count = read_u16(page + PAGE_OFF_COUNT);

    if (count < INTERNAL_MAX_KEYS) {
        memmove(internal_entry_ptr(page, i + 1), internal_entry_ptr(page, i),
                (size_t)(count - i) * INTERNAL_ENTRY_LEN);
        uint8_t *slot = internal_entry_ptr(page, i);
        memcpy(slot, child_split_key, BSON_BTREE_KEY_SIZE);
        write_u64(slot + BSON_BTREE_KEY_SIZE, child_split_page);
        write_u16(page + PAGE_OFF_COUNT, count + 1);
        *did_split = false;
        bson_error_clear(err);
        return BSON_OK;
    }

    /* Full: split this internal page too. Standard B+Tree internal
     * split: the middle key is hoisted to the parent WITHOUT being
     * duplicated into either half (unlike a leaf split, which copies
     * its promoted key up into the parent as well as keeping it in
     * the right half). */
    uint64_t leftmost = read_u64(page + PAGE_OFF_LINK);
    uint8_t tmp[(INTERNAL_MAX_KEYS + 1) * INTERNAL_ENTRY_LEN];
    memcpy(tmp, internal_entry_ptr(page, 0), (size_t)i * INTERNAL_ENTRY_LEN);
    {
        uint8_t *slot = tmp + (size_t)i * INTERNAL_ENTRY_LEN;
        memcpy(slot, child_split_key, BSON_BTREE_KEY_SIZE);
        write_u64(slot + BSON_BTREE_KEY_SIZE, child_split_page);
    }
    memcpy(tmp + (size_t)(i + 1) * INTERNAL_ENTRY_LEN, internal_entry_ptr(page, i),
           (size_t)(count - i) * INTERNAL_ENTRY_LEN);

    uint16_t total = (uint16_t)(count + 1);
    uint16_t mid = (uint16_t)(total / 2);
    uint16_t left_n = mid;
    uint16_t right_n = (uint16_t)(total - mid - 1);

    uint64_t new_page_no;
    st = allocate_page(tree, &new_page_no, err);
    if (st != BSON_OK) {
        *did_split = false;
        return st;
    }

    page = page_ptr(tree, page_no); /* re-fetch after allocate_page's possible resize */

    memcpy(internal_entry_ptr(page, 0), tmp, (size_t)left_n * INTERNAL_ENTRY_LEN);
    write_u16(page + PAGE_OFF_COUNT, left_n);
    write_u64(page + PAGE_OFF_LINK, leftmost);

    uint8_t *right_page = page_ptr(tree, new_page_no);
    right_page[PAGE_OFF_TYPE] = PAGE_TYPE_INTERNAL;
    uint64_t right_leftmost = read_u64(tmp + (size_t)mid * INTERNAL_ENTRY_LEN + BSON_BTREE_KEY_SIZE);
    write_u64(right_page + PAGE_OFF_LINK, right_leftmost);
    memcpy(internal_entry_ptr(right_page, 0), tmp + (size_t)(mid + 1) * INTERNAL_ENTRY_LEN,
           (size_t)right_n * INTERNAL_ENTRY_LEN);
    write_u16(right_page + PAGE_OFF_COUNT, right_n);

    memcpy(split_key_out, tmp + (size_t)mid * INTERNAL_ENTRY_LEN, BSON_BTREE_KEY_SIZE);
    *split_page_out = new_page_no;
    *did_split = true;
    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_btree_insert(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 uint64_t record_offset, bson_error_t *err) {
    uint64_t root = current_root(tree);
    bool unique = bson_btree_unique(tree);

    uint8_t split_key[BSON_BTREE_KEY_SIZE];
    uint64_t split_page = 0;
    bool did_split = false;
    bson_status_t st =
        insert_recursive(tree, root, key, record_offset, unique, split_key, &split_page, &did_split, err);
    if (st != BSON_OK) return st;

    if (did_split) {
        uint64_t new_root;
        st = allocate_page(tree, &new_root, err);
        if (st != BSON_OK) return st;

        uint8_t *new_root_page = page_ptr(tree, new_root);
        new_root_page[PAGE_OFF_TYPE] = PAGE_TYPE_INTERNAL;
        write_u64(new_root_page + PAGE_OFF_LINK, root);
        uint8_t *slot = internal_entry_ptr(new_root_page, 0);
        memcpy(slot, split_key, BSON_BTREE_KEY_SIZE);
        write_u64(slot + BSON_BTREE_KEY_SIZE, split_page);
        write_u16(new_root_page + PAGE_OFF_COUNT, 1);

        uint8_t *base = mutable_base(tree->file); /* re-fetch after allocate_page's resize */
        write_u64(base + OFF_ROOT_PAGE_NO, new_root);
    }
    bson_error_clear(err);
    return BSON_OK;
}
