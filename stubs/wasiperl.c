#include <stdio.h>
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

static void xs_init(pTHX);
static PerlInterpreter *my_perl;

int main(int argc, char *argv[]) {
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

static void xs_init(pTHX) {
    EXTERN_C void boot_DynaLoader(pTHX_ CV* cv);
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, __FILE__);
}
