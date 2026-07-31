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

void bson_reader_init(bson_reader_t *r, const uint8_t *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

size_t bson_reader_remaining(const bson_reader_t *r) {
    /* Invariant: pos <= len always, maintained by every function below. */
    return r->len - r->pos;
}

bson_status_t bson_reader_read_u8(bson_reader_t *r, uint8_t *out) {
    if (bson_reader_remaining(r) < 1) return BSON_ERR_TRUNCATED_BUFFER;
    *out = r->buf[r->pos];
    r->pos += 1;
    return BSON_OK;
}

bson_status_t bson_reader_read_i32(bson_reader_t *r, int32_t *out) {
    if (bson_reader_remaining(r) < 4) return BSON_ERR_TRUNCATED_BUFFER;
    uint32_t raw;
    memcpy(&raw, r->buf + r->pos, 4);
    *out = (int32_t)bson_le32_to_host(raw);
    r->pos += 4;
    return BSON_OK;
}

bson_status_t bson_reader_read_i64(bson_reader_t *r, int64_t *out) {
    if (bson_reader_remaining(r) < 8) return BSON_ERR_TRUNCATED_BUFFER;
    uint64_t raw;
    memcpy(&raw, r->buf + r->pos, 8);
    *out = (int64_t)bson_le64_to_host(raw);
    r->pos += 8;
    return BSON_OK;
}

bson_status_t bson_reader_read_double(bson_reader_t *r, double *out) {
    if (bson_reader_remaining(r) < 8) return BSON_ERR_TRUNCATED_BUFFER;
    uint64_t raw;
    memcpy(&raw, r->buf + r->pos, 8);
    raw = bson_le64_to_host(raw);
    memcpy(out, &raw, 8);
    r->pos += 8;
    return BSON_OK;
}

bson_status_t bson_reader_read_bytes(bson_reader_t *r, size_t n, const uint8_t **out) {
    if (bson_reader_remaining(r) < n) return BSON_ERR_TRUNCATED_BUFFER;
    *out = r->buf + r->pos;
    r->pos += n;
    return BSON_OK;
}

bson_status_t bson_reader_read_cstring_bounded(bson_reader_t *r, size_t bound, const char **out,
                                                size_t *out_len) {
    if (bound > r->len) bound = r->len;
    if (r->pos > bound) return BSON_ERR_TRUNCATED_BUFFER;
    size_t i = r->pos;
    while (i < bound && r->buf[i] != 0x00) {
        i++;
    }
    if (i >= bound) return BSON_ERR_UNTERMINATED_STRING;
    *out = (const char *)(r->buf + r->pos);
    *out_len = i - r->pos;
    r->pos = i + 1; /* consume the NUL */
    return BSON_OK;
}

bson_status_t bson_reader_skip(bson_reader_t *r, size_t n) {
    if (bson_reader_remaining(r) < n) return BSON_ERR_TRUNCATED_BUFFER;
    r->pos += n;
    return BSON_OK;
}

/* ======================================================================
 * UTF-8 validation
 * ==================================================================== */

bool bson_utf8_validate(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = data[i];
        size_t extra;
        uint32_t cp;
        uint32_t min_cp;

        if (b0 < 0x80) {
            i += 1;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            extra = 1;
            cp = b0 & 0x1Fu;
            min_cp = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            extra = 2;
            cp = b0 & 0x0Fu;
            min_cp = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            extra = 3;
            cp = b0 & 0x07u;
            min_cp = 0x10000;
        } else {
            return false; /* stray continuation byte or invalid lead byte */
        }

        if (i + extra + 1 > len) return false;

        for (size_t k = 1; k <= extra; k++) {
            uint8_t b = data[i + k];
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }

        if (cp < min_cp) return false;         /* overlong encoding */
        if (cp > 0x10FFFFu) return false;       /* beyond Unicode range */
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false; /* surrogate half */

        i += extra + 1;
    }
    return true;
}

/* ======================================================================
 * Iterator
 * ==================================================================== */

bson_status_t bson_iter_init_nested(bson_iter_t *it, const uint8_t *buf, size_t offset,
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
    bson_status_t st = bson_reader_read_i32(&tmp, &doc_len);
    if (st != BSON_OK) {
        bson_error_set(err, st, "failed to read document length at offset %zu", offset);
        return st;
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

    it->reader.buf = buf;
    it->reader.len = parent_end;
    it->reader.pos = offset + BSON_LENGTH_PREFIX_LEN;
    it->doc_end = end - 1; /* offset of the EOO byte */
    it->type = 0;
    it->key_off = 0;
    it->key_len = 0;
    it->value_off = 0;
    return BSON_OK;
}

bson_status_t bson_iter_init_document(bson_iter_t *it, const uint8_t *buf, size_t len,
                                       bson_error_t *err) {
    return bson_iter_init_nested(it, buf, 0, len, err);
}

bool bson_iter_next(bson_iter_t *it, bson_error_t *err) {
    bson_error_clear(err);

    if (it->reader.pos >= it->doc_end) {
        if (it->reader.pos != it->doc_end) {
            bson_error_set(err, BSON_ERR_INVALID_LENGTH, "element overran document bounds");
            return false;
        }
        uint8_t eoo;
        bson_status_t st = bson_reader_read_u8(&it->reader, &eoo);
        if (st != BSON_OK || eoo != BSON_EOO_BYTE) {
            bson_error_set(err, BSON_ERR_MISSING_EOO, "missing or invalid end-of-object byte");
            return false;
        }
        return false; /* clean end of document; err->code == BSON_OK */
    }

    uint8_t type;
    bson_status_t st = bson_reader_read_u8(&it->reader, &type);
    if (st != BSON_OK) {
        bson_error_set(err, st, "truncated element type byte");
        return false;
    }

    const char *key;
    size_t key_len;
    st = bson_reader_read_cstring_bounded(&it->reader, it->doc_end, &key, &key_len);
    if (st != BSON_OK) {
        bson_error_set(err, st, "truncated or unterminated element key");
        return false;
    }
    if (!bson_utf8_validate((const uint8_t *)key, key_len)) {
        bson_error_set(err, BSON_ERR_INVALID_UTF8, "element key is not valid UTF-8");
        return false;
    }

    it->type = type;
    it->key_off = (size_t)(key - (const char *)it->reader.buf);
    it->key_len = key_len;

    switch (type) {
        case BSON_TYPE_DOUBLE:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, 8);
            break;

        case BSON_TYPE_STRING: {
            it->value_off = it->reader.pos;
            int32_t slen;
            st = bson_reader_read_i32(&it->reader, &slen);
            if (st != BSON_OK) break;
            if (slen < 1) {
                st = BSON_ERR_INVALID_LENGTH;
                break;
            }
            st = bson_reader_skip(&it->reader, (size_t)slen);
            break;
        }

        case BSON_TYPE_DOCUMENT:
        case BSON_TYPE_ARRAY: {
            it->value_off = it->reader.pos;
            int32_t dlen;
            st = bson_reader_read_i32(&it->reader, &dlen);
            if (st != BSON_OK) break;
            if (dlen < BSON_DOC_MIN_LEN) {
                st = BSON_ERR_INVALID_LENGTH;
                break;
            }
            size_t end = it->value_off + (size_t)dlen;
            if (end > it->doc_end) {
                st = BSON_ERR_TRUNCATED_BUFFER;
                break;
            }
            it->reader.pos = end;
            break;
        }

        case BSON_TYPE_BINARY: {
            it->value_off = it->reader.pos;
            int32_t blen;
            st = bson_reader_read_i32(&it->reader, &blen);
            if (st != BSON_OK) break;
            if (blen < 0) {
                st = BSON_ERR_INVALID_LENGTH;
                break;
            }
            st = bson_reader_skip(&it->reader, 1); /* subtype byte */
            if (st != BSON_OK) break;
            st = bson_reader_skip(&it->reader, (size_t)blen);
            break;
        }

        case BSON_TYPE_OBJECTID:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, BSON_OBJECTID_LEN);
            break;

        case BSON_TYPE_BOOL:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, 1);
            break;

        case BSON_TYPE_DATETIME:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, 8);
            break;

        case BSON_TYPE_NULL:
            it->value_off = it->reader.pos;
            st = BSON_OK;
            break;

        case BSON_TYPE_INT32:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, 4);
            break;

        case BSON_TYPE_INT64:
            it->value_off = it->reader.pos;
            st = bson_reader_skip(&it->reader, 8);
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
            it->value_off = it->reader.pos;
            st = BSON_ERR_UNSUPPORTED_TYPE;
            break;

        default:
            st = BSON_ERR_INVALID_TYPE_BYTE;
            break;
    }

    if (st != BSON_OK) {
        bson_error_set(err, st, "invalid element of BSON type 0x%02x", type);
        return false;
    }
    if (it->reader.pos > it->doc_end) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER, "element value overran document bounds");
        return false;
    }
    return true;
}

bson_type_t bson_iter_type(const bson_iter_t *it) {
    return (bson_type_t)it->type;
}

const char *bson_iter_key(const bson_iter_t *it, size_t *len) {
    if (len) *len = it->key_len;
    return (const char *)(it->reader.buf + it->key_off);
}

/* Independent bounds-checked re-read from value_off. Never trusts
 * bson_iter_next's bookkeeping alone -- defense in depth. */
static bson_reader_t iter_value_reader(const bson_iter_t *it) {
    bson_reader_t r;
    r.buf = it->reader.buf;
    r.len = it->reader.len;
    r.pos = it->value_off;
    return r;
}

bson_status_t bson_iter_value_double(const bson_iter_t *it, double *out) {
    if (it->type != BSON_TYPE_DOUBLE) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    return bson_reader_read_double(&r, out);
}

bson_status_t bson_iter_value_utf8(const bson_iter_t *it, const char **out, size_t *out_len) {
    if (it->type != BSON_TYPE_STRING) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    int32_t slen;
    bson_status_t st = bson_reader_read_i32(&r, &slen);
    if (st != BSON_OK) return st;
    if (slen < 1) return BSON_ERR_INVALID_LENGTH;
    const uint8_t *bytes;
    st = bson_reader_read_bytes(&r, (size_t)slen, &bytes);
    if (st != BSON_OK) return st;
    if (bytes[slen - 1] != 0x00) return BSON_ERR_UNTERMINATED_STRING;
    if (!bson_utf8_validate(bytes, (size_t)slen - 1)) return BSON_ERR_INVALID_UTF8;
    *out = (const char *)bytes;
    *out_len = (size_t)slen - 1;
    return BSON_OK;
}

bson_status_t bson_iter_value_document(const bson_iter_t *it, size_t *doc_off, size_t *doc_len) {
    if (it->type != BSON_TYPE_DOCUMENT && it->type != BSON_TYPE_ARRAY) {
        return BSON_ERR_INVALID_TYPE_BYTE;
    }
    bson_reader_t r = iter_value_reader(it);
    int32_t dlen;
    bson_status_t st = bson_reader_read_i32(&r, &dlen);
    if (st != BSON_OK) return st;
    if (dlen < BSON_DOC_MIN_LEN) return BSON_ERR_INVALID_LENGTH;
    *doc_off = it->value_off;
    *doc_len = (size_t)dlen;
    return BSON_OK;
}

bson_status_t bson_iter_value_binary(const bson_iter_t *it, uint8_t *subtype, const uint8_t **data,
                                      size_t *len) {
    if (it->type != BSON_TYPE_BINARY) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    int32_t blen;
    bson_status_t st = bson_reader_read_i32(&r, &blen);
    if (st != BSON_OK) return st;
    if (blen < 0) return BSON_ERR_INVALID_LENGTH;
    uint8_t sub;
    st = bson_reader_read_u8(&r, &sub);
    if (st != BSON_OK) return st;
    const uint8_t *bytes;
    st = bson_reader_read_bytes(&r, (size_t)blen, &bytes);
    if (st != BSON_OK) return st;
    *subtype = sub;
    *data = bytes;
    *len = (size_t)blen;
    return BSON_OK;
}

bson_status_t bson_iter_value_objectid(const bson_iter_t *it, const uint8_t **oid12) {
    if (it->type != BSON_TYPE_OBJECTID) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    return bson_reader_read_bytes(&r, BSON_OBJECTID_LEN, oid12);
}

bson_status_t bson_iter_value_bool(const bson_iter_t *it, bool *out) {
    if (it->type != BSON_TYPE_BOOL) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    uint8_t b;
    bson_status_t st = bson_reader_read_u8(&r, &b);
    if (st != BSON_OK) return st;
    *out = (b != 0);
    return BSON_OK;
}

bson_status_t bson_iter_value_datetime_ms(const bson_iter_t *it, int64_t *out) {
    if (it->type != BSON_TYPE_DATETIME) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    return bson_reader_read_i64(&r, out);
}

bson_status_t bson_iter_value_int32(const bson_iter_t *it, int32_t *out) {
    if (it->type != BSON_TYPE_INT32) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    return bson_reader_read_i32(&r, out);
}

bson_status_t bson_iter_value_int64(const bson_iter_t *it, int64_t *out) {
    if (it->type != BSON_TYPE_INT64) return BSON_ERR_INVALID_TYPE_BYTE;
    bson_reader_t r = iter_value_reader(it);
    return bson_reader_read_i64(&r, out);
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

    bson_iter_t it;
    bson_status_t st = bson_iter_init_nested(&it, buf, offset, bound, err);
    if (st != BSON_OK) return st;

    while (bson_iter_next(&it, err)) {
        if (it.type == BSON_TYPE_DOCUMENT || it.type == BSON_TYPE_ARRAY) {
            size_t child_end;
            st = validate_recursive(buf, it.value_off, it.doc_end, depth + 1, err, &child_end);
            if (st != BSON_OK) return st;
        }
    }
    if (err->code != BSON_OK) return err->code;

    if (out_end) *out_end = it.doc_end + 1;
    return BSON_OK;
}

bson_status_t bson_validate_document(const uint8_t *buf, size_t len, bson_error_t *err) {
    bson_error_clear(err);
    if (len < BSON_DOC_MIN_LEN) {
        bson_error_set(err, BSON_ERR_TRUNCATED_BUFFER, "buffer shorter than minimum document size");
        return BSON_ERR_TRUNCATED_BUFFER;
    }

    size_t doc_end = 0;
    bson_status_t st = validate_recursive(buf, 0, len, 0, err, &doc_end);
    if (st != BSON_OK) return st;

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

bson_status_t bson_writer_init(bson_writer_t *w, size_t initial_capacity) {
    if (initial_capacity < 16) initial_capacity = 16;
    w->buf = (uint8_t *)malloc(initial_capacity);
    if (!w->buf) {
        w->len = 0;
        w->cap = 0;
        return BSON_ERR_OUT_OF_MEMORY;
    }
    w->len = 0;
    w->cap = initial_capacity;
    return BSON_OK;
}

void bson_writer_free(bson_writer_t *w) {
    free(w->buf);
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
}

uint8_t *bson_writer_release(bson_writer_t *w, size_t *out_len) {
    uint8_t *buf = w->buf;
    if (out_len) *out_len = w->len;
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
    return buf;
}

/* Centralizes both the growth strategy and the BSON_MAX_DOCUMENT_SIZE
 * cap so every append function gets the size guard for free. */
static bson_status_t writer_reserve(bson_writer_t *w, size_t additional) {
    size_t needed = w->len + additional;
    if (needed < w->len) return BSON_ERR_OUT_OF_MEMORY; /* size_t overflow */
    if (needed > BSON_MAX_DOCUMENT_SIZE) return BSON_ERR_DOCUMENT_TOO_LARGE;
    if (needed <= w->cap) return BSON_OK;

    size_t new_cap = w->cap == 0 ? 256 : w->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(w->buf, new_cap);
    if (!nb) return BSON_ERR_OUT_OF_MEMORY;
    w->buf = nb;
    w->cap = new_cap;
    return BSON_OK;
}

static bson_status_t writer_put_bytes(bson_writer_t *w, const void *data, size_t n) {
    bson_status_t st = writer_reserve(w, n);
    if (st != BSON_OK) return st;
    memcpy(w->buf + w->len, data, n);
    w->len += n;
    return BSON_OK;
}

static bson_status_t writer_put_u8(bson_writer_t *w, uint8_t v) {
    return writer_put_bytes(w, &v, 1);
}

static bson_status_t writer_put_i32(bson_writer_t *w, int32_t v) {
    uint32_t le = BSON_HOST_TO_LE32((uint32_t)v);
    return writer_put_bytes(w, &le, 4);
}

static bson_status_t writer_put_i64(bson_writer_t *w, int64_t v) {
    uint64_t le = BSON_HOST_TO_LE64((uint64_t)v);
    return writer_put_bytes(w, &le, 8);
}

static bson_status_t writer_put_double(bson_writer_t *w, double v) {
    uint64_t raw;
    memcpy(&raw, &v, 8);
    uint64_t le = BSON_HOST_TO_LE64(raw);
    return writer_put_bytes(w, &le, 8);
}

bson_status_t bson_writer_begin_document(bson_writer_t *w, size_t *len_patch_offset) {
    *len_patch_offset = w->len;
    return writer_put_i32(w, 0); /* placeholder, backpatched in end_document */
}

bson_status_t bson_writer_end_document(bson_writer_t *w, size_t len_patch_offset) {
    bson_status_t st = writer_put_u8(w, BSON_EOO_BYTE);
    if (st != BSON_OK) return st;

    size_t total = w->len - len_patch_offset;
    if (total > BSON_MAX_DOCUMENT_SIZE) return BSON_ERR_DOCUMENT_TOO_LARGE;

    uint32_t le = BSON_HOST_TO_LE32((uint32_t)total);
    memcpy(w->buf + len_patch_offset, &le, 4);
    return BSON_OK;
}

bson_status_t bson_writer_append_element_header(bson_writer_t *w, uint8_t type, const char *key,
                                                  size_t key_len) {
    bson_status_t st = writer_put_u8(w, type);
    if (st != BSON_OK) return st;
    st = writer_put_bytes(w, key, key_len);
    if (st != BSON_OK) return st;
    return writer_put_u8(w, 0x00);
}

bson_status_t bson_writer_append_double(bson_writer_t *w, double v) {
    return writer_put_double(w, v);
}

bson_status_t bson_writer_append_utf8(bson_writer_t *w, const char *s, size_t len) {
    if (len > (size_t)INT32_MAX - 1) return BSON_ERR_VALUE_OUT_OF_RANGE;
    bson_status_t st = writer_put_i32(w, (int32_t)(len + 1));
    if (st != BSON_OK) return st;
    st = writer_put_bytes(w, s, len);
    if (st != BSON_OK) return st;
    return writer_put_u8(w, 0x00);
}

bson_status_t bson_writer_append_binary(bson_writer_t *w, uint8_t subtype, const uint8_t *data,
                                          size_t len) {
    if (len > (size_t)INT32_MAX) return BSON_ERR_VALUE_OUT_OF_RANGE;
    bson_status_t st = writer_put_i32(w, (int32_t)len);
    if (st != BSON_OK) return st;
    st = writer_put_u8(w, subtype);
    if (st != BSON_OK) return st;
    return writer_put_bytes(w, data, len);
}

bson_status_t bson_writer_append_objectid(bson_writer_t *w, const uint8_t oid12[12]) {
    return writer_put_bytes(w, oid12, BSON_OBJECTID_LEN);
}

bson_status_t bson_writer_append_bool(bson_writer_t *w, bool v) {
    return writer_put_u8(w, v ? 0x01 : 0x00);
}

bson_status_t bson_writer_append_datetime_ms(bson_writer_t *w, int64_t millis) {
    return writer_put_i64(w, millis);
}

bson_status_t bson_writer_append_null(bson_writer_t *w) {
    (void)w;
    return BSON_OK;
}

bson_status_t bson_writer_append_int32(bson_writer_t *w, int32_t v) {
    return writer_put_i32(w, v);
}

bson_status_t bson_writer_append_int64(bson_writer_t *w, int64_t v) {
    return writer_put_i64(w, v);
}
