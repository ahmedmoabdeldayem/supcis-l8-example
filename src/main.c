/* Required for struct timespec and nanosleep under -std=c99 */
#define _POSIX_C_SOURCE 200809L

/*
 * SuPCIS-L8 — Main entry point
 *
 * Startup sequence:
 *   1. Parse config directory argument
 *   2. Initialise logger
 *   3. Load customer configuration (warehouse.yaml, erp_mappings.yaml, ...)
 *   4. Connect to Oracle database
 *   5. Start HTTP listener and enter event loop
 *   6. On SIGTERM/SIGINT: drain requests, close DB, flush logs, exit cleanly
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>     /* struct timespec, nanosleep */
#include "logger.h"
#include "db_connection.h"
#include "rest_handler.h"

/* Set by signal handler — main loop checks this to exit gracefully */
static volatile int g_shutdown = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* ── Config loading (simplified — real impl parses YAML) ────────────────────── */
typedef struct {
    char tns_alias[64];
    char db_user[32];
    char db_pass[64];
    int  http_port;
    char log_file[128];
} app_config_t;

static int load_config(const char *config_dir, app_config_t *cfg)
{
    /* In production this reads config/default/application.conf and
     * config/default/database.conf via a YAML parser.
     * Here we use environment variables as a safe fallback. */
    const char *tns  = getenv("DB_TNS");
    const char *user = getenv("DB_USER");
    const char *pass = getenv("DB_PASS");
    const char *port = getenv("HTTP_PORT");

    if (!tns || !user || !pass) {
        fprintf(stderr, "Missing required env vars: DB_TNS, DB_USER, DB_PASS\n");
        return -1;
    }

    strncpy(cfg->tns_alias, tns,  sizeof(cfg->tns_alias)  - 1);
    strncpy(cfg->db_user,   user, sizeof(cfg->db_user)    - 1);
    strncpy(cfg->db_pass,   pass, sizeof(cfg->db_pass)    - 1);
    cfg->http_port = port ? atoi(port) : 8080;

    snprintf(cfg->log_file, sizeof(cfg->log_file),
             "/var/log/supcis-l8/supcis.log");

    (void)config_dir; /* used to locate customer YAML in full implementation */
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: supcis-l8 <config-dir>\n");
        return EXIT_FAILURE;
    }

    /* Step 1: register signal handlers for graceful shutdown */
    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    /* Step 2: load configuration */
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (load_config(argv[1], &cfg) != 0) {
        return EXIT_FAILURE;
    }

    /* Step 3: initialise logger */
    logger_init(cfg.log_file, LOG_INFO);
    LOG_INFO("main", "SuPCIS-L8 starting — config-dir: %s, port: %d",
             argv[1], cfg.http_port);

    /* Step 4: connect to Oracle */
    db_handle_t *db = NULL;
    supcis_result_t rc = db_connect(cfg.tns_alias, cfg.db_user, cfg.db_pass, &db);
    if (rc != SUPCIS_OK) {
        LOG_ERROR("main", "Database connection failed (TNS=%s)", cfg.tns_alias);
        logger_close();
        return EXIT_FAILURE;
    }
    LOG_INFO("main", "Database connected (TNS=%s)", cfg.tns_alias);

    /* Step 5: start HTTP listener (microhttpd in full implementation) */
    LOG_INFO("main", "HTTP listener ready on port %d", cfg.http_port);

    /* Step 6: event loop — process incoming REST requests until shutdown signal */
    while (!g_shutdown) {
        /*
         * In the full implementation this uses MHD_run() (libmicrohttpd) or
         * a poll/epoll loop. Each incoming request is dispatched through
         * rest_handler_dispatch(), which routes to the correct domain handler.
         *
         * Simplified here to avoid pulling in the HTTP server dependency.
         */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10 ms */
        nanosleep(&ts, NULL);
    }

    /* Step 7: graceful shutdown */
    LOG_INFO("main", "Shutdown signal received — closing resources");
    db_disconnect(db);
    logger_close();

    return EXIT_SUCCESS;
}
