#include <unistd.h>

// Stub implementations for missing system calls

// Stubs for user/group management
int getuid() { return 0; }
int geteuid() { return 0; }
int getgid() { return 0; }
int getegid() { return 0; }
int setuid(int uid) { return 0; }
int setgid(int gid) { return 0; }

// Stub for process signaling
int kill(pid_t pid, int sig) { return 0; }

// Stub for file descriptor duplication
int dup(int oldfd) { return -1; }

// Stub for file mode creation mask
mode_t umask(mode_t mask) { return 0; }

// Stub for executing a program
int execvp(const char *file, char *const argv[]) { return -1; }
int execl(const char *path, const char *arg, ...) { return -1; }
