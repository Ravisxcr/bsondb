/*
 * platform_mmap.h -- cross-platform memory-mapped file abstraction.
 *
 * SLICE STATUS: this header's interface is final/real. The
 * implementation in src/c_engine/platform_mmap.c is a STUB this slice
 * (every function returns BSON_ERR_NOT_IMPLEMENTED) -- it exists so
 * the storage-engine slice can implement against a stable API without
 * a redesign, and so scanner.c/tests/c_tests/test_mmap.c have a real
 * header to compile against today.
 *
 * Design: a single opaque bson_mmap_file_t abstracts POSIX mmap(2)/
 * munmap(2) and Win32 CreateFileMappingW/MapViewOfFile/UnmapViewOfFile
 * behind one API. Callers never see platform-specific handles.
 */
#ifndef CUSTOM_BSON_PLATFORM_MMAP_H
#define CUSTOM_BSON_PLATFORM_MMAP_H

#include <stddef.h>
#include <stdint.h>

#include "custom_bson/bson_engine.h" /* bson_status_t, bson_error_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSON_MMAP_READ_ONLY = 0,
    BSON_MMAP_READ_WRITE = 1,
} bson_mmap_mode_t;

/* Opaque handle. Layout is platform-specific (POSIX: fd + mmap base;
 * Win32: HANDLE file + HANDLE mapping + LPVOID view) and defined only
 * in platform_mmap.c, behind #ifdef _WIN32 / #else branches, so no
 * platform-specific type ever appears in this public header. */
typedef struct bson_mmap_file bson_mmap_file_t;

/* Opens `path`, maps the entire file into memory in the given mode,
 * and populates *out_file. On BSON_MMAP_READ_ONLY, `path` must exist.
 * On BSON_MMAP_READ_WRITE, `path` is created if it doesn't exist and
 * grown to `initial_size` if smaller (0 = don't grow / require
 * existing size). Returns a bson_mmap_file_t the caller must later
 * pass to bson_mmap_close(). */
bson_status_t bson_mmap_open(const char *path, bson_mmap_mode_t mode, size_t initial_size,
                              bson_mmap_file_t **out_file, bson_error_t *err);

/* Unmaps and closes the file. Safe to call with *file == NULL. */
bson_status_t bson_mmap_close(bson_mmap_file_t *file, bson_error_t *err);

/* Read-only zero-copy view of the mapped region. Valid until
 * bson_mmap_close() or a resize. */
const uint8_t *bson_mmap_data(const bson_mmap_file_t *file);
size_t          bson_mmap_size(const bson_mmap_file_t *file);

/* Grows the backing file and remaps it (BSON_MMAP_READ_WRITE only).
 * Invalidates any pointer previously returned by bson_mmap_data(). */
bson_status_t bson_mmap_resize(bson_mmap_file_t *file, size_t new_size, bson_error_t *err);

/* Flushes dirty pages to disk (msync(MS_SYNC) on POSIX, FlushViewOfFile
 * + FlushFileBuffers on Win32). BSON_MMAP_READ_WRITE only. */
bson_status_t bson_mmap_flush(bson_mmap_file_t *file, bson_error_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BSON_PLATFORM_MMAP_H */
