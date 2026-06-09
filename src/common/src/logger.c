/*
 * logger.c — implementation of the structured logger
 *
 * Uses fprintf to a FILE* opened at startup.
 * Thread safety: in production this would use a mutex around fwrite.
 * Here it is single-threaded for simplicity.
 */
#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* Module-level state: file handle and minimum level filter */
static FILE       *g_log_file  = NULL;
static log_level_t g_min_level = LOG_INFO;

/* Human-readable level labels for log output */
static const char *level_names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

void logger_init(const char *log_file, log_level_t min_level)
{
    g_min_level = min_level;

    /* Append to existing log file so restarts don't erase history */
    g_log_file = fopen(log_file, "a");
    if (!g_log_file) {
        /* Fall back to stderr if the log file can't be opened
         * (e.g. /var/log/ permissions issue in development) */
        g_log_file = stderr;
        fprintf(stderr, "[logger] WARNING: could not open %s, falling back to stderr\n",
                log_file);
    }
}

void logger_log(log_level_t level, const char *module, const char *fmt, ...)
{
    /* Drop messages below the configured minimum level */
    if (level < g_min_level) return;
    if (!g_log_file) return;

    /* Build timestamp string: "2026-04-30T14:22:15" */
    time_t    now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);  /* localtime_r is thread-safe (unlike localtime) */
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm_info);

    /* Write: [2026-04-30T14:22:15] [INFO ] [inventory] Reserved 5 of SKU-001 */
    fprintf(g_log_file, "[%s] [%s] [%s] ", timestamp, level_names[level], module);

    /* Forward the variadic args to vfprintf for printf-style formatting */
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);

    fprintf(g_log_file, "\n");

    /* Flush immediately on WARN/ERROR so messages appear before a crash */
    if (level >= LOG_WARN) fflush(g_log_file);
}

void logger_close(void)
{
    if (g_log_file && g_log_file != stderr) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
}
