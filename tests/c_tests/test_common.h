/*
 * test_common.h -- helpers shared by the C smoke tests.
 *
 * The tests need a writable scratch file path outside the source tree.
 * The OS temp directory lives in a different place -- and is discovered
 * a different way -- on Windows (GetTempPathA) vs POSIX (TMPDIR env var,
 * falling back to /tmp).
 */
#ifndef CUSTOM_BSON_TEST_COMMON_H
#define CUSTOM_BSON_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Writes an absolute path "<tmpdir>/<name>" into buf (size bufsize). */
static void test_tmp_path(char *buf, size_t bufsize, const char *name) {
#ifdef _WIN32
    char dir[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0 || n >= sizeof(dir)) {
        strncpy(dir, "C:\\Windows\\Temp\\", sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
    }
    snprintf(buf, bufsize, "%s%s", dir, name);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || dir[0] == '\0') dir = "/tmp";
    snprintf(buf, bufsize, "%s/%s", dir, name);
#endif
}

#endif /* CUSTOM_BSON_TEST_COMMON_H */
