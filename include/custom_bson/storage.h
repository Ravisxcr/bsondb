/*
 * storage.h -- on-disk collection data file format and the sequential
 * scanner over it.
 *
 * One data file per collection, memory-mapped via platform_mmap.h.
 * Layout:
 *
 *   [0, 64)   fixed header (see field table below)
 *   [64, ...) records: status_byte(1) + BSON document bytes(N), packed
 *             back to back, where N is the document's own leading
 *             int32 length prefix (already fully validated on every
 *             read -- no separate redundant length field is stored).
 *
 * Header fields (all multi-byte fields little-endian):
 *   offset  size  field
 *   0       4     magic "CBD1"
 *   4       2     version (u16) = 1
 *   6       2     flags (u16); bit0 = DIRTY
 *   8       4     header_len (u32) = 64
 *   12      4     reserved
 *   16      8     data_end (u64) -- authoritative append cursor
 *   24      8     live_count (u64) -- approximate, maintained incrementally
 *   32      8     total_count (u64) -- records ever appended (live+tombstone)
 *   40      4     next_index_ordinal (u32)
 *   44      20    reserved
 *
 * data_end is the *logical* length; bson_mmap_size() is the *mapped
 * capacity*, which is pre-grown ahead of data_end (double-on-full,
 * rounded to a page) so appends don't resize on every call.
 */
#ifndef CUSTOM_BSON_STORAGE_H
#define CUSTOM_BSON_STORAGE_H

#include "custom_bson/bson_engine.h"
#include "custom_bson/platform_mmap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSON_STORAGE_MAGIC "CBD1"
#define BSON_STORAGE_MAGIC_LEN 4
#define BSON_STORAGE_VERSION 1
#define BSON_STORAGE_HEADER_LEN 64

#define BSON_STORAGE_FLAG_DIRTY 0x0001u

/* Record status byte values. */
#define BSON_RECORD_UNUSED 0x00
#define BSON_RECORD_LIVE 0x01
#define BSON_RECORD_TOMBSTONE 0x02

#define BSON_STORAGE_INITIAL_CAPACITY 4096
#define BSON_STORAGE_PAGE_SIZE 4096

/* Opens (creating if the file is empty) and validates a collection
 * data file's header in place. On an existing file with flags.DIRTY
 * set, runs a recovery scan (bson_storage_recover) before returning --
 * only when mode == BSON_MMAP_READ_WRITE, since recovery mutates the
 * header. Sets flags.DIRTY on successful open in READ_WRITE mode, so
 * an unclean process exit is detectable on the next open. */
bson_status_t bson_storage_open_header(bson_mmap_file_t *file, bson_mmap_mode_t mode,
                                        bson_error_t *err);

/* Clears flags.DIRTY and flushes. Call before a clean bson_mmap_close(). */
bson_status_t bson_storage_mark_clean(bson_mmap_file_t *file, bson_error_t *err);

/* Recovery: walks records from the header forward, validating each via
 * bson_validate_document, stopping at the first invalid/truncated one.
 * Rewrites data_end/live_count/total_count to the recovered values (a
 * logical truncation -- no physical file truncation, no WAL). */
bson_status_t bson_storage_recover(bson_mmap_file_t *file, bson_error_t *err);

/* Appends one record (status=LIVE + doc bytes), growing the file
 * (double-on-full, rounded to a page) if needed. *out_offset receives
 * the record's status-byte offset (what tombstone/read take). */
bson_status_t bson_storage_append(bson_mmap_file_t *file, const uint8_t *doc, size_t doc_len,
                                   size_t *out_offset, bson_error_t *err);

/* Flips the record at record_off to TOMBSTONE. Idempotent: tombstoning
 * an already-tombstoned record is not an error. */
bson_status_t bson_storage_tombstone(bson_mmap_file_t *file, size_t record_off, bson_error_t *err);

/* Reads back one record's status + document bounds (zero-copy: callers
 * read straight from bson_mmap_data(file) + *out_doc_off). */
bson_status_t bson_storage_read(const bson_mmap_file_t *file, size_t record_off, uint8_t *out_status,
                                 size_t *out_doc_off, size_t *out_doc_len, bson_error_t *err);

size_t bson_storage_data_end(const bson_mmap_file_t *file);
uint64_t bson_storage_live_count(const bson_mmap_file_t *file);
uint64_t bson_storage_total_count(const bson_mmap_file_t *file);

/* ---- Sequential scanner: walk over live records, skipping tombstones */

typedef struct {
    const uint8_t *base; /* snapshot of bson_mmap_data(file) at open time */
    size_t data_end;      /* snapshot of the header's data_end at open time */
    size_t pos;
} bson_scanner_t;

/* Takes a raw base+data_end snapshot rather than the bson_mmap_file_t
 * itself, so callers control exactly when a fresh pointer is fetched --
 * always re-fetch bson_mmap_data() immediately before opening a
 * scanner, never hold one across an operation that could resize the
 * mapping. */
bson_status_t bson_scanner_open(bson_scanner_t *scanner, const uint8_t *base, size_t data_end,
                                 bson_error_t *err);

/* Advances to the next LIVE record (transparently skipping TOMBSTONE
 * records). Returns false at end-of-data or on error; check err after
 * a false return, same convention as bson_iter_next. */
bool bson_scanner_next(bson_scanner_t *scanner, size_t *out_record_off, size_t *out_doc_off,
                        size_t *out_doc_len, bson_error_t *err);

void bson_scanner_close(bson_scanner_t *scanner);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BSON_STORAGE_H */
