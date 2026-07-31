/*
 * wire_spec.h -- BSON wire-format constants.
 *
 * Pure constants/macros only, no function declarations and no dependency
 * on any other project header. Shared by the C engine, the Python
 * bindings, and docs/wire_protocol.md (keep that doc in sync with this
 * file, it is the source of truth for type byte values).
 */
#ifndef CUSTOM_BSON_WIRE_SPEC_H
#define CUSTOM_BSON_WIRE_SPEC_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- BSON element type bytes (per the BSON spec) ------------------- */

#define BSON_TYPE_DOUBLE           0x01 /* implemented */
#define BSON_TYPE_STRING           0x02 /* implemented */
#define BSON_TYPE_DOCUMENT         0x03 /* implemented */
#define BSON_TYPE_ARRAY            0x04 /* implemented */
#define BSON_TYPE_BINARY           0x05 /* implemented (generic subtype only) */
#define BSON_TYPE_UNDEFINED        0x06 /* deferred: deprecated in spec */
#define BSON_TYPE_OBJECTID         0x07 /* implemented */
#define BSON_TYPE_BOOL             0x08 /* implemented */
#define BSON_TYPE_DATETIME         0x09 /* implemented (UTC datetime, epoch ms) */
#define BSON_TYPE_NULL             0x0A /* implemented */
#define BSON_TYPE_REGEX            0x0B /* deferred: needs a Python wrapper type */
#define BSON_TYPE_DBPOINTER        0x0C /* deferred: deprecated in spec */
#define BSON_TYPE_JS_CODE          0x0D /* deferred: deprecated in spec */
#define BSON_TYPE_SYMBOL           0x0E /* deferred: deprecated in spec */
#define BSON_TYPE_JS_CODE_W_SCOPE  0x0F /* deferred: deprecated in spec */
#define BSON_TYPE_INT32            0x10 /* implemented */
#define BSON_TYPE_TIMESTAMP        0x11 /* deferred: needs a Python wrapper type */
#define BSON_TYPE_INT64            0x12 /* implemented */
#define BSON_TYPE_DECIMAL128       0x13 /* deferred: needs a Python wrapper type */
#define BSON_TYPE_MINKEY           0xFF /* deferred: needs a Python wrapper type */
#define BSON_TYPE_MAXKEY           0x7F /* deferred: needs a Python wrapper type */

/* Binary (0x05) subtypes. Only GENERIC is round-tripped this slice --
 * decode discards the subtype byte it read and always reports GENERIC
 * to Python; encode always writes GENERIC. See docs/wire_protocol.md. */
#define BSON_SUBTYPE_GENERIC        0x00
#define BSON_SUBTYPE_FUNCTION       0x01
#define BSON_SUBTYPE_BINARY_OLD     0x02
#define BSON_SUBTYPE_UUID_OLD       0x03
#define BSON_SUBTYPE_UUID           0x04
#define BSON_SUBTYPE_MD5            0x05
#define BSON_SUBTYPE_USER_DEFINED   0x80

/* ---- Structural constants ------------------------------------------ */

/* Minimum legal BSON document: int32 length (4) + EOO byte (1). */
#define BSON_DOC_MIN_LEN            5

/* Length of the leading int32 document/string/binary length prefix. */
#define BSON_LENGTH_PREFIX_LEN      4

/* Length of a raw ObjectId payload. */
#define BSON_OBJECTID_LEN           12

/* End-of-object marker terminating every document/array. */
#define BSON_EOO_BYTE               0x00

/* Cap on encoded document size (16 MiB, matches MongoDB's own default
 * limit). Guards bson_writer_t against unbounded growth and gives
 * decode a sane upper bound to reject obviously-corrupt length fields
 * against before ever allocating. */
#define BSON_MAX_DOCUMENT_SIZE      (16 * 1024 * 1024)

/* Cap on recursive nesting depth (documents-within-arrays-within-...).
 * Bounds both the encoder's Python-side recursion and the decoder's
 * iterator recursion so malicious/malformed input can't blow the C
 * stack. */
#define BSON_MAX_DEPTH              100

/* ---- Little-endian <-> host byte order helpers ---------------------
 *
 * BSON is little-endian on the wire. All target platforms today
 * (x86/x86_64/ARM in their default modes) are little-endian, so these
 * are no-ops in practice, but every multi-byte field is routed through
 * them so the codebase stays technically portable to a big-endian host
 * without a redesign.
 */

static inline uint32_t bson_le32_to_host(uint32_t v) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint32_t)((v >> 24) | ((v >> 8) & 0x0000FF00u) |
                       ((v << 8) & 0x00FF0000u) | (v << 24));
#else
    return v;
#endif
}

static inline uint64_t bson_le64_to_host(uint64_t v) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint64_t)((v >> 56) | ((v >> 40) & 0x000000000000FF00ull) |
                       ((v >> 24) & 0x0000000000FF0000ull) |
                       ((v >> 8)  & 0x00000000FF000000ull) |
                       ((v << 8)  & 0x000000FF00000000ull) |
                       ((v << 24) & 0x0000FF0000000000ull) |
                       ((v << 40) & 0x00FF000000000000ull) |
                       (v << 56));
#else
    return v;
#endif
}

#define BSON_HOST_TO_LE32(v) bson_le32_to_host((uint32_t)(v))
#define BSON_HOST_TO_LE64(v) bson_le64_to_host((uint64_t)(v))

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BSON_WIRE_SPEC_H */
