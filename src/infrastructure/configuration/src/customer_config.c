/*
 * customer_config.c — YAML configuration loading stub
 *
 * Real implementation would use a YAML parser library (libyaml or similar).
 * This stub shows the structure: where env vars are substituted,
 * where validation happens, and what errors are caught.
 */
#include "customer_config.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * substitute_env_var — replace "${ENV_VAR}" placeholders with actual values.
 *
 * Config files use ${VAR_NAME} syntax so secrets are never stored in YAML.
 * This function looks up the env var and copies its value into the field.
 * If the env var is not set, returns SUPCIS_ERR_INVALID_ARG.
 *
 * Example:
 *   input  = "${AUTOSTORE_API_KEY}"
 *   getenv("AUTOSTORE_API_KEY") = "secret-key-abc"
 *   output = "secret-key-abc"
 */
static supcis_result_t substitute_env_var(const char *template, char *out, size_t out_size)
{
    /* Check if the value starts with ${ and ends with } */
    if (template[0] == '$' && template[1] == '{') {
        char var_name[64] = {0};
        /* Extract the variable name between ${ and } */
        const char *end = strchr(template, '}');
        if (!end) return SUPCIS_ERR_INVALID_ARG;
        size_t name_len = (size_t)(end - template - 2);
        if (name_len >= sizeof(var_name)) {
            LOG_ERROR("config", "Environment variable name too long (max %zu chars)",
                      sizeof(var_name) - 1);
            return SUPCIS_ERR_INVALID_ARG;
        }
        strncpy(var_name, template + 2, name_len);

        const char *value = getenv(var_name);
        if (!value) {
            LOG_ERROR("config", "Required env var not set: %s", var_name);
            return SUPCIS_ERR_INVALID_ARG;
        }
        strncpy(out, value, out_size - 1);
    } else {
        /* Literal value — use as-is */
        strncpy(out, template, out_size - 1);
    }
    return SUPCIS_OK;
}

supcis_result_t config_load(const char *config_dir, customer_config_t *cfg)
{
    if (!config_dir || !cfg) return SUPCIS_ERR_INVALID_ARG;

    memset(cfg, 0, sizeof(*cfg));

    /*
     * Real implementation: parse config_dir/warehouse.yaml with libyaml,
     * walk the YAML tree, map each key to the corresponding struct field,
     * call substitute_env_var() for any ${...} values.
     *
     * Stub: populate with safe defaults so callers can compile and test.
     */
    strncpy(cfg->warehouse_id,    "WH001",             sizeof(cfg->warehouse_id)    - 1);
    strncpy(cfg->warehouse_name,  "Default Warehouse", sizeof(cfg->warehouse_name)  - 1);
    strncpy(cfg->picking_strategy,"wave_optimization", sizeof(cfg->picking_strategy)- 1);
    cfg->max_lines_per_wave = 100;

    /*
     * AutoStore config — in production the controller URL comes from YAML:
     *   autostore:
     *     controller_url: "${AUTOSTORE_URL}"
     * substitute_env_var() replaces "${AUTOSTORE_URL}" with the real value.
     * Here we call it with the literal default so the function is exercised.
     */
    {
        char url_buf[128] = {0};
        substitute_env_var("http://autostore-controller.local", url_buf, sizeof(url_buf));
        strncpy(cfg->autostore.controller_url, url_buf, sizeof(cfg->autostore.controller_url) - 1);
    }
    cfg->autostore.enabled = 0;  /* disabled in stub — no controller available */
    cfg->autostore.heartbeat_interval_sec = 30;
    cfg->autostore.command_timeout_ms = 5000;

    /* ERP config */
    cfg->erp.enabled = 0;  /* disabled in stub */

    LOG_INFO("config", "Configuration loaded from: %s", config_dir);
    return config_validate(cfg);
}

supcis_result_t config_validate(const customer_config_t *cfg)
{
    /* warehouse_id is mandatory */
    if (strlen(cfg->warehouse_id) == 0) {
        LOG_ERROR("config", "warehouse_id is required but not set");
        return SUPCIS_ERR_INVALID_ARG;
    }

    /* max_lines_per_wave must be positive and within hardware limits */
    if (cfg->max_lines_per_wave <= 0 || cfg->max_lines_per_wave > 500) {
        LOG_ERROR("config", "max_lines_per_wave must be between 1 and 500 (got %d)",
                  cfg->max_lines_per_wave);
        return SUPCIS_ERR_INVALID_ARG;
    }

    /* If AutoStore is enabled, controller_url is mandatory */
    if (cfg->autostore.enabled && strlen(cfg->autostore.controller_url) == 0) {
        LOG_ERROR("config", "autostore.controller_url required when autostore is enabled");
        return SUPCIS_ERR_INVALID_ARG;
    }

    LOG_DEBUG("config", "Configuration validated OK for warehouse %s", cfg->warehouse_id);
    return SUPCIS_OK;
}
