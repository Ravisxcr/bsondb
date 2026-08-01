/*
 * bson_engine.c -- BSON reader/writer/iterator/validation implementation.
 *
 * Pure C11. Deliberately has NO dependency on Python.h anywhere in this
 * file -- see include/custom_bson/bson_engine.h for the rationale. All
 * multi-byte field access goes through memcpy (never a pointer cast)
 * to stay safe under strict aliasing and on buffers of arbitrary
 * alignment (important once this buffer can be a raw mmap'd region).
 *
 * Every length-prefixed read is bounds-checked against the *smaller*
 * of the remaining buffer and the enclosing document's declared end
 * before any memory is touched, because BSON documents handled here
 * are treated as fully attacker-controllable (see bson_validate_document
 * and the corruption test suite in tests/python_tests/test_serializer.py).
 */
#include "custom_bson/bson_engine.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * Error helpers
 * ==================================================================== */

void bson_error_clear(bson_error_t *err) {
    if (!err) return;
    err->code = BSON_OK;
    err->message[0] = '\0';
}

void bson_error_set(bson_error_t *err, bson_status_t code, const char *fmt, ...) {
    if (!err) return;
    err->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

/* ======================================================================
 * Reader
 * ==================================================================== */

void bson_reader_init(bson_reader_t *reader, const uint8_t *buf, size_t len) {
    reader->buf = buf;
    reader->len = len;
    reader->pos = 0;
}

size_t bson_reader_remaining(const bson_reader_t *reader) {
    /* Invariant: pos <= len always, maintained by every function below. */
    return reader->len - reader->pos;
}

bson_status_t bson_reader_read_u8(bson_reader_t *reader, uint8_t *out) {
    if (bson_reader_remaining(reader) < 1) return BSON_ERR_TRUNCATED_BUFFER;
    *out = reader->buf[reader->pos];
    reader->pos += 1;
    return BSON_OK;
}

bson_status_t bson_reader_read_i32(bson_reader_t *reader, int32_t *out) {
    if (bson_reader_remaining(reader) < 4) return BSON_ERR_TRUNCATED_BUFFER;
    uint32_t raw;
    memcpy(&raw, reader->buf + reader->pos, 4);
    *out = (int32_t)bson_le32_to_host(raw);
    reader->pos += 4;
    return BSON_OK;
}

bson_status_t bson_reader_read_i64(bson_reader_t *reader, int64_t *out) {
    if (bson_reader_remaining(reader) < 8) return BSON_ERR_TRUNCATED_BUFFER;
    uint64_t raw;
    memcpy(&raw, reader->buf + reader->pos, 8);
    *out = (int64_t)bson_le64_to_host(raw);
    reader->pos += 8;
    return BSON_OK;
}

bson_status_t bson_reader_read_double(bson_reader_t *reader, double *out) {
    if (bson_reader_remaining(reader) < 8) return BSON_ERR_TRUNCATED_BUFFER;
    uint64_t raw;
    memcpy(&raw, reader->buf + reader->pos, 8);
    raw = bson_le64_to_host(raw);
    memcpy(out, &raw, 8);
    reader->pos += 8;
    return BSON_OK;
}

bson_status_t bson_reader_read_bytes(bson_reader_t *reader, size_t count, const uint8_t **out) {
    if (bson_reader_remaining(reader) < count) return BSON_ERR_TRUNCATED_BUFFER;
    *out = reader->buf + reader->pos;
    reader->pos += count;
    return BSON_OK;
}

bson_status_t bson_reader_read_cstring_bounded(bson_reader_t *reader, size_t bound, const char **out,
                                                size_t *out_len) {
    if (bound > reader->len) bound = reader->len;
    if (reader->pos > bound) return BSON_ERR_TRUNCATED_BUFFER;
    size_t i = reader->pos;
    while (i < bound && reader->buf[i] != 0x00) {
        i++;
    }
    if (i >= bound) return BSON_ERR_UNTERMINATED_STRING;
    *out = (const char *)(reader->buf + reader->pos);
    *out_len = i - reader->pos;
    reader->pos = i + 1; /* consume the NUL */
    return BSON_OK;
}

bson_status_t bson_reader_skip(bson_reader_t *reader, size_t count) {
    if (bson_reader_remaining(reader) < count) return BSON_ERR_TRUNCATED_BUFFER;
    reader->pos += count;
    return BSON_OK;
}

/* ======================================================================
 * UTF-8 validation
 * ==================================================================== */

bool bson_utf8_validate(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t lead_byte = data[i];
        size_t extra;
        uint32_t code_point;
        uint32_t min_code_point;

        if (lead_byte < 0x80) {
            i += 1;
            continue;
        } else if ((lead_byte & 0xE0) == 0xC0) {
            extra = 1;
            code_point = lead_byte & 0x1Fu;
            min_code_point = 0x80;
        } else if ((lead_byte & 0xF0) == 0xE0) {
            extra = 2;
            code_point = lead_byte & 0x0Fu;
            min_code_point = 0x800;
        } else if ((lead_byte & 0xF8) == 0xF0) {
            extra = 3;
            code_point = lead_byte & 0x07u;
            min_code_point = 0x10000;
        } else {
            return false; /* stray continuation byte or invalid lead byte */
        }

        if (i + extra + 1 > len) return false;

        for (size_t k = 1; k <= extra; k++) {
            uint8_t cont_byte = data[i + k];
            if ((cont_byte & 0xC0) != 0x80) return false;
            code_point = (code_point << 6) | (cont_byte & 0x3Fu);
        }

        if (code_point < min_code_point) return false;         /* overlong encoding */
        if (code_point > 0x10FFFFu) return false;       /* beyond Unicode range */
        if (code_point >= 0xD800u && code_point <= 0xDFFFu) return false; /* surrogate half */

        i += extra + 1;
    }
    return true;
}

/* ======================================================================
 * Iterator
 * ==================================================================== */

bson_status_t bson_iter_init_nested(bson_iter_t *iter, const uint8_t *buf, size_t offset,
                                     size_t parent_end, bson_error_t *err) {
    if (offset + BSON_DOC_MIN_LEN > parent_end) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER,
                        "document header at offset %zu exceeds enclosing bounds (%zu)", offset,
                        parent_end);
        return BSON_ERR_TRUNCATED_BUFFER;
    }

    bson_reader_t tmp;
    bson_reader_init(&tmp, buf, parent_end);
    tmp.pos = offset;

    int32_t doc_len;
    bson_status_t status = bson_reader_read_i32(&tmp, &doc_len);
    if (status != BSON_OK) {
        bson_error_set(err, status, "failed to read document length at offset %zu", offset);
        return status;
    }
    if (doc_len < BSON_DOC_MIN_LEN) {
        bson_error_set(err, BSON_ERR_INVALID_LENGTH, "document length %d is below the minimum %d",
                        doc_len, BSON_DOC_MIN_LEN);
        return BSON_ERR_INVALID_LENGTH;
    }
    if ((uint32_t)doc_len > BSON_MAX_DOCUMENT_SIZE) {
        bson_error_set(err, BSON_ERR_DOCUMENT_TOO_LARGE, "document length %d exceeds max %d",
                        doc_len, BSON_MAX_DOCUMENT_SIZE);
        return BSON_ERR_DOCUMENT_TOO_LARGE;
    }

    size_t end = offset + (size_t)doc_len;
    if (end > parent_end) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER,
                        "document length %d at offset %zu exceeds enclosing bounds", doc_len,
                        offset);
        return BSON_ERR_TRUNCATED_BUFFER;
    }

    iter->reader.buf = buf;
    iter->reader.len = parent_end;
    iter->reader.pos = offset + BSON_LENGTH_PREFIX_LEN;
    iter->doc_end = end - 1; /* offset of the EOO byte */
    iter->type = 0;
    iter->key_off = 0;
    iter->key_len = 0;
    iter->value_off = 0;
    return BSON_OK;
}

bson_status_t bson_iter_init_document(bson_iter_t *iter, const uint8_t *buf, size_t len,
                                       bson_error_t *err) {
    return bson_iter_init_nested(iter, buf, 0, len, err);
}

bool bson_iter_next(bson_iter_t *iter, bson_error_t *err) {
    bson_error_clear(err);

    if (iter->reader.pos >= iter->doc_end) {
        if (iter->reader.pos != iter->doc_end) {
            bson_error_set(err, BSON_ERR_INVALID_LENGTH, "element overran document bounds");
            return false;
        }
        uint8_t eoo;
        bson_status_t status = bson_reader_read_u8(&iter->reader, &eoo);
        if (status != BSON_OK || eoo != BSON_EOO_BYTE) {
            bson_error_set(err, BSON_ERR_MISSING_EOO, "missing or invalid end-of-object byte");
            return false;
        }
        return false; /* clean end of document; err->code == BSON_OK */
    }

    uint8_t type;
    bson_status_t status = bson_reader_read_u8(&iter->reader, &type);
    if (status != BSON_OK) {
        bson_error_set(err, status, "truncated element type byte");
        return false;
    }

    const char *key;
    size_t key_len;
    status = bson_reader_read_cstring_bounded(&iter->reader, iter->doc_end, &key, &key_len);
    if (status != BSON_OK) {
        bson_error_set(err, status, "truncated or unterminated element key");
        return false;
    }
    if (!bson_utf8_validate((const uint8_t *)key, key_len)) {
        bson_error_set(err, BSON_ERR_INVALID_UTF8, "element key is not valid UTF-8");
        return false;
    }

    iter->type = type;
    iter->key_off = (size_t)(key - (const char *)iter->reader.buf);
    iter->key_len = key_len;

    switch (type) {
        case BSON_TYPE_DOUBLE:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, 8);
            break;

        case BSON_TYPE_STRING: {
            iter->value_off = iter->reader.pos;
            int32_t slen;
            status = bson_reader_read_i32(&iter->reader, &slen);
            if (status != BSON_OK) break;
            if (slen < 1) {
                status = BSON_ERR_INVALID_LENGTH;
                break;
            }
            status = bson_reader_skip(&iter->reader, (size_t)slen);
            break;
        }

        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY: {
            iter->value_off = iter->reader.pos;
            int32_t dlen;
            status = bson_reader_read_i32(&iter->reader, &dlen);
            if (status != BSON_OK) break;
            if (dlen < BSON_DOC_MIN_LEN) {
                status = BSON_ERR_INVALID_LENGTH;
                break;
            }
            size_t end = iter->value_off + (size_t)dlen;
            if (end > iter->doc_end) {
                status = BSON_ERR_TRUNCATED_BUFFER;
                break;
            }
            iter->reader.pos = end;
            break;
        }

        case BSON_TYPE_BINARY: {
            iter->value_off = iter->reader.pos;
            int32_t blen;
            status = bson_reader_read_i32(&iter->reader, &blen);
            if (status != BSON_OK) break;
            if (blen < 0) {
                status = BSON_ERR_INVALID_LENGTH;
                break;
            }
            status = bson_reader_skip(&iter->reader, 1); /* subtype byte */
            if (status != BSON_OK) break;
            status = bson_reader_skip(&iter->reader, (size_t)blen);
            break;
        }

        case BSON_TYPE_OBJECTID:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, BSON_OBJECTID_LEN);
            break;

        case BSON_TYPE_BOOL:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, 1);
            break;

        case BSON_TYPE_DATETIME:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, 8);
            break;

        case BSON_TYPE_NULL:
            iter->value_off = iter->reader.pos;
            status = BSON_OK;
            break;

        case BSON_TYPE_INT32:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, 4);
            break;

        case BSON_TYPE_INT64:
            iter->value_off = iter->reader.pos;
            status = bson_reader_skip(&iter->reader, 8);
            break;

        /* Structurally valid BSON types this slice does not support
         * materializing. Reported distinctly from a truly invalid type
         * byte so callers can tell "corrupt data" from "valid data we
         * don't handle yet" (see docs/wire_protocol.md). */
        case BSON_TYPE_UNDEFINED:
        case BSON_TYPE_REGEX:
        case BSON_TYPE_DBPOINTER:
        case BSON_TYPE_JS_CODE:
        case BSON_TYPE_SYMBOL:
        case BSON_TYPE_JS_CODE_W_SCOPE:
        case BSON_TYPE_TIMESTAMP:
        case BSON_TYPE_DECIMAL128:
        case BSON_TYPE_MINKEY:
        case BSON_TYPE_MAXKEY:
            iter->value_off = iter->reader.pos;
            status = BSON_ERR_UNSUPPORTED_TYPE;
            break;

        default:
            status = BSON_ERR_INVALID_TYPE_BYTE;
            break;
    }

    if (status != BSON_OK) {
        bson_error_set(err, status, "invalid element of BSON type 0x%02x", type);
        return false;
    }
    if (iter->reader.pos > iter->doc_end) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER, "element value overran document bounds");
        return false;
    }
    return true;
}

bson_type_t bson_iter_type(const bson_iter_t *iter) {
    return (bson_type_t)iter->type;
}

const char *bson_iter_key(const bson_iter_t *iter, size_t *len) {
    if (len) *len = iter->key_len;
    return (const char *)(iter->reader.buf + iter->key_off);
}

/* Independent bounds-checked re-read from value_off. Never trusts
 * bson_iter_next's bookkeeping alone -- defense in depth. */
static bson_reader_t iter_value_reader(const bson_iter_t *iter) {
    bson_reader_t reader;
    reader.buf = iter->reader.buf;
    reader.len = iter->reader.len;
    reader.pos = iter->value_off;
    return reader;
}

bson_status_t bson_iter_value_double(const bson_iter_t *iter, double *out) {
    if (iter->type != BSON_TYPE_DOUBLE) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    return bson_reader_read_double(&reader, out);
}

bson_status_t bson_iter_value_utf8(const bson_iter_t *iter, const char **out, size_t *out_len) {
    if (iter->type != BSON_TYPE_STRING) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    int32_t slen;
    bson_status_t status = bson_reader_read_i32(&reader, &slen);
    if (status != BSON_OK) return status;
    if (slen < 1) return BSON_ERR_INVALID_LENGTH;
    const uint8_t *bytes;
    status = bson_reader_read_bytes(&reader, (size_t)slen, &bytes);
    if (status != BSON_OK) return status;
    if (bytes[slen - 1] != 0x00) return BSON_ERR_UNTERMINATED_STRING;
    if (!bson_utf8_validate(bytes, (size_t)slen - 1)) return BSON_ERR_INVALID_UTF8;
    *out = (const char *)bytes;
    *out_len = (size_t)slen - 1;
    return BSON_OK;
}

bson_status_t bson_iter_value_document(const bson_iter_t *iter, size_t *doc_off, size_t *doc_len) {
    if (iter->type != BSON_TYPE_DOCUMENT && iter->type != BSON_TYPE_ARRAY) {
        return BSON_ERR_INVALID_TYPE_BYTE;
    }
    bson_reader_t reader = iter_value_reader(iter);
    int32_t dlen;
    bson_status_t status = bson_reader_read_i32(&reader, &dlen);
    if (status != BSON_OK) return status;
    if (dlen < BSON_DOC_MIN_LEN) return BSON_ERR_INVALID_LENGTH;
    *doc_off = iter->value_off;
    *doc_len = (size_t)dlen;
    return BSON_OK;
}

bson_status_t bson_iter_value_binary(const bson_iter_t *iter, uint8_t *subtype, const uint8_t **data,
                                      size_t *len) {
    if (iter->type != BSON_TYPE_BINARY) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    int32_t blen;
    bson_status_t status = bson_reader_read_i32(&reader, &blen);
    if (status != BSON_OK) return status;
    if (blen < 0) return BSON_ERR_INVALID_LENGTH;
    uint8_t subtype_byte;
    status = bson_reader_read_u8(&reader, &subtype_byte);
    if (status != BSON_OK) return status;
    const uint8_t *bytes;
    status = bson_reader_read_bytes(&reader, (size_t)blen, &bytes);
    if (status != BSON_OK) return status;
    *subtype = subtype_byte;
    *data = bytes;
    *len = (size_t)blen;
    return BSON_OK;
}

bson_status_t bson_iter_value_objectid(const bson_iter_t *iter, const uint8_t **oid12) {
    if (iter->type != BSON_TYPE_OBJECTID) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    return bson_reader_read_bytes(&reader, BSON_OBJECTID_LEN, oid12);
}

bson_status_t bson_iter_value_bool(const bson_iter_t *iter, bool *out) {
    if (iter->type != BSON_TYPE_BOOL) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    uint8_t byte_value;
    bson_status_t status = bson_reader_read_u8(&reader, &byte_value);
    if (status != BSON_OK) return status;
    *out = (byte_value != 0);
    return BSON_OK;
}

bson_status_t bson_iter_value_datetime_ms(const bson_iter_t *iter, int64_t *out) {
    if (iter->type != BSON_TYPE_DATETIME) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    return bson_reader_read_i64(&reader, out);
}

bson_status_t bson_iter_value_int32(const bson_iter_t *iter, int32_t *out) {
    if (iter->type != BSON_TYPE_INT32) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    return bson_reader_read_i32(&reader, out);
}

bson_status_t bson_iter_value_int64(const bson_iter_t *iter, int64_t *out) {
    if (iter->type != BSON_TYPE_INT64) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t reader = iter_value_reader(iter);
    return bson_reader_read_i64(&reader, out);
}

/* ======================================================================
 * Validation: a single pure-buffer recursive pass. Safe to run with the
 * GIL released by the Python bindings (see _bson_core.c) since it never
 * allocates and never touches anything but the input buffer and the C
 * stack.
 * ==================================================================== */

static bson_status_t validate_recursive(const uint8_t *buf, size_t offset, size_t bound,
                                         int depth, bson_error_t *err, size_t *out_end) {
    if (depth > BSON_MAX_DEPTH) {
        bson_error_set(err, BSON_ERR_MAX_DEPTH_EXCEEDED, "document nesting exceeds max depth %d",
                        BSON_MAX_DEPTH);
        return BSON_ERR_MAX_DEPTH_EXCEEDED;
    }

    bson_iter_t iter;
    bson_status_t status = bson_iter_init_nested(&iter, buf, offset, bound, err);
    if (status != BSON_OK) return status;

    while (bson_iter_next(&iter, err)) {
        if (iter.type == BSON_TYPE_DOCUMENT || iter.type == BSON_TYPE_ARRAY) {
            size_t child_end;
            status = validate_recursive(buf, iter.value_off, iter.doc_end, depth + 1, err, &child_end);
            if (status != BSON_OK) return status;
        }
    }
    if (err->code != BSON_OK) return err->code;

    if (out_end) *out_end = iter.doc_end + 1;
    return BSON_OK;
}

bson_status_t bson_validate_document(const uint8_t *buf, size_t len, bson_error_t *err) {
    bson_error_clear(err);
    if (len < BSON_DOC_MIN_LEN) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER, "buffer shorter than minimum document size");
        return BSON_ERR_TRUNCATED_BUFFER;
    }

    size_t doc_end = 0;
    bson_status_t status = validate_recursive(buf, 0, len, 0, err, &doc_end);
    if (status != BSON_OK) return status;

    if (doc_end != len) {
        bson_error_set(err, BSON_ERR_TRAILING_BYTES, "%zu trailing byte(s) after top-level document",
                        len - doc_end);
        return BSON_ERR_TRAILING_BYTES;
    }
    return BSON_OK;
}

/* ======================================================================
 * Writer
 * ==================================================================== */

bson_status_t bson_writer_init(bson_writer_t *writer, size_t initial_capacity) {
    if (initial_capacity < 16) initial_capacity = 16;
    writer->buf = (uint8_t *)malloc(initial_capacity);
    if (!writer->buf) {
        writer->len = 0;
        writer->cap = 0;
        return BSON_ERR_OUT_OF_MEMORY;
    }
    writer->len = 0;
    writer->cap = initial_capacity;
    return BSON_OK;
}

void bson_writer_free(bson_writer_t *writer) {
    free(writer->buf);
    writer->buf = NULL;
    writer->len = 0;
    writer->cap = 0;
}

uint8_t *bson_writer_release(bson_writer_t *writer, size_t *out_len) {
    uint8_t *buf = writer->buf;
    if (out_len) *out_len = writer->len;
    writer->buf = NULL;
    writer->len = 0;
    writer->cap = 0;
    return buf;
}

/* Centralizes both the growth strategy and the BSON_MAX_DOCUMENT_SIZE
 * cap so every append function gets the size guard for free. */
static bson_status_t writer_reserve(bson_writer_t *writer, size_t additional) {
    size_t needed = writer->len + additional;
    if (needed < writer->len) return BSON_ERR_OUT_OF_MEMORY; /* size_t overflow */
    if (needed > BSON_MAX_DOCUMENT_SIZE) return BSON_ERR_DOCUMENT_TOO_LARGE;
    if (needed <= writer->cap) return BSON_OK;

    size_t new_cap = writer->cap == 0 ? 256 : writer->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    uint8_t *new_buf = (uint8_t *)realloc(writer->buf, new_cap);
    if (!new_buf) return BSON_ERR_OUT_OF_MEMORY;
    writer->buf = new_buf;
    writer->cap = new_cap;
    return BSON_OK;
}

static bson_status_t writer_put_bytes(bson_writer_t *writer, const void *data, size_t count) {
    bson_status_t status = writer_reserve(writer, count);
    if (status != BSON_OK) return status;
    memcpy(writer->buf + writer->len, data, count);
    writer->len += count;
    return BSON_OK;
}

static bson_status_t writer_put_u8(bson_writer_t *writer, uint8_t value) {
    return writer_put_bytes(writer, &value, 1);
}

static bson_status_t writer_put_i32(bson_writer_t *writer, int32_t value) {
    uint32_t little_endian_value = BSON_HOST_TO_LE32((uint32_t)value);
    return writer_put_bytes(writer, &little_endian_value, 4);
}

static bson_status_t writer_put_i64(bson_writer_t *writer, int64_t value) {
    uint64_t little_endian_value = BSON_HOST_TO_LE64((uint64_t)value);
    return writer_put_bytes(writer, &little_endian_value, 8);
}

static bson_status_t writer_put_double(bson_writer_t *writer, double value) {
    uint64_t raw;
    memcpy(&raw, &value, 8);
    uint64_t little_endian_value = BSON_HOST_TO_LE64(raw);
    return writer_put_bytes(writer, &little_endian_value, 8);
}

bson_status_t bson_writer_begin_document(bson_writer_t *writer, size_t *len_patch_offset) {
    *len_patch_offset = writer->len;
    return writer_put_i32(writer, 0); /* placeholder, backpatched in end_document */
}

bson_status_t bson_writer_end_document(bson_writer_t *writer, size_t len_patch_offset) {
    bson_status_t status = writer_put_u8(writer, BSON_EOO_BYTE);
    if (status != BSON_OK) return status;

    size_t total = writer->len - len_patch_offset;
    if (total > BSON_MAX_DOCUMENT_SIZE) return BSON_ERR_DOCUMENT_TOO_LARGE;

    uint32_t little_endian_value = BSON_HOST_TO_LE32((uint32_t)total);
    memcpy(writer->buf + len_patch_offset, &little_endian_value, 4);
    return BSON_OK;
}

bson_status_t bson_writer_append_element_header(bson_writer_t *writer, uint8_t type, const char *key,
                                                  size_t key_len) {
    bson_status_t status = writer_put_u8(writer, type);
    if (status != BSON_OK) return status;
    status = writer_put_bytes(writer, key, key_len);
    if (status != BSON_OK) return status;
    return writer_put_u8(writer, 0x00);
}

bson_status_t bson_writer_append_double(bson_writer_t *writer, double value) {
    return writer_put_double(writer, value);
}

bson_status_t bson_writer_append_utf8(bson_writer_t *writer, const char *str, size_t len) {
    if (len > (size_t)INT32_MAX - 1) return BSON_ERR_VALUE_OUT_OF_RANGE;
    bson_status_t status = writer_put_i32(writer, (int32_t)(len + 1));
    if (status != BSON_OK) return status;
    status = writer_put_bytes(writer, str, len);
    if (status != BSON_OK) return status;
    return writer_put_u8(writer, 0x00);
}

bson_status_t bson_writer_append_binary(bson_writer_t *writer, uint8_t subtype, const uint8_t *data,
                                          size_t len) {
    if (len > (size_t)INT32_MAX) return BSON_ERR_VALUE_OUT_OF_RANGE;
    bson_status_t status = writer_put_i32(writer, (int32_t)len);
    if (status != BSON_OK) return status;
    status = writer_put_u8(writer, subtype);
    if (status != BSON_OK) return status;
    return writer_put_bytes(writer, data, len);
}

bson_status_t bson_writer_append_objectid(bson_writer_t *writer, const uint8_t oid12[12]) {
    return writer_put_bytes(writer, oid12, BSON_OBJECTID_LEN);
}

bson_status_t bson_writer_append_bool(bson_writer_t *writer, bool value) {
    return writer_put_u8(writer, value ? 0x01 : 0x00);
}

bson_status_t bson_writer_append_datetime_ms(bson_writer_t *writer, int64_t millis) {
    return writer_put_i64(writer, millis);
}

bson_status_t bson_writer_append_null(bson_writer_t *writer) {
    (void)writer;
    return BSON_OK;
}

bson_status_t bson_writer_append_int32(bson_writer_t *writer, int32_t value) {
    return writer_put_i32(writer, value);
}

bson_status_t bson_writer_append_int64(bson_writer_t *writer, int64_t value) {
    return writer_put_i64(writer, value);
}
