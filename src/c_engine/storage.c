/*
 * storage.c -- collection data file header/append/tombstone/recovery.
 *
 * Pure C11, no Python.h dependency (same rule as bson_engine.c). Builds
 * on bson_engine.h (document validation) and platform_mmap.h (the
 * mapped byte buffer).
 */
#include "custom_bson/storage.h"

#include <string.h>

/* ---- little-endian header field helpers -------------------------------
 * u32/u64 reuse wire_spec.h's byte-swap macros; u16 has no equivalent
 * there so it's handled inline. All are no-ops on little-endian hosts. */

static uint16_t read_u16(const uint8_t *bytes) {
    uint16_t value;
    memcpy(&value, bytes, 2);
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = (uint16_t)((value >> 8) | (value << 8));
#endif
    return value;
}

static void write_u16(uint8_t *bytes, uint16_t value) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = (uint16_t)((value >> 8) | (value << 8));
#endif
    memcpy(bytes, &value, 2);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    uint32_t little_endian_value = BSON_HOST_TO_LE32(value);
    memcpy(bytes, &little_endian_value, 4);
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, 8);
    return bson_le64_to_host(value);
}

static void write_u64(uint8_t *bytes, uint64_t value) {
    uint64_t little_endian_value = BSON_HOST_TO_LE64(value);
    memcpy(bytes, &little_endian_value, 8);
}

/* Only ever called on a BSON_MMAP_READ_WRITE handle -- platform_mmap.h
 * returns `const uint8_t *` from bson_mmap_data() unconditionally, so
 * mutation requires this cast. Forced by platform_mmap.h staying frozen. */
static uint8_t *mutable_base(bson_mmap_file_t *file) {
    return (uint8_t *)(uintptr_t)bson_mmap_data(file);
}

/* ======================================================================
 * Header lifecycle
 * ==================================================================== */

bson_status_t bson_storage_open_header(bson_mmap_file_t *file, bson_mmap_mode_t mode,
                                        bson_error_t *err) {
    size_t size = bson_mmap_size(file);

    if (size == 0) {
        if (mode != BSON_MMAP_READ_WRITE) {
            bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER, "collection data file is empty");
            return BSON_ERR_INVALID_FILE_HEADER;
        }
        bson_status_t status = bson_mmap_resize(file, BSON_STORAGE_INITIAL_CAPACITY, err);
        if (status != BSON_OK) return status;

        uint8_t *base = mutable_base(file);
        memset(base, 0, bson_mmap_size(file));
        memcpy(base, BSON_STORAGE_MAGIC, BSON_STORAGE_MAGIC_LEN);
        write_u16(base + 4, BSON_STORAGE_VERSION);
        write_u16(base + 6, 0); /* flags; DIRTY set below */
        write_u32(base + 8, BSON_STORAGE_HEADER_LEN);
        write_u64(base + 16, BSON_STORAGE_HEADER_LEN); /* data_end starts right after the header */
        write_u64(base + 24, 0);                        /* live_count */
        write_u64(base + 32, 0);                        /* total_count */
        write_u32(base + 40, 0);                         /* next_index_ordinal */
    } else {
        if (size < BSON_STORAGE_HEADER_LEN) {
            bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER,
                            "collection data file is too small to contain a header");
            return BSON_ERR_INVALID_FILE_HEADER;
        }
        const uint8_t *base = bson_mmap_data(file);
        if (memcmp(base, BSON_STORAGE_MAGIC, BSON_STORAGE_MAGIC_LEN) != 0) {
            bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER, "bad magic in collection data file");
            return BSON_ERR_INVALID_FILE_HEADER;
        }
        uint16_t version = read_u16(base + 4);
        if (version != BSON_STORAGE_VERSION) {
            bson_error_set(err, BSON_ERR_INVALID_FILE_HEADER,
                            "unsupported collection data file version %u", (unsigned)version);
            return BSON_ERR_INVALID_FILE_HEADER;
        }
        uint16_t flags = read_u16(base + 6);
        if ((flags & BSON_STORAGE_FLAG_DIRTY) != 0 && mode == BSON_MMAP_READ_WRITE) {
            bson_status_t status = bson_storage_recover(file, err);
            if (status != BSON_OK) return status;
        }
    }

    if (mode == BSON_MMAP_READ_WRITE) {
        uint8_t *base = mutable_base(file);
        uint16_t flags = read_u16(base + 6);
        write_u16(base + 6, (uint16_t)(flags | BSON_STORAGE_FLAG_DIRTY));
        bson_status_t status = bson_mmap_flush(file, err);
        if (status != BSON_OK) return status;
    }

    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_storage_mark_clean(bson_mmap_file_t *file, bson_error_t *err) {
    uint8_t *base = mutable_base(file);
    if (!base) {
        bson_error_clear(err);
        return BSON_OK;
    }
    uint16_t flags = read_u16(base + 6);
    write_u16(base + 6, (uint16_t)(flags & ~(uint16_t)BSON_STORAGE_FLAG_DIRTY));
    return bson_mmap_flush(file, err);
}

bson_status_t bson_storage_recover(bson_mmap_file_t *file, bson_error_t *err) {
    const uint8_t *base = bson_mmap_data(file);
    size_t cap = bson_mmap_size(file);
    size_t pos = BSON_STORAGE_HEADER_LEN;
    uint64_t live = 0;
    uint64_t total = 0;

    while (pos < cap) {
        if (pos + 1 > cap) break;
        uint8_t status = base[pos];
        if (status != BSON_RECORD_LIVE && status != BSON_RECORD_TOMBSTONE) break;

        size_t doc_off = pos + 1;
        if (doc_off + BSON_DOC_MIN_LEN > cap) break;

        bson_reader_t reader;
        bson_reader_init(&reader, base + doc_off, cap - doc_off);
        int32_t doc_len;
        if (bson_reader_read_i32(&reader, &doc_len) != BSON_OK) break;
        if (doc_len < BSON_DOC_MIN_LEN || (uint32_t)doc_len > BSON_MAX_DOCUMENT_SIZE) break;

        size_t record_len = 1 + (size_t)doc_len;
        if (pos + record_len > cap) break;

        bson_error_t doc_err;
        if (bson_validate_document(base + doc_off, (size_t)doc_len, &doc_err) != BSON_OK) break;

        total += 1;
        if (status == BSON_RECORD_LIVE) live += 1;
        pos += record_len;
    }

    uint8_t *mbase = mutable_base(file);
    write_u64(mbase + 16, (uint64_t)pos);
    write_u64(mbase + 24, live);
    write_u64(mbase + 32, total);

    bson_error_clear(err);
    return BSON_OK;
}

/* ======================================================================
 * Records
 * ==================================================================== */

bson_status_t bson_storage_append(bson_mmap_file_t *file, const uint8_t *doc, size_t doc_len,
                                   size_t *out_offset, bson_error_t *err) {
    size_t data_end = bson_storage_data_end(file);
    size_t needed = data_end + 1 + doc_len;
    size_t cap = bson_mmap_size(file);

    if (needed > cap) {
        size_t new_cap = cap == 0 ? BSON_STORAGE_INITIAL_CAPACITY : cap;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        new_cap = ((new_cap + BSON_STORAGE_PAGE_SIZE - 1) / BSON_STORAGE_PAGE_SIZE) * BSON_STORAGE_PAGE_SIZE;
        bson_status_t status = bson_mmap_resize(file, new_cap, err);
        if (status != BSON_OK) return status;
    }

    /* Re-fetch after any possible resize -- never reuse a pointer
     * captured before bson_mmap_resize(). */
    uint8_t *base = mutable_base(file);
    base[data_end] = BSON_RECORD_LIVE;
    memcpy(base + data_end + 1, doc, doc_len);

    size_t new_data_end = data_end + 1 + doc_len;
    write_u64(base + 16, (uint64_t)new_data_end);
    write_u64(base + 24, read_u64(base + 24) + 1);
    write_u64(base + 32, read_u64(base + 32) + 1);

    if (out_offset) *out_offset = data_end;
    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_storage_tombstone(bson_mmap_file_t *file, size_t record_off, bson_error_t *err) {
    size_t data_end = bson_storage_data_end(file);
    if (record_off >= data_end) {
        bson_error_set(err, BSON_ERR_RECORD_NOT_FOUND, "offset %zu is not a valid record", record_off);
        return BSON_ERR_RECORD_NOT_FOUND;
    }

    uint8_t *base = mutable_base(file);
    uint8_t status = base[record_off];
    if (status == BSON_RECORD_TOMBSTONE) {
        bson_error_clear(err);
        return BSON_OK; /* idempotent */
    }
    if (status != BSON_RECORD_LIVE) {
        bson_error_set(err, BSON_ERR_RECORD_NOT_FOUND, "offset %zu does not reference a live record",
                        record_off);
        return BSON_ERR_RECORD_NOT_FOUND;
    }

    base[record_off] = BSON_RECORD_TOMBSTONE;
    uint64_t live = read_u64(base + 24);
    if (live > 0) write_u64(base + 24, live - 1);

    bson_error_clear(err);
    return BSON_OK;
}

bson_status_t bson_storage_read(const bson_mmap_file_t *file, size_t record_off, uint8_t *out_status,
                                 size_t *out_doc_off, size_t *out_doc_len, bson_error_t *err) {
    size_t data_end = bson_storage_data_end(file);
    if (record_off >= data_end) {
        bson_error_set(err, BSON_ERR_RECORD_NOT_FOUND, "offset %zu is not a valid record", record_off);
        return BSON_ERR_RECORD_NOT_FOUND;
    }

    const uint8_t *base = bson_mmap_data(file);
    uint8_t record_status = base[record_off];
    size_t doc_off = record_off + 1;

    bson_reader_t reader;
    bson_reader_init(&reader, base + doc_off, data_end - doc_off);
    int32_t doc_len;
    bson_status_t status = bson_reader_read_i32(&reader, &doc_len);
    if (status != BSON_OK || doc_len < BSON_DOC_MIN_LEN) {
        bson_error_set(err, BSON_ERR_RECORD_NOT_FOUND, "offset %zu does not reference a valid record",
                        record_off);
        return BSON_ERR_RECORD_NOT_FOUND;
    }

    if (out_status) *out_status = record_status;
    if (out_doc_off) *out_doc_off = doc_off;
    if (out_doc_len) *out_doc_len = (size_t)doc_len;

    bson_error_clear(err);
    return BSON_OK;
}

size_t bson_storage_data_end(const bson_mmap_file_t *file) {
    const uint8_t *base = bson_mmap_data(file);
    if (!base) return BSON_STORAGE_HEADER_LEN;
    return (size_t)read_u64(base + 16);
}

uint64_t bson_storage_live_count(const bson_mmap_file_t *file) {
    const uint8_t *base = bson_mmap_data(file);
    if (!base) return 0;
    return read_u64(base + 24);
}

uint64_t bson_storage_total_count(const bson_mmap_file_t *file) {
    const uint8_t *base = bson_mmap_data(file);
    if (!base) return 0;
    return read_u64(base + 32);
}
