#ifndef ASYNCJMP_SUPPORT_MACHINE_H
#define ASYNCJMP_SUPPORT_MACHINE_H

// Function pointer used as scan callbacks
typedef void (*asyncjmp_scan_func)(void *, void *);

// Scan WebAssembly locals in the all call stack (like registers) spilled by Asyncify
// Used by conservative GC
void asyncjmp_scan_locals(asyncjmp_scan_func scan);

// Get base address of userland C-stack memory space in WebAssembly. Used by conservative GC
void *asyncjmp_stack_get_base(void);

// Get the current stack pointer
void *asyncjmp_get_stack_pointer(void);

// Set the current stack pointer
void asyncjmp_set_stack_pointer(void *sp);

// Returns the Asyncify buffer of next rewinding if unwound for spilling locals.
// Used by the top level Asyncify handling in wasm/runtime.c
void *asyncjmp_handle_scan_unwind(void);

#endif