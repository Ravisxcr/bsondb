/*
 * btree.h -- persisted on-disk B+Tree index file format and API.
 *
 * One index file per (collection, field). Data lives only in leaves,
 * which are chained via a next-leaf pointer for ordered scans (a
 * B+Tree, not a plain B-Tree). Single-field indexes only -- no
 * compound keys this slice.
 *
 * File layout: page 0 is a 256-byte header padded to a full page;
 * real pages are numbered 1..page_count, each BSON_BTREE_PAGE_SIZE
 * bytes, at file offset `page_no * BSON_BTREE_PAGE_SIZE`.
 *
 * Header fields (offsets within page 0, little-endian):
 *   offset  size  field
 *   0       4     magic "CBI1"
 *   4       2     version (u16) = 1
 *   6       2     flags (u16); bit1 = UNIQUE, bit2 = DESCENDING
 *   8       4     header_len (u32) = 256
 *   12      4     page_size (u32) = 4096
 *   16      8     root_page_no (u64)
 *   24      8     page_count (u64) -- pages in use (1..page_count)
 *   32      8     free_page_head (u64) -- always 0 this slice; no page
 *                 reclamation (delete is tombstone-only; a future
 *                 reindex paired with Collection.compact() is the only
 *                 reclamation path)
 *   40      1     key_type_tag (u8) -- one of BSON_BTREE_TAG_*
 *   41      1     field_path_len (u8)
 *   42      14    reserved
 *   56      200   field_path (UTF-8, NUL-padded)
 *
 * Page layout (both leaf and internal pages share a 16-byte header):
 *   offset 0   page_type (u8): 1 = leaf, 2 = internal
 *   offset 1   flags (u8), reserved
 *   offset 2   count (u16): entry_count (leaf) / key_count (internal)
 *   offset 4   reserved (u32)
 *   offset 8   link (u64): next_leaf_page_no (leaf) or leftmost_child
 *              page number (internal)
 *   offset 16  entries start
 *
 * Leaf entries (22 bytes each, up to LEAF_MAX_ENTRIES):
 *   key[13] | record_offset (u64) | entry_flags (u8, bit0 = DEAD)
 * Duplicates (non-unique indexes) are simply adjacent equal-key
 * entries, kept contiguous by insertion order.
 *
 * Internal entries (21 bytes each, up to INTERNAL_MAX_KEYS), paired
 * with the page header's `link` field as the 0th (leftmost) child:
 *   key[13] | child_page_no (u64)
 * child_i covers keys in [key_{i-1}, key_i); the leftmost child covers
 * keys < key_0.
 *
 * Delete is tombstone-only (the DEAD bit) -- no merge/rebalance.
 */
#ifndef CUSTOM_BSON_BTREE_H
#define CUSTOM_BSON_BTREE_H

#include "custom_bson/bson_engine.h"
#include "custom_bson/platform_mmap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSON_BTREE_MAGIC "CBI1"
#define BSON_BTREE_MAGIC_LEN 4
#define BSON_BTREE_VERSION 1
#define BSON_BTREE_HEADER_LEN 256
#define BSON_BTREE_PAGE_SIZE 4096
#define BSON_BTREE_FIELD_PATH_CAP 200

#define BSON_BTREE_FLAG_UNIQUE 0x0002u
#define BSON_BTREE_FLAG_DESCENDING 0x0004u

#define BSON_BTREE_KEY_SIZE 13 /* 1 tag byte + 12 payload bytes */

/* Fixed key-tag byte values -- also the tag comparison order used for
 * differing-tag keys. INT covers both BSON Int32 and Int64 values
 * (always widened to a little-endian int64 payload), so a field
 * holding a mix of the two BSON integer subtypes still compares
 * purely by numeric value within the tree instead of being split into
 * two tag-ordered groups.
 *
 * IMPORTANT: this tag order is used only to keep the tree internally
 * well-ordered and to support exact-key equality lookup. It is NOT a
 * cross-type numeric total order (e.g. an INT-tagged key always
 * compares less than a DOUBLE-tagged key regardless of value). The
 * Python query planner therefore only ever uses this index for
 * equality lookups, never for $gt/$gte/$lt/$lte range queries on a
 * field that might mix numeric subtypes -- seebson_btree_range's own
 * doc comment below.
 */
#define BSON_BTREE_TAG_NULL 0
#define BSON_BTREE_TAG_BOOL 1
#define BSON_BTREE_TAG_INT 2
#define BSON_BTREE_TAG_DOUBLE 3
#define BSON_BTREE_TAG_DATETIME 4
#define BSON_BTREE_TAG_OBJECTID 5

int bson_btree_key_cmp(const uint8_t a[BSON_BTREE_KEY_SIZE], const uint8_t b[BSON_BTREE_KEY_SIZE]);

typedef struct bson_btree bson_btree_t; /* opaque */

/* Creates a new index file and writes its header + an empty root leaf.
 * `field_path` (the dotted path this index covers) and `key_type_tag`
 * are stored so list_indexes() can report them without external
 * metadata -- the index file is fully self-describing. */
bson_status_t bson_btree_create(const char *path, const char *field_path, uint8_t key_type_tag,
                                 bool unique, bool descending, bson_btree_t **out_tree,
                                 bson_error_t *err);

bson_status_t bson_btree_open(const char *path, bson_btree_t **out_tree, bson_error_t *err);
bson_status_t bson_btree_close(bson_btree_t *tree, bson_error_t *err);
bson_status_t bson_btree_flush(bson_btree_t *tree, bson_error_t *err);

const char *bson_btree_field_path(const bson_btree_t *tree);
bool bson_btree_unique(const bson_btree_t *tree);
bool bson_btree_descending(const bson_btree_t *tree);
uint8_t bson_btree_key_type_tag(const bson_btree_t *tree);

/* Inserts (key, record_offset). If the index is unique and a live
 * (non-DEAD) entry for `key` already exists anywhere in the tree,
 * returns BSON_ERR_DUPLICATE_KEY and inserts nothing. */
bson_status_t bson_btree_insert(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 uint64_t record_offset, bson_error_t *err);

/* Marks the (key, record_offset) leaf entry DEAD. Not an error if no
 * such live entry exists (idempotent, matches bson_storage_tombstone's
 * convention). */
bson_status_t bson_btree_delete(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 uint64_t record_offset, bson_error_t *err);

/* Caller-owned growable output buffer for lookup/range results, the
 * same pattern as bson_writer_t: zero-init via
 * bson_btree_offset_list_init, free via bson_btree_offset_list_free. */
typedef struct {
    uint64_t *data;
    size_t len;
    size_t cap;
} bson_btree_offset_list_t;

void bson_btree_offset_list_init(bson_btree_offset_list_t *list);
void bson_btree_offset_list_free(bson_btree_offset_list_t *list);

/* Appends every live record_offset with an exact key match to *out, in
 * tree order (ascending; duplicates for a non-unique index all
 * included). */
bson_status_t bson_btree_lookup(bson_btree_t *tree, const uint8_t key[BSON_BTREE_KEY_SIZE],
                                 bson_btree_offset_list_t *out, bson_error_t *err);

/* Appends every live record_offset with low <= key <= high (either
 * bound may be NULL for unbounded) to *out, in key order. Implemented
 * for completeness/testability; the Python query planner does not
 * route $gt/$gte/$lt/$lte through this index this slice, because
 * bson_btree_key_cmp's tag order is not a cross-type numeric order
 * (see the comment above BSON_BTREE_TAG_NULL) -- using it for a range
 * scan on a field mixing e.g. int and double values could silently
 * skip qualifying entries. Equality lookup (bson_btree_lookup) doesn't
 * have this hazard since it only ever compares same-tagged keys. */
bson_status_t bson_btree_range(bson_btree_t *tree, const uint8_t *low, bool low_inclusive,
                                const uint8_t *high, bool high_inclusive,
                                bson_btree_offset_list_t *out, bson_error_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BSON_BTREE_H */
