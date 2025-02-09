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

/* For WASI, we can include __wasi_libc.h or __wasi_api.h,
   or forward-declare __wasi_proc_exit() if needed. */
#ifdef __wasi__
#include <wasi/api.h>
#else
/* If not compiling for WASI, define a no-op fallback. */
static void __wasi_proc_exit(unsigned long code)
{
    exit((int)code);
}
#endif

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
 * In-memory data for the “virtual filesystem.”
 * ------------------------------------------------------------------------- */
extern size_t sfs_builtin_files_num;
extern struct
{
    const char *abspath;
    const unsigned char *start;
    const unsigned char *end;
} sfs_entries[];

/* -------------------------------------------------------------------------
 * We'll track at most 32 FDs (0..31).
 * This is a small limit, but you can adjust as needed.
 * ------------------------------------------------------------------------- */
#define FD_MAX_TRACK 32

static bool g_fd_in_use[FD_MAX_TRACK] = {false};

/* Mark an FD as in-use if within [0..FD_MAX_TRACK-1]. */
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
    return (fd >= 0 && fd < FD_MAX_TRACK) ? g_fd_in_use[fd] : true;
    /* If out of range, treat as "in use" to prevent usage. */
}

/* -------------------------------------------------------------------------
 * We'll store a small table (up to 16) of open VFS files. Each slot tracks:
 *   - an integer FD
 *   - a FILE* (fmemopen)
 *   - file size
 *   - a "used" flag
 *   - the path for reference/debug
 * ------------------------------------------------------------------------- */
#define SFS_MAX_OPEN_FILES 16
typedef struct
{
    bool used;
    int fd;      /* The FD we assigned for this VFS file */
    FILE *fp;    /* The fmemopen() pointer */
    size_t size; /* File size for stat calls */
    char path[256];
} SFS_Entry;

static SFS_Entry sfs_table[SFS_MAX_OPEN_FILES];

/* Starting FD offset for SFS. We won't use 0..2 so we skip standard fds. */
static int sfs_fd_start = 3;

/* -------------------------------------------------------------------------
 * Helper: remove consecutive duplicate '/' from a path for canonicalization.
 * ------------------------------------------------------------------------- */
static void sfs_sanitize_path(char *dst, size_t dstsize, const char *src)
{
    size_t j = 0, limit = (dstsize > 0) ? (dstsize - 1) : 0;

    for (size_t i = 0; src[i] != '\0' && j < limit; i++)
    {
        if (i > 0 && src[i] == '/' && src[i - 1] == '/')
        {
            /* skip consecutive slash */
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
 * sfs_allocate_fd: find the next free descriptor in [sfs_fd_start..FD_MAX_TRACK-1]
 * If none are free, forcibly exit via __wasi_proc_exit(10).
 * ------------------------------------------------------------------------- */
static int sfs_allocate_fd(void)
{
    for (int fd = sfs_fd_start; fd < FD_MAX_TRACK; fd++)
    {
        if (!fd_is_in_use(fd))
        {
            fd_mark_in_use(fd);
            return fd;
        }
    }
    /* No free FD => forcibly exit or handle error. */
    __wasi_proc_exit(10);
    /* not reached */
    return -1;
}

/* -------------------------------------------------------------------------
 * sfs_find_by_fd: returns pointer to sfs_table entry if it matches FD, or NULL.
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
 * sfs_open: tries to find the path in sfs_entries, if found => fmemopen + FD.
 * Returns the FD on success, or -1 if not found or error. (No fallback here.)
 * ------------------------------------------------------------------------- */
static int sfs_open(const char *path, FILE **outfp)
{
    char sanitized[256];
    sfs_sanitize_path(sanitized, sizeof(sanitized), path);

    /* Search sfs_entries. */
    const unsigned char *start = NULL;
    size_t size = 0;

    for (size_t i = 0; i < sfs_builtin_files_num; i++)
    {
        if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
        {
            start = sfs_entries[i].start;
            size = (size_t)(sfs_entries[i].end - sfs_entries[i].start);
            break;
        }
    }
    if (!start)
    {
        errno = ENOENT; /* not found in SFS */
        if (outfp)
        {
            *outfp = NULL;
        }
        return -1;
    }

    /* fmemopen => read-only. */
    FILE *fp = fmemopen((void *)start, size, "r");
    if (!fp)
    {
        /* fmemopen error => pass back. */
        if (outfp)
        {
            *outfp = NULL;
        }
        return -1;
    }

    /* Find free slot in sfs_table. */
    for (int i = 0; i < SFS_MAX_OPEN_FILES; i++)
    {
        if (!sfs_table[i].used)
        {
            int newfd = sfs_allocate_fd();
            /* fill in the sfs_table entry. */
            sfs_table[i].used = true;
            sfs_table[i].fd = newfd;
            sfs_table[i].fp = fp;
            sfs_table[i].size = size;
            strncpy(sfs_table[i].path, sanitized, sizeof(sfs_table[i].path) - 1);
            sfs_table[i].path[sizeof(sfs_table[i].path) - 1] = '\0';

            if (outfp)
            {
                *outfp = fp;
            }
            return newfd;
        }
    }

    /* table is full => fail */
    fclose(fp);
    errno = EMFILE;
    if (outfp)
    {
        *outfp = NULL;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * sfs_close: free the slot if FD is ours, returns 0 on success, -2 if not ours.
 * ------------------------------------------------------------------------- */
static int sfs_close(int fd)
{
    SFS_Entry *e = sfs_find_by_fd(fd);
    if (!e)
    {
        return -2; /* not ours => fallback to real close. */
    }

    fclose(e->fp);
    e->fp = NULL;
    fd_mark_free(e->fd);
    e->used = false;
    e->fd = -1;
    e->size = 0;
    e->path[0] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------
 * sfs_read: read from in-memory data if FD is ours, else -1.
 * ------------------------------------------------------------------------- */
static ssize_t sfs_read(int fd, void *buf, size_t count)
{
    SFS_Entry *e = sfs_find_by_fd(fd);
    if (!e || !e->fp)
    {
        return -1;
    }
    return (ssize_t)fread(buf, 1, count, e->fp);
}

/* -------------------------------------------------------------------------
 * sfs_lseek: do fseek/ftell if FD is ours, else -1.
 * ------------------------------------------------------------------------- */
static off_t sfs_lseek(int fd, off_t offset, int whence)
{
    SFS_Entry *e = sfs_find_by_fd(fd);
    if (!e || !e->fp)
    {
        return (off_t)-1;
    }
    if (fseek(e->fp, (long)offset, whence) != 0)
    {
        return (off_t)-1;
    }
    long pos = ftell(e->fp);
    if (pos < 0)
    {
        return (off_t)-1;
    }
    return (off_t)pos;
}

/* -------------------------------------------------------------------------
 * sfs_access: see if path is in sfs_entries. Return 0 if found, -1 otherwise.
 * (No fallback if path has our prefix and isn’t in SFS.)
 * ------------------------------------------------------------------------- */
static int sfs_access(const char *path)
{
    char sanitized[256];
    sfs_sanitize_path(sanitized, sizeof(sanitized), path);

    for (size_t i = 0; i < sfs_builtin_files_num; i++)
    {
        if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
        {
            return 0; /* found */
        }
    }
    errno = ENOENT;
    return -1;
}

/* -------------------------------------------------------------------------
 * sfs_stat: if path != NULL => do path-based stat, if FD => do FD-based.
 * always try SFS logic first, else fail => caller can fallback if path is not ours.
 * ------------------------------------------------------------------------- */
static int sfs_stat(const char *path, int fd, struct stat *stbuf)
{
    if (path)
    {
        char sanitized[256];
        sfs_sanitize_path(sanitized, sizeof(sanitized), path);

        /* If it is in SFS prefix => check sfs_entries. */
        if (strncmp(sanitized, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) == 0)
        {
            for (size_t i = 0; i < sfs_builtin_files_num; i++)
            {
                if (strcmp(sanitized, sfs_entries[i].abspath) == 0)
                {
                    /* fill out stbuf */
                    memset(stbuf, 0, sizeof(*stbuf));
                    stbuf->st_size = (off_t)(sfs_entries[i].end - sfs_entries[i].start);
                    stbuf->st_mode = S_IFREG;
                    return 0;
                }
            }
            errno = ENOENT;
            return -1; /* not found in SFS => no fallback. */
        }
        /* Not an SFS path => let caller do fallback. Return 1 => "not ours". */
        return 1;
    }
    else
    {
        /* FD-based => see if FD is ours. */
        SFS_Entry *e = sfs_find_by_fd(fd);
        if (!e)
        {
            return 1; /* not ours => fallback. */
        }
        /* fill stbuf from our data. */
        memset(stbuf, 0, sizeof(*stbuf));
        stbuf->st_size = (off_t)e->size;
        stbuf->st_mode = S_IFREG;
        return 0;
    }
}

/* =========================================================================
 * Wrappers that always try SFS first, then fallback to real if that fails.
 * ========================================================================= */

/* __wrap_fopen */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    /* If path has our prefix => try SFS. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) == 0)
    {
        FILE *fp = NULL;
        int sfd = sfs_open(path, &fp);
        if (sfd >= 0)
        {
            /* SFS success => return that. */
            return fp;
        }
        /* sfs_open failed => no fallback => return NULL. */
        return NULL;
    }

    /* Otherwise => real fopen. */
    FILE *realfp = __real_fopen(path, mode);
    if (realfp)
    {
        int realfd = fileno(realfp);
        if (realfd >= 0 && realfd < FD_MAX_TRACK)
        {
            fd_mark_in_use(realfd);
        }
    }
    return realfp;
}

/* __wrap_open */
int __wrap_open(const char *path, int flags, ...)
{
    va_list args;
    va_start(args, flags);
    int mode = 0;
    if (flags & O_CREAT)
    {
        mode = va_arg(args, int);
    }
    va_end(args);

    /* If path has our prefix => try SFS first. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) == 0)
    {
        int sfd = sfs_open(path, NULL);
        if (sfd >= 0)
        {
            return sfd;
        }
        /* no fallback => return -1. */
        return -1;
    }

    /* Otherwise => real open. */
    int realfd = __real_open(path, flags, mode);
    if (realfd >= 0 && realfd < FD_MAX_TRACK)
    {
        fd_mark_in_use(realfd);
    }
    return realfd;
}

/* __wrap_close */
int __wrap_close(int fd)
{
    /* Try SFS first. */
    int rc = sfs_close(fd);
    if (rc == 0)
    {
        return 0;
    }
    if (rc == -2)
    {
        /* not ours => real close. */
        if (fd >= 0 && fd < FD_MAX_TRACK)
        {
            fd_mark_free(fd);
        }
        return __real_close(fd);
    }
    /* unexpected => just return it. */
    return rc;
}

/* __wrap_access */
int __wrap_access(const char *path, int amode)
{
    /* If prefix => try SFS. */
    if (strncmp(path, SFS_BUILTIN_PREFIX, strlen(SFS_BUILTIN_PREFIX)) == 0)
    {
        return sfs_access(path);
    }
    /* else => real. */
    return __real_access(path, amode);
}

/* __wrap_stat */
int __wrap_stat(const char *restrict path, struct stat *restrict stbuf)
{
    /* Attempt SFS. */
    int rc = sfs_stat(path, -1, stbuf);
    if (rc == 0)
    {
        return 0; /* success in SFS */
    }
    if (rc == -1)
    {
        return -1; /* not found => no fallback. */
    }
    /* rc==1 => not ours => fallback. */
    return __real_stat(path, stbuf);
}

/* __wrap_fstat */
int __wrap_fstat(int fd, struct stat *stbuf)
{
    int rc = sfs_stat(NULL, fd, stbuf);
    if (rc == 0)
    {
        return 0; /* SFS success */
    }
    if (rc == -1)
    {
        return -1; /* not found => no fallback */
    }
    /* rc==1 => fallback. */
    return __real_fstat(fd, stbuf);
}

/* __wrap_read */
ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    ssize_t r = sfs_read(fd, buf, count);
    if (r >= 0)
    {
        return r; /* SFS success */
    }
    /* fallback => real read */
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
    /* fallback => real lseek */
    return __real_lseek(fd, offset, whence);
}

/* __wrap_fileno */
int __wrap_fileno(FILE *stream)
{
    /* 1) Check SFS first: see if this FILE* is one of ours. */
    for (int i = 0; i < SFS_MAX_OPEN_FILES; i++)
    {
        if (sfs_table[i].used && sfs_table[i].fp == stream)
        {
            return sfs_table[i].fd; /* found => SFS FD */
        }
    }

    /* 2) If not ours => real fileno. */
    int realfd = __real_fileno(stream);
    if (realfd >= 0 && realfd < FD_MAX_TRACK)
    {
        fd_mark_in_use(realfd);
    }
    return realfd; /* might be negative if real fileno fails. */
}

/* -------------------------------------------------------------------------
 * Minimal main() for embedded Perl
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
 * XS bootstrap table. (Adjust as your Perl config requires.)
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
