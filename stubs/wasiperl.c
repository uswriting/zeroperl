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

// Include the generated header for the packed prefix.
// This header must define the following symbols:
//   - PACKFS_BUILTIN_PREFIX (e.g. "/my/perl")
//   - size_t packfs_builtin_files_num
//   - const char* packfs_builtin_abspaths[]
//   - const char* packfs_builtin_safepaths[]
//   - const char* packfs_builtin_starts[]
//   - const char* packfs_builtin_ends[]
#include "perlpack.h"

extern char **environ;

// -------------------------------------------------------------------------
// Extern declarations for the underlying ("real") functions.
// These symbols are provided via linker wrapping (e.g. --wrap=open, etc.)
extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_read(int fd, void *buf, size_t count);
extern int __real_access(const char *path, int flags);
extern off_t __real_lseek(int fd, off_t offset, int whence);
extern int __real_stat(const char *restrict path, struct stat *restrict statbuf);
extern int __real_fstat(int fd, struct stat *statbuf);
extern FILE *__real_fopen(const char *path, const char *mode);
extern int __real_fileno(FILE *stream);

// -------------------------------------------------------------------------
// Configuration constants for the virtual filesystem.
#define packfs_filefd_min 1000000000
#define packfs_filefd_max 1000001000
#define packfs_filefd_array_sz (packfs_filefd_max - packfs_filefd_min)
#define packfs_filepath_max_len 128

// Global arrays to track open virtual file streams.
int packfs_filefd[packfs_filefd_array_sz] = {0};
FILE *packfs_fileptr[packfs_filefd_array_sz] = {0};
size_t packfs_filesize[packfs_filefd_array_sz] = {0};

// -------------------------------------------------------------------------
// Helper routines
// -------------------------------------------------------------------------

// Remove duplicate slashes from src and write into dst.
void packfs_sanitize_path(char *dst, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < packfs_filepath_max_len - 1; i++)
    {
        if (i > 0 && src[i] == '/' && src[i - 1] == '/')
            continue;
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

// Try to open a file from the packed prefix.
// Returns a FILE* if the sanitized path begins with PACKFS_BUILTIN_PREFIX
// and the file is found in the packed arrays; otherwise returns NULL.
// If out is non-NULL, *out is set to the FILE*.
int packfs_open(const char *path, FILE **out)
{
    char path_sanitized[packfs_filepath_max_len];
    packfs_sanitize_path(path_sanitized, path);

    FILE *fileptr = NULL;
    size_t filesize = 0;
    if (packfs_builtin_files_num > 0 &&
        strncmp(path_sanitized, PACKFS_BUILTIN_PREFIX, strlen(PACKFS_BUILTIN_PREFIX)) == 0)
    {
        for (size_t i = 0; i < packfs_builtin_files_num; i++)
        {
            if (strcmp(path_sanitized, packfs_builtin_abspaths[i]) == 0)
            {
                filesize = (size_t)(packfs_builtin_ends[i] - packfs_builtin_starts[i]);
                fileptr = fmemopen((void *)packfs_builtin_starts[i], filesize, "r");
                break;
            }
        }
    }
    if (out != NULL)
        *out = fileptr;
    if (fileptr)
    {
        for (size_t k = 0; k < packfs_filefd_array_sz; k++)
        {
            if (packfs_filefd[k] == 0)
            {
                packfs_filefd[k] = packfs_filefd_min + k;
                packfs_fileptr[k] = fileptr;
                packfs_filesize[k] = filesize;
                return packfs_filefd[k];
            }
        }
    }
    return -1;
}

// Close a virtual file.
int packfs_close(int fd)
{
    if (fd < packfs_filefd_min || fd >= packfs_filefd_max)
        return -2;
    for (size_t k = 0; k < packfs_filefd_array_sz; k++)
    {
        if (packfs_filefd[k] == fd)
        {
            packfs_filefd[k] = 0;
            packfs_filesize[k] = 0;
            int res = fclose(packfs_fileptr[k]);
            packfs_fileptr[k] = NULL;
            return res;
        }
    }
    return -2;
}

// Find the FILE* corresponding to a fake descriptor,
// or if a FILE* is given, return a pointer to the fake descriptor.
void *packfs_find(int fd, FILE *ptr)
{
    if (ptr != NULL)
    {
        for (size_t k = 0; k < packfs_filefd_array_sz; k++)
        {
            if (packfs_fileptr[k] == ptr)
                return &packfs_filefd[k];
        }
        return NULL;
    }
    else
    {
        if (fd < packfs_filefd_min || fd >= packfs_filefd_max)
            return NULL;
        for (size_t k = 0; k < packfs_filefd_array_sz; k++)
        {
            if (packfs_filefd[k] == fd)
                return packfs_fileptr[k];
        }
    }
    return NULL;
}

// Read from a virtual file.
ssize_t packfs_read(int fd, void *buf, size_t count)
{
    FILE *ptr = packfs_find(fd, NULL);
    if (!ptr)
        return -1;
    return (ssize_t)fread(buf, 1, count, ptr);
}

// Seek in a virtual file.
int packfs_seek(int fd, long offset, int whence)
{
    FILE *ptr = packfs_find(fd, NULL);
    if (!ptr)
        return -1;
    return fseek(ptr, offset, whence);
}

// Check access for a virtual file.
int packfs_access(const char *path)
{
    char path_sanitized[packfs_filepath_max_len];
    packfs_sanitize_path(path_sanitized, path);
    if (strncmp(path_sanitized, PACKFS_BUILTIN_PREFIX, strlen(PACKFS_BUILTIN_PREFIX)) == 0)
    {
        for (size_t i = 0; i < packfs_builtin_files_num; i++)
        {
            if (strcmp(path_sanitized, packfs_builtin_abspaths[i]) == 0)
                return 0;
        }
        return -1;
    }
    return -2;
}

// Provide a minimal stat for a virtual file.
// If path is given, search by name; if fd is provided, search by file descriptor.
int packfs_stat(const char *path, int fd, struct stat *statbuf)
{
    char path_sanitized[packfs_filepath_max_len];
    if (path != NULL)
        packfs_sanitize_path(path_sanitized, path);

    if (path != NULL &&
        strncmp(path_sanitized, PACKFS_BUILTIN_PREFIX, strlen(PACKFS_BUILTIN_PREFIX)) == 0)
    {
        for (size_t i = 0; i < packfs_builtin_files_num; i++)
        {
            if (strcmp(path_sanitized, packfs_builtin_abspaths[i]) == 0)
            {
                memset(statbuf, 0, sizeof(struct stat));
                statbuf->st_size = (off_t)(packfs_builtin_ends[i] - packfs_builtin_starts[i]);
                statbuf->st_mode = S_IFREG;
                return 0;
            }
        }
        return -1;
    }
    if (fd >= 0 && fd >= packfs_filefd_min && fd < packfs_filefd_max)
    {
        for (size_t k = 0; k < packfs_filefd_array_sz; k++)
        {
            if (packfs_filefd[k] == fd)
            {
                memset(statbuf, 0, sizeof(struct stat));
                statbuf->st_size = packfs_filesize[k];
                statbuf->st_mode = S_IFREG;
                return 0;
            }
        }
        return -1;
    }
    return -2;
}

// -------------------------------------------------------------------------
// Wrapped functions (to be used via linker wrapping)
// -------------------------------------------------------------------------

FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *f = packfs_open(path, NULL);
    if (f)
        return f;
    return __real_fopen(path, mode);
}

int __wrap_fileno(FILE *stream)
{
    int res = __real_fileno(stream);
    if (res < 0)
    {
        int *ptr = packfs_find(-1, stream);
        res = (ptr == NULL) ? -1 : (*ptr);
    }
    return res;
}

int __wrap_open(const char *path, int flags, ...)
{
    int res = packfs_open(path, NULL);
    if (res >= 0)
        return res;
    va_list args;
    va_start(args, flags);
    int mode = va_arg(args, int);
    int ret = __real_open(path, flags, mode);
    va_end(args);
    return ret;
}

int __wrap_close(int fd)
{
    int ret = packfs_close(fd);
    if (ret >= -1)
        return ret;
    return __real_close(fd);
}

ssize_t __wrap_read(int fd, void *buf, size_t count)
{
    ssize_t res = packfs_read(fd, buf, count);
    if (res >= 0)
        return res;
    return __real_read(fd, buf, count);
}

off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    int res = packfs_seek(fd, (long)offset, whence);
    if (res >= 0)
        return res;
    return __real_lseek(fd, offset, whence);
}

int __wrap_access(const char *path, int flags)
{
    int res = packfs_access(path);
    if (res >= -1)
        return res;
    return __real_access(path, flags);
}

int __wrap_stat(const char *restrict path, struct stat *restrict statbuf)
{
    int res = packfs_stat(path, -1, statbuf);
    if (res >= -1)
        return res;
    return __real_stat(path, statbuf);
}

int __wrap_fstat(int fd, struct stat *statbuf)
{
    int res = packfs_stat(NULL, fd, statbuf);
    if (res >= -1)
        return res;
    return __real_fstat(fd, statbuf);
}

// -------------------------------------------------------------------------
// Perl bootstrap routines
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// Main: Initialize and run the Perl interpreter.
// -------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int exitstatus;

    PERL_SYS_INIT3(&argc, &argv, &environ);
    PerlInterpreter *myperl = perl_alloc();
    if (myperl == NULL)
        return -1;
    perl_construct(myperl);
    PL_exit_flags &= ~PERL_EXIT_DESTRUCT_END; // Do minimal cleanup for WASI

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
