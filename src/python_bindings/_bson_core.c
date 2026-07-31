/*
 * _bson_core.c -- CPython C-API bindings for the BSON engine.
 *
 * One of two files in the project that include Python.h (the other is
 * _storage_core.c, which handles the collection data file storage
 * engine). This file owns:
 *   - the Python <-> BSON type dispatch table (see encode_element /
 *     build_value below),
 *   - translating bson_status_t/bson_error_t into Python exceptions
 *     (bson_error_to_python), always outside any ALLOW_THREADS region,
 *   - the GIL-release strategy for decode (see py_decode).
 *
 * bson_engine.c never calls into Python at all; this file is the only
 * bridge between the two.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <datetime.h>

#include "custom_bson/bson_engine.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ======================================================================
 * Module-init-time cached objects (process lifetime; intentionally
 * never released -- standard practice for a C extension module, not
 * sub-interpreter-safe, irrelevant at this project's current scope).
 * ==================================================================== */

static PyObject *g_ObjectId_type = NULL;
static PyObject *g_InvalidBSON = NULL;
static PyObject *g_InvalidDocument = NULL;
static PyObject *g_BSONNotImplementedError = NULL;
static PyObject *g_DocumentTooLarge = NULL;

/* Buffers larger than this get their validation pass run with the GIL
 * released (see py_decode). Below it, the flip overhead isn't worth it. */
#define GIL_RELEASE_THRESHOLD 4096

/* ======================================================================
 * Error translation. Never called from inside Py_BEGIN_ALLOW_THREADS.
 * ==================================================================== */

static void bson_error_to_python(const bson_error_t *err) {
    switch (err->code) {
        case BSON_OK:
            return; /* should never be reached */
        case BSON_ERR_OUT_OF_MEMORY:
            PyErr_NoMemory();
            return;
        case BSON_ERR_VALUE_OUT_OF_RANGE:
            PyErr_SetString(PyExc_OverflowError, err->message);
            return;
        case BSON_ERR_UNSUPPORTED_TYPE:
            PyErr_SetString(g_BSONNotImplementedError, err->message);
            return;
        case BSON_ERR_UNSUPPORTED_PYTHON_TYPE:
        case BSON_ERR_INVALID_KEY:
            PyErr_SetString(g_InvalidDocument, err->message);
            return;
        case BSON_ERR_DOCUMENT_TOO_LARGE:
            PyErr_SetString(g_DocumentTooLarge, err->message);
            return;
        default:
            /* Every decode-side corruption code (truncated buffer,
             * invalid length, bad type byte, unterminated string,
             * invalid UTF-8, trailing bytes, missing EOO, integer
             * overflow, max depth exceeded) lands here. */
            PyErr_SetString(g_InvalidBSON, err->message);
            return;
    }
}

/* Wraps a raw bson_status_t from an engine call (which carries no
 * message of its own) into `err` with a human-readable message, so
 * every non-OK status reaching bson_error_to_python has real text. */
static bson_status_t writer_call(bson_error_t *err, bson_status_t st, const char *what) {
    if (st != BSON_OK) {
        bson_error_set(err, st, "%s", what);
    }
    return st;
}

/* ======================================================================
 * Gregorian calendar <-> days-since-epoch (Howard Hinnant's algorithm).
 * Used for datetime.datetime <-> BSON UTC datetime (epoch milliseconds)
 * conversion without depending on the platform C library's time
 * functions (which don't handle the full proleptic range and are not
 * reliably UTC-agnostic).
 * ==================================================================== */

static int64_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1; /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                    /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         /* [0, 365] */
    unsigned mp = (5 * doy + 2) / 153;                              /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;                              /* [1, 31] */
    *m = mp + (mp < 10 ? 3 : (unsigned)-9);                         /* [1, 12] */
    *y = (int)(yr + (*m <= 2));
}

static int64_t floordiv_i64(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q -= 1;
    return q;
}

static bson_status_t datetime_to_epoch_ms(PyObject *dt, int64_t *out_ms, bson_error_t *err) {
    int y = PyDateTime_GET_YEAR(dt);
    int mo = PyDateTime_GET_MONTH(dt);
    int d = PyDateTime_GET_DAY(dt);
    int hh = PyDateTime_DATE_GET_HOUR(dt);
    int mm = PyDateTime_DATE_GET_MINUTE(dt);
    int ss = PyDateTime_DATE_GET_SECOND(dt);
    int us = PyDateTime_DATE_GET_MICROSECOND(dt);

    int64_t days = days_from_civil(y, mo, d);
    int64_t ms = days * 86400000LL + (int64_t)hh * 3600000LL + (int64_t)mm * 60000LL +
                 (int64_t)ss * 1000LL + (int64_t)(us / 1000);

    /* Naive datetimes are treated as already-UTC (documented behavior,
     * matches pymongo's default). Aware datetimes are converted using
     * their UTC offset. */
    PyObject *tzinfo = PyDateTime_DATE_GET_TZINFO(dt);
    if (tzinfo != Py_None) {
        PyObject *offset = PyObject_CallMethod(dt, "utcoffset", NULL);
        if (!offset) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
                            "failed to compute datetime UTC offset");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        if (offset != Py_None) {
            PyObject *total = PyObject_CallMethod(offset, "total_seconds", NULL);
            Py_DECREF(offset);
            if (!total) {
                bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
                                "failed to compute datetime UTC offset");
                return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
            }
            double secs = PyFloat_AsDouble(total);
            Py_DECREF(total);
            if (secs == -1.0 && PyErr_Occurred()) {
                bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
                                "failed to compute datetime UTC offset");
                return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
            }
            ms -= (int64_t)(secs * 1000.0);
        } else {
            Py_DECREF(offset);
        }
    }

    *out_ms = ms;
    return BSON_OK;
}

/* Decoded datetimes are always naive (no tzinfo), symmetric with the
 * "naive == UTC" encode-side convention. */
static PyObject *datetime_from_epoch_ms(int64_t ms, bson_error_t *err) {
    int64_t days = floordiv_i64(ms, 86400000LL);
    int64_t rem = ms - days * 86400000LL; /* [0, 86400000) */

    int y;
    unsigned mo, d;
    civil_from_days(days, &y, &mo, &d);

    int hour = (int)(rem / 3600000LL);
    rem %= 3600000LL;
    int minute = (int)(rem / 60000LL);
    rem %= 60000LL;
    int second = (int)(rem / 1000LL);
    rem %= 1000LL;
    int micro = (int)(rem * 1000LL);

    if (y < 1 || y > 9999) {
        bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE,
                        "datetime value is outside Python's supported year range (1-9999)");
        return NULL;
    }

    return PyDateTime_FromDateAndTime(y, (int)mo, (int)d, hour, minute, second, micro);
}

/* ======================================================================
 * Encode: Python object graph -> BSON bytes.
 *
 * The GIL is held for the entire encode -- the recursive walk touches
 * a Python object (dict/list item, type check) at essentially every
 * step, so there is no extended pure-buffer phase analogous to
 * decode's validation pass worth releasing the GIL around. This is a
 * deliberate scope decision (see docs/api_reference.md), not an
 * oversight.
 * ==================================================================== */

#define ENC_CHECK_DEPTH(depth, err)                                                          \
    do {                                                                                     \
        if ((depth) > BSON_MAX_DEPTH) {                                                      \
            bson_error_set((err), BSON_ERR_MAX_DEPTH_EXCEEDED,                               \
                            "document nesting exceeds max depth %d", BSON_MAX_DEPTH);        \
            return BSON_ERR_MAX_DEPTH_EXCEEDED;                                              \
        }                                                                                    \
    } while (0)

static bson_status_t encode_document_body(bson_writer_t *w, PyObject *dict, int depth,
                                           bson_error_t *err);
static bson_status_t encode_array_body(bson_writer_t *w, PyObject *fast_seq, int depth,
                                        bson_error_t *err);

static bson_status_t encode_element(bson_writer_t *w, const char *key, Py_ssize_t key_len,
                                     PyObject *value, int depth, bson_error_t *err) {
    bson_status_t st;

    if (value == Py_None) {
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_NULL, key, (size_t)key_len),
                          "failed to write null element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_null(w), "failed to write null value");
    }

    if (PyBool_Check(value)) {
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_BOOL, key, (size_t)key_len),
                          "failed to write bool element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_bool(w, value == Py_True), "failed to write bool value");
    }

    if (PyLong_Check(value)) {
        int overflow = 0;
        long long v = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (overflow != 0) {
            bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE,
                            "integer value out of range for BSON int64 (max 64-bit signed)");
            return BSON_ERR_VALUE_OUT_OF_RANGE;
        }
        if (v == -1 && PyErr_Occurred()) {
            bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE, "failed to convert Python int");
            return BSON_ERR_VALUE_OUT_OF_RANGE;
        }
        if (v >= INT32_MIN && v <= INT32_MAX) {
            st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_INT32, key, (size_t)key_len),
                              "failed to write int32 element header");
            if (st != BSON_OK) return st;
            return writer_call(err, bson_writer_append_int32(w, (int32_t)v), "failed to write int32 value");
        }
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_INT64, key, (size_t)key_len),
                          "failed to write int64 element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_int64(w, (int64_t)v), "failed to write int64 value");
    }

    if (PyFloat_Check(value)) {
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_DOUBLE, key, (size_t)key_len),
                          "failed to write double element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_double(w, PyFloat_AS_DOUBLE(value)),
                            "failed to write double value");
    }

    if (PyUnicode_Check(value)) {
        Py_ssize_t slen;
        const char *s = PyUnicode_AsUTF8AndSize(value, &slen);
        if (!s) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "string value is not valid unicode");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_STRING, key, (size_t)key_len),
                          "failed to write string element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_utf8(w, s, (size_t)slen), "failed to write string value");
    }

    if (PyBytes_Check(value) || PyByteArray_Check(value)) {
        Py_buffer view;
        if (PyObject_GetBuffer(value, &view, PyBUF_SIMPLE) != 0) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
                            "failed to get a buffer for bytes-like value");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_BINARY, key, (size_t)key_len),
                          "failed to write binary element header");
        if (st == BSON_OK) {
            st = writer_call(err,
                              bson_writer_append_binary(w, BSON_SUBTYPE_GENERIC, (const uint8_t *)view.buf,
                                                          (size_t)view.len),
                              "failed to write binary value");
        }
        PyBuffer_Release(&view);
        return st;
    }

    if (g_ObjectId_type && PyObject_IsInstance(value, g_ObjectId_type)) {
        PyObject *binary = PyObject_GetAttrString(value, "binary");
        if (!binary) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "ObjectId is missing its 'binary' attribute");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        if (!PyBytes_Check(binary) || PyBytes_GET_SIZE(binary) != BSON_OBJECTID_LEN) {
            Py_DECREF(binary);
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "ObjectId.binary must be exactly 12 bytes");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_OBJECTID, key, (size_t)key_len),
                          "failed to write ObjectId element header");
        if (st == BSON_OK) {
            st = writer_call(err, bson_writer_append_objectid(w, (const uint8_t *)PyBytes_AS_STRING(binary)),
                              "failed to write ObjectId value");
        }
        Py_DECREF(binary);
        return st;
    }

    if (PyDateTime_Check(value)) {
        int64_t ms;
        bson_status_t dst = datetime_to_epoch_ms(value, &ms, err);
        if (dst != BSON_OK) return dst;
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_DATETIME, key, (size_t)key_len),
                          "failed to write datetime element header");
        if (st != BSON_OK) return st;
        return writer_call(err, bson_writer_append_datetime_ms(w, ms), "failed to write datetime value");
    }

    if (PyDict_Check(value)) {
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_DOCUMENT, key, (size_t)key_len),
                          "failed to write nested document element header");
        if (st != BSON_OK) return st;
        return encode_document_body(w, value, depth, err);
    }

    if (PyList_Check(value) || PyTuple_Check(value)) {
        st = writer_call(err, bson_writer_append_element_header(w, BSON_TYPE_ARRAY, key, (size_t)key_len),
                          "failed to write array element header");
        if (st != BSON_OK) return st;
        PyObject *fast = PySequence_Fast(value, "expected a list or tuple");
        if (!fast) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "failed to iterate sequence value");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        st = encode_array_body(w, fast, depth, err);
        Py_DECREF(fast);
        return st;
    }

    bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "cannot encode value of type '%s' to BSON",
                    Py_TYPE(value)->tp_name);
    return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
}

static bson_status_t encode_document_body(bson_writer_t *w, PyObject *dict, int depth,
                                           bson_error_t *err) {
    ENC_CHECK_DEPTH(depth, err);

    size_t patch;
    bson_status_t st = writer_call(err, bson_writer_begin_document(w, &patch),
                                     "failed to begin document");
    if (st != BSON_OK) return st;

    PyObject *key;
    PyObject *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dict, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            bson_error_set(err, BSON_ERR_INVALID_KEY, "document keys must be str, got '%s'",
                            Py_TYPE(key)->tp_name);
            return BSON_ERR_INVALID_KEY;
        }
        Py_ssize_t klen;
        const char *kstr = PyUnicode_AsUTF8AndSize(key, &klen);
        if (!kstr) {
            bson_error_set(err, BSON_ERR_INVALID_KEY, "document key is not valid unicode");
            return BSON_ERR_INVALID_KEY;
        }
        if (memchr(kstr, '\0', (size_t)klen) != NULL) {
            bson_error_set(err, BSON_ERR_INVALID_KEY, "document key contains an embedded NUL byte");
            return BSON_ERR_INVALID_KEY;
        }
        st = encode_element(w, kstr, klen, value, depth + 1, err);
        if (st != BSON_OK) return st;
    }

    return writer_call(err, bson_writer_end_document(w, patch), "failed to finalize document");
}

static bson_status_t encode_array_body(bson_writer_t *w, PyObject *fast_seq, int depth,
                                        bson_error_t *err) {
    ENC_CHECK_DEPTH(depth, err);

    size_t patch;
    bson_status_t st = writer_call(err, bson_writer_begin_document(w, &patch), "failed to begin array");
    if (st != BSON_OK) return st;

    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast_seq);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast_seq, i); /* borrowed */
        char keybuf[24];
        int klen = snprintf(keybuf, sizeof(keybuf), "%zd", i);
        st = encode_element(w, keybuf, (size_t)klen, item, depth + 1, err);
        if (st != BSON_OK) return st;
    }

    return writer_call(err, bson_writer_end_document(w, patch), "failed to finalize array");
}

static PyObject *py_encode(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *doc;
    if (!PyArg_ParseTuple(args, "O", &doc)) return NULL;
    if (!PyDict_Check(doc)) {
        PyErr_SetString(PyExc_TypeError, "encode() argument must be a dict");
        return NULL;
    }

    bson_writer_t w;
    size_t hint = 256 + (size_t)PyDict_Size(doc) * 32;
    if (bson_writer_init(&w, hint) != BSON_OK) {
        return PyErr_NoMemory();
    }

    bson_error_t err;
    bson_error_clear(&err);
    bson_status_t st = encode_document_body(&w, doc, 0, &err);
    if (st != BSON_OK) {
        bson_writer_free(&w);
        if (!PyErr_Occurred()) {
            bson_error_to_python(&err);
        }
        return NULL;
    }

    size_t out_len;
    uint8_t *buf = bson_writer_release(&w, &out_len);
    PyObject *result = PyBytes_FromStringAndSize((const char *)buf, (Py_ssize_t)out_len);
    free(buf);
    return result;
}

/* ======================================================================
 * Decode: BSON bytes -> Python object graph.
 * ==================================================================== */

static PyObject *build_document(const uint8_t *buf, size_t offset, size_t parent_end, int depth,
                                 bson_error_t *err);
static PyObject *build_array(const uint8_t *buf, size_t offset, size_t parent_end, int depth,
                              bson_error_t *err);

static PyObject *build_value(const uint8_t *buf, const bson_iter_t *it, int depth, bson_error_t *err) {
    bson_status_t st;

    switch (it->type) {
        case BSON_TYPE_DOUBLE: {
            double v;
            st = bson_iter_value_double(it, &v);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt double element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyFloat_FromDouble(v);
        }
        case BSON_TYPE_STRING: {
            const char *s;
            size_t slen;
            st = bson_iter_value_utf8(it, &s, &slen);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt string element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyUnicode_FromStringAndSize(s, (Py_ssize_t)slen);
        }
        case BSON_TYPE_DOCUMENT: {
            size_t doff, dlen;
            st = bson_iter_value_document(it, &doff, &dlen);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt embedded document element");
                bson_error_to_python(err);
                return NULL;
            }
            return build_document(buf, doff, doff + dlen, depth + 1, err);
        }
        case BSON_TYPE_ARRAY: {
            size_t doff, dlen;
            st = bson_iter_value_document(it, &doff, &dlen);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt embedded array element");
                bson_error_to_python(err);
                return NULL;
            }
            return build_array(buf, doff, doff + dlen, depth + 1, err);
        }
        case BSON_TYPE_BINARY: {
            uint8_t subtype;
            const uint8_t *data;
            size_t len;
            st = bson_iter_value_binary(it, &subtype, &data, &len);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt binary element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)len);
        }
        case BSON_TYPE_OBJECTID: {
            const uint8_t *oid;
            st = bson_iter_value_objectid(it, &oid);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt ObjectId element");
                bson_error_to_python(err);
                return NULL;
            }
            PyObject *raw = PyBytes_FromStringAndSize((const char *)oid, BSON_OBJECTID_LEN);
            if (!raw) return NULL;
            PyObject *obj = PyObject_CallFunctionObjArgs(g_ObjectId_type, raw, NULL);
            Py_DECREF(raw);
            return obj;
        }
        case BSON_TYPE_BOOL: {
            bool v;
            st = bson_iter_value_bool(it, &v);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt bool element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyBool_FromLong(v ? 1 : 0);
        }
        case BSON_TYPE_DATETIME: {
            int64_t ms;
            st = bson_iter_value_datetime_ms(it, &ms);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt datetime element");
                bson_error_to_python(err);
                return NULL;
            }
            PyObject *dt = datetime_from_epoch_ms(ms, err);
            if (!dt) {
                bson_error_to_python(err);
                return NULL;
            }
            return dt;
        }
        case BSON_TYPE_NULL:
            Py_RETURN_NONE;
        case BSON_TYPE_INT32: {
            int32_t v;
            st = bson_iter_value_int32(it, &v);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt int32 element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyLong_FromLong(v);
        }
        case BSON_TYPE_INT64: {
            int64_t v;
            st = bson_iter_value_int64(it, &v);
            if (st != BSON_OK) {
                bson_error_set(err, st, "corrupt int64 element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyLong_FromLongLong(v);
        }
        default:
            bson_error_set(err, BSON_ERR_UNSUPPORTED_TYPE, "BSON type 0x%02x is not supported in this version",
                            it->type);
            bson_error_to_python(err);
            return NULL;
    }
}

static PyObject *build_document(const uint8_t *buf, size_t offset, size_t parent_end, int depth,
                                 bson_error_t *err) {
    if (depth > BSON_MAX_DEPTH) {
        bson_error_set(err, BSON_ERR_MAX_DEPTH_EXCEEDED, "document nesting exceeds max depth %d",
                        BSON_MAX_DEPTH);
        bson_error_to_python(err);
        return NULL;
    }

    bson_iter_t it;
    bson_status_t st = bson_iter_init_nested(&it, buf, offset, parent_end, err);
    if (st != BSON_OK) {
        bson_error_to_python(err);
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    while (bson_iter_next(&it, err)) {
        size_t klen;
        const char *k = bson_iter_key(&it, &klen);
        PyObject *pykey = PyUnicode_FromStringAndSize(k, (Py_ssize_t)klen);
        if (!pykey) {
            Py_DECREF(dict);
            return NULL;
        }
        PyObject *value = build_value(buf, &it, depth, err);
        if (!value) {
            Py_DECREF(pykey);
            Py_DECREF(dict);
            return NULL;
        }
        int rc = PyDict_SetItem(dict, pykey, value);
        Py_DECREF(pykey);
        Py_DECREF(value);
        if (rc != 0) {
            Py_DECREF(dict);
            return NULL;
        }
    }
    if (err->code != BSON_OK) {
        Py_DECREF(dict);
        bson_error_to_python(err);
        return NULL;
    }
    return dict;
}

static PyObject *build_array(const uint8_t *buf, size_t offset, size_t parent_end, int depth,
                              bson_error_t *err) {
    if (depth > BSON_MAX_DEPTH) {
        bson_error_set(err, BSON_ERR_MAX_DEPTH_EXCEEDED, "document nesting exceeds max depth %d",
                        BSON_MAX_DEPTH);
        bson_error_to_python(err);
        return NULL;
    }

    bson_iter_t it;
    bson_status_t st = bson_iter_init_nested(&it, buf, offset, parent_end, err);
    if (st != BSON_OK) {
        bson_error_to_python(err);
        return NULL;
    }

    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    while (bson_iter_next(&it, err)) {
        PyObject *value = build_value(buf, &it, depth, err);
        if (!value) {
            Py_DECREF(list);
            return NULL;
        }
        int rc = PyList_Append(list, value);
        Py_DECREF(value);
        if (rc != 0) {
            Py_DECREF(list);
            return NULL;
        }
    }
    if (err->code != BSON_OK) {
        Py_DECREF(list);
        bson_error_to_python(err);
        return NULL;
    }
    return list;
}

static PyObject *py_decode(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *data_obj;
    if (!PyArg_ParseTuple(args, "O", &data_obj)) return NULL;

    Py_buffer view;
    if (PyObject_GetBuffer(data_obj, &view, PyBUF_SIMPLE) != 0) {
        return NULL; /* TypeError already set */
    }

    const uint8_t *buf = (const uint8_t *)view.buf;
    size_t len = (size_t)view.len;

    bson_error_t err;
    bson_error_clear(&err);

    /* Always validate the whole buffer first -- this is what catches
     * trailing-bytes/corruption uniformly regardless of size. For
     * large buffers this pure-C, no-Python-object-touching scan runs
     * with the GIL released; for small buffers the flip isn't worth
     * it, so it just runs inline. */
    bson_status_t vst;
    if (len > GIL_RELEASE_THRESHOLD) {
        Py_BEGIN_ALLOW_THREADS
        vst = bson_validate_document(buf, len, &err);
        Py_END_ALLOW_THREADS
    } else {
        vst = bson_validate_document(buf, len, &err);
    }
    if (vst != BSON_OK) {
        PyBuffer_Release(&view);
        bson_error_to_python(&err);
        return NULL;
    }

    PyObject *result = build_document(buf, 0, len, 0, &err);
    PyBuffer_Release(&view);
    return result; /* build_document already set a Python exception on failure */
}

/* ======================================================================
 * Module definition
 * ==================================================================== */

static PyMethodDef bson_core_methods[] = {
    {"encode", py_encode, METH_VARARGS, "encode(document: dict) -> bytes"},
    {"decode", py_decode, METH_VARARGS, "decode(data: bytes) -> dict"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef bson_core_module = {
    PyModuleDef_HEAD_INIT,
    "_bson_core",
    "Native BSON encode/decode engine for custom_bson.",
    -1,
    bson_core_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC PyInit__bson_core(void) {
    PyObject *m = PyModule_Create(&bson_core_module);
    if (!m) return NULL;

    PyDateTime_IMPORT;
    if (!PyDateTimeAPI) {
        Py_DECREF(m);
        return NULL;
    }

    PyObject *oid_mod = PyImport_ImportModule("custom_bson.object_id");
    if (!oid_mod) {
        Py_DECREF(m);
        return NULL;
    }
    g_ObjectId_type = PyObject_GetAttrString(oid_mod, "ObjectId");
    Py_DECREF(oid_mod);
    if (!g_ObjectId_type) {
        Py_DECREF(m);
        return NULL;
    }

    PyObject *exc_mod = PyImport_ImportModule("custom_bson.exceptions");
    if (!exc_mod) {
        Py_DECREF(m);
        return NULL;
    }
    g_InvalidBSON = PyObject_GetAttrString(exc_mod, "InvalidBSON");
    g_InvalidDocument = PyObject_GetAttrString(exc_mod, "InvalidDocument");
    g_BSONNotImplementedError = PyObject_GetAttrString(exc_mod, "BSONNotImplementedError");
    g_DocumentTooLarge = PyObject_GetAttrString(exc_mod, "DocumentTooLarge");
    Py_DECREF(exc_mod);
    if (!g_InvalidBSON || !g_InvalidDocument || !g_BSONNotImplementedError || !g_DocumentTooLarge) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
