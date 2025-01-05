#ifndef #ifndef STUBS_H
#define STUBS_H

// Inline stub implementations for missing system calls
static inline int getuid() { return 0; }
static inline int geteuid() { return 0; }
static inline int getgid() { return 0; }
static inline int getegid() { return 0; }

#endif // STUBS_H
