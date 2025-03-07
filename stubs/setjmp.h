#ifndef ASYNCJMP_SUPPORT_SETJMP_H
#define ASYNCJMP_SUPPORT_SETJMP_H

#include <stdbool.h>

#ifndef WASM_SETJMP_STACK_BUFFER_SIZE
#define WASM_SETJMP_STACK_BUFFER_SIZE 32768
#endif

struct __asyncjmp_asyncify_jmp_buf
{
    void *top;
    void *end;
    char buffer[WASM_SETJMP_STACK_BUFFER_SIZE];
};

typedef struct
{
    // Internal Asyncify buffer space to save execution context
    struct __asyncjmp_asyncify_jmp_buf setjmp_buf;
    // Internal Asyncify buffer space used while unwinding from longjmp
    // but never used for rewinding.
    struct __asyncjmp_asyncify_jmp_buf *longjmp_buf_ptr;
    // Used to save top address of Asyncify stack `setjmp_buf`, which is
    // overwritten during first rewind.
    void *dst_buf_top;
    // A payload value given by longjmp and returned by setjmp for the second time
    int payload;
    // Internal state field
    int state;
} asyncjmp_jmp_buf;

// noinline to avoid breaking Asyncify assumption
__attribute__((noinline)) int _asyncjmp_setjmp(asyncjmp_jmp_buf *env);
__attribute__((noinline)) void _asyncjmp_longjmp(asyncjmp_jmp_buf *env, int payload);

#define asyncjmp_setjmp(env) ((env).state = 0, _asyncjmp_setjmp(&(env)))

// NOTE: Why is `_asyncjmp_longjmp` not `noreturn`? Why put `unreachable` in the call site?
// Asyncify expects that `_asyncjmp_longjmp` returns its control, and Asyncify inserts a return
// for unwinding after the call. This means that "`_asyncjmp_longjmp` returns its control but the
// next line in the caller (C level) won't be executed".
// On the other hand, `noreturn` means the callee won't return its control to the caller,
// so compiler can assume that a function with the attribute won't reach the end of the function.
// Therefore `_asyncjmp_longjmp`'s semantics is not exactly same as `noreturn`.
#define asyncjmp_longjmp(env, payload) (_asyncjmp_longjmp(&env, payload), __builtin_unreachable())

// Returns the Asyncify buffer of next rewinding if unwound for setjmp capturing or longjmp.
// Used by the top level Asyncify handling in wasm/runtime.c
void *asyncjmp_handle_jmp_unwind(void);

//
// POSIX-compatible declarations
//

typedef asyncjmp_jmp_buf jmp_buf;

#define setjmp(env) asyncjmp_setjmp(env)
#define longjmp(env, payload) asyncjmp_longjmp(env, payload)

typedef void (*asyncjmp_try_catch_func_t)(void *ctx);

struct asyncjmp_try_catch
{
    asyncjmp_try_catch_func_t try_f;
    asyncjmp_try_catch_func_t catch_f;
    void *context;
    int state;
};

//
// Lightweight try-catch API without unwinding to root frame.
//

void asyncjmp_try_catch_init(struct asyncjmp_try_catch *try_catch,
                             asyncjmp_try_catch_func_t try_f,
                             asyncjmp_try_catch_func_t catch_f,
                             void *context);

// Run, catch longjmp thrown by run, and re-catch longjmp thrown by catch, ...
//
// 1. run try_f of try_catch struct
// 2. catch longjmps with the given target jmp_buf or exit
// 3. run catch_f if not NULL, otherwise exit
// 4. catch longjmps with the given target jmp_buf or exit
// 5. repeat from step 3
//
// NOTICE: This API assumes that all longjmp targeting the given jmp_buf are NOT called
// after the function that called this function has exited.
//
void asyncjmp_try_catch_loop_run(struct asyncjmp_try_catch *try_catch, asyncjmp_jmp_buf *target);

//
// Main function startup wrapper
//

int asyncjmp_rt_start(int(main)(int argc, char **argv), int argc, char **argv);

#endif
