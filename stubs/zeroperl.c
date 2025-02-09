/**
 * @file miniperlmain.c
 * @brief A minimal Perl interpreter main program with a virtual filesystem hook.
 *
 * This file implements an embedded Perl interpreter (`miniperl` style)
 * that intercepts various I/O system calls (such as `open`, `read`, `stat`, etc.)
 * to optionally redirect them to an in-memory (virtual) filesystem. The virtual
 * filesystem is controlled by checking if the requested path begins with a
 * specific prefix defined by SFS_BUILTIN_PREFIX (from `zeropearl.h`). If so,
 * file access is redirected to the in-memory copies; otherwise, real system
 * calls are made.
 *
 * The code uses linker-based function wrapping on systems (particularly clang)
 * that support the -Wl,--wrap=foo option, and defines __wrap_* and __real_*
 * versions of each wrapped function.
 */

#define PERL_IN_MINIPERLMAIN_C
#define PERL_CORE 1

#include <stdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "zeroperl.h" /* Generated header: must define SFS_BUILTIN_PREFIX */

extern char **environ;

/* -------------------------------------------------------------------------
 * External declarations for the underlying ("real") functions.
 *
 * We expect these symbols to be resolved at link time (e.g., using clang's
 * linker wrapping). We do not provide the definitions here.
 * ------------------------------------------------------------------------- */
extern FILE *__real_fopen(const char *path, const char *mode);
extern int __real_fileno(FILE *stream);
extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_read(int fd, void *buf, size_t count);
extern off_t __real_lseek(int fd, off_t offset, int whence);
extern int __real_access(const char *path, int flags);
extern int __real_stat(const char *restrict path, struct stat *restrict statbuf);
extern int __real_fstat(int fd, struct stat *statbuf);

/* -------------------------------------------------------------------------
 * Configuration constants for the virtual filesystem.
 * ------------------------------------------------------------------------- */
#define sfs_filefd_min 1000000000
#define sfs_filefd_max 1000001000
#define sfs_filefd_array_sz (sfs_filefd_max - sfs_filefd_min)
#define sfs_filepath_max_len 128

/*
 * Global arrays to track open virtual file streams.
 *
 * - sfs_filefd[i]   : The "virtual" file descriptor number (>= sfs_filefd_min).
 * - sfs_fileptr[i]  : The FILE* pointer corresponding to the in-memory file.
 * - sfs_filesize[i] : The size of the in-memory file.
 *
 * Each index `i` in these arrays represents a slot in the virtual FS open file
 * table.
 */
int sfs_filefd[sfs_filefd_array_sz] = {0};
FILE *sfs_fileptr[sfs_filefd_array_sz] = {0};
size_t sfs_filesize[sfs_filefd_array_sz] = {0};

/* -------------------------------------------------------------------------
 * Helper routine: Remove consecutive duplicate slashes from src and write
 * into dst, ensuring we do not exceed sfs_filepath_max_len - 1.
 * ------------------------------------------------------------------------- */
/**
 * @brief Sanitize a path by removing consecutive '/' characters.
 *
 * @param dst The destination buffer (size >= sfs_filepath_max_len).
 * @param src The original path to sanitize.
 */
void sfs_sanitize_path(char *dst, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < sfs_filepath_max_len - 1; i++)
    {
        if (i > 0 && src[i] == '/' && src[i - 1] == '/')
        {
            /* Skip consecutive '/' */
            continue;
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* -------------------------------------------------------------------------
 * Virtual filesystem lookup functions (using the mapping array in
 * zeropearl_data.c). These are used by our wrappers.
 * ------------------------------------------------------------------------- */

/**
 * @brief Open a path from the in-memory (virtual) filesystem.
 *
 * If the path is found in the sfs_entries[] array, we open an fmemopen() handle
 * and store it in our global arrays. Returns a synthetic file descriptor if
 * successful.
 *
 * @param path The path to open.
 * @param out  Optional pointer to store the resulting FILE* (may be NULL).
 *
 * @return A virtual file descriptor on success, or -1 on failure.
 */
int sfs_open(const char *path, FILE **out)
{
    char path_sanitized[sfs_filepath_max_len];
    sfs_sanitize_path(path_sanitized, path);

    FILE *fileptr = NULL;
    size_t filesize = 0;

    /* Search the built-in VFS for the given path. */
    if (sfs_builtin_files_num > 0)
    {
        for (size_t i = 0; i < sfs_builtin_files_num; i++)
        {
            if (strcmp(path_sanitized, sfs_entries[i].abspath) == 0)
            {
                filesize = (size_t)(sfs_entries[i].end - sfs_entries[i].start);
                fileptr = fmemopen((void *)sfs_entries[i].start,
                                   filesize, "r");
                break;
            }
        }
    }

    if (out != NULL)
        *out = fileptr;

    /* If we found a match, allocate a slot in the virtual FS arrays. */
    if (fileptr)
    {
        for (size_t k = 0; k < sfs_filefd_array_sz; k++)
        {
            if (sfs_filefd[k] == 0)
            {
                sfs_filefd[k] = sfs_filefd_min + k;
                sfs_fileptr[k] = fileptr;
                sfs_filesize[k] = filesize;
                return sfs_filefd[k];
            }
        }
    }
    return -1;
}

/**
 * @brief Close a virtual file descriptor.
 *
 * @param fd The synthetic file descriptor to close.
 * @return 0 on success, -2 if not found or invalid, or the fclose() result.
 */
int sfs_close(int fd)
{
    if (fd < sfs_filefd_min || fd >= sfs_filefd_max)
        return -2;

    for (size_t k = 0; k < sfs_filefd_array_sz; k++)
    {
        if (sfs_filefd[k] == fd)
        {
            sfs_filefd[k] = 0;
            sfs_filesize[k] = 0;
            int res = fclose(sfs_fileptr[k]);
            sfs_fileptr[k] = NULL;
            return res;
        }
    }
    return -2;
}

/**
 * @brief Find a virtual file descriptor or FILE* pointer in the global arrays.
 *
 * @param fd  If >= 0, look up the FILE* for this synthetic fd. Otherwise ignored.
 * @param ptr If non-NULL, look for the synthetic fd associated with this FILE*.
 *
 * @return Pointer to the matching file descriptor (int *) if ptr is non-NULL,
 *         or a FILE* if fd is given, or NULL if not found.
 */
void *sfs_find(int fd, FILE *ptr)
{
    /* If we are looking by FILE* pointer: */
    if (ptr != NULL)
    {
        for (size_t k = 0; k < sfs_filefd_array_sz; k++)
        {
            if (sfs_fileptr[k] == ptr)
                return &sfs_filefd[k];
        }
        return NULL;
    }
    else
    {
        /* Otherwise, we look by fd. */
        if (fd < sfs_filefd_min || fd >= sfs_filefd_max)
            return NULL;
        for (size_t k = 0; k < sfs_filefd_array_sz; k++)
        {
            if (sfs_filefd[k] == fd)
                return sfs_fileptr[k];
        }
    }
    return NULL;
}

/**
 * @brief Read data from a virtual file descriptor.
 *
 * @param fd    The synthetic file descriptor.
 * @param buf   Buffer to receive data.
 * @param count Number of bytes to read.
 *
 * @return Number of bytes read, or -1 on failure.
 */
ssize_t sfs_read(int fd, void *buf, size_t count)
{
    FILE *ptr = (FILE *)sfs_find(fd, NULL);
    if (!ptr)
        return -1;

    return (ssize_t)fread(buf, 1, count, ptr);
}

/**
 * @brief Seek within a virtual file descriptor's data.
 *
 * @param fd     The synthetic file descriptor.
 * @param offset Offset relative to `whence`.
 * @param whence One of SEEK_SET, SEEK_CUR, SEEK_END.
 *
 * @return 0 on success, or -1 on failure.
 */
int sfs_seek(int fd, long offset, int whence)
{
    FILE *ptr = (FILE *)sfs_find(fd, NULL);
    if (!ptr)
        return -1;

    return fseek(ptr, offset, whence);
}

/**
 * @brief Check access for a path in the virtual filesystem.
 *
 * If the path is found in the virtual filesystem, return 0. If not found, return -1.
 * If the path does not match the prefix for our in-memory FS, call the real access().
 *
 * @param path The path to check.
 * @return 0 if path found in the VFS, or -1 if not found, or real system call value otherwise.
 */
int sfs_access(const char *path)
{
    char path_sanitized[sfs_filepath_max_len];
    sfs_sanitize_path(path_sanitized, path);

    /* If path does not begin with our VFS prefix, pass through. */
    if (strncmp(path_sanitized, SFS_BUILTIN_PREFIX,
                strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_access(path, F_OK);
    }

    for (size_t i = 0; i < sfs_builtin_files_num; i++)
    {
        if (strcmp(path_sanitized, sfs_entries[i].abspath) == 0)
        {
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Helper to emulate stat() calls on the virtual filesystem.
 *
 * @param path     The path to stat (may be NULL if fd is used).
 * @param fd       A synthetic file descriptor, if path is NULL.
 * @param statbuf  Output stat buffer.
 *
 * @return 0 on success, -1 or -2 on failure (depending on reason).
 */
int sfs_stat(const char *path, int fd, struct stat *statbuf)
{
    char path_sanitized[sfs_filepath_max_len];
    if (path != NULL)
        sfs_sanitize_path(path_sanitized, path);

    /* If path is provided and does not begin with our VFS prefix, pass through. */
    if (path != NULL &&
        strncmp(path_sanitized, SFS_BUILTIN_PREFIX,
                strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_stat(path, statbuf);
    }

    /* If path is provided, try looking it up in the VFS. */
    if (path != NULL)
    {
        for (size_t i = 0; i < sfs_builtin_files_num; i++)
        {
            if (strcmp(path_sanitized, sfs_entries[i].abspath) == 0)
            {
                memset(statbuf, 0, sizeof(struct stat));
                statbuf->st_size = (off_t)(sfs_entries[i].end - sfs_entries[i].start);
                statbuf->st_mode = S_IFREG; /* Mark as regular file. */
                return 0;
            }
        }
        return -1;
    }

    /* If fd is valid, try looking it up in our open file table. */
    if (fd >= 0 && fd >= sfs_filefd_min && fd < sfs_filefd_max)
    {
        for (size_t k = 0; k < sfs_filefd_array_sz; k++)
        {
            if (sfs_filefd[k] == fd)
            {
                memset(statbuf, 0, sizeof(struct stat));
                statbuf->st_size = sfs_filesize[k];
                statbuf->st_mode = S_IFREG;
                return 0;
            }
        }
        return -1;
    }
    return -2;
}

/* -------------------------------------------------------------------------
 * Wrapped functions for interception.
 *
 * These intercept calls to fopen, open, access, and stat. For paths that do not
 * start with SFS_BUILTIN_PREFIX, we pass them through to the real calls.
 * ------------------------------------------------------------------------- */

/**
 * @brief Wrapped version of fopen().
 *
 * If the path starts with SFS_BUILTIN_PREFIX, try opening from the virtual FS.
 * Otherwise, call the real fopen().
 */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_fopen(path, mode);
    }

    FILE *f = NULL;
    int fd = sfs_open(path, &f);
    if (f)
        return f;

    /* If VFS open failed, fallback to real fopen. */
    return __real_fopen(path, mode);
}

/**
 * @brief Wrapped version of open().
 *
 * If the path starts with SFS_BUILTIN_PREFIX, try opening from the virtual FS.
 * Otherwise, call the real open().
 */
int __wrap_open(const char *path, int flags, ...)
{
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        va_list args;
        va_start(args, flags);

        int mode = 0;
        if (flags & O_CREAT)
            mode = va_arg(args, int);

        va_end(args);
        return __real_open(path, flags, mode);
    }

    int res = sfs_open(path, NULL);
    if (res >= 0)
        return res;

    /* If VFS open failed, fallback to real open. */
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    va_end(args);

    return __real_open(path, flags, mode);
}

/**
 * @brief Wrapped version of access().
 *
 * If the path starts with SFS_BUILTIN_PREFIX, check the virtual FS. Otherwise,
 * call the real access().
 */
int __wrap_access(const char *path, int flags)
{
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_access(path, flags);
    }

    int res = sfs_access(path);
    if (res >= -1)
        return res;

    /* If not found in VFS, fallback to real access. */
    return __real_access(path, flags);
}

/**
 * @brief Wrapped version of stat().
 *
 * If the path starts with SFS_BUILTIN_PREFIX, call sfs_stat(). Otherwise,
 * call the real stat().
 */
int __wrap_stat(const char *restrict path, struct stat *restrict statbuf)
{
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_stat(path, statbuf);
    }

    int res = sfs_stat(path, -1, statbuf);
    if (res >= -1)
        return res;

    /* If not found in VFS, fallback to real stat. */
    return __real_stat(path, statbuf);
}

/**
 * @brief Wrapped version of fstat().
 *
 * If fd is a synthetic file descriptor, call sfs_stat(). Otherwise,
 * call the real fstat().
 */
int __wrap_fstat(int fd, struct stat *statbuf)
{
    int res = sfs_stat(NULL, fd, statbuf);
    if (res >= -1)
        return res;

    return __real_fstat(fd, statbuf);
}

/**
 * @brief Wrapped version of fileno().
 *
 * Calls the real fileno(). If that fails, tries to see if the pointer corresponds
 * to our virtual FS.
 */
int __wrap_fileno(FILE *stream)
{
    int res = __real_fileno(stream);
    if (res < 0)
    {
        int *ptr = (int *)sfs_find(-1, stream);
        res = (ptr == NULL) ? -1 : (*ptr);
    }
    return res;
}

/**
 * @brief Wrapped version of close().
 *
 * If fd is a synthetic file descriptor, closes in VFS. Otherwise, calls the real close().
 */
int __wrap_close(int fd)
{
    int ret = sfs_close(fd);
    if (ret >= -1)
        return ret;

    return __real_close(fd);
}

/**
 * @brief Wrapped version of read().
 *
 * If fd is a synthetic file descriptor, read from VFS. Otherwise, call the real read().
 */
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    ssize_t res = sfs_read(fd, buf, count);
    if (res >= 0)
        return res;

    return __real_read(fd, buf, count);
}

/**
 * @brief Wrapped version of lseek().
 *
 * If fd is a synthetic file descriptor, seek in VFS. Otherwise, call the real lseek().
 */
off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    int res = sfs_seek(fd, (long)offset, whence);
    if (res >= 0)
        return res;

    return __real_lseek(fd, offset, whence);
}

/* -------------------------------------------------------------------------
 * Perl bootstrap routines
 * ------------------------------------------------------------------------- */
/**
 * @brief Initialize various XS modules (like DynaLoader, mro, etc.).
 *
 * This function is used by the embedded Perl interpreter to bootstrap core
 * modules. It is essentially the same pattern used by "miniperlmain.c" in
 * the Perl source distribution.
 */
void xs_init(pTHX)
{
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    extern void boot_DynaLoader(pTHX_ CV * cv);
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);

    extern void boot_mro(pTHX_ CV * cv);
    newXS("mro::bootstrap", boot_mro, file);

    extern void boot_Devel__Peek(pTHX_ CV * cv);
    newXS("Devel::Peek", boot_Devel__Peek, file);

    extern void boot_File__DosGlob(pTHX_ CV * cv);
    newXS("File::DosGlob::bootstrap", boot_File__DosGlob, file);

    extern void boot_File__Glob(pTHX_ CV * cv);
    newXS("File::Glob::bootstrap", boot_File__Glob, file);

    extern void boot_Sys__Syslog(pTHX_ CV * cv);
    newXS("Sys::Syslog::bootstrap", boot_Sys__Syslog, file);

    extern void boot_Sys__Hostname(pTHX_ CV * cv);
    newXS("Sys::Hostname::bootstrap", boot_Sys__Hostname, file);

    extern void boot_PerlIO__via(pTHX_ CV * cv);
    newXS("PerlIO::via::bootstrap", boot_PerlIO__via, file);

    extern void boot_PerlIO__mmap(pTHX_ CV * cv);
    newXS("PerlIO::mmap::bootstrap", boot_PerlIO__mmap, file);

    extern void boot_PerlIO__encoding(pTHX_ CV * cv);
    newXS("PerlIO::encoding::bootstrap", boot_PerlIO__encoding, file);

    extern void boot_B(pTHX_ CV * cv);
    newXS("B::bootstrap", boot_B, file);

    extern void boot_attributes(pTHX_ CV * cv);
    newXS("attributes::bootstrap", boot_attributes, file);

    extern void boot_Unicode__Normalize(pTHX_ CV * cv);
    newXS("Unicode::Normalize::bootstrap", boot_Unicode__Normalize, file);

    extern void boot_Unicode__Collate(pTHX_ CV * cv);
    newXS("Unicode::Collate::bootstrap", boot_Unicode__Collate, file);

    extern void boot_threads(pTHX_ CV * cv);
    newXS("threads::bootstrap", boot_threads, file);

    extern void boot_threads__shared(pTHX_ CV * cv);
    newXS("threads::shared::bootstrap", boot_threads__shared, file);

    extern void boot_IPC__SysV(pTHX_ CV * cv);
    newXS("IPC::SysV::bootstrap", boot_IPC__SysV, file);

    extern void boot_re(pTHX_ CV * cv);
    newXS("re::bootstrap", boot_re, file);

    extern void boot_Digest__MD5(pTHX_ CV * cv);
    newXS("Digest::MD5::bootstrap", boot_Digest__MD5, file);

    extern void boot_Digest__SHA(pTHX_ CV * cv);
    newXS("Digest::SHA::bootstrap", boot_Digest__SHA, file);

    extern void boot_SDBM_File(pTHX_ CV * cv);
    newXS("SDBM_File::bootstrap", boot_SDBM_File, file);

    extern void boot_Math__BigInt__FastCalc(pTHX_ CV * cv);
    newXS("Math::BigInt::FastCalc::bootstrap", boot_Math__BigInt__FastCalc, file);

    extern void boot_Data__Dumper(pTHX_ CV * cv);
    newXS("Data::Dumper::bootstrap", boot_Data__Dumper, file);

    extern void boot_I18N__Langinfo(pTHX_ CV * cv);
    newXS("I18N::Langinfo::bootstrap", boot_I18N__Langinfo, file);

    extern void boot_Time__Piece(pTHX_ CV * cv);
    newXS("Time::Piece::bootstrap", boot_Time__Piece, file);

    extern void boot_IO(pTHX_ CV * cv);
    newXS("IO::bootstrap", boot_IO, file);

    extern void boot_Hash__Util__FieldHash(pTHX_ CV * cv);
    newXS("Hash::Util::FieldHash::bootstrap", boot_Hash__Util__FieldHash, file);

    extern void boot_Hash__Util(pTHX_ CV * cv);
    newXS("Hash::Util::bootstrap", boot_Hash__Util, file);

    extern void boot_Filter__Util__Call(pTHX_ CV * cv);
    newXS("Filter::Util::Call::bootstrap", boot_Filter__Util__Call, file);

    extern void boot_POSIX(pTHX_ CV * cv);
    newXS("POSIX::bootstrap", boot_POSIX, file);

    extern void boot_Encode__Unicode(pTHX_ CV * cv);
    newXS("Encode::Unicode::bootstrap", boot_Encode__Unicode, file);

    extern void boot_Encode(pTHX_ CV * cv);
    newXS("Encode::bootstrap", boot_Encode, file);

    extern void boot_Encode__JP(pTHX_ CV * cv);
    newXS("Encode::JP::bootstrap", boot_Encode__JP, file);

    extern void boot_Encode__KR(pTHX_ CV * cv);
    newXS("Encode::KR::bootstrap", boot_Encode__KR, file);

    extern void boot_Encode__EBCDIC(pTHX_ CV * cv);
    newXS("Encode::EBCDIC::bootstrap", boot_Encode__EBCDIC, file);

    extern void boot_Encode__CN(pTHX_ CV * cv);
    newXS("Encode::CN::bootstrap", boot_Encode__CN, file);

    extern void boot_Encode__Symbol(pTHX_ CV * cv);
    newXS("Encode::Symbol::bootstrap", boot_Encode__Symbol, file);

    extern void boot_Encode__Byte(pTHX_ CV * cv);
    newXS("Encode::Byte::bootstrap", boot_Encode__Byte, file);

    extern void boot_Encode__TW(pTHX_ CV * cv);
    newXS("Encode::TW::bootstrap", boot_Encode__TW, file);

    extern void boot_Compress__Raw__Zlib(pTHX_ CV * cv);
    newXS("Compress::Raw::Zlib::bootstrap", boot_Compress__Raw__Zlib, file);

    extern void boot_Compress__Raw__Bzip2(pTHX_ CV * cv);
    newXS("Compress::Raw::Bzip2::bootstrap", boot_Compress__Raw__Bzip2, file);

    extern void boot_MIME__Base64(pTHX_ CV * cv);
    newXS("MIME::Base64::bootstrap", boot_MIME__Base64, file);

    extern void boot_Cwd(pTHX_ CV * cv);
    newXS("Cwd::bootstrap", boot_Cwd, file);

    extern void boot_Storable(pTHX_ CV * cv);
    newXS("Storable::bootstrap", boot_Storable, file);

    extern void boot_List__Util(pTHX_ CV * cv);
    newXS("List::Util::bootstrap", boot_List__Util, file);

    extern void boot_Fcntl(pTHX_ CV * cv);
    newXS("Fcntl::bootstrap", boot_Fcntl, file);

    extern void boot_Opcode(pTHX_ CV * cv);
    newXS("Opcode::bootstrap", boot_Opcode, file);
}

/* -------------------------------------------------------------------------
 * Main: Initialize and run the Perl interpreter.
 * ------------------------------------------------------------------------- */
/**
 * @brief The main entry point for this minimal Perl interpreter.
 *
 * This function creates a PerlInterpreter instance, initializes it, and then
 * runs the Perl code given by command-line arguments. It also sets up the
 * minimal cleanup level for WASI or other limited environments.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 *
 * @return Exit status from the interpreter.
 */
int main(int argc, char *argv[])
{
    int exitstatus;

    PERL_SYS_INIT3(&argc, &argv, &environ);

    PerlInterpreter *myperl = perl_alloc();
    if (myperl == NULL)
        return -1;

    perl_construct(myperl);

    /* Do minimal cleanup for WASI or other environments. */
    PL_exit_flags &= ~PERL_EXIT_DESTRUCT_END;

    exitstatus = 0;
    if (!perl_parse(myperl, xs_init, argc, argv, (char **)NULL))
    {
        exitstatus = perl_run(myperl);
    }

    PL_perl_destruct_level = 0;
    exitstatus = perl_destruct(myperl);

    perl_free(myperl);
    PERL_SYS_TERM();

    return exitstatus;
}