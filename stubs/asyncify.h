#ifndef ASYNCJMP_SUPPORT_ASYNCIFY_H
#define ASYNCJMP_SUPPORT_ASYNCIFY_H

__attribute__((import_module("asyncify"), import_name("start_unwind"))) void asyncify_start_unwind(void *buf);
#define asyncify_start_unwind(buf)         \
    do                                     \
    {                                      \
        extern void *pl_asyncify_unwind_buf; \
        pl_asyncify_unwind_buf = (buf);      \
        asyncify_start_unwind((buf));      \
    } while (0)

__attribute__((import_module("asyncify"), import_name("stop_unwind"))) void asyncify_stop_unwind(void);
#define asyncify_stop_unwind()             \
    do                                     \
    {                                      \
        extern void *pl_asyncify_unwind_buf; \
        pl_asyncify_unwind_buf = NULL;       \
        asyncify_stop_unwind();            \
    } while (0)

__attribute__((import_module("asyncify"), import_name("start_rewind"))) void asyncify_start_rewind(void *buf);

__attribute__((import_module("asyncify"), import_name("stop_rewind"))) void asyncify_stop_rewind(void);

__attribute__((import_module("asyncify"), import_name("get_state")))
int asyncify_get_state(void);

#endif
