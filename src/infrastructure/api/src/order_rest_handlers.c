/*
 * order_rest_handlers.c -- HTTP handlers for order lifecycle endpoints
 *
 * Endpoints covered:
 *   POST /api/v1/orders                  handle_create_order
 *   GET  /api/v1/orders/:id              handle_get_order
 *   POST /api/v1/orders/:id/release      handle_release_order
 *   POST /api/v1/orders/:id/cancel       handle_cancel_order
 *
 * Each handler follows the same three-step pattern:
 *   1. Parse  -- extract IDs or JSON fields from the request
 *   2. Call   -- invoke the application service (which orchestrates domain + DB)
 *   3. Respond -- map result codes to HTTP status codes, serialize JSON
 */
#include "rest_handler.h"
#include "application_service.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * handle_create_order
 *   POST /api/v1/orders
 *   Body: {"customer_ref":"SAP-4500123","lines":[{"sku":"SKU-A","qty":5},...]}
 *
 * Receives a new order from an ERP or integration layer.
 * Stores it in Oracle with status NEW. Does NOT release it to the floor --
 * that is a separate deliberate step via /release.
 *
 * Who calls this:
 *   Non-SAP systems (custom ERP, marketplace integrators) use this REST endpoint.
 *   SAP-based installations typically push orders via IDoc, which are handled
 *   by the ERP adapter background thread rather than this endpoint.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_create_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    (void)ctx; /* ctx->db would be used by app_create_order in production */

    char customer_ref[64] = {0};

    /*
     * Parse the customer_ref field from the JSON body.
     * In production: use jansson to parse the full JSON body including
     * the lines array. sscanf here gives the structural idea only.
     *
     * Real parsing:
     *   json_t *root  = json_loads(req->body, 0, &err);
     *   json_t *ref   = json_object_get(root, "customer_ref");
     *   json_t *lines = json_object_get(root, "lines");
     *   ... iterate lines array ...
     */
    if (sscanf(req->body, "{\"customer_ref\":\"%63[^\"]\"", customer_ref) != 1) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\",\"detail\":\"customer_ref is required\"}");
        return SUPCIS_OK;
    }

    if (customer_ref[0] == '\0') {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\",\"detail\":\"customer_ref is required\"}");
        return SUPCIS_OK;
    }

    /*
     * In production: parse the lines array from the JSON body and pass
     * them to app_create_order. Here we pass a stub empty set.
     */
    supcis_id_t new_order_id;
    supcis_result_t rc = app_create_order(customer_ref, NULL, 0, &new_order_id);

    if (rc == SUPCIS_ERR_CONFLICT) {
        /*
         * 409 Conflict -- an order with this customer_ref already exists.
         * This can happen if SAP sends the same IDoc twice (retry scenario).
         * The ERP integration should treat 409 as "already accepted", not an error.
         */
        res->status_code = 409;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"conflict\",\"detail\":\"order already exists\","
                 "\"customer_ref\":\"%s\"}", customer_ref);
        return SUPCIS_OK;
    }
    if (rc == SUPCIS_ERR_INVALID_ARG) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    if (rc != SUPCIS_OK) {
        res->status_code = 500;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"internal\"}");
        return rc;
    }

    /* 201 Created -- return the new order_id so the caller can track this order */
    res->status_code = 201;
    snprintf(res->body, sizeof(res->body),
             "{\"order_id\":\"%s\",\"customer_ref\":\"%s\",\"status\":\"NEW\"}",
             new_order_id, customer_ref);

    LOG_INFO("api", "Order created: id=%s ref=%s", new_order_id, customer_ref);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_get_order
 *   GET /api/v1/orders/:id
 *
 * Returns the current status and line progress for one order.
 * Used by operator dashboards and ERP systems polling for status changes.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_get_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    (void)ctx;

    /* Extract the order ID from the URL path: /api/v1/orders/<id> */
    char order_id[64] = {0};
    const char *prefix = "/api/v1/orders/";
    const char *id_start = req->path + strlen(prefix);
    /* ID ends at '?' (query string) or end of string */
    size_t id_len = strcspn(id_start, "?");
    if (id_len == 0 || id_len >= sizeof(order_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(order_id, id_start, id_len);
    order_id[id_len] = '\0';

    /*
     * In production:
     *   order_t order;
     *   supcis_result_t rc = order_repository_find_by_id(repo, order_id, &order);
     *   if (rc == SUPCIS_ERR_NOT_FOUND) { 404 }
     *   Serialize order fields to JSON.
     *
     * Status labels map enum -> string:
     *   ORDER_STATUS_NEW       -> "NEW"
     *   ORDER_STATUS_RELEASED  -> "RELEASED"
     *   ORDER_STATUS_PICKING   -> "PICKING"
     *   ORDER_STATUS_PACKING   -> "PACKING"
     *   ORDER_STATUS_SHIPPED   -> "SHIPPED"
     *   ORDER_STATUS_CANCELLED -> "CANCELLED"
     */

    /* Stub response */
    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"order_id\":\"%s\",\"status\":\"NEW\",\"line_count\":0}",
             order_id);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_release_order
 *   POST /api/v1/orders/:id/release
 *
 * The most important order endpoint. Starts physical warehouse activity:
 *   1. Check stock availability for ALL lines (before touching anything)
 *   2. If any line is short: return 409 with a per-line shortage report
 *   3. If all stock OK: call app_release_order() which transitions the order,
 *      creates the pick wave, reserves stock, and notifies SAP
 *
 * After this call, pickers can see the wave on their handheld scanners.
 * -------------------------------------------------------------------------*/
supcis_result_t handle_release_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    /* Extract order ID from path: /api/v1/orders/<id>/release */
    char order_id[64] = {0};
    const char *id_start = req->path + strlen("/api/v1/orders/");
    size_t id_len = strcspn(id_start, "/");
    if (id_len == 0 || id_len >= sizeof(order_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(order_id, id_start, id_len);
    order_id[id_len] = '\0';

    /*
     * Step 1: Check availability BEFORE calling app_release_order.
     *
     * Why check here instead of inside app_release_order?
     * The handler needs the full shortage report (per-line details) to build
     * a useful 409 response. app_release_order only returns SUPCIS_ERR_CONFLICT
     * without line details. So the handler does the check and builds the report,
     * then calls the release only if all lines are OK.
     *
     * order_availability_t avail;
     * supcis_result_t rc = app_check_order_availability(
     *     order_id, ctx->inventory_repo, &avail);
     *
     * if (rc == SUPCIS_ERR_NOT_FOUND) { 404 }
     * if (rc != SUPCIS_OK) { 500 }
     *
     * if (!avail.all_lines_ok) {
     *     -- Build a JSON array listing every short line:
     *     -- [{"sku":"SKU-A","qty_ordered":10,"qty_available":3,"can_fulfil":0}, ...]
     *     res->status_code = 409;
     *     snprintf(res->body, ..., "{\"error\":\"stock_shortage\",\"lines\":[...]}");
     *     return SUPCIS_OK;
     * }
     */

    /*
     * Step 2: All lines OK -- run the full release workflow.
     *
     * pick_wave_t wave;
     * supcis_result_t rc = app_release_order(order_id, ctx->inventory_repo, &wave);
     *
     * if (rc == SUPCIS_ERR_NOT_FOUND) { 404 }
     * if (rc == SUPCIS_ERR_CONFLICT)  { 409 "already_released" }
     * if (rc != SUPCIS_OK)            { 500 }
     *
     * 200: {"order_id":"...","status":"RELEASED","wave_id":"...","task_count":N}
     */

    (void)ctx;

    /* Stub response */
    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"order_id\":\"%s\",\"status\":\"RELEASED\","
             "\"wave_id\":\"stub-wave-id\",\"task_count\":0}",
             order_id);

    LOG_INFO("api", "Order %s released via REST", order_id);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * handle_cancel_order
 *   POST /api/v1/orders/:id/cancel
 *
 * Cancels the order and releases all stock reservations.
 * Called by ERP when a customer cancels before their goods are shipped.
 * Blocked if the order is already SHIPPED (goods left the building).
 * -------------------------------------------------------------------------*/
supcis_result_t handle_cancel_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    /* Extract order ID from path: /api/v1/orders/<id>/cancel */
    char order_id[64] = {0};
    const char *id_start = req->path + strlen("/api/v1/orders/");
    size_t id_len = strcspn(id_start, "/");
    if (id_len == 0 || id_len >= sizeof(order_id)) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"invalid\"}");
        return SUPCIS_OK;
    }
    memcpy(order_id, id_start, id_len);
    order_id[id_len] = '\0';

    supcis_result_t rc = app_cancel_order(order_id);

    if (rc == SUPCIS_ERR_NOT_FOUND) {
        res->status_code = 404;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"not_found\",\"order_id\":\"%s\"}", order_id);
        return SUPCIS_OK;
    }
    if (rc == SUPCIS_ERR_CONFLICT) {
        /*
         * 409 Conflict -- order is already SHIPPED.
         * Cannot cancel goods that have already left the warehouse.
         * The ERP must handle this as a return/credit note instead.
         */
        res->status_code = 409;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"conflict\",\"reason\":\"already_shipped\","
                 "\"order_id\":\"%s\"}", order_id);
        return SUPCIS_OK;
    }
    if (rc != SUPCIS_OK) {
        res->status_code = 500;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"internal\"}");
        return rc;
    }

    (void)ctx;

    res->status_code = 200;
    snprintf(res->body, sizeof(res->body),
             "{\"order_id\":\"%s\",\"status\":\"CANCELLED\"}", order_id);

    LOG_INFO("api", "Order %s cancelled via REST", order_id);
    return SUPCIS_OK;
}
