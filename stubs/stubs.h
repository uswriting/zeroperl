#ifndef STUBS_H
#define STUBS_H

#include <unistd.h>

// Inline stub implementations for missing system calls

// User and group management
static inline int getuid() { return 0; }
static inline int geteuid() { return 0; }
static inline int getgid() { return 0; }
static inline int getegid() { return 0; }
static inline int setuid(int uid) { return 0; }
static inline int setgid(int gid) { return 0; }

// Process signaling
static inline int kill(pid_t pid, int sig) { return 0; }

// File descriptor duplication
static inline int dup(int oldfd) { return -1; }

// File mode creation mask
static inline mode_t umask(mode_t mask) { return 0; }

// Program execution
static inline int execvp(const char *file, char *const argv[]) { return -1; }
static inline int execl(const char *path, const char *arg, ...) { return -1; }

#endif // STUBS_H
