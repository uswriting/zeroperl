#define PERL_IN_MINIPERLMAIN_C

#include <stdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "zeroperl.h" /* Must define SFS_BUILTIN_PREFIX, e.g. "builtin:" */

extern char **environ;
static void xs_init(pTHX);
static PerlInterpreter *zero_perl;

/* -------------------------------------------------------------------------
 * External declarations for the underlying ("real") functions.
 * We expect these symbols to be resolved at link time via linker wrapping.
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
 * In-memory data for the “virtual” filesystem.
 * ------------------------------------------------------------------------- */
extern size_t sfs_builtin_files_num;
extern struct
{
    const char *abspath;
    const unsigned char *start; /* start of file in memory */
    const unsigned char *end;   /* end of file in memory */
} sfs_entries[];

/* -------------------------------------------------------------------------
 * Keep track of which FDs are “in use” so we don't accidentally reuse a real FD
 * for an SFS file or vice versa. We'll store up to 32 FDs here for simplicity.
 * ------------------------------------------------------------------------- */
#define FD_MAX_TRACK 32
static bool g_fd_in_use[FD_MAX_TRACK] = {false}; /* false => free, true => in use */

/* Mark an FD as in use, if within range. */
static void fd_mark_in_use(int fd)
{
    if (fd >= 0 && fd < FD_MAX_TRACK)
    {
        g_fd_in_use[fd] = true;
    }
}

/* Mark an FD as free. */
static void fd_mark_free(int fd)
{
    if (fd >= 0 && fd < FD_MAX_TRACK)
    {
        g_fd_in_use[fd] = false;
    }
}

/* Check if an FD is in use. */
static bool fd_is_in_use(int fd)
{
    if (fd < 0 || fd >= FD_MAX_TRACK)
    {
        return false;
    }
    return g_fd_in_use[fd];
}

/* -------------------------------------------------------------------------
 * We store a small table (up to 16) of open VFS files. Each slot tracks:
 *   - whether it is used
 *   - a unique integer FD
 *   - a FILE* from fmemopen()
 *   - the file size
 *   - an optional reference count or usage count (here we do 1:1 open->close).
 * If a prefix-based path is not found in the VFS, we simply fail (no fallback).
 * ------------------------------------------------------------------------- */
#define SFS_MAX_OPEN_FILES 16
typedef struct
{
    bool used;
    int fd;      /* unique integer FD assigned by our code */
    FILE *fp;    /* fmemopen() pointer */
    size_t size; /* file size */
    char path[256];
} SFS_Entry;

static SFS_Entry sfs_table[SFS_MAX_OPEN_FILES];

/* Next FD we will assign to a new SFS file. Start from some number that is
 * unlikely to collide with typical OS usage like 0,1,2. We'll do 100. */
static int sfs_next_fd = 100;

/* -------------------------------------------------------------------------
 * Helper routine: remove consecutive duplicate '/' from path (sanitizing).
 * ------------------------------------------------------------------------- */
static void sfs_sanitize_path(char *dst, size_t dstsize, const char *src)
{
    size_t j = 0;
    size_t limit = (dstsize > 0) ? (dstsize - 1) : 0;

    for (size_t i = 0; src[i] != '\0' && j < limit; i++)
    {
        if (i > 0 && src[i] == '/' && src[i - 1] == '/')
        {
            /* skip consecutive '/' */
            continue;
        }
        dst[j++] = src[i];
    }
    if (dstsize > 0)
    {
        dst[j] = '\0';
    }
}

/* -------------------------------------------------------------------------
 * Find a free FD that doesn't collide with any real FD in use, nor with SFS.
 * We'll increment from sfs_next_fd until we find a free slot. If we exceed
 * FD_MAX_TRACK, we fail.
 * ------------------------------------------------------------------------- */
static int sfs_allocate_fd(void)
{
    for (;;)
    {
        if (sfs_next_fd >= FD_MAX_TRACK)
        {
            /* No available FD in our simplistic scheme => fail. */
            return -1;
        }
        if (!fd_is_in_use(sfs_next_fd))
        {
            /* Mark it used and return it. */
            fd_mark_in_use(sfs_next_fd);
            return sfs_next_fd++;
        }
        sfs_next_fd++;
    }
}

/* -------------------------------------------------------------------------
 * sfs_find_by_fd: given an FD, find the corresponding slot in sfs_table.
 * ------------------------------------------------------------------------- */
static SFS_Entry *sfs_find_by_fd(int fd)
{
    for (int i = 0; i < SFS_MAX_OPEN_FILES; i++)
    {
        if (sfs_table[i].used && sfs_table[i].fd == fd)
        {
            return &sfs_table[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * sfs_open: open a path from the in-memory VFS. If not found, fail with -1.
 * If found, fmemopen, assign an FD, store in sfs_table, return it.
 * ------------------------------------------------------------------------- */
static int sfs_open(const char *path, FILE **outfp)
{
    char sanitized[256];
    sfs_sanitize_path(sanitized, sizeof(sanitized), path);

    /* Find the data in sfs_entries[] if present. */
    const unsigned char *start = NULL;
    size_t file_size = 0;

    for (size_t i = 0; i < sfs_builtin_files_num; i++)
    {
        if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
        {
            start = sfs_entries[i].start;
            file_size = (size_t)(sfs_entries[i].end - sfs_entries[i].start);
            break;
        }
    }
    if (!start)
    {
        /* Not found => fail. We do *not* fallback to real open. */
        if (outfp)
        {
            *outfp = NULL;
        }
        errno = ENOENT; 
        return -1;
    }

    /* fmemopen the data (read-only). */
    FILE *fp = fmemopen((void *)start, file_size, "r");
    if (!fp)
    {
        /* fmemopen failed => fail. */
        if (outfp)
        {
            *outfp = NULL;
        }
        /* errno set by fmemopen */
        return -1;
    }

    /* Find a free slot in sfs_table. */
    for (int i = 0; i < SFS_MAX_OPEN_FILES; i++)
    {
        if (!sfs_table[i].used)
        {
            int assigned_fd = sfs_allocate_fd();
            if (assigned_fd < 0)
            {
                /* No FD left => fail. */
                fclose(fp);
                if (outfp)
                {
                    *outfp = NULL;
                }
                errno = EMFILE; /* too many files open */
                return -1;
            }

            /* Fill out the slot. */
            sfs_table[i].used = true;
            sfs_table[i].fd = assigned_fd;
            sfs_table[i].fp = fp;
            sfs_table[i].size = file_size;
            strncpy(sfs_table[i].path, sanitized, sizeof(sfs_table[i].path) - 1);
            sfs_table[i].path[sizeof(sfs_table[i].path) - 1] = '\0';

            if (outfp)
            {
                *outfp = fp;
            }
            return assigned_fd;
        }
    }

    /* If we reach here, the table is full. */
    fclose(fp);
    errno = EMFILE;
    if (outfp)
    {
        *outfp = NULL;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * sfs_close: if FD belongs to us, free it. Returns 0 if closed, -2 if not ours.
 * ------------------------------------------------------------------------- */
static int sfs_close(int fd)
{
    SFS_Entry *entry = sfs_find_by_fd(fd);
    if (!entry)
    {
        return -2; /* Not ours => fallback to real close. */
    }

    /* Found => close and free the slot. */
    fclose(entry->fp);
    entry->fp = NULL;
    entry->used = false;
    entry->fd = -1;
    entry->size = 0;
    entry->path[0] = '\0';

    /* Mark the FD as free. */
    fd_mark_free(fd);
    return 0;
}

/* -------------------------------------------------------------------------
 * sfs_read: if FD is ours, read from the in-memory file. else return -1.
 * ------------------------------------------------------------------------- */
static ssize_t sfs_read(int fd, void *buf, size_t count)
{
    SFS_Entry *entry = sfs_find_by_fd(fd);
    if (!entry || !entry->fp)
    {
        return -1; /* Not ours => fallback to real read. */
    }

    return (ssize_t)fread(buf, 1, count, entry->fp);
}

/* -------------------------------------------------------------------------
 * sfs_lseek: if FD is ours, do fseek/fread logic. else return -1.
 * ------------------------------------------------------------------------- */
static off_t sfs_lseek(int fd, off_t offset, int whence)
{
    SFS_Entry *entry = sfs_find_by_fd(fd);
    if (!entry || !entry->fp)
    {
        return (off_t)-1;
    }

    if (fseek(entry->fp, (long)offset, whence) != 0)
    {
        return (off_t)-1;
    }
    long pos = ftell(entry->fp);
    if (pos < 0)
    {
        return (off_t)-1;
    }
    return (off_t)pos;
}

/* -------------------------------------------------------------------------
 * sfs_access: if path starts with our prefix, check if it’s in sfs_entries[].
 * Return 0 if found, -1 if not. We do *not* fallback if it’s in our prefix.
 * If it’s not in our prefix => call the real access().
 * ------------------------------------------------------------------------- */
static int sfs_access(const char *path)
{
    char sanitized[256];
    sfs_sanitize_path(sanitized, sizeof(sanitized), path);

    if (strncmp(sanitized, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        /* not ours => real system call */
        return __real_access(path, F_OK);
    }

    /* check if in sfs_entries[] */
    for (size_t i = 0; i < sfs_builtin_files_num; i++)
    {
        if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
        {
            return 0; /* found */
        }
    }
    /* not found => -1, no fallback. */
    errno = ENOENT;
    return -1;
}

/* -------------------------------------------------------------------------
 * sfs_stat: if path is ours => fill stbuf. if FD is ours => fill stbuf.
 * otherwise fallback to real.
 * path != NULL => path-based, path == NULL => FD-based
 * ------------------------------------------------------------------------- */
static int sfs_stat(const char *path, int fd, struct stat *stbuf)
{
    if (path)
    {
        char sanitized[256];
        sfs_sanitize_path(sanitized, sizeof(sanitized), path);

        if (strncmp(sanitized, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
        {
            /* not ours => real stat() */
            return __real_stat(path, stbuf);
        }

        /* Check sfs_entries[] */
        for (size_t i = 0; i < sfs_builtin_files_num; i++)
        {
            if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
            {
                memset(stbuf, 0, sizeof(*stbuf));
                stbuf->st_size = (off_t)(sfs_entries[i].end - sfs_entries[i].start);
                stbuf->st_mode = S_IFREG; /* a regular file */
                return 0;
            }
        }
        errno = ENOENT;
        return -1;
    }
    else
    {
        /* FD-based => see if FD is ours. */
        SFS_Entry *entry = sfs_find_by_fd(fd);
        if (!entry)
        {
            /* not ours => real fstat if >=0, else error */
            if (fd >= 0)
            {
                return __real_fstat(fd, stbuf);
            }
            errno = EBADF;
            return -1;
        }
        /* fill out stbuf */
        memset(stbuf, 0, sizeof(*stbuf));
        stbuf->st_size = (off_t)entry->size;
        stbuf->st_mode = S_IFREG;
        return 0;
    }
}

/* =========================================================================
 * Wrappers
 * ========================================================================= */

/* __wrap_fopen */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    /* If path does NOT start with our prefix => real fopen. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        FILE *realfp = __real_fopen(path, mode);
        if (realfp)
        {
            /* Mark the underlying FD as used, if we can obtain it. */
            int realfd = fileno(realfp);
            if (realfd >= 0)
            {
                fd_mark_in_use(realfd);
            }
        }
        return realfp;
    }

    /* Otherwise => SFS open only. If not found in SFS => fail. */
    FILE *fptr = NULL;
    int fd = sfs_open(path, &fptr);
    if (fd < 0)
    {
        /* sfs_open failed => no fallback => return NULL, errno set. */
        return NULL;
    }
    return fptr;
}

/* __wrap_open */
int __wrap_open(const char *path, int flags, ...)
{
    /* If it’s not in our prefix => real open. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        va_list args;
        va_start(args, flags);
        int mode = 0;
        if (flags & O_CREAT)
        {
            mode = va_arg(args, int);
        }
        int realfd = __real_open(path, flags, mode);
        va_end(args);

        if (realfd >= 0)
        {
            fd_mark_in_use(realfd);
        }
        return realfd;
    }

    /* Otherwise => SFS only, no fallback. */
    int fd;
    {
        FILE *dummy = NULL;
        fd = sfs_open(path, &dummy);
    }
    return fd; /* if -1 => fail, errno set, no fallback */
}

/* __wrap_close */
int __wrap_close(int fd)
{
    /* Attempt SFS close. */
    int rc = sfs_close(fd);
    if (rc == -2)
    {
        /* Not ours => real close. */
        fd_mark_free(fd);
        return __real_close(fd);
    }
    return rc; /* 0 => success, or possibly an error if unexpected. */
}

/* __wrap_access */
int __wrap_access(const char *path, int amode)
{
    /* If not our prefix => real access. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_access(path, amode);
    }

    /* Otherwise => sfs_access. */
    return sfs_access(path);
}

/* __wrap_stat */
int __wrap_stat(const char *restrict path, struct stat *restrict stbuf)
{
    /* If not ours => real stat. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) != 0)
    {
        return __real_stat(path, stbuf);
    }

    /* sfs_stat with path. */
    return sfs_stat(path, -1, stbuf);
}

/* __wrap_fstat */
int __wrap_fstat(int fd, struct stat *stbuf)
{
    /* If ours => do it, else real. */
    SFS_Entry *entry = sfs_find_by_fd(fd);
    if (entry)
    {
        memset(stbuf, 0, sizeof(*stbuf));
        stbuf->st_size = entry->size;
        stbuf->st_mode = S_IFREG;
        return 0;
    }
    /* Not ours => real fstat. */
    return __real_fstat(fd, stbuf);
}

/* __wrap_read */
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    ssize_t r = sfs_read(fd, buf, count);
    if (r >= 0)
    {
        return r;
    }
    /* fallback => real read. */
    return __real_read(fd, buf, count);
}

/* __wrap_lseek */
off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    off_t pos = sfs_lseek(fd, offset, whence);
    if (pos >= 0)
    {
        return pos;
    }
    /* fallback => real lseek. */
    return __real_lseek(fd, offset, whence);
}

/* __wrap_fileno */
int __wrap_fileno(FILE *stream)
{
    /* Try real fileno first. If it’s valid, we mark it in-use to avoid reuse. */
    int realfd = __real_fileno(stream);
    if (realfd >= 0)
    {
        fd_mark_in_use(realfd);
        return realfd;
    }

    /* If that fails, maybe it’s one of ours. */
    for (int i = 0; i < SFS_MAX_OPEN_FILES; i++)
    {
        if (sfs_table[i].used && sfs_table[i].fp == stream)
        {
            return sfs_table[i].fd; /* Our FD. */
        }
    }
    return -1; /* Not found. */
}

/* -------------------------------------------------------------------------
 * The minimal main() for our embedded Perl interpreter
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int exitstatus;

    PERL_SYS_INIT3(&argc, &argv, &environ);
    PERL_SYS_FPU_INIT;

    zero_perl = perl_alloc();
    if (!zero_perl)
        return 1;

    perl_construct(zero_perl);

    /* Minimal cleanup for restricted environments. */
    PL_perl_destruct_level = 0;
    PL_exit_flags &= ~PERL_EXIT_DESTRUCT_END;

    exitstatus = 0;
    if (!perl_parse(zero_perl, xs_init, argc, argv, NULL))
    {
        assert(!PL_restartop);
        exitstatus = perl_run(zero_perl);
    }

    perl_destruct(zero_perl);
    perl_free(zero_perl);
    PERL_SYS_TERM();
    return exitstatus;
}

/* -------------------------------------------------------------------------
 * XS bootstrap table. (Adjust to match your build.)
 * ------------------------------------------------------------------------- */
EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
EXTERN_C void boot_mro(pTHX_ CV *cv);
EXTERN_C void boot_Devel__Peek(pTHX_ CV *cv);
EXTERN_C void boot_File__DosGlob(pTHX_ CV *cv);
EXTERN_C void boot_File__Glob(pTHX_ CV *cv);
EXTERN_C void boot_Sys__Syslog(pTHX_ CV *cv);
EXTERN_C void boot_Sys__Hostname(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__via(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__mmap(pTHX_ CV *cv);
EXTERN_C void boot_PerlIO__encoding(pTHX_ CV *cv);
EXTERN_C void boot_B(pTHX_ CV *cv);
EXTERN_C void boot_attributes(pTHX_ CV *cv);
EXTERN_C void boot_Unicode__Normalize(pTHX_ CV *cv);
EXTERN_C void boot_Unicode__Collate(pTHX_ CV *cv);
EXTERN_C void boot_threads(pTHX_ CV *cv);
EXTERN_C void boot_threads__shared(pTHX_ CV *cv);
EXTERN_C void boot_IPC__SysV(pTHX_ CV *cv);
EXTERN_C void boot_re(pTHX_ CV *cv);
EXTERN_C void boot_Digest__MD5(pTHX_ CV *cv);
EXTERN_C void boot_Digest__SHA(pTHX_ CV *cv);
EXTERN_C void boot_SDBM_File(pTHX_ CV *cv);
EXTERN_C void boot_Math__BigInt__FastCalc(pTHX_ CV *cv);
EXTERN_C void boot_Data__Dumper(pTHX_ CV *cv);
EXTERN_C void boot_I18N__Langinfo(pTHX_ CV *cv);
EXTERN_C void boot_Time__Piece(pTHX_ CV *cv);
EXTERN_C void boot_IO(pTHX_ CV *cv);
EXTERN_C void boot_Hash__Util__FieldHash(pTHX_ CV *cv);
EXTERN_C void boot_Hash__Util(pTHX_ CV *cv);
EXTERN_C void boot_Filter__Util__Call(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Unicode(pTHX_ CV *cv);
EXTERN_C void boot_Encode(pTHX_ CV *cv);
EXTERN_C void boot_Encode__JP(pTHX_ CV *cv);
EXTERN_C void boot_Encode__KR(pTHX_ CV *cv);
EXTERN_C void boot_Encode__EBCDIC(pTHX_ CV *cv);
EXTERN_C void boot_Encode__CN(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Symbol(pTHX_ CV *cv);
EXTERN_C void boot_Encode__Byte(pTHX_ CV *cv);
EXTERN_C void boot_Encode__TW(pTHX_ CV *cv);
EXTERN_C void boot_Compress__Raw__Zlib(pTHX_ CV *cv);
EXTERN_C void boot_Compress__Raw__Bzip2(pTHX_ CV *cv);
EXTERN_C void boot_MIME__Base64(pTHX_ CV *cv);
EXTERN_C void boot_Cwd(pTHX_ CV *cv);
EXTERN_C void boot_Storable(pTHX_ CV *cv);
EXTERN_C void boot_List__Util(pTHX_ CV *cv);
EXTERN_C void boot_Fcntl(pTHX_ CV *cv);
EXTERN_C void boot_Opcode(pTHX_ CV *cv);

static void xs_init(pTHX)
{
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
    newXS("mro::bootstrap", boot_mro, file);
    newXS("Devel::Peek::bootstrap", boot_Devel__Peek, file);
    newXS("File::DosGlob::bootstrap", boot_File__DosGlob, file);
    newXS("File::Glob::bootstrap", boot_File__Glob, file);
    newXS("Sys::Syslog::bootstrap", boot_Sys__Syslog, file);
    newXS("Sys::Hostname::bootstrap", boot_Sys__Hostname, file);
    newXS("PerlIO::via::bootstrap", boot_PerlIO__via, file);
    newXS("PerlIO::mmap::bootstrap", boot_PerlIO__mmap, file);
    newXS("PerlIO::encoding::bootstrap", boot_PerlIO__encoding, file);
    newXS("B::bootstrap", boot_B, file);
    newXS("attributes::bootstrap", boot_attributes, file);
    newXS("Unicode::Normalize::bootstrap", boot_Unicode__Normalize, file);
    newXS("Unicode::Collate::bootstrap", boot_Unicode__Collate, file);
    newXS("threads::bootstrap", boot_threads, file);
    newXS("threads::shared::bootstrap", boot_threads__shared, file);
    newXS("IPC::SysV::bootstrap", boot_IPC__SysV, file);
    newXS("re::bootstrap", boot_re, file);
    newXS("Digest::MD5::bootstrap", boot_Digest__MD5, file);
    newXS("Digest::SHA::bootstrap", boot_Digest__SHA, file);
    newXS("SDBM_File::bootstrap", boot_SDBM_File, file);
    newXS("Math::BigInt::FastCalc::bootstrap", boot_Math__BigInt__FastCalc, file);
    newXS("Data::Dumper::bootstrap", boot_Data__Dumper, file);
    newXS("I18N::Langinfo::bootstrap", boot_I18N__Langinfo, file);
    newXS("Time::Piece::bootstrap", boot_Time__Piece, file);
    newXS("IO::bootstrap", boot_IO, file);
    newXS("Hash::Util::FieldHash::bootstrap", boot_Hash__Util__FieldHash, file);
    newXS("Hash::Util::bootstrap", boot_Hash__Util, file);
    newXS("Filter::Util::Call::bootstrap", boot_Filter__Util__Call, file);
    newXS("Encode::Unicode::bootstrap", boot_Encode__Unicode, file);
    newXS("Encode::bootstrap", boot_Encode, file);
    newXS("Encode::JP::bootstrap", boot_Encode__JP, file);
    newXS("Encode::KR::bootstrap", boot_Encode__KR, file);
    newXS("Encode::EBCDIC::bootstrap", boot_Encode__EBCDIC, file);
    newXS("Encode::CN::bootstrap", boot_Encode__CN, file);
    newXS("Encode::Symbol::bootstrap", boot_Encode__Symbol, file);
    newXS("Encode::Byte::bootstrap", boot_Encode__Byte, file);
    newXS("Encode::TW::bootstrap", boot_Encode__TW, file);
    newXS("Compress::Raw::Zlib::bootstrap", boot_Compress__Raw__Zlib, file);
    newXS("Compress::Raw::Bzip2::bootstrap", boot_Compress__Raw__Bzip2, file);
    newXS("MIME::Base64::bootstrap", boot_MIME__Base64, file);
    newXS("Cwd::bootstrap", boot_Cwd, file);
    newXS("Storable::bootstrap", boot_Storable, file);
    newXS("List::Util::bootstrap", boot_List__Util, file);
    newXS("Fcntl::bootstrap", boot_Fcntl, file);
    newXS("Opcode::bootstrap", boot_Opcode, file);
}
