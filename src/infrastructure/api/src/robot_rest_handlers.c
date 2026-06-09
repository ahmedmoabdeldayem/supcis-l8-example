/*
 * robot_rest_handlers.c -- HTTP handlers for AutoStore robot commands
 *
 * Endpoints covered:
 *   POST /api/v1/robots/commands              handle_robot_command
 *   GET  /api/v1/robots/commands/:id/status   handle_robot_status
 *   GET  /api/v1/health                       handle_health_check
 *
 * IMPORTANT: SuPCIS does not navigate robots.
 * These handlers only pass commands to the AutoStore CONTROLLER.
 * The controller owns all navigation, robot positions, bin stack depths,
 * and collision avoidance. SuPCIS only says:
 *   "I need bin BIN-042 at port PORT-01"
 * The controller figures out which robot to use and how to get there.
 *
 * The command+poll pattern:
 *   1. POST /robots/commands     -- send the command, get command_id back
 *   2. GET  /robots/commands/:id/status  -- poll until complete=true
 */
#include "rest_handler.h"
#include "autostore_port.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * handle_robot_command
 *   POST /api/v1/robots/commands
 *   Body: {"bin_id":"BIN-042","target_port":"PORT-01","type":"FETCH_BIN"}
 *
 * Sends a fetch or return command to the AutoStore controller.
 * Does NOT wait for the robot to complete -- returns immediately with a
 * command_id that the caller uses to poll status.
 *
 * Typical caller: the picking task handler, after assigning a task to a picker.
 * The sequence is:
 *   1. Picker is assigned to TASK-001 (SKU at BIN-042, deliver to PORT-01)
 *   2. SuPCIS calls this endpoint: POST /robots/commands {bin_id="BIN-042", port="PORT-01"}
 *   3. SuPCIS gets command_id back
 *   4. SuPCIS polls GET /robots/commands/{id}/status every 500ms
 *   5. When complete=true, the picker's screen shows "go to PORT-01, bin is ready"
 *
 * In the database, bin_id is looked up from warehouse_location via location_code:
 *   SELECT autostore_bin_id FROM warehouse_location WHERE location_code = :1
 * That mapping lives in Oracle; the picking task knows the location_code.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_robot_command(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    if (!ctx->autostore_cfg) {
        /*
         * AutoStore is not configured for this customer installation.
         * Not all warehouses use AutoStore -- some use conventional racking.
         * Return 503 so the caller knows the feature is unavailable.
         */
        res->status_code = 503;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"autostore_not_configured\"}");
        return SUPCIS_OK;
    }

    /* Parse the command fields from the JSON body */
    char bin_id[24]      = {0};
    char target_port[16] = {0};
    int  cmd_type        = 0;

    /*
     * In production: use jansson for proper JSON parsing.
     * sscanf here shows the fields we expect without pulling in a library.
     */
    /* bin_id and target_port are required; type is optional (defaults to FETCH_BIN).
     * Accept >=2 matched fields — type field may be absent or have a non-int value. */
    int matched = sscanf(req->body,
                         "{\"bin_id\":\"%23[^\"]\",\"target_port\":\"%15[^\"]\","
                         "\"type\":%d",
                         bin_id, target_port, &cmd_type);
    if (matched < 2) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\","
                 "\"detail\":\"bin_id and target_port are required\"}");
        return SUPCIS_OK;
    }

    if (bin_id[0] == '\0' || target_port[0] == '\0') {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\","
                 "\"detail\":\"bin_id and target_port are required\"}");
        return SUPCIS_OK;
    }

    /* Build the command struct */
    robot_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    /*
     * command_id is a UUID -- in production generated via SYS_GUID() in Oracle
     * or a UUID library. We use a placeholder here.
     */
    strncpy(cmd.command_id, "stub-cmd-00000001", sizeof(cmd.command_id) - 1);
    strncpy(cmd.bin_id,      bin_id,      sizeof(cmd.bin_id)      - 1);
    strncpy(cmd.target_port, target_port, sizeof(cmd.target_port) - 1);
    cmd.type = (cmd_type > 0) ? (robot_cmd_type_t)cmd_type : ROBOT_CMD_FETCH_BIN;

    /* Send the command to the AutoStore controller via HTTP POST */
    supcis_result_t rc = autostore_send_command(ctx->autostore_cfg, &cmd);

    if (rc != SUPCIS_OK) {
        /*
         * 503 Service Unavailable -- the AutoStore controller did not respond.
         * Possible causes: controller offline, network issue, timeout.
         * The picking task should NOT be marked ASSIGNED until this succeeds.
         */
        res->status_code = 503;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"controller_unreachable\"}");
        return SUPCIS_OK;
    }

    /* 201 Created -- command accepted by controller, return its ID for polling */
    res->status_code = 201;
    snprintf(res->body, sizeof(res->body),
             "{\"command_id\":\"%s\",\"status\":\"SENT\","
             "\"bin_id\":\"%s\",\"target_port\":\"%s\"}",
             cmd.command_id, bin_id, target_port);

    LOG_INFO("api", "Robot command sent: bin=%s port=%s cmd_id=%s",
             bin_id, target_port, cmd.command_id);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_robot_status
 *   GET /api/v1/robots/commands/:id/status
 *
 * Polls whether the robot has finished executing a command.
 * Returns complete=true when the bin is at the port and ready for the picker.
 *
 * The caller (typically an internal SuPCIS background thread or the scanner app)
 * calls this every 500ms after sending a FETCH_BIN command. When complete=true,
 * the picker is notified: "go to PORT-01, your bin is ready".
 *
 * Why poll instead of webhook/callback?
 * AutoStore uses a simple command+poll model. Implementing a webhook would
 * require the controller to call back to SuPCIS, which adds firewall rules and
 * network complexity. For a warehouse with bounded latency (seconds, not hours),
 * polling every 500ms is simple and sufficient.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_robot_status(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    if (!ctx->autostore_cfg) {
        res->status_code = 503;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"autostore_not_configured\"}");
        return SUPCIS_OK;
    }

    /* Extract command ID from path: /api/v1/robots/commands/<id>/status */
    char command_id[64] = {0};
    const char *id_start = req->path + strlen("/api/v1/robots/commands/");
    size_t id_len = strcspn(id_start, "/");
    if (id_len == 0 || id_len >= sizeof(command_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(command_id, id_start, id_len);
    command_id[id_len] = '\0';

    bool complete = false;
    supcis_result_t rc = autostore_poll_status(ctx->autostore_cfg,
                                               command_id, &complete);

    if (rc != SUPCIS_OK) {
        res->status_code = 503;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"controller_unreachable\"}");
        return SUPCIS_OK;
    }

    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"command_id\":\"%s\",\"complete\":%s}",
             command_id, complete ? "true" : "false");

    LOG_DEBUG("api", "Robot status poll: cmd=%s complete=%s",
              command_id, complete ? "true" : "false");
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_health_check
 *   GET /api/v1/health
 *
 * Returns 200 if the server is running and Oracle is reachable.
 * Returns 503 if Oracle is unreachable (server is up but degraded).
 *
 * Used by:
 *   - Deployment script (scripts/deployment/health_check.sh) after restart
 *   - Load balancer probes to decide whether to route traffic to this instance
 *   - Monitoring/alerting systems (PagerDuty, Nagios, etc.)
 *
 * This endpoint must respond within ~1 second or the load balancer marks
 * the instance as down and stops routing traffic to it.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_health_check(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    (void)req;

    /*
     * Check Oracle connectivity by running a trivial query.
     * "SELECT 1 FROM dual" is Oracle's standard no-op health query.
     * If the database is reachable it returns in < 10ms.
     *
     * In production:
     *   int dummy;
     *   supcis_result_t rc = db_query_one(ctx->db,
     *       "SELECT 1 FROM dual", &dummy, sizeof(dummy));
     *   if (rc != SUPCIS_OK) -> 503 degraded
     */

    if (!ctx->db) {
        /* No DB handle -- startup failed or DB was never connected */
        res->status_code = 503;
        snprintf(res->body, sizeof(res->body),
                 "{\"status\":\"degraded\",\"db\":\"unreachable\"}");
        return SUPCIS_OK;
    }

    /* Stub: assume DB is healthy if the handle exists */
    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"status\":\"ok\",\"db\":\"connected\"}");
    return SUPCIS_OK;
}
