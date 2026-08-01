/*
 * platform_mmap.c -- cross-platform memory-mapped file implementation.
 *
 * POSIX (Linux/macOS) backend is implemented via open(2)/fstat(2)/
 * ftruncate(2)/mmap(2)/munmap(2)/msync(2). The Windows backend mirrors
 * the same semantics via CreateFileW/CreateFileMappingW/MapViewOfFile/
 * SetEndOfFile/FlushViewOfFile+FlushFileBuffers.
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

/* ---- Windows backend. --------------------------------------------------
 *
 * A mapping of size 0 is left unmapped (base = NULL, hMapping = NULL)
 * rather than calling CreateFileMapping(), mirroring the POSIX backend's
 * treatment of a zero-length mmap() as undefined behavior. FILE_SHARE_DELETE
 * is included on every open so a file can still be os.replace()'d/removed
 * out from under an open handle, matching POSIX unlink-while-open semantics
 * (see collection.py's compact(), which relies on exactly this).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct bson_mmap_file {
    HANDLE hFile;
    HANDLE hMapping; /* NULL iff size == 0 */
    bson_mmap_mode_t mode;
    uint8_t *base; /* NULL iff size == 0 */
    size_t size;    /* current mapped length == current file capacity (not logical data length) */
    char *path;      /* owned copy, used only for error messages */
};

static wchar_t *utf8_to_wide(const char *path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen) <= 0) {
        free(wpath);
        return NULL;
    }
    return wpath;
}

static void set_win32_error(bson_error_t *err, bson_status_t code, const char *context,
                             const char *path) {
    DWORD last = GetLastError();
    char msgbuf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msgbuf, sizeof(msgbuf), NULL);
    while (n > 0 && (msgbuf[n - 1] == '\n' || msgbuf[n - 1] == '\r')) msgbuf[--n] = '\0';
    if (n == 0) msgbuf[0] = '\0';
    bson_error_set(err, code, "%s for '%s': %s (error %lu)", context, path, msgbuf,
                    (unsigned long)last);
}

/* Unmaps/closes any current mapping without touching file->hFile. */
static void unmap_current_handle(bson_mmap_file_t *file) {
    if (file->base) {
        UnmapViewOfFile(file->base);
        file->base = NULL;
    }
    if (file->hMapping) {
        CloseHandle(file->hMapping);
        file->hMapping = NULL;
    }
    file->size = 0;
}

/* (Re)maps [0, size) of file->hFile. Caller must have already unmapped any
 * prior mapping. size == 0 leaves the file unmapped. */
static bson_status_t map_current_handle(bson_mmap_file_t *file, size_t size, bson_error_t *err) {
    if (size == 0) {
        file->base = NULL;
        file->hMapping = NULL;
        file->size = 0;
        return BSON_OK;
    }
    DWORD protect = (file->mode == BSON_MMAP_READ_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    DWORD sizeHigh = (DWORD)((unsigned __int64)size >> 32);
    DWORD sizeLow = (DWORD)((unsigned __int64)size & 0xFFFFFFFFu);
    HANDLE hMapping = CreateFileMappingW(file->hFile, NULL, protect, sizeHigh, sizeLow, NULL);
    if (hMapping == NULL) {
        set_win32_error(err, BSON_ERR_IO, "CreateFileMapping failed", file->path);
        return BSON_ERR_IO;
    }
    DWORD access = (file->mode == BSON_MMAP_READ_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    LPVOID addr = MapViewOfFile(hMapping, access, 0, 0, size);
    if (addr == NULL) {
        set_win32_error(err, BSON_ERR_IO, "MapViewOfFile failed", file->path);
        CloseHandle(hMapping);
        return BSON_ERR_IO;
    }
    file->hMapping = hMapping;
    file->base = (uint8_t *)addr;
    file->size = size;
    return BSON_OK;
}

/* Grows/shrinks the underlying file. Caller must have unmapped any current
 * view first -- Windows refuses to resize a file with a live mapping. */
static bson_status_t set_file_size(bson_mmap_file_t *file, size_t new_size, bson_error_t *err) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)new_size;
    if (!SetFilePointerEx(file->hFile, li, NULL, FILE_BEGIN)) {
        set_win32_error(err, BSON_ERR_IO, "SetFilePointerEx failed", file->path);
        return BSON_ERR_IO;
    }
    if (!SetEndOfFile(file->hFile)) {
        set_win32_error(err, BSON_ERR_IO, "SetEndOfFile failed", file->path);
        return BSON_ERR_IO;
    }
    return BSON_OK;
}

bson_status_t bson_mmap_open(const char *path, bson_mmap_mode_t mode, size_t initial_size,
                              bson_mmap_file_t **out_file, bson_error_t *err) {
    if (out_file) *out_file = NULL;

    wchar_t *wpath = utf8_to_wide(path);
    if (!wpath) {
        bson_error_set(err, BSON_ERR_IO, "failed to convert path '%s' to UTF-16", path);
        return BSON_ERR_IO;
    }

    DWORD desiredAccess = (mode == BSON_MMAP_READ_WRITE) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD creationDisposition = (mode == BSON_MMAP_READ_WRITE) ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    HANDLE hFile = CreateFileW(wpath, desiredAccess, shareMode, NULL, creationDisposition,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (hFile == INVALID_HANDLE_VALUE) {
        set_win32_error(err, BSON_ERR_IO, "failed to open", path);
        return BSON_ERR_IO;
    }

    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        set_win32_error(err, BSON_ERR_IO, "GetFileSizeEx failed", path);
        CloseHandle(hFile);
        return BSON_ERR_IO;
    }
    size_t file_size = (size_t)liSize.QuadPart;

    bson_mmap_file_t *file = (bson_mmap_file_t *)malloc(sizeof(bson_mmap_file_t));
    if (!file) {
        CloseHandle(hFile);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate mmap file handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }
    file->hFile = hFile;
    file->hMapping = NULL;
    file->mode = mode;
    file->base = NULL;
    file->size = 0;
    file->path = _strdup(path);
    if (!file->path) {
        CloseHandle(hFile);
        free(file);
        bson_error_set(err, BSON_ERR_OUT_OF_MEMORY, "failed to allocate mmap file handle");
        return BSON_ERR_OUT_OF_MEMORY;
    }

    if (mode == BSON_MMAP_READ_WRITE && initial_size > file_size) {
        bson_status_t st = set_file_size(file, initial_size, err);
        if (st != BSON_OK) {
            CloseHandle(hFile);
            free(file->path);
            free(file);
            return st;
        }
        file_size = initial_size;
    }

    bson_status_t map_st = map_current_handle(file, file_size, err);
    if (map_st != BSON_OK) {
        CloseHandle(hFile);
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
    unmap_current_handle(file);
    if (file->hFile != INVALID_HANDLE_VALUE) CloseHandle(file->hFile);
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
    unmap_current_handle(file);
    bson_status_t st = set_file_size(file, new_size, err);
    if (st != BSON_OK) return st;
    return map_current_handle(file, new_size, err);
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
    if (!FlushViewOfFile(file->base, file->size)) {
        set_win32_error(err, BSON_ERR_IO, "FlushViewOfFile failed", file->path);
        return BSON_ERR_IO;
    }
    if (!FlushFileBuffers(file->hFile)) {
        set_win32_error(err, BSON_ERR_IO, "FlushFileBuffers failed", file->path);
        return BSON_ERR_IO;
    }
    bson_error_clear(err);
    return BSON_OK;
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
