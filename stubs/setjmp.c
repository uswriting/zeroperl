/*
 This is a WebAssembly userland setjmp/longjmp implementation based on
 Binaryen's Asyncify. Inspired by Alon Zakai's snippet released under the MIT
 License:
 * https://github.com/kripken/talks/blob/991fb1e4b6d7e4b0ea6b3e462d5643f11d422771/jmp.c

 WebAssembly doesn't have context-switching mechanism for now, so emulate it by
 Asyncify, which transforms WebAssembly binary to unwind/rewind the execution
 point and store/restore locals.

 The basic concept of this implementation is:
 1. setjmp captures the current execution context by unwinding to the root
 frame, then immediately rewind to the setjmp call using the captured context.
 The context is saved in jmp_buf.
 2. longjmp unwinds to the root frame and rewinds to a setjmp call re-using a
 passed jmp_buf.

 This implementation also supports switching context across different call stack
 (non-standard)

 This approach is good at behavior reproducibility and self-containedness
 compared to Emscripten's JS exception approach. However this is super expensive
 because Asyncify inserts many glue code to control execution point in userland.

 This implementation will be replaced with future stack-switching feature.
 */
#include "setjmp.h"
#include "asyncify.h"
#include "machine.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef ASYNCJMP_ENABLE_DEBUG_LOG
#define STRINGIZE_HELPER(x) #x
#define STRINGIZE(x) STRINGIZE_HELPER(x)
#include <unistd.h>
#include <wasi/api.h>
// NOTE: We can't use printf() and most of library function that are
// Asyncified due to the use of them in the application itself.
// Use of printf() causes "unreachable" error because Asyncified
// function misunderstands Asyncify's internal state during
// start_unwind()...stop_unwind() and start_rewind()...stop_rewind().
#define ASYNCJMP_DEBUG_LOG_INTERNAL(msg)                                          \
    do                                                                            \
    {                                                                             \
        const uint8_t *msg_start = (uint8_t *)msg;                                \
        const uint8_t *msg_end = msg_start;                                       \
        for (; *msg_end != '\0'; msg_end++)                                       \
        {                                                                         \
        }                                                                         \
        __wasi_ciovec_t iov = {.buf = msg_start, .buf_len = msg_end - msg_start}; \
        size_t nwritten;                                                          \
        __wasi_fd_write(STDERR_FILENO, &iov, 1, &nwritten);                       \
    } while (0)
#define ASYNCJMP_DEBUG_LOG(msg) \
    ASYNCJMP_DEBUG_LOG_INTERNAL(__FILE__ ":" STRINGIZE(__LINE__) ": " msg "\n")
#else
#define ASYNCJMP_DEBUG_LOG(msg)
#endif

enum asyncjmp_jmp_buf_state
{
    // Initial state
    JMP_BUF_STATE_INITIALIZED = 0,
    // Unwinding to the root or rewinding to the setjmp call
    // to capture the current execution context
    JMP_BUF_STATE_CAPTURING = 1,
    // Ready for longjmp
    JMP_BUF_STATE_CAPTURED = 2,
    // Unwinding to the root or rewinding to the setjmp call
    // to restore the execution context
    JMP_BUF_STATE_RETURNING = 3,
};

void async_buf_init(struct __asyncjmp_asyncify_jmp_buf *buf)
{
    buf->top = &buf->buffer[0];
    buf->end = &buf->buffer[WASM_SETJMP_STACK_BUFFER_SIZE];
}

// Global unwinding/rewinding jmpbuf state
static asyncjmp_jmp_buf *_asyncjmp_active_jmpbuf;
void *pl_asyncify_unwind_buf;

__attribute__((noinline)) int _asyncjmp_setjmp_internal(asyncjmp_jmp_buf *env)
{
    ASYNCJMP_DEBUG_LOG("enter _asyncjmp_setjmp_internal");
    switch (env->state)
    {
    case JMP_BUF_STATE_INITIALIZED:
    {
        ASYNCJMP_DEBUG_LOG("  JMP_BUF_STATE_INITIALIZED");
        env->state = JMP_BUF_STATE_CAPTURING;
        env->payload = 0;
        env->longjmp_buf_ptr = NULL;
        _asyncjmp_active_jmpbuf = env;
        async_buf_init(&env->setjmp_buf);
        asyncify_start_unwind(&env->setjmp_buf);
        return -1; // return a dummy value
    }
    case JMP_BUF_STATE_CAPTURING:
    {
        asyncify_stop_rewind();
        ASYNCJMP_DEBUG_LOG("  JMP_BUF_STATE_CAPTURING");
        env->state = JMP_BUF_STATE_CAPTURED;
        _asyncjmp_active_jmpbuf = NULL;
        return 0;
    }
    case JMP_BUF_STATE_RETURNING:
    {
        asyncify_stop_rewind();
        ASYNCJMP_DEBUG_LOG("  JMP_BUF_STATE_RETURNING");
        env->state = JMP_BUF_STATE_CAPTURED;
        free(env->longjmp_buf_ptr);
        _asyncjmp_active_jmpbuf = NULL;
        return env->payload;
    }
    default:
        assert(0 && "unexpected state");
    }
    return 0;
}

void _asyncjmp_longjmp(asyncjmp_jmp_buf *env, int value)
{
    ASYNCJMP_DEBUG_LOG("enter _asyncjmp_longjmp");
    assert(env->state == JMP_BUF_STATE_CAPTURED);
    assert(value != 0);
    env->state = JMP_BUF_STATE_RETURNING;
    env->payload = value;
    // Asyncify buffer built during unwinding for longjmp will not
    // be used to rewind, so re-use static-variable.
    static struct __asyncjmp_asyncify_jmp_buf tmp_longjmp_buf;
    env->longjmp_buf_ptr = &tmp_longjmp_buf;
    _asyncjmp_active_jmpbuf = env;
    async_buf_init(env->longjmp_buf_ptr);
    asyncify_start_unwind(env->longjmp_buf_ptr);
}

enum try_catch_phase
{
    TRY_CATCH_PHASE_MAIN = 0,
    TRY_CATCH_PHASE_RESCUE = 1,
};

void asyncjmp_try_catch_init(struct asyncjmp_try_catch *try_catch,
                             asyncjmp_try_catch_func_t try_f,
                             asyncjmp_try_catch_func_t catch_f, void *context)
{
    try_catch->state = TRY_CATCH_PHASE_MAIN;
    try_catch->try_f = try_f;
    try_catch->catch_f = catch_f;
    try_catch->context = context;
}

// NOTE: This function is not processed by Asyncify due to a call of
// asyncify_stop_rewind
void asyncjmp_try_catch_loop_run(struct asyncjmp_try_catch *try_catch,
                                 asyncjmp_jmp_buf *target)
{
    extern void *pl_asyncify_unwind_buf;
    extern asyncjmp_jmp_buf *_asyncjmp_active_jmpbuf;

    target->state = JMP_BUF_STATE_CAPTURED;

    switch ((enum try_catch_phase)try_catch->state)
    {
    case TRY_CATCH_PHASE_MAIN:
        // may unwind
        try_catch->try_f(try_catch->context);
        break;
    case TRY_CATCH_PHASE_RESCUE:
        if (try_catch->catch_f)
        {
            // may unwind
            try_catch->catch_f(try_catch->context);
        }
        break;
    }

    {
        // catch longjmp with target jmp_buf
        while (pl_asyncify_unwind_buf && _asyncjmp_active_jmpbuf == target)
        {
            // do similar steps setjmp does when JMP_BUF_STATE_RETURNING

            // stop unwinding
            // (but call stop_rewind to update the asyncify state to "normal" from
            // "unwind")
            asyncify_stop_rewind();
            // clear the active jmpbuf because it's already stopped
            _asyncjmp_active_jmpbuf = NULL;
            // reset jmpbuf state to be able to unwind again
            target->state = JMP_BUF_STATE_CAPTURED;
            // move to catch loop phase
            try_catch->state = TRY_CATCH_PHASE_RESCUE;
            if (try_catch->catch_f)
            {
                try_catch->catch_f(try_catch->context);
            }
        }
        // no unwind or unrelated unwind, then exit
    }
}

void *asyncjmp_handle_jmp_unwind(void)
{
    ASYNCJMP_DEBUG_LOG("enter asyncjmp_handle_jmp_unwind");
    if (!_asyncjmp_active_jmpbuf)
    {
        return NULL;
    }

    switch (_asyncjmp_active_jmpbuf->state)
    {
    case JMP_BUF_STATE_CAPTURING:
        ASYNCJMP_DEBUG_LOG("  JMP_BUF_STATE_CAPTURING");
        // save the captured Asyncify stack top
        _asyncjmp_active_jmpbuf->dst_buf_top =
            _asyncjmp_active_jmpbuf->setjmp_buf.top;
        break;
    case JMP_BUF_STATE_RETURNING:
        ASYNCJMP_DEBUG_LOG("  JMP_BUF_STATE_RETURNING");
        // restore the saved Asyncify stack top
        _asyncjmp_active_jmpbuf->setjmp_buf.top =
            _asyncjmp_active_jmpbuf->dst_buf_top;
        break;
    default:
        assert(0 && "unexpected state");
    }
    return &_asyncjmp_active_jmpbuf->setjmp_buf;
}
