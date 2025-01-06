#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

// Stubbed jmp_buf structure
typedef struct __jmp_buf_tag {
    char placeholder[256]; // Placeholder for potential state
} jmp_buf[1];

// Stubbed sigjmp_buf structure
typedef jmp_buf sigjmp_buf;

// setjmp returns 0 to simulate no jumps have occurred
static inline int setjmp(jmp_buf env) {
    (void)env; // Suppress unused parameter warnings
    return 0;  // Always return 0
}

// longjmp is a no-op and terminates with an error message
static inline void longjmp(jmp_buf env, int val) {
    (void)env; // Suppress unused parameter warnings
    (void)val; // Suppress unused parameter warnings
    __builtin_trap(); // Aborts the program as there's no jump support
}

// sigsetjmp and siglongjmp are similarly stubbed
static inline int sigsetjmp(sigjmp_buf env, int savemask) {
    (void)env;      // Suppress unused parameter warnings
    (void)savemask; // Suppress unused parameter warnings
    return 0;       // Always return 0
}

static inline void siglongjmp(sigjmp_buf env, int val) {
    (void)env; // Suppress unused parameter warnings
    (void)val; // Suppress unused parameter warnings
    __builtin_trap(); // Aborts the program as there's no jump support
}

// _setjmp and _longjmp are also stubbed
static inline int _setjmp(jmp_buf env) {
    (void)env; // Suppress unused parameter warnings
    return 0;  // Always return 0
}

static inline void _longjmp(jmp_buf env, int val) {
    (void)env; // Suppress unused parameter warnings
    (void)val; // Suppress unused parameter warnings
    __builtin_trap(); // Aborts the program as there's no jump support
}

#ifdef __cplusplus
}
#endif

#endif // _SETJMP_H
