// wasi_vfs.c
// A VFS layer that serves files from our packed prefix.
// Files whose (sanitized) path begins with PACKFS_BUILTIN_PREFIX
// are served from in‑memory buffers. All other calls are forwarded.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "perlpack.h" // our generated header which defines PACKFS_BUILTIN_PREFIX and arrays

// Configuration constants.
#define PACKFS_FILEFD_MIN 1000000000
#define PACKFS_FILEFD_MAX 1000001000
#define PACKFS_FILEPTR_ARRAY_SZ (PACKFS_FILEFD_MAX - PACKFS_FILEFD_MIN)
#define PACKFS_FILEPATH_MAX_LEN 256

// Arrays to track “open” virtual file streams.
static int packfs_filefd[PACKFS_FILEPTR_ARRAY_SZ] = {0};
static FILE *packfs_fileptr[PACKFS_FILEPTR_ARRAY_SZ] = {0};
static size_t packfs_filesize[PACKFS_FILEPTR_ARRAY_SZ] = {0};

// Simple sanitizer: remove duplicate slashes.
static void packfs_sanitize_path(char *dst, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < PACKFS_FILEPATH_MAX_LEN - 1; i++)
    {
        if (i > 0 && src[i] == '/' && src[i - 1] == '/')
            continue;
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

// Try to open a file from our packed VFS.
// Returns a FILE* if the sanitized path begins with PACKFS_BUILTIN_PREFIX
// and the file is found; otherwise returns NULL.
static FILE *packfs_open_internal(const char *path)
{
    char sanitized[PACKFS_FILEPATH_MAX_LEN];
    packfs_sanitize_path(sanitized, path);

    size_t prefix_len = strlen(PACKFS_BUILTIN_PREFIX);
    if (strncmp(sanitized, PACKFS_BUILTIN_PREFIX, prefix_len) != 0)
    {
        return NULL;
    }
    // Search for the file in our packed arrays.
    for (size_t i = 0; i < packfs_builtin_files_num; i++)
    {
        if (strcmp(sanitized, packfs_builtin_abspaths[i]) == 0)
        {
            size_t filesize = (size_t)(packfs_builtin_ends[i] - packfs_builtin_starts[i]);
            FILE *f = fmemopen((void *)packfs_builtin_starts[i], filesize, "r");
            return f;
        }
    }
    return NULL;
}

/* ======================================================================
   Wrapped functions.
   These will be used in place of the standard library functions via
   linker wrapping (for example, with -Wl,--wrap=fopen, etc.)
   ====================================================================== */

// Wrap fopen.
FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *f = packfs_open_internal(path);
    if (f)
        return f;
    return __real_fopen(path, mode);
}

// Wrap open. (Note: open is a varargs function.)
int __wrap_open(const char *path, int flags, ...)
{
    FILE *f = packfs_open_internal(path);
    if (f)
    {
        // Register this virtual file.
        for (size_t i = 0; i < PACKFS_FILEPTR_ARRAY_SZ; i++)
        {
            if (packfs_filefd[i] == 0)
            {
                packfs_filefd[i] = PACKFS_FILEFD_MIN + i;
                packfs_fileptr[i] = f;
                fseek(f, 0, SEEK_END);
                packfs_filesize[i] = ftell(f);
                fseek(f, 0, SEEK_SET);
                return packfs_filefd[i];
            }
        }
    }
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    int ret = __real_open(path, flags, mode);
    va_end(args);
    return ret;
}

// Wrap close.
int __wrap_close(int fd)
{
    if (fd >= PACKFS_FILEFD_MIN && fd < PACKFS_FILEFD_MAX)
    {
        for (size_t i = 0; i < PACKFS_FILEPTR_ARRAY_SZ; i++)
        {
            if (packfs_filefd[i] == fd)
            {
                packfs_filefd[i] = 0;
                int res = fclose(packfs_fileptr[i]);
                packfs_fileptr[i] = NULL;
                return res;
            }
        }
    }
    return __real_close(fd);
}

// Wrap read.
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    if (fd >= PACKFS_FILEFD_MIN && fd < PACKFS_FILEFD_MAX)
    {
        for (size_t i = 0; i < PACKFS_FILEPTR_ARRAY_SZ; i++)
        {
            if (packfs_filefd[i] == fd)
            {
                return fread(buf, 1, count, packfs_fileptr[i]);
            }
        }
    }
    return __real_read(fd, buf, count);
}

// Wrap lseek.
off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    if (fd >= PACKFS_FILEFD_MIN && fd < PACKFS_FILEFD_MAX)
    {
        for (size_t i = 0; i < PACKFS_FILEPTR_ARRAY_SZ; i++)
        {
            if (packfs_filefd[i] == fd)
            {
                if (fseek(packfs_fileptr[i], offset, whence) == 0)
                    return ftell(packfs_fileptr[i]);
                else
                    return -1;
            }
        }
    }
    return __real_lseek(fd, offset, whence);
}

// Wrap stat.
int __wrap_stat(const char *path, struct stat *buf)
{
    char sanitized[PACKFS_FILEPATH_MAX_LEN];
    packfs_sanitize_path(sanitized, path);
    if (strncmp(sanitized, PACKFS_BUILTIN_PREFIX, strlen(PACKFS_BUILTIN_PREFIX)) == 0)
    {
        for (size_t i = 0; i < packfs_builtin_files_num; i++)
        {
            if (strcmp(sanitized, packfs_builtin_abspaths[i]) == 0)
            {
                memset(buf, 0, sizeof(struct stat));
                buf->st_size = (off_t)(packfs_builtin_ends[i] - packfs_builtin_starts[i]);
                buf->st_mode = S_IFREG | 0444;
                return 0;
            }
        }
        errno = ENOENT;
        return -1;
    }
    return __real_stat(path, buf);
}

// Wrap fstat.
int __wrap_fstat(int fd, struct stat *buf)
{
    if (fd >= PACKFS_FILEFD_MIN && fd < PACKFS_FILEFD_MAX)
    {
        for (size_t i = 0; i < PACKFS_FILEPTR_ARRAY_SZ; i++)
        {
            if (packfs_filefd[i] == fd)
            {
                memset(buf, 0, sizeof(struct stat));
                buf->st_size = packfs_filesize[i];
                buf->st_mode = S_IFREG | 0444;
                return 0;
            }
        }
        errno = EBADF;
        return -1;
    }
    return __real_fstat(fd, buf);
}
