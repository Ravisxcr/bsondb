/*
 * scanner.c -- zero-copy sequential walk over a collection data file's
 * live records (see storage.h for the file/record format).
 *
 * Takes a raw base+data_end snapshot rather than a bson_mmap_file_t, so
 * callers control exactly when a fresh pointer is fetched -- see the
 * use-after-free discipline documented in storage.h. Used by both
 * Collection.find()'s full-scan fallback and compact() (walk once,
 * re-append every LIVE record into a fresh file).
 */
#include "custom_bson/storage.h"

bson_status_t bson_scanner_open(bson_scanner_t *scanner, const uint8_t *base, size_t data_end,
                                 bson_error_t *err) {
    scanner->base = base;
    scanner->data_end = data_end;
    scanner->pos = BSON_STORAGE_HEADER_LEN;
    bson_error_clear(err);
    return BSON_OK;
}

bool bson_scanner_next(bson_scanner_t *scanner, size_t *out_record_off, size_t *out_doc_off,
                        size_t *out_doc_len, bson_error_t *err) {
    bson_error_clear(err);

    while (scanner->pos < scanner->data_end) {
        size_t record_off = scanner->pos;
        size_t doc_off = record_off + 1;
        if (doc_off > scanner->data_end) {
            bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER, "scanner: truncated record status byte");
            return false;
        }
        uint8_t status = scanner->base[record_off];

        bson_reader_t reader;
        bson_reader_init(&reader, scanner->base + doc_off, scanner->data_end - doc_off);
        int32_t doc_len;
        bson_status_t read_status = bson_reader_read_i32(&reader, &doc_len);
        if (read_status != BSON_OK || doc_len < BSON_DOC_MIN_LEN) {
            bson_error_set(err, BSON_ERR_INVALID_LENGTH, "scanner: invalid record length at offset %zu",
                            record_off);
            return false;
        }

        size_t record_len = 1 + (size_t)doc_len;
        if (record_off + record_len > scanner->data_end) {
            bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER,
                            "scanner: record at offset %zu exceeds the data file's logical end",
                            record_off);
            return false;
        }

        scanner->pos = record_off + record_len;

        if (status == BSON_RECORD_LIVE) {
            if (out_record_off) *out_record_off = record_off;
            if (out_doc_off) *out_doc_off = doc_off;
            if (out_doc_len) *out_doc_len = (size_t)doc_len;
            return true;
        }
        /* TOMBSTONE (or, unexpectedly mid-scan, UNUSED): skip, continue. */
    }
    return false; /* clean end of data; err->code == BSON_OK */
}

void bson_scanner_close(bson_scanner_t *scanner) {
    (void)scanner; /* no-op: the scanner owns no resources of its own */
}
