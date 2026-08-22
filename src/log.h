#ifndef SIMPLEPOOL_LOG_H
#define SIMPLEPOOL_LOG_H

#include <stdarg.h>

enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3
};

void log_init(int level);
/* True when a message at `level` would actually be emitted. Lets a caller skip
 * work it only needs for the log line itself — at minimum difficulty the submit
 * path runs tens of thousands of times a second. */
int  log_enabled(int level);
void log_msg(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif /* SIMPLEPOOL_LOG_H */
