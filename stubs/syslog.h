#ifndef _SYSLOG_H
#define _SYSLOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Priority levels */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* Priority masks */
#define LOG_PRIMASK 7
#define LOG_PRI(p) ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(f, p) (((f) << 3) | (p))

/* Facility codes */
#define LOG_KERN     (0 << 3)
#define LOG_USER     (1 << 3)
#define LOG_MAIL     (2 << 3)
#define LOG_DAEMON   (3 << 3)
#define LOG_AUTH     (4 << 3)
#define LOG_SYSLOG   (5 << 3)
#define LOG_LPR      (6 << 3)
#define LOG_NEWS     (7 << 3)
#define LOG_UUCP     (8 << 3)
#define LOG_CRON     (9 << 3)
#define LOG_AUTHPRIV (10 << 3)
#define LOG_FTP      (11 << 3)

#define LOG_LOCAL0   (16 << 3)
#define LOG_LOCAL1   (17 << 3)
#define LOG_LOCAL2   (18 << 3)
#define LOG_LOCAL3   (19 << 3)
#define LOG_LOCAL4   (20 << 3)
#define LOG_LOCAL5   (21 << 3)
#define LOG_LOCAL6   (22 << 3)
#define LOG_LOCAL7   (23 << 3)

/* Facility and priority masks */
#define LOG_NFACILITIES 24
#define LOG_FACMASK 0x3f8
#define LOG_FAC(p) (((p) & LOG_FACMASK) >> 3)

#define LOG_MASK(p) (1 << (p))
#define LOG_UPTO(p) ((1 << ((p) + 1)) - 1)

/* Option flags */
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

/* NOOP implementations of syslog functions */
void openlog(const char *ident, int option, int facility) {
    /* NOOP: No operation performed */
}

void syslog(int priority, const char *format, ...) {
    /* NOOP: No operation performed */
}

void closelog(void) {
    /* NOOP: No operation performed */
}

int setlogmask(int mask) {
    /* NOOP: Returns the provided mask without modification */
    return mask;
}

#ifdef __cplusplus
}
#endif

#endif /* _SYSLOG_H */
