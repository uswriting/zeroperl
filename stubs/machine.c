#include "machine.h"
#include "asyncify.h"
#include <stdint.h>
#include <stdlib.h>

#ifndef WASM_SCAN_STACK_BUFFER_SIZE
#define WASM_SCAN_STACK_BUFFER_SIZE 6144
#endif

struct asyncify_buf
{
    void *top;
    void *end;
    uint8_t buffer[WASM_SCAN_STACK_BUFFER_SIZE];
};

static void init_asyncify_buf(struct asyncify_buf *buf)
{
    buf->top = &buf->buffer[0];
    buf->end = &buf->buffer[WASM_SCAN_STACK_BUFFER_SIZE];
}

static void *_asyncjmp_active_scan_buf = NULL;

void asyncjmp_scan_locals(asyncjmp_scan_func scan)
{
    static struct asyncify_buf buf;
    static int spilling = 0;
    if (!spilling)
    {
        spilling = 1;
        init_asyncify_buf(&buf);
        _asyncjmp_active_scan_buf = &buf;
        asyncify_start_unwind(&buf);
    }
    else
    {
        asyncify_stop_rewind();
        spilling = 0;
        _asyncjmp_active_scan_buf = NULL;
        scan(buf.top, buf.end);
    }
}

static void *asyncjmp_stack_base = NULL;

__attribute__((constructor)) int asyncjmp_record_stack_base(void)
{
    asyncjmp_stack_base = asyncjmp_get_stack_pointer();
    return 0;
}

void *asyncjmp_stack_get_base(void) { return asyncjmp_stack_base; }

void *asyncjmp_handle_scan_unwind(void) { return _asyncjmp_active_scan_buf; }