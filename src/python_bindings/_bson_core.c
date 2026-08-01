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
static bson_status_t writer_call(bson_error_t *err, bson_status_t status, const char *what) {
    if (status != BSON_OK) {
        bson_error_set(err, status, "%s", what);
    }
    return status;
}

/* ======================================================================
 * Gregorian calendar <-> days-since-epoch (Howard Hinnant's algorithm).
 * Used for datetime.datetime <-> BSON UTC datetime (epoch milliseconds)
 * conversion without depending on the platform C library's time
 * functions (which don't handle the full proleptic range and are not
 * reliably UTC-agnostic).
 * ==================================================================== */

static int64_t days_from_civil(int year, int month, int day) {
    year -= (month <= 2);
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);              /* [0, 399] */
    unsigned doy = (153 * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (unsigned)day - 1; /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t days_since_epoch, int *out_year, unsigned *out_month, unsigned *out_day) {
    int64_t z = days_since_epoch + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                    /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    int64_t computed_year = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         /* [0, 365] */
    unsigned mp = (5 * doy + 2) / 153;                              /* [0, 11] */
    *out_day = doy - (153 * mp + 2) / 5 + 1;                        /* [1, 31] */
    *out_month = mp + (mp < 10 ? 3 : (unsigned)-9);                 /* [1, 12] */
    *out_year = (int)(computed_year + (*out_month <= 2));
}

static int64_t floordiv_i64(int64_t dividend, int64_t divisor) {
    int64_t quotient = dividend / divisor;
    int64_t remainder = dividend % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) quotient -= 1;
    return quotient;
}

static bson_status_t datetime_to_epoch_ms(PyObject *py_datetime, int64_t *out_ms, bson_error_t *err) {
    int year = PyDateTime_GET_YEAR(py_datetime);
    int month = PyDateTime_GET_MONTH(py_datetime);
    int day = PyDateTime_GET_DAY(py_datetime);
    int hour = PyDateTime_DATE_GET_HOUR(py_datetime);
    int minute = PyDateTime_DATE_GET_MINUTE(py_datetime);
    int second = PyDateTime_DATE_GET_SECOND(py_datetime);
    int microsecond = PyDateTime_DATE_GET_MICROSECOND(py_datetime);

    int64_t days = days_from_civil(year, month, day);
    int64_t ms = days * 86400000LL + (int64_t)hour * 3600000LL + (int64_t)minute * 60000LL +
                 (int64_t)second * 1000LL + (int64_t)(microsecond / 1000);

    /* Naive datetimes are treated as already-UTC (documented behavior,
     * matches pymongo's default). Aware datetimes are converted using
     * their UTC offset. */
    PyObject *tzinfo = PyDateTime_DATE_GET_TZINFO(py_datetime);
    if (tzinfo != Py_None) {
        PyObject *offset = PyObject_CallMethod(py_datetime, "utcoffset", NULL);
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

    int year;
    unsigned month, day;
    civil_from_days(days, &year, &month, &day);

    int hour = (int)(rem / 3600000LL);
    rem %= 3600000LL;
    int minute = (int)(rem / 60000LL);
    rem %= 60000LL;
    int second = (int)(rem / 1000LL);
    rem %= 1000LL;
    int microsecond = (int)(rem * 1000LL);

    if (year < 1 || year > 9999) {
        bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE,
                        "datetime value is outside Python's supported year range (1-9999)");
        return NULL;
    }

    return PyDateTime_FromDateAndTime(year, (int)month, (int)day, hour, minute, second, microsecond);
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

static bson_status_t encode_document_body(bson_writer_t *writer, PyObject *dict, int depth,
                                           bson_error_t *err);
static bson_status_t encode_array_body(bson_writer_t *writer, PyObject *fast_seq, int depth,
                                        bson_error_t *err);

static bson_status_t encode_element(bson_writer_t *writer, const char *key, Py_ssize_t key_len,
                                     PyObject *value, int depth, bson_error_t *err) {
    bson_status_t status;

    if (value == Py_None) {
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_NULL, key, (size_t)key_len),
                          "failed to write null element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_null(writer), "failed to write null value");
    }

    if (PyBool_Check(value)) {
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_BOOL, key, (size_t)key_len),
                          "failed to write bool element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_bool(writer, value == Py_True), "failed to write bool value");
    }

    if (PyLong_Check(value)) {
        int overflow = 0;
        long long int_value = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (overflow != 0) {
            bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE,
                            "integer value out of range for BSON int64 (max 64-bit signed)");
            return BSON_ERR_VALUE_OUT_OF_RANGE;
        }
        if (int_value == -1 && PyErr_Occurred()) {
            bson_error_set(err, BSON_ERR_VALUE_OUT_OF_RANGE, "failed to convert Python int");
            return BSON_ERR_VALUE_OUT_OF_RANGE;
        }
        if (int_value >= INT32_MIN && int_value <= INT32_MAX) {
            status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_INT32, key, (size_t)key_len),
                              "failed to write int32 element header");
            if (status != BSON_OK) return status;
            return writer_call(err, bson_writer_append_int32(writer, (int32_t)int_value), "failed to write int32 value");
        }
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_INT64, key, (size_t)key_len),
                          "failed to write int64 element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_int64(writer, (int64_t)int_value), "failed to write int64 value");
    }

    if (PyFloat_Check(value)) {
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_DOUBLE, key, (size_t)key_len),
                          "failed to write double element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_double(writer, PyFloat_AS_DOUBLE(value)),
                            "failed to write double value");
    }

    if (PyUnicode_Check(value)) {
        Py_ssize_t slen;
        const char *utf8_str = PyUnicode_AsUTF8AndSize(value, &slen);
        if (!utf8_str) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "string value is not valid unicode");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_STRING, key, (size_t)key_len),
                          "failed to write string element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_utf8(writer, utf8_str, (size_t)slen), "failed to write string value");
    }

    if (PyBytes_Check(value) || PyByteArray_Check(value)) {
        Py_buffer view;
        if (PyObject_GetBuffer(value, &view, PyBUF_SIMPLE) != 0) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE,
                            "failed to get a buffer for bytes-like value");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_BINARY, key, (size_t)key_len),
                          "failed to write binary element header");
        if (status == BSON_OK) {
            status = writer_call(err,
                              bson_writer_append_binary(writer, BSON_SUBTYPE_GENERIC, (const uint8_t *)view.buf,
                                                          (size_t)view.len),
                              "failed to write binary value");
        }
        PyBuffer_Release(&view);
        return status;
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
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_OBJECTID, key, (size_t)key_len),
                          "failed to write ObjectId element header");
        if (status == BSON_OK) {
            status = writer_call(err, bson_writer_append_objectid(writer, (const uint8_t *)PyBytes_AS_STRING(binary)),
                              "failed to write ObjectId value");
        }
        Py_DECREF(binary);
        return status;
    }

    if (PyDateTime_Check(value)) {
        int64_t ms;
        bson_status_t datetime_status = datetime_to_epoch_ms(value, &ms, err);
        if (datetime_status != BSON_OK) return datetime_status;
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_DATETIME, key, (size_t)key_len),
                          "failed to write datetime element header");
        if (status != BSON_OK) return status;
        return writer_call(err, bson_writer_append_datetime_ms(writer, ms), "failed to write datetime value");
    }

    if (PyDict_Check(value)) {
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_DOCUMENT, key, (size_t)key_len),
                          "failed to write nested document element header");
        if (status != BSON_OK) return status;
        return encode_document_body(writer, value, depth, err);
    }

    if (PyList_Check(value) || PyTuple_Check(value)) {
        status = writer_call(err, bson_writer_append_element_header(writer, BSON_TYPE_ARRAY, key, (size_t)key_len),
                          "failed to write array element header");
        if (status != BSON_OK) return status;
        PyObject *fast = PySequence_Fast(value, "expected a list or tuple");
        if (!fast) {
            bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "failed to iterate sequence value");
            return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
        }
        status = encode_array_body(writer, fast, depth, err);
        Py_DECREF(fast);
        return status;
    }

    bson_error_set(err, BSON_ERR_UNSUPPORTED_PYTHON_TYPE, "cannot encode value of type '%s' to BSON",
                    Py_TYPE(value)->tp_name);
    return BSON_ERR_UNSUPPORTED_PYTHON_TYPE;
}

static bson_status_t encode_document_body(bson_writer_t *writer, PyObject *dict, int depth,
                                           bson_error_t *err) {
    ENC_CHECK_DEPTH(depth, err);

    size_t patch;
    bson_status_t status = writer_call(err, bson_writer_begin_document(writer, &patch),
                                     "failed to begin document");
    if (status != BSON_OK) return status;

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
        status = encode_element(writer, kstr, klen, value, depth + 1, err);
        if (status != BSON_OK) return status;
    }

    return writer_call(err, bson_writer_end_document(writer, patch), "failed to finalize document");
}

static bson_status_t encode_array_body(bson_writer_t *writer, PyObject *fast_seq, int depth,
                                        bson_error_t *err) {
    ENC_CHECK_DEPTH(depth, err);

    size_t patch;
    bson_status_t status = writer_call(err, bson_writer_begin_document(writer, &patch), "failed to begin array");
    if (status != BSON_OK) return status;

    Py_ssize_t item_count = PySequence_Fast_GET_SIZE(fast_seq);
    for (Py_ssize_t i = 0; i < item_count; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast_seq, i); /* borrowed */
        char keybuf[24];
        int klen = snprintf(keybuf, sizeof(keybuf), "%zd", i);
        status = encode_element(writer, keybuf, (size_t)klen, item, depth + 1, err);
        if (status != BSON_OK) return status;
    }

    return writer_call(err, bson_writer_end_document(writer, patch), "failed to finalize array");
}

static PyObject *py_encode(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *doc;
    if (!PyArg_ParseTuple(args, "O", &doc)) return NULL;
    if (!PyDict_Check(doc)) {
        PyErr_SetString(PyExc_TypeError, "encode() argument must be a dict");
        return NULL;
    }

    bson_writer_t writer;
    size_t hint = 256 + (size_t)PyDict_Size(doc) * 32;
    if (bson_writer_init(&writer, hint) != BSON_OK) {
        return PyErr_NoMemory();
    }

    bson_error_t err;
    bson_error_clear(&err);
    bson_status_t status = encode_document_body(&writer, doc, 0, &err);
    if (status != BSON_OK) {
        bson_writer_free(&writer);
        if (!PyErr_Occurred()) {
            bson_error_to_python(&err);
        }
        return NULL;
    }

    size_t out_len;
    uint8_t *buf = bson_writer_release(&writer, &out_len);
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

static PyObject *build_value(const uint8_t *buf, const bson_iter_t *iter, int depth, bson_error_t *err) {
    bson_status_t status;

    switch (iter->type) {
        case BSON_TYPE_DOUBLE: {
            double value;
            status = bson_iter_value_double(iter, &value);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt double element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyFloat_FromDouble(value);
        }
        case BSON_TYPE_STRING: {
            const char *utf8_str;
            size_t slen;
            status = bson_iter_value_utf8(iter, &utf8_str, &slen);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt string element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyUnicode_FromStringAndSize(utf8_str, (Py_ssize_t)slen);
        }
        case BSON_TYPE_DOCUMENT: {
            size_t doc_off, doc_len;
            status = bson_iter_value_document(iter, &doc_off, &doc_len);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt embedded document element");
                bson_error_to_python(err);
                return NULL;
            }
            return build_document(buf, doc_off, doc_off + doc_len, depth + 1, err);
        }
        case BSON_TYPE_ARRAY: {
            size_t doc_off, doc_len;
            status = bson_iter_value_document(iter, &doc_off, &doc_len);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt embedded array element");
                bson_error_to_python(err);
                return NULL;
            }
            return build_array(buf, doc_off, doc_off + doc_len, depth + 1, err);
        }
        case BSON_TYPE_BINARY: {
            uint8_t subtype;
            const uint8_t *data;
            size_t len;
            status = bson_iter_value_binary(iter, &subtype, &data, &len);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt binary element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)len);
        }
        case BSON_TYPE_OBJECTID: {
            const uint8_t *oid;
            status = bson_iter_value_objectid(iter, &oid);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt ObjectId element");
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
            bool value;
            status = bson_iter_value_bool(iter, &value);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt bool element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyBool_FromLong(value ? 1 : 0);
        }
        case BSON_TYPE_DATETIME: {
            int64_t ms;
            status = bson_iter_value_datetime_ms(iter, &ms);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt datetime element");
                bson_error_to_python(err);
                return NULL;
            }
            PyObject *datetime_obj = datetime_from_epoch_ms(ms, err);
            if (!datetime_obj) {
                bson_error_to_python(err);
                return NULL;
            }
            return datetime_obj;
        }
        case BSON_TYPE_NULL:
            Py_RETURN_NONE;
        case BSON_TYPE_INT32: {
            int32_t value;
            status = bson_iter_value_int32(iter, &value);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt int32 element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyLong_FromLong(value);
        }
        case BSON_TYPE_INT64: {
            int64_t value;
            status = bson_iter_value_int64(iter, &value);
            if (status != BSON_OK) {
                bson_error_set(err, status, "corrupt int64 element");
                bson_error_to_python(err);
                return NULL;
            }
            return PyLong_FromLongLong(value);
        }
        default:
            bson_error_set(err, BSON_ERR_UNSUPPORTED_TYPE, "BSON type 0x%02x is not supported in this version",
                            iter->type);
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

    bson_iter_t iter;
    bson_status_t status = bson_iter_init_nested(&iter, buf, offset, parent_end, err);
    if (status != BSON_OK) {
        bson_error_to_python(err);
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    while (bson_iter_next(&iter, err)) {
        size_t klen;
        const char *key_ptr = bson_iter_key(&iter, &klen);
        PyObject *pykey = PyUnicode_FromStringAndSize(key_ptr, (Py_ssize_t)klen);
        if (!pykey) {
            Py_DECREF(dict);
            return NULL;
        }
        PyObject *value = build_value(buf, &iter, depth, err);
        if (!value) {
            Py_DECREF(pykey);
            Py_DECREF(dict);
            return NULL;
        }
        int set_result = PyDict_SetItem(dict, pykey, value);
        Py_DECREF(pykey);
        Py_DECREF(value);
        if (set_result != 0) {
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

    bson_iter_t iter;
    bson_status_t status = bson_iter_init_nested(&iter, buf, offset, parent_end, err);
    if (status != BSON_OK) {
        bson_error_to_python(err);
        return NULL;
    }

    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    while (bson_iter_next(&iter, err)) {
        PyObject *value = build_value(buf, &iter, depth, err);
        if (!value) {
            Py_DECREF(list);
            return NULL;
        }
        int append_result = PyList_Append(list, value);
        Py_DECREF(value);
        if (append_result != 0) {
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
    bson_status_t validate_status;
    if (len > GIL_RELEASE_THRESHOLD) {
        Py_BEGIN_ALLOW_THREADS
        validate_status = bson_validate_document(buf, len, &err);
        Py_END_ALLOW_THREADS
    } else {
        validate_status = bson_validate_document(buf, len, &err);
    }
    if (validate_status != BSON_OK) {
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

    PyObject *oid_mod = PyImport_ImportModule("bsondb.object_id");
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

    PyObject *exc_mod = PyImport_ImportModule("bsondb.exceptions");
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
