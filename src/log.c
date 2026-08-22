#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int g_level = LOG_LVL_INFO;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(int level) {
    pthread_mutex_lock(&g_lock);
    g_level = level;
    pthread_mutex_unlock(&g_lock);
}

static const char *level_tag(int level) {
    switch (level) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_ERROR: return "ERROR";
        default:            return "?    ";
    }
}

int log_enabled(int level) {
    return level >= g_level;
}

void log_msg(int level, const char *fmt, ...) {
    if (level < g_level) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", &tm_buf);

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "%s.%03ld %s ",
            tbuf, ts.tv_nsec / 1000000L, level_tag(level));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_lock);

    va_end(ap);
}
