#include <unistd.h>

// Stub implementations for missing system calls
int getuid() { return 0; }
int geteuid() { return 0; }
int getgid() { return 0; }
int getegid() { return 0; }
