#include <unistd.h>

// Define mode_t as unsigned int if not already defined
typedef unsigned int mode_t;

// Stub implementations for missing system calls

// User and group management
int getuid() { return 0; }
int geteuid() { return 0; }
int getgid() { return 0; }
int getegid() { return 0; }
int setuid(int uid) { return 0; }
int setgid(int gid) { return 0; }

// Process signaling
int kill(pid_t pid, int sig) { return 0; }

// File descriptor duplication
int dup(int oldfd) { return -1; }

// File mode creation mask
mode_t umask(mode_t mask) { return 0; }

// Program execution
int execvp(const char *file, char *const argv[]) { return -1; }
int execl(const char *path, const char *arg, ...) { return -1; }
