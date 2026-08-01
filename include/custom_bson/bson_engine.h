/*
 * bson_engine.h -- public C API for the BSON serialization engine.
 *
 * This header (and its implementation in src/c_engine/bson_engine.c) has
 * ZERO dependency on Python.h. It is pure C11 and compiles/links
 * standalone (see CMakeLists.txt's bson_engine_static target). All
 * Python object traversal lives in src/python_bindings/_bson_core.c,
 * which is the only file permitted to include Python.h and call into
 * this API.
 *
 * Memory layout notes:
 *  - bson_reader_t/bson_iter_t never own the buffer they read from;
 *    callers (the Python bindings, or a future mmap-backed scanner)
 *    are responsible for keeping the underlying memory valid for the
 *    lifetime of the reader/iterator.
 *  - bson_writer_t owns a single heap-allocated growable buffer.
 *    Ownership transfers to the caller via bson_writer_release(); on
 *    any error path the caller must call bson_writer_free() instead
 *    to avoid a leak.
 */
#ifndef CUSTOM_BSON_ENGINE_H
#define CUSTOM_BSON_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "custom_bson/wire_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Status codes ---------------------------------------------------- */

typedef enum {
    BSON_OK = 0,

    /* Decode-side: malformed/corrupt/truncated wire data. */
    BSON_ERR_TRUNCATED_BUFFER,
    BSON_ERR_INVALID_LENGTH,
    BSON_ERR_INVALID_TYPE_BYTE,
    BSON_ERR_UNTERMINATED_STRING,
    BSON_ERR_INVALID_UTF8,
    BSON_ERR_TRAILING_BYTES,
    BSON_ERR_MISSING_EOO,
    BSON_ERR_INTEGER_OVERFLOW,
    BSON_ERR_MAX_DEPTH_EXCEEDED,

    /* Decode-side: structurally valid but not supported this slice. */
    BSON_ERR_UNSUPPORTED_TYPE,

    /* Encode-side. */
    BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
    BSON_ERR_INVALID_KEY,
    BSON_ERR_DOCUMENT_TOO_LARGE,
    BSON_ERR_VALUE_OUT_OF_RANGE,

    /* Shared. */
    BSON_ERR_OUT_OF_MEMORY,
    BSON_ERR_NOT_IMPLEMENTED, /* unused now that platform_mmap/scanner are real; kept for ABI stability */

    /* Storage engine (collection data files, B-Tree index files). */
    BSON_ERR_IO,                      /* open/read/write/ftruncate/mmap syscall failure */
    BSON_ERR_INVALID_FILE_HEADER,     /* bad magic/version on a data or index file */
    BSON_ERR_RECORD_NOT_FOUND,        /* offset doesn't reference a live record */
    BSON_ERR_INDEX_UNSUPPORTED,       /* compound keys, or a non-fixed-width key type (e.g. str) */
    BSON_ERR_DUPLICATE_KEY,           /* unique index violation */
    BSON_ERR_INVALID_UPDATE_DOCUMENT, /* update doc without $ operators / mixed operator+literal */
    BSON_ERR_READ_ONLY_VIOLATION,     /* resize/flush/insert attempted on a READ_ONLY mmap */
} bson_status_t;

/* Fixed-size error detail: no heap allocation on the error path. */
typedef struct {
    bson_status_t code;
    char message[256];
} bson_error_t;

void bson_error_set(bson_error_t *err, bson_status_t code, const char *fmt, ...);
void bson_error_clear(bson_error_t *err);

/* ---- BSON logical type enum (decoded from wire_spec.h type bytes) --- */

typedef enum {
    BSON_TYPE_ID_DOUBLE    = BSON_TYPE_DOUBLE,
    BSON_TYPE_ID_STRING    = BSON_TYPE_STRING,
    BSON_TYPE_ID_DOCUMENT  = BSON_TYPE_DOCUMENT,
    BSON_TYPE_ID_ARRAY     = BSON_TYPE_ARRAY,
    BSON_TYPE_ID_BINARY    = BSON_TYPE_BINARY,
    BSON_TYPE_ID_OBJECTID  = BSON_TYPE_OBJECTID,
    BSON_TYPE_ID_BOOL      = BSON_TYPE_BOOL,
    BSON_TYPE_ID_DATETIME  = BSON_TYPE_DATETIME,
    BSON_TYPE_ID_NULL      = BSON_TYPE_NULL,
    BSON_TYPE_ID_INT32     = BSON_TYPE_INT32,
    BSON_TYPE_ID_INT64     = BSON_TYPE_INT64,
} bson_type_t;

/* ---- Reader: bounds-checked cursor over a borrowed buffer ------------ */

typedef struct {
    const uint8_t *buf; /* borrowed, not owned */
    size_t len;
    size_t pos;
} bson_reader_t;

void   bson_reader_init(bson_reader_t *reader, const uint8_t *buf, size_t len);
size_t bson_reader_remaining(const bson_reader_t *reader);

bson_status_t bson_reader_read_u8(bson_reader_t *reader, uint8_t *out);
bson_status_t bson_reader_read_i32(bson_reader_t *reader, int32_t *out);
bson_status_t bson_reader_read_i64(bson_reader_t *reader, int64_t *out);
bson_status_t bson_reader_read_double(bson_reader_t *reader, double *out);
/* Zero-copy: *out points into reader->buf, valid as long as the
 * underlying buffer is. */
bson_status_t bson_reader_read_bytes(bson_reader_t *reader, size_t count, const uint8_t **out);
/* Zero-copy: scans for a NUL terminator within [pos, bound), where
 * bound is passed by the caller (typically doc_end) so a key/cstring
 * can never be read past its enclosing document. out/out_len exclude
 * the terminating NUL. */
bson_status_t bson_reader_read_cstring_bounded(bson_reader_t *reader, size_t bound,
                                                const char **out, size_t *out_len);
bson_status_t bson_reader_skip(bson_reader_t *reader, size_t count);

/* Validates that [0, len) is well-formed UTF-8. Used on decoded string
 * payloads. */
bool bson_utf8_validate(const uint8_t *data, size_t len);

/* ---- Validation: single pure-buffer pass, safe to run with the GIL
 * released (see _bson_core.c's two-phase decode). Recursively confirms
 * every length/type-byte/cstring/UTF-8/depth invariant without
 * constructing any output; a subsequent bson_iter_t walk over the same
 * buffer is guaranteed not to hit a bounds error (though iter still
 * re-checks defensively). ------------------------------------------- */
bson_status_t bson_validate_document(const uint8_t *buf, size_t len, bson_error_t *err);

/* ---- Iterator: structural walk over one document/array level -------- */

typedef struct {
    bson_reader_t reader; /* independent cursor into the same buffer */
    size_t doc_end;       /* absolute offset where this document ends (EOO byte) */
    uint8_t type;         /* current element's type byte, valid after bson_iter_next */
    size_t key_off;
    size_t key_len;
    size_t value_off; /* offset where the current value's payload begins */
} bson_iter_t;

/* Initializes an iterator over a top-level BSON document/array whose
 * encoded bytes are buf[0..len). Validates the outer length prefix
 * immediately; per-element validation happens lazily in
 * bson_iter_next(). */
bson_status_t bson_iter_init_document(bson_iter_t *iter, const uint8_t *buf, size_t len,
                                       bson_error_t *err);

/* Initializes an iterator over a nested document/array whose length
 * prefix starts at buf[offset), constrained not to read past
 * parent_end. */
bson_status_t bson_iter_init_nested(bson_iter_t *iter, const uint8_t *buf, size_t offset,
                                     size_t parent_end, bson_error_t *err);

/* Advances to the next element. Returns false both on clean
 * end-of-document (err->code == BSON_OK) and on error (err->code !=
 * BSON_OK) -- callers must check err after a false return. */
bool bson_iter_next(bson_iter_t *iter, bson_error_t *err);

bson_type_t bson_iter_type(const bson_iter_t *iter);
const char *bson_iter_key(const bson_iter_t *iter, size_t *len); /* zero-copy */

bson_status_t bson_iter_value_double(const bson_iter_t *iter, double *out);
bson_status_t bson_iter_value_utf8(const bson_iter_t *iter, const char **out, size_t *out_len);
bson_status_t bson_iter_value_document(const bson_iter_t *iter, size_t *doc_off, size_t *doc_len);
bson_status_t bson_iter_value_binary(const bson_iter_t *iter, uint8_t *subtype,
                                      const uint8_t **data, size_t *len);
bson_status_t bson_iter_value_objectid(const bson_iter_t *iter, const uint8_t **oid12);
bson_status_t bson_iter_value_bool(const bson_iter_t *iter, bool *out);
bson_status_t bson_iter_value_datetime_ms(const bson_iter_t *iter, int64_t *out);
bson_status_t bson_iter_value_int32(const bson_iter_t *iter, int32_t *out);
bson_status_t bson_iter_value_int64(const bson_iter_t *iter, int64_t *out);

/* ---- Writer: growable buffer with backpatched document lengths ------ */

typedef struct {
    uint8_t *buf;
    size_t len; /* bytes written so far */
    size_t cap; /* allocated capacity */
} bson_writer_t;

bson_status_t bson_writer_init(bson_writer_t *writer, size_t initial_capacity);
void          bson_writer_free(bson_writer_t *writer);
/* Transfers ownership of the internal buffer to the caller, who must
 * eventually free() it. The writer is left in a freshly-init'd state
 * (empty, zero capacity) and remains safe to bson_writer_free(). */
uint8_t *bson_writer_release(bson_writer_t *writer, size_t *out_len);

/* Begins a nested document/array: writes a placeholder int32(0) length
 * and records its offset in *len_patch_offset for bson_writer_end_document. */
bson_status_t bson_writer_begin_document(bson_writer_t *writer, size_t *len_patch_offset);
/* Writes the EOO terminator and backpatches the length field recorded
 * by the matching begin_document call. */
bson_status_t bson_writer_end_document(bson_writer_t *writer, size_t len_patch_offset);

bson_status_t bson_writer_append_element_header(bson_writer_t *writer, uint8_t type,
                                                  const char *key, size_t key_len);
bson_status_t bson_writer_append_double(bson_writer_t *writer, double value);
bson_status_t bson_writer_append_utf8(bson_writer_t *writer, const char *str, size_t len);
bson_status_t bson_writer_append_binary(bson_writer_t *writer, uint8_t subtype, const uint8_t *data,
                                         size_t len);
bson_status_t bson_writer_append_objectid(bson_writer_t *writer, const uint8_t oid12[12]);
bson_status_t bson_writer_append_bool(bson_writer_t *writer, bool value);
bson_status_t bson_writer_append_datetime_ms(bson_writer_t *writer, int64_t millis);
bson_status_t bson_writer_append_null(bson_writer_t *writer);
bson_status_t bson_writer_append_int32(bson_writer_t *writer, int32_t value);
bson_status_t bson_writer_append_int64(bson_writer_t *writer, int64_t value);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BSON_ENGINE_H */
