#ifndef SUPCIS_LOGGER_H
#define SUPCIS_LOGGER_H

/*
 * logger.h — structured logging for all modules
 *
 * HOW LOGGING WORKS IN THIS CODEBASE:
 *   Every module that wants to log calls one of the four macros at the bottom:
 *     LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR
 *
 *   The macros forward to logger_log() which stamps a timestamp, the module
 *   name, the level, and the message — then writes to the configured log file.
 *
 *   In production the log file is /var/log/supcis-l8/supcis.log.
 *   In tests the min_level is usually LOG_WARN so debug noise is suppressed.
 *
 * WHY A MODULE NAME PARAMETER?
 *   A single log file receives output from inventory, order, picking, robot,
 *   and API modules simultaneously. Including the module name in each line
 *   makes it possible to grep for just one area:
 *     grep "\[picking\]" supcis.log
 */

/* ─────────────────────────────────────────────────────────────────────────────
 * LOG LEVELS — ordered from least to most severe.
 *
 *   DEBUG  verbose trace, only useful during development
 *   INFO   normal operation events (order created, wave released, ...)
 *   WARN   something unexpected happened but the system kept running
 *   ERROR  something failed and the caller must handle it
 *
 * The min_level passed to logger_init() acts as a filter:
 *   if min_level == LOG_WARN, DEBUG and INFO messages are silently dropped.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/*
 * logger_init — must be called once at startup before any LOG_* macro is used.
 *   log_file  : path to the output file (created/appended to)
 *   min_level : messages below this level are discarded
 */
void logger_init(const char *log_file, log_level_t min_level);

/*
 * logger_log — the underlying log function. Do NOT call this directly.
 *   Use the LOG_* macros below — they are shorter and pass the right arguments.
 *   The `...` is a printf-style format string + arguments.
 */
void logger_log(log_level_t level, const char *module, const char *fmt, ...);

/*
 * logger_close — flushes and closes the log file. Call at shutdown.
 */
void logger_close(void);

/* ─────────────────────────────────────────────────────────────────────────────
 * CONVENIENCE MACROS
 *
 * Usage:  LOG_INFO("inventory", "Reserved %d of SKU %s", qty, sku);
 *         LOG_ERROR("db",       "Oracle OCI error: %d", oci_code);
 *
 * `mod`  — short string identifying the calling module, e.g. "picking"
 * `...`  — printf-style format + args
 *
 * The ##__VA_ARGS__ trick handles the case where no extra args are given,
 * avoiding a trailing-comma compiler warning.
 * ───────────────────────────────────────────────────────────────────────────── */
#define LOG_DEBUG(mod, ...) logger_log(LOG_DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  logger_log(LOG_INFO,  mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  logger_log(LOG_WARN,  mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) logger_log(LOG_ERROR, mod, __VA_ARGS__)

#endif /* SUPCIS_LOGGER_H */
