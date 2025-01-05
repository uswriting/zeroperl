// pwd.h (stub)

#ifndef _PWD_H
#define _PWD_H

#include <sys/types.h> 

// Minimal struct passwd definition
struct passwd {
    uid_t pw_uid; 
};

// Stub implementations of functions
struct passwd *getpwnam(const char *name) {
    return NULL; // Or return a static dummy struct passwd
}

struct passwd *getpwuid(uid_t uid) {
    return NULL; // Or return a static dummy struct passwd
}

// Add other necessary stub functions (getpwent, etc.) as needed
// ...

#endif // _PWD_H
