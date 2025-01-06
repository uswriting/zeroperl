#include <stdio.h>
#include <locale.h>
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

static void xs_init(pTHX);
static PerlInterpreter *my_perl;

int main(int argc, char *argv[]) {
    puts("wasiperl: starting up...");
    setlocale(LC_ALL, "C.UTF-8");

    
    PERL_SYS_INIT3(&argc, &argv, NULL);
    
    my_perl = perl_alloc();
    perl_construct(my_perl);
    
    if (!perl_parse(my_perl, xs_init, argc, argv, NULL)) {
        perl_run(my_perl);
    }
    
    perl_destruct(my_perl);
    perl_free(my_perl);
    PERL_SYS_TERM();
    return 0;
}

/* External module bootstraps */
EXTERN_C void boot_DynaLoader (pTHX_ CV* cv);
EXTERN_C void boot_mro (pTHX_ CV* cv);
EXTERN_C void boot_Devel__Peek (pTHX_ CV* cv);
EXTERN_C void boot_File__DosGlob (pTHX_ CV* cv);
EXTERN_C void boot_Sys__Syslog (pTHX_ CV* cv);
EXTERN_C void boot_Sys__Hostname (pTHX_ CV* cv);
EXTERN_C void boot_PerlIO__via (pTHX_ CV* cv);
EXTERN_C void boot_PerlIO__mmap (pTHX_ CV* cv);
EXTERN_C void boot_PerlIO__encoding (pTHX_ CV* cv);
EXTERN_C void boot_B (pTHX_ CV* cv);
EXTERN_C void boot_attributes (pTHX_ CV* cv);
EXTERN_C void boot_Unicode__Normalize (pTHX_ CV* cv);
EXTERN_C void boot_Unicode__Collate (pTHX_ CV* cv);
EXTERN_C void boot_threads (pTHX_ CV* cv);
EXTERN_C void boot_threads__shared (pTHX_ CV* cv);
EXTERN_C void boot_IPC__SysV (pTHX_ CV* cv);
EXTERN_C void boot_re (pTHX_ CV* cv);
EXTERN_C void boot_Digest__MD5 (pTHX_ CV* cv);
EXTERN_C void boot_Digest__SHA (pTHX_ CV* cv);
EXTERN_C void boot_SDBM_File (pTHX_ CV* cv);
EXTERN_C void boot_Math__BigInt__FastCalc (pTHX_ CV* cv);
EXTERN_C void boot_Data__Dumper (pTHX_ CV* cv);
EXTERN_C void boot_I18N__Langinfo (pTHX_ CV* cv);
EXTERN_C void boot_Time__HiRes (pTHX_ CV* cv);
EXTERN_C void boot_Time__Piece (pTHX_ CV* cv);
EXTERN_C void boot_IO (pTHX_ CV* cv);
EXTERN_C void boot_Hash__Util__FieldHash (pTHX_ CV* cv);
EXTERN_C void boot_Hash__Util (pTHX_ CV* cv);
EXTERN_C void boot_Filter__Util__Call (pTHX_ CV* cv);
EXTERN_C void boot_Encode__Unicode (pTHX_ CV* cv);
EXTERN_C void boot_Encode (pTHX_ CV* cv);
EXTERN_C void boot_Encode__JP (pTHX_ CV* cv);
EXTERN_C void boot_Encode__KR (pTHX_ CV* cv);
EXTERN_C void boot_Encode__EBCDIC (pTHX_ CV* cv);
EXTERN_C void boot_Encode__CN (pTHX_ CV* cv);
EXTERN_C void boot_Encode__Symbol (pTHX_ CV* cv);
EXTERN_C void boot_Encode__Byte (pTHX_ CV* cv);
EXTERN_C void boot_Encode__TW (pTHX_ CV* cv);
EXTERN_C void boot_Compress__Raw__Zlib (pTHX_ CV* cv);
EXTERN_C void boot_Compress__Raw__Bzip2 (pTHX_ CV* cv);
EXTERN_C void boot_MIME__Base64 (pTHX_ CV* cv);
EXTERN_C void boot_Cwd (pTHX_ CV* cv);
EXTERN_C void boot_Storable (pTHX_ CV* cv);
EXTERN_C void boot_List__Util (pTHX_ CV* cv);
EXTERN_C void boot_Fcntl (pTHX_ CV* cv);
EXTERN_C void boot_Opcode (pTHX_ CV* cv);

static void xs_init(pTHX) {
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    /* DynaLoader is a special case */
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
    newXS("mro::bootstrap", boot_mro, file);
    newXS("Devel::Peek::bootstrap", boot_Devel__Peek, file);
    newXS("File::DosGlob::bootstrap", boot_File__DosGlob, file);
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
    newXS("Time::HiRes::bootstrap", boot_Time__HiRes, file);
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
