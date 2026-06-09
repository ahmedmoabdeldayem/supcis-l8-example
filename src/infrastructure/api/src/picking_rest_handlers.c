/*
 * picking_rest_handlers.c -- HTTP handlers for picking wave and task endpoints
 *
 * Endpoints covered:
 *   GET  /api/v1/picking/waves/:id              handle_get_wave
 *   POST /api/v1/picking/tasks/:id/confirm      handle_confirm_task
 *
 * Who uses these:
 *   handle_get_wave    -- supervisor dashboard polling wave progress in real time
 *   handle_confirm_task -- picker's handheld scanner app after physically picking
 */
#include "rest_handler.h"
#include "application_service.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * handle_get_wave
 *   GET /api/v1/picking/waves/:id
 *
 * Returns how many tasks in the wave are pending, confirmed, or cancelled.
 * The supervisor uses this to monitor floor throughput:
 *   - If too many tasks are pending, send another picker to that zone
 *   - If a task has been pending for too long, check if the picker is blocked
 *   - Once all tasks are confirmed/cancelled, the wave is marked closed
 * -------------------------------------------------------------------------*/
supcis_result_t handle_get_wave(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    (void)ctx;

    /* Extract wave ID from path: /api/v1/picking/waves/<id> */
    char wave_id[64] = {0};
    const char *id_start = req->path + strlen("/api/v1/picking/waves/");
    size_t id_len = strcspn(id_start, "/?");
    if (id_len == 0 || id_len >= sizeof(wave_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(wave_id, id_start, id_len);
    wave_id[id_len] = '\0';

    /*
     * In production:
     *   pick_wave_t wave;
     *   rc = picking_repository_find_by_id(repo, wave_id, &wave);
     *   if (rc == SUPCIS_ERR_NOT_FOUND) { 404 }
     *
     *   Count task statuses:
     *     int pending   = 0, confirmed = 0, cancelled = 0;
     *     for (int i = 0; i < wave.task_count; i++) {
     *         if (wave.tasks[i].status == PICK_TASK_PENDING ||
     *             wave.tasks[i].status == PICK_TASK_ASSIGNED)   pending++;
     *         else if (wave.tasks[i].status == PICK_TASK_CONFIRMED) confirmed++;
     *         else cancelled++;
     *     }
     *
     *   Or use pick_wave_pending_count() domain function.
     *   is_closed comes directly from wave.is_closed.
     */

    /* Stub response */
    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{"
               "\"wave_id\":\"%s\","
               "\"task_count\":0,"
               "\"pending\":0,"
               "\"confirmed\":0,"
               "\"cancelled\":0,"
               "\"is_closed\":false"
             "}",
             wave_id);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_confirm_task
 *   POST /api/v1/picking/tasks/:id/confirm
 *   Body: {"qty_picked": 7}
 *
 * This is the MOST FREQUENT endpoint in a running warehouse.
 * Every time a picker physically takes items from a bin, they scan their
 * handheld and this endpoint fires. For a busy warehouse it may be called
 * hundreds of times per hour.
 *
 * Sequence of events after this call:
 *   1. pick_task moves to CONFIRMED
 *   2. stock_item.qty_on_hand decreases (items left the bin physically)
 *   3. stock_item.qty_reserved decreases (reservation consumed)
 *   4. If this was the last pending task: wave closes
 *   5. If wave closed and all order lines fully picked: order moves to PACKING
 *
 * Short picks (qty_picked < qty_to_pick):
 *   Allowed by the domain. The task still confirms with the lower quantity.
 *   The missing units become a backorder -- handled by ERP, not SuPCIS.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_confirm_task(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    (void)ctx;

    /* Extract task ID from path: /api/v1/picking/tasks/<id>/confirm */
    char task_id[64] = {0};
    const char *id_start = req->path + strlen("/api/v1/picking/tasks/");
    size_t id_len = strcspn(id_start, "/");
    if (id_len == 0 || id_len >= sizeof(task_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(task_id, id_start, id_len);
    task_id[id_len] = '\0';

    /* Parse qty_picked from JSON body; the field is mandatory. */
    int qty_picked = 0;
    if (sscanf(req->body, "{\"qty_picked\":%d", &qty_picked) != 1) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\","
                 "\"detail\":\"qty_picked is required\"}");
        return SUPCIS_OK;
    }

    if (qty_picked <= 0) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid_qty\","
                 "\"detail\":\"qty_picked must be greater than zero\"}");
        return SUPCIS_OK;
    }

    supcis_result_t rc = app_confirm_pick_task(task_id, (quantity_t)qty_picked);

    if (rc == SUPCIS_ERR_NOT_FOUND) {
        res->status_code = 404;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"not_found\",\"task_id\":\"%s\"}", task_id);
        return SUPCIS_OK;
    }
    if (rc == SUPCIS_ERR_CONFLICT) {
        /*
         * 409 Conflict -- task is not in ASSIGNED state.
         * Possible reasons:
         *   - Already confirmed (picker scanned twice by accident)
         *   - Task was cancelled (order was cancelled while picker was working)
         * The 409 tells the scanner app to refresh its task list.
         */
        res->status_code = 409;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"conflict\","
                 "\"detail\":\"task not in ASSIGNED state\","
                 "\"task_id\":\"%s\"}", task_id);
        return SUPCIS_OK;
    }
    if (rc != SUPCIS_OK) {
        res->status_code = 500;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"internal\"}");
        return rc;
    }

    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"task_id\":\"%s\",\"status\":\"CONFIRMED\",\"qty_picked\":%d}",
             task_id, qty_picked);

    LOG_INFO("api", "Task %s confirmed, qty=%d", task_id, qty_picked);
    return SUPCIS_OK;
}
