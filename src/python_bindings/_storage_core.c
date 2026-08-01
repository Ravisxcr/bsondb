/*
 * _storage_core.c -- CPython C-API bindings for the collection data
 * file storage engine (append-only mmap'd file, see storage.h).
 *
 * This is one of two files in the project that include Python.h (the
 * other is _bson_core.c, which handles BSON encode/decode only). This
 * file owns the Python <-> storage-engine boundary: a CollectionHandle
 * type wrapping one open bson_mmap_file_t and an IndexHandle type
 * wrapping one open bson_btree_t, both translating bson_status_t into
 * Python exceptions, and neither ever returning a zero-copy view into
 * the mmap region -- every document/key read is copied into a PyBytes
 * before crossing back into Python, which sidesteps an entire class of
 * mmap-resize use-after-free risk at the Python boundary (see
 * storage.h's use-after-free discipline notes).
 *
 * Index key encoding/decoding from typed Python values lives in
 * python/custom_bson/index.py, not here -- IndexHandle's methods only
 * ever handle opaque BSON_BTREE_KEY_SIZE-byte blobs.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "custom_bson/btree.h"
#include "custom_bson/storage.h"

/* ======================================================================
 * Module-init-time cached objects
 * ==================================================================== */

static PyObject *g_InvalidBSON = NULL;
static PyObject *g_DuplicateKeyError = NULL;

static void storage_error_to_python(const bson_error_t *err) {
    switch (err->code) {
        case BSON_OK:
            return; /* should never be reached */
        case BSON_ERR_OUT_OF_MEMORY:
            PyErr_NoMemory();
            return;
        case BSON_ERR_IO:
            PyErr_SetString(PyExc_OSError, err->message);
            return;
        case BSON_ERR_INVALID_FILE_HEADER:
            PyErr_SetString(g_InvalidBSON, err->message);
            return;
        case BSON_ERR_RECORD_NOT_FOUND:
            PyErr_SetString(PyExc_LookupError, err->message);
            return;
        case BSON_ERR_READ_ONLY_VIOLATION:
            PyErr_SetString(PyExc_ValueError, err->message);
            return;
        case BSON_ERR_DUPLICATE_KEY:
            PyErr_SetString(g_DuplicateKeyError, err->message);
            return;
        case BSON_ERR_INDEX_UNSUPPORTED:
            PyErr_SetString(PyExc_ValueError, err->message);
            return;
        default:
            PyErr_SetString(PyExc_RuntimeError, err->message);
            return;
    }
}

/* ======================================================================
 * CollectionHandle type
 * ==================================================================== */

typedef struct {
    PyObject_HEAD
    bson_mmap_file_t *file; /* NULL once closed */
} CollectionHandleObject;

#define CHECK_OPEN(self)                                                            \
    do {                                                                            \
        if (!(self)->file) {                                                        \
            PyErr_SetString(PyExc_ValueError, "operation on a closed CollectionHandle"); \
            return NULL;                                                             \
        }                                                                            \
    } while (0)

static PyObject *CollectionHandle_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    (void)type;
    (void)args;
    (void)kwds;
    PyErr_SetString(PyExc_TypeError,
                     "CollectionHandle cannot be instantiated directly; use open_collection()");
    return NULL;
}

static void CollectionHandle_dealloc(CollectionHandleObject *self) {
    if (self->file) {
        bson_error_t err;
        bson_storage_mark_clean(self->file, &err);
        bson_mmap_close(self->file, &err);
        self->file = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *CollectionHandle_close(CollectionHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    if (self->file) {
        bson_error_t err;
        bson_status_t status = bson_storage_mark_clean(self->file, &err);
        if (status != BSON_OK) {
            storage_error_to_python(&err);
            return NULL;
        }
        status = bson_mmap_close(self->file, &err);
        self->file = NULL;
        if (status != BSON_OK) {
            storage_error_to_python(&err);
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *CollectionHandle_flush(CollectionHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    CHECK_OPEN(self);
    bson_error_t err;
    bson_status_t status = bson_mmap_flush(self->file, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *CollectionHandle_append_record(CollectionHandleObject *self, PyObject *args) {
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*", &view)) return NULL;
    if (!self->file) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "operation on a closed CollectionHandle");
        return NULL;
    }

    size_t offset;
    bson_error_t err;
    bson_status_t status =
        bson_storage_append(self->file, (const uint8_t *)view.buf, (size_t)view.len, &offset, &err);
    PyBuffer_Release(&view);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    return PyLong_FromSize_t(offset);
}

static PyObject *CollectionHandle_read_record(CollectionHandleObject *self, PyObject *args) {
    Py_ssize_t offset;
    if (!PyArg_ParseTuple(args, "n", &offset)) return NULL;
    CHECK_OPEN(self);
    if (offset < 0) {
        PyErr_SetString(PyExc_ValueError, "offset must be non-negative");
        return NULL;
    }

    uint8_t status;
    size_t doc_off, doc_len;
    bson_error_t err;
    bson_status_t read_status = bson_storage_read(self->file, (size_t)offset, &status, &doc_off, &doc_len, &err);
    if (read_status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    if (status != BSON_RECORD_LIVE) {
        Py_RETURN_NONE;
    }
    const uint8_t *base = bson_mmap_data(self->file);
    return PyBytes_FromStringAndSize((const char *)(base + doc_off), (Py_ssize_t)doc_len);
}

static PyObject *CollectionHandle_tombstone_record(CollectionHandleObject *self, PyObject *args) {
    Py_ssize_t offset;
    if (!PyArg_ParseTuple(args, "n", &offset)) return NULL;
    CHECK_OPEN(self);
    if (offset < 0) {
        PyErr_SetString(PyExc_ValueError, "offset must be non-negative");
        return NULL;
    }

    bson_error_t err;
    bson_status_t status = bson_storage_tombstone(self->file, (size_t)offset, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* next_live_offset(prev): prev == -1 means "start from the beginning";
 * otherwise prev must be an offset previously returned by this same
 * function (or by append_record), and the next LIVE record's offset
 * strictly after it is returned, or None at end of data. Single-step
 * (not a held cursor/scanner object) so a Python-level Cursor never
 * holds a raw offset->pointer mapping across steps -- each call
 * re-derives everything from the current bson_mmap_data() fresh. */
static PyObject *CollectionHandle_next_live_offset(CollectionHandleObject *self, PyObject *args) {
    Py_ssize_t prev;
    if (!PyArg_ParseTuple(args, "n", &prev)) return NULL;
    CHECK_OPEN(self);
    if (prev < -1) {
        PyErr_SetString(PyExc_ValueError, "prev must be -1 or a non-negative offset");
        return NULL;
    }

    bson_error_t err;
    const uint8_t *base = bson_mmap_data(self->file);
    size_t data_end = bson_storage_data_end(self->file);

    bson_scanner_t scanner;
    bson_scanner_open(&scanner, base, data_end, &err);

    if (prev >= 0) {
        uint8_t status;
        size_t doc_off, doc_len;
        bson_status_t read_status = bson_storage_read(self->file, (size_t)prev, &status, &doc_off, &doc_len, &err);
        if (read_status != BSON_OK) {
            storage_error_to_python(&err);
            return NULL;
        }
        scanner.pos = doc_off + doc_len;
    }

    size_t record_off, doc_off, doc_len;
    bool found = bson_scanner_next(&scanner, &record_off, &doc_off, &doc_len, &err);
    (void)doc_off;
    (void)doc_len;
    if (!found) {
        if (err.code != BSON_OK) {
            storage_error_to_python(&err);
            return NULL;
        }
        Py_RETURN_NONE;
    }
    return PyLong_FromSize_t(record_off);
}

static PyObject *CollectionHandle_live_count(CollectionHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    CHECK_OPEN(self);
    return PyLong_FromUnsignedLongLong(bson_storage_live_count(self->file));
}

static PyObject *CollectionHandle_data_end(CollectionHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    CHECK_OPEN(self);
    return PyLong_FromSize_t(bson_storage_data_end(self->file));
}

static PyMethodDef CollectionHandle_methods[] = {
    {"close", (PyCFunction)CollectionHandle_close, METH_NOARGS, "Flush and close the underlying file."},
    {"flush", (PyCFunction)CollectionHandle_flush, METH_NOARGS, "msync the mapped region to disk."},
    {"append_record", (PyCFunction)CollectionHandle_append_record, METH_VARARGS,
     "append_record(doc: bytes) -> int (record offset)"},
    {"read_record", (PyCFunction)CollectionHandle_read_record, METH_VARARGS,
     "read_record(offset: int) -> Optional[bytes]"},
    {"tombstone_record", (PyCFunction)CollectionHandle_tombstone_record, METH_VARARGS,
     "tombstone_record(offset: int) -> None"},
    {"next_live_offset", (PyCFunction)CollectionHandle_next_live_offset, METH_VARARGS,
     "next_live_offset(prev: int) -> Optional[int]"},
    {"live_count", (PyCFunction)CollectionHandle_live_count, METH_NOARGS, "live_count() -> int"},
    {"data_end", (PyCFunction)CollectionHandle_data_end, METH_NOARGS, "data_end() -> int"},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject CollectionHandleType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "custom_bson._storage_core.CollectionHandle",
    .tp_basicsize = sizeof(CollectionHandleObject),
    .tp_dealloc = (destructor)CollectionHandle_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Handle to one open collection data file. Create via open_collection().",
    .tp_methods = CollectionHandle_methods,
    .tp_new = CollectionHandle_new,
};

/* ======================================================================
 * IndexHandle type
 * ==================================================================== */

typedef struct {
    PyObject_HEAD
    bson_btree_t *tree; /* NULL once closed */
} IndexHandleObject;

#define CHECK_TREE_OPEN(self)                                                     \
    do {                                                                          \
        if (!(self)->tree) {                                                      \
            PyErr_SetString(PyExc_ValueError, "operation on a closed IndexHandle"); \
            return NULL;                                                           \
        }                                                                          \
    } while (0)

static PyObject *IndexHandle_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    (void)type;
    (void)args;
    (void)kwds;
    PyErr_SetString(PyExc_TypeError,
                     "IndexHandle cannot be instantiated directly; use create_index_file()/open_index_file()");
    return NULL;
}

static void IndexHandle_dealloc(IndexHandleObject *self) {
    if (self->tree) {
        bson_error_t err;
        bson_btree_close(self->tree, &err);
        self->tree = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *IndexHandle_close(IndexHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    if (self->tree) {
        bson_error_t err;
        bson_status_t status = bson_btree_close(self->tree, &err);
        self->tree = NULL;
        if (status != BSON_OK) {
            storage_error_to_python(&err);
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *IndexHandle_flush(IndexHandleObject *self, PyObject *Py_UNUSED(ignored)) {
    CHECK_TREE_OPEN(self);
    bson_error_t err;
    bson_status_t status = bson_btree_flush(self->tree, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *IndexHandle_insert(IndexHandleObject *self, PyObject *args) {
    Py_buffer key_view;
    unsigned long long offset;
    if (!PyArg_ParseTuple(args, "y*K", &key_view, &offset)) return NULL;
    if (!self->tree) {
        PyBuffer_Release(&key_view);
        PyErr_SetString(PyExc_ValueError, "operation on a closed IndexHandle");
        return NULL;
    }
    if (key_view.len != BSON_BTREE_KEY_SIZE) {
        PyBuffer_Release(&key_view);
        PyErr_Format(PyExc_ValueError, "index key must be exactly %d bytes", BSON_BTREE_KEY_SIZE);
        return NULL;
    }
    bson_error_t err;
    bson_status_t status = bson_btree_insert(self->tree, (const uint8_t *)key_view.buf, (uint64_t)offset, &err);
    PyBuffer_Release(&key_view);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *offset_list_to_pylist(const bson_btree_offset_list_t *out) {
    PyObject *list = PyList_New((Py_ssize_t)out->len);
    if (!list) return NULL;
    for (size_t i = 0; i < out->len; i++) {
        PyObject *offset_value = PyLong_FromUnsignedLongLong(out->data[i]);
        if (!offset_value) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)i, offset_value); /* steals reference */
    }
    return list;
}

static PyObject *IndexHandle_lookup(IndexHandleObject *self, PyObject *args) {
    Py_buffer key_view;
    if (!PyArg_ParseTuple(args, "y*", &key_view)) return NULL;
    if (!self->tree) {
        PyBuffer_Release(&key_view);
        PyErr_SetString(PyExc_ValueError, "operation on a closed IndexHandle");
        return NULL;
    }
    if (key_view.len != BSON_BTREE_KEY_SIZE) {
        PyBuffer_Release(&key_view);
        PyErr_Format(PyExc_ValueError, "index key must be exactly %d bytes", BSON_BTREE_KEY_SIZE);
        return NULL;
    }

    bson_btree_offset_list_t out;
    bson_btree_offset_list_init(&out);
    bson_error_t err;
    bson_status_t status = bson_btree_lookup(self->tree, (const uint8_t *)key_view.buf, &out, &err);
    PyBuffer_Release(&key_view);
    if (status != BSON_OK) {
        bson_btree_offset_list_free(&out);
        storage_error_to_python(&err);
        return NULL;
    }
    PyObject *list = offset_list_to_pylist(&out);
    bson_btree_offset_list_free(&out);
    return list;
}

static PyObject *IndexHandle_range(IndexHandleObject *self, PyObject *args) {
    PyObject *low_obj;
    PyObject *high_obj;
    int low_incl;
    int high_incl;
    if (!PyArg_ParseTuple(args, "OpOp", &low_obj, &low_incl, &high_obj, &high_incl)) return NULL;
    CHECK_TREE_OPEN(self);

    Py_buffer low_view, high_view;
    bool have_low = false, have_high = false;
    const uint8_t *low_ptr = NULL;
    const uint8_t *high_ptr = NULL;

    if (low_obj != Py_None) {
        if (PyObject_GetBuffer(low_obj, &low_view, PyBUF_SIMPLE) != 0) return NULL;
        if (low_view.len != BSON_BTREE_KEY_SIZE) {
            PyBuffer_Release(&low_view);
            PyErr_Format(PyExc_ValueError, "index key must be exactly %d bytes", BSON_BTREE_KEY_SIZE);
            return NULL;
        }
        low_ptr = (const uint8_t *)low_view.buf;
        have_low = true;
    }
    if (high_obj != Py_None) {
        if (PyObject_GetBuffer(high_obj, &high_view, PyBUF_SIMPLE) != 0) {
            if (have_low) PyBuffer_Release(&low_view);
            return NULL;
        }
        if (high_view.len != BSON_BTREE_KEY_SIZE) {
            if (have_low) PyBuffer_Release(&low_view);
            PyBuffer_Release(&high_view);
            PyErr_Format(PyExc_ValueError, "index key must be exactly %d bytes", BSON_BTREE_KEY_SIZE);
            return NULL;
        }
        high_ptr = (const uint8_t *)high_view.buf;
        have_high = true;
    }

    bson_btree_offset_list_t out;
    bson_btree_offset_list_init(&out);
    bson_error_t err;
    bson_status_t status =
        bson_btree_range(self->tree, low_ptr, low_incl != 0, high_ptr, high_incl != 0, &out, &err);

    if (have_low) PyBuffer_Release(&low_view);
    if (have_high) PyBuffer_Release(&high_view);

    if (status != BSON_OK) {
        bson_btree_offset_list_free(&out);
        storage_error_to_python(&err);
        return NULL;
    }
    PyObject *list = offset_list_to_pylist(&out);
    bson_btree_offset_list_free(&out);
    return list;
}

static PyObject *IndexHandle_delete_entry(IndexHandleObject *self, PyObject *args) {
    Py_buffer key_view;
    unsigned long long offset;
    if (!PyArg_ParseTuple(args, "y*K", &key_view, &offset)) return NULL;
    if (!self->tree) {
        PyBuffer_Release(&key_view);
        PyErr_SetString(PyExc_ValueError, "operation on a closed IndexHandle");
        return NULL;
    }
    if (key_view.len != BSON_BTREE_KEY_SIZE) {
        PyBuffer_Release(&key_view);
        PyErr_Format(PyExc_ValueError, "index key must be exactly %d bytes", BSON_BTREE_KEY_SIZE);
        return NULL;
    }
    bson_error_t err;
    bson_status_t status = bson_btree_delete(self->tree, (const uint8_t *)key_view.buf, (uint64_t)offset, &err);
    PyBuffer_Release(&key_view);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *IndexHandle_get_field_path(IndexHandleObject *self, void *closure) {
    (void)closure;
    CHECK_TREE_OPEN(self);
    return PyUnicode_FromString(bson_btree_field_path(self->tree));
}

static PyObject *IndexHandle_get_unique(IndexHandleObject *self, void *closure) {
    (void)closure;
    CHECK_TREE_OPEN(self);
    return PyBool_FromLong(bson_btree_unique(self->tree));
}

static PyObject *IndexHandle_get_descending(IndexHandleObject *self, void *closure) {
    (void)closure;
    CHECK_TREE_OPEN(self);
    return PyBool_FromLong(bson_btree_descending(self->tree));
}

static PyGetSetDef IndexHandle_getset[] = {
    {"field_path", (getter)IndexHandle_get_field_path, NULL, "dotted field path this index covers", NULL},
    {"unique", (getter)IndexHandle_get_unique, NULL, "whether this is a unique index", NULL},
    {"descending", (getter)IndexHandle_get_descending, NULL, "whether this index is descending-ordered", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static PyMethodDef IndexHandle_methods[] = {
    {"close", (PyCFunction)IndexHandle_close, METH_NOARGS, "Flush and close the underlying index file."},
    {"flush", (PyCFunction)IndexHandle_flush, METH_NOARGS, "msync the mapped region to disk."},
    {"insert", (PyCFunction)IndexHandle_insert, METH_VARARGS,
     "insert(key: bytes[13], record_offset: int) -> None; raises DuplicateKeyError for a unique index"},
    {"lookup", (PyCFunction)IndexHandle_lookup, METH_VARARGS, "lookup(key: bytes[13]) -> List[int]"},
    {"range", (PyCFunction)IndexHandle_range, METH_VARARGS,
     "range(low: Optional[bytes[13]], low_inclusive: bool, high: Optional[bytes[13]], "
     "high_inclusive: bool) -> List[int]"},
    {"delete_entry", (PyCFunction)IndexHandle_delete_entry, METH_VARARGS,
     "delete_entry(key: bytes[13], record_offset: int) -> None"},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject IndexHandleType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "custom_bson._storage_core.IndexHandle",
    .tp_basicsize = sizeof(IndexHandleObject),
    .tp_dealloc = (destructor)IndexHandle_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Handle to one open B-Tree index file. Create via create_index_file()/open_index_file().",
    .tp_methods = IndexHandle_methods,
    .tp_getset = IndexHandle_getset,
    .tp_new = IndexHandle_new,
};

/* ======================================================================
 * Module-level functions
 * ==================================================================== */

static PyObject *py_open_collection(PyObject *self, PyObject *args) {
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path)) return NULL;

    bson_mmap_file_t *file = NULL;
    bson_error_t err;
    bson_status_t status = bson_mmap_open(path, BSON_MMAP_READ_WRITE, 0, &file, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }

    status = bson_storage_open_header(file, BSON_MMAP_READ_WRITE, &err);
    if (status != BSON_OK) {
        bson_error_t close_err;
        bson_mmap_close(file, &close_err);
        storage_error_to_python(&err);
        return NULL;
    }

    CollectionHandleObject *obj = PyObject_New(CollectionHandleObject, &CollectionHandleType);
    if (!obj) {
        bson_error_t close_err;
        bson_mmap_close(file, &close_err);
        return NULL;
    }
    obj->file = file;
    return (PyObject *)obj;
}

static PyObject *py_create_index_file(PyObject *self, PyObject *args) {
    (void)self;
    const char *path;
    const char *field_path;
    unsigned int key_type_tag;
    int unique;
    int descending;
    if (!PyArg_ParseTuple(args, "ssIpp", &path, &field_path, &key_type_tag, &unique, &descending)) {
        return NULL;
    }

    bson_btree_t *tree = NULL;
    bson_error_t err;
    bson_status_t status =
        bson_btree_create(path, field_path, (uint8_t)key_type_tag, unique != 0, descending != 0, &tree, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }

    IndexHandleObject *obj = PyObject_New(IndexHandleObject, &IndexHandleType);
    if (!obj) {
        bson_error_t close_err;
        bson_btree_close(tree, &close_err);
        return NULL;
    }
    obj->tree = tree;
    return (PyObject *)obj;
}

static PyObject *py_open_index_file(PyObject *self, PyObject *args) {
    (void)self;
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path)) return NULL;

    bson_btree_t *tree = NULL;
    bson_error_t err;
    bson_status_t status = bson_btree_open(path, &tree, &err);
    if (status != BSON_OK) {
        storage_error_to_python(&err);
        return NULL;
    }

    IndexHandleObject *obj = PyObject_New(IndexHandleObject, &IndexHandleType);
    if (!obj) {
        bson_error_t close_err;
        bson_btree_close(tree, &close_err);
        return NULL;
    }
    obj->tree = tree;
    return (PyObject *)obj;
}

static PyMethodDef storage_core_methods[] = {
    {"open_collection", py_open_collection, METH_VARARGS, "open_collection(path: str) -> CollectionHandle"},
    {"create_index_file", py_create_index_file, METH_VARARGS,
     "create_index_file(path: str, field_path: str, key_type_tag: int, unique: bool, "
     "descending: bool) -> IndexHandle"},
    {"open_index_file", py_open_index_file, METH_VARARGS, "open_index_file(path: str) -> IndexHandle"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef storage_core_module = {
    PyModuleDef_HEAD_INIT,
    "_storage_core",
    "Native collection data file storage engine for custom_bson.",
    -1,
    storage_core_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC PyInit__storage_core(void) {
    if (PyType_Ready(&CollectionHandleType) < 0) return NULL;
    if (PyType_Ready(&IndexHandleType) < 0) return NULL;

    PyObject *m = PyModule_Create(&storage_core_module);
    if (!m) return NULL;

    PyObject *exc_mod = PyImport_ImportModule("bsondb.exceptions");
    if (!exc_mod) {
        Py_DECREF(m);
        return NULL;
    }
    g_InvalidBSON = PyObject_GetAttrString(exc_mod, "InvalidBSON");
    g_DuplicateKeyError = PyObject_GetAttrString(exc_mod, "DuplicateKeyError");
    Py_DECREF(exc_mod);
    if (!g_InvalidBSON || !g_DuplicateKeyError) {
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&CollectionHandleType);
    if (PyModule_AddObject(m, "CollectionHandle", (PyObject *)&CollectionHandleType) < 0) {
        Py_DECREF(&CollectionHandleType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&IndexHandleType);
    if (PyModule_AddObject(m, "IndexHandle", (PyObject *)&IndexHandleType) < 0) {
        Py_DECREF(&IndexHandleType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
