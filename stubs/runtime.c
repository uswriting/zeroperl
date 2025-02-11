#include "asyncify.h"
#include "machine.h"
#include "setjmp.h"
#include <stdlib.h>

int asyncjmp_rt_start(int(main)(int argc, char **argv), int argc, char **argv)
{
    int result;
    void *asyncify_buf;
    void *arg0 = NULL, *arg1 = NULL;

    while (1)
    {
        result = main(argc, argv);

        // NOTE: it's important to call 'asyncify_stop_unwind' here instead in
        // asyncjmp_handle_jmp_unwind because unless that, Asyncify inserts another
        // unwind check here and it unwinds to the root frame.
        asyncify_stop_unwind();

        if ((asyncify_buf = asyncjmp_handle_jmp_unwind()) != NULL)
        {
            asyncify_start_rewind(asyncify_buf);
            continue;
        }
        if ((asyncify_buf = asyncjmp_handle_scan_unwind()) != NULL)
        {
            asyncify_start_rewind(asyncify_buf);
            continue;
        }

        break;
    }
    return result;
}