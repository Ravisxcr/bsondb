/*
 * platform_mmap.c -- cross-platform memory-mapped file implementation.
 *
 * POSIX (Linux/macOS) backend is fully implemented via open(2)/fstat(2)/
 * ftruncate(2)/mmap(2)/munmap(2)/msync(2). The Windows backend remains a
 * documented stub behind #ifdef _WIN32 -- platform_mmap.h's interface is
 * final, so a future slice can fill in CreateFileW/CreateFileMappingW/
 * MapViewOfFile without any API change here.
 *
 * _POSIX_C_SOURCE must be defined before any system header is included
 * so glibc exposes mmap/ftruncate/msync/strdup under -std=c11 (which
 * otherwise restricts to strict ISO C and hides POSIX extensions).
 */
#define _POSIX_C_SOURCE 200809L

#include "custom_bson/platform_mmap.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

/* ---- Windows backend: not implemented in this slice. ------------------ */

struct bson_mmap_file {
    int _unused;
};

bson_status_t bson_mmap_open(const char *path, bson_mmap_mode_t mode, size_t initial_size,
                              bson_mmap_file_t **out_file, bson_error_t *err) {
    (void)path;
    (void)mode;
    (void)initial_size;
    if (out_file) *out_file = NULL;
    bson_error_set(err, BSON_ERR_NOT_IMPLEMENTED, "Windows mmap backend is not implemented yet");
    return BSON_ERR_NOT_IMPLEMENTED;
}

bson_status_t bson_mmap_close(bson_mmap_file_t *file, bson_error_t *err) {
    if (file) free(file);
    bson_error_clear(err);
    return BSON_OK;
}

const uint8_t *bson_mmap_data(const bson_mmap_file_t *file) {
    (void)file;
    return NULL;
}

size_t bson_mmap_size(const bson_mmap_file_t *file) {
    (void)file;
    return 0;
}

bson_status_t bson_mmap_resize(bson_mmap_file_t *file, size_t new_size, bson_error_t *err) {
    (void)file;
    (void)new_size;
    bson_error_set(err, BSON_ERR_NOT_IMPLEMENTED, "Windows mmap backend is not implemented yet");
    return BSON_ERR_NOT_IMPLEMENTED;
}

bson_status_t bson_mmap_flush(bson_mmap_file_t *file, bson_error_t *err) {
    (void)file;
    bson_error_set(err, BSON_ERR_NOT_IMPLEMENTED, "Windows mmap backend is not implemented yet");
    return BSON_ERR_NOT_IMPLEMENTED;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct bson_mmap_file {
    int fd;
    bson_mmap_mode_t mode;
    uint8_t *base; /* NULL iff size == 0; mapping zero bytes is undefined behavior */
    size_t size;    /* current mapped length == current file capacity (not logical data length) */
    char *path;      /* owned copy, used only for error messages */
};

/* (Re)maps [0, size) of file->fd. Caller must have already unmapped any
 * prior mapping. size == 0 leaves the file unmapped (base = NULL) rather
 * than calling mmap(), since mapping a zero-length region is UB. */
static bson_status_t map_current_fd(bson_mmap_file_t *file, size_t size, bson_error_t *err) {
    if (size == 0) {
        file->base = NULL;
        file->size = 0;
        return BSON_OK;
    }
    int prot = PROT_READ;
    if (file->mode == BSON_MMAP_READ_WRITE) prot |= PROT_WRITE;
    void *addr = mmap(NULL, size, prot, MAP_SHARED, file->fd, 0);
    if (addr == MAP_FAILED) {
        bson_error_set(err, BSON_ERR_IO, "mmap failed for '%s': %s", file->path, strerror(errno));
        return BSON_ERR_IO;
    }
    file->base = (uint8_t *)addr;
    file->size = size;
    return BSON_OK;
}

bson_status_t bson_mmap_open(const char *path, bson_mmap_mode_t mode, size_t initial_size,
                              bson_mmap_file_t **out_file, bson_error_t *err) {
    if (out_file) *out_file = NULL;

    int flags = (mode == BSON_MMAP_READ_WRITE) ? (O_RDWR | O_CREAT) : O_RDONLY;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        bson_error_set(err, BSON_ERR_IO, "failed to open '%s': %s", path, strerror(errno));
        return BSON_ERR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        bson_error_set(err, BSON_ERR_IO, "fstat failed for '%s': %s", path, strerror(errno));
        close(fd);
        return BSON_ERR_IO;
    }

    size_t file_size = (size_t)st.st_size;
    if (mode == BSON_MMAP_READ_WRITE && initial_size > file_size) {
        if (ftruncate(fd, (off_t)initial_size) != 0) {
            bson_error_set(err, BSON_ERR_IO, "ftruncate failed for '%s': %s", path, strerror(errno));
            close(fd);
            return BSON_ERR_IO;
        }
        file_size = initial_size;
    }

    bson_mmap_file_t *file = (bson_mmap_file_t *)malloc(sizeof(bson_mmap_file_t));
    if (!file) {
        close(fd);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate mmap file handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }
    file->fd = fd;
    file->mode = mode;
    file->base = NULL;
    file->size = 0;
    file->path = strdup(path);
    if (!file->path) {
        close(fd);
        free(file);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate mmap file handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }

    bson_status_t map_st = map_current_fd(file, file_size, err);
    if (map_st != BSON_OK) {
        close(fd);
        free(file->path);
        free(file);
        return map_st;
    }

    *out_file = file;
    return BSON_OK;
}

bson_status_t bson_mmap_close(bson_mmap_file_t *file, bson_error_t *err) {
    bson_error_clear(err);
    if (!file) return BSON_OK;
    if (file->base) munmap(file->base, file->size);
    close(file->fd);
    free(file->path);
    free(file);
    return BSON_OK;
}

const uint8_t *bson_mmap_data(const bson_mmap_file_t *file) {
    return file ? file->base : NULL;
}

size_t bson_mmap_size(const bson_mmap_file_t *file) {
    return file ? file->size : 0;
}

bson_status_t bson_mmap_resize(bson_mmap_file_t *file, size_t new_size, bson_error_t *err) {
    if (!file) {
        bson_error_set(err, BSON_ERR_IO, "resize called on a NULL mmap handle");
        return BSON_ERR_IO;
    }
    if (file->mode != BSON_MMAP_READ_WRITE) {
        bson_error_set(err, BSON_ERR_READ_ONLY_VIOLATION, "cannot resize a read-only mapping");
        return BSON_ERR_READ_ONLY_VIOLATION;
    }
    if (new_size == file->size) {
        bson_error_clear(err);
        return BSON_OK;
    }
    if (ftruncate(file->fd, (off_t)new_size) != 0) {
        bson_error_set(err, BSON_ERR_IO, "ftruncate failed for '%s': %s", file->path, strerror(errno));
        return BSON_ERR_IO;
    }
    if (file->base) {
        munmap(file->base, file->size);
        file->base = NULL;
        file->size = 0;
    }
    return map_current_fd(file, new_size, err);
}

bson_status_t bson_mmap_flush(bson_mmap_file_t *file, bson_error_t *err) {
    if (!file) {
        bson_error_set(err, BSON_ERR_IO, "flush called on a NULL mmap handle");
        return BSON_ERR_IO;
    }
    if (file->mode != BSON_MMAP_READ_WRITE) {
        bson_error_set(err, BSON_ERR_READ_ONLY_VIOLATION, "cannot flush a read-only mapping");
        return BSON_ERR_READ_ONLY_VIOLATION;
    }
    if (!file->base) {
        bson_error_clear(err);
        return BSON_OK;
    }
    if (msync(file->base, file->size, MS_SYNC) != 0) {
        bson_error_set(err, BSON_ERR_IO, "msync failed for '%s': %s", file->path, strerror(errno));
        return BSON_ERR_IO;
    }
    bson_error_clear(err);
    return BSON_OK;
}

#endif
