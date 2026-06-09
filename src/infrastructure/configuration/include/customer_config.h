#ifndef CUSTOMER_CONFIG_H
#define CUSTOMER_CONFIG_H

/*
 * customer_config.h — per-customer configuration structs
 *
 * Each customer's installation is configured via YAML files in
 * config/customers/<customer_name>/. This header defines the C structs
 * that those YAML files are parsed into at startup.
 *
 * After calling config_load(), the rest of the application reads
 * configuration from these structs — never directly from files.
 *
 * WHY STRUCTS AND NOT RUNTIME YAML PARSING?
 *   Parsing YAML at every config read would be slow and error-prone.
 *   We parse once at startup, validate everything, then use typed structs.
 *   If a customer has a typo in their YAML, the server refuses to start
 *   and logs exactly which field is wrong — before any order is processed.
 */

#include "types.h"

/* AutoStore robot controller connection settings */
typedef struct {
    int  enabled;              /* 0 = no AutoStore at this site */
    char controller_url[128];  /* e.g. "http://192.168.1.100:8080" */
    char api_key[64];          /* injected from env var at load time */
    int  heartbeat_interval_sec;
    int  command_timeout_ms;
} autostore_config_t;

/* ERP integration (SAP) settings */
typedef struct {
    int  enabled;
    char host[128];       /* SAP application server hostname */
    char client[8];       /* SAP client/mandant, e.g. "100" */
    char user[32];        /* RFC user */
    char password[64];    /* injected from env var */
} erp_config_t;

/* Top-level customer configuration (one instance per running server) */
typedef struct {
    char             warehouse_id[16];
    char             warehouse_name[128];
    char             picking_strategy[32];  /* "wave_optimization" | "single_order" */
    int              max_lines_per_wave;
    autostore_config_t autostore;
    erp_config_t       erp;
} customer_config_t;

/*
 * config_load — parse all YAML files in config_dir and populate cfg.
 *   config_dir: path to the customer's config folder (e.g. "config/customers/acme")
 *   cfg:        caller-allocated struct to populate
 *
 *   Returns SUPCIS_ERR_NOT_FOUND if the config directory doesn't exist.
 *   Returns SUPCIS_ERR_INVALID_ARG if a required field is missing or invalid.
 */
supcis_result_t config_load(const char *config_dir, customer_config_t *cfg);

/*
 * config_validate — check all fields for valid values after loading.
 *   Called automatically by config_load() — also useful to call after
 *   manual struct population in tests.
 */
supcis_result_t config_validate(const customer_config_t *cfg);

#endif /* CUSTOMER_CONFIG_H */
