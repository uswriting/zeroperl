// grp.h (stub)

#ifndef _GRP_H
#define _GRP_H

#include <sys/types.h>

struct group {
    gid_t gr_gid;
    char *gr_name; 
    char **gr_mem; 
};

struct group *getgrnam(const char *name) {
    return NULL; 
}

struct group *getgrgid(gid_t gid) {
    return NULL; 
}

#endif // _GRP_H
