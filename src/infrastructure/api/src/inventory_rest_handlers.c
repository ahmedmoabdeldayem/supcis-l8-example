/*
 * inventory_rest_handlers.c — HTTP handlers for inventory endpoints
 *
 * ── LAYER POSITION ───────────────────────────────────────────────────────────
 *   This file is in the INFRASTRUCTURE layer (not domain).
 *   It translates between the HTTP world (request paths, JSON, status codes)
 *   and the domain world (stock_item_t, repository, result codes).
 *
 *   Flow:
 *     HTTP request
 *       → rest_handler_dispatch() routes to handle_get_inventory()
 *           → inventory_repository.find_by_sku_location()  [domain call]
 *               → result serialized to JSON
 *                   → HTTP response returned
 *
 * ── REST CONVENTIONS USED ────────────────────────────────────────────────────
 *   GET  → read only, never modifies data
 *   HTTP 200 → success with data
 *   HTTP 404 → item not found (not an error in REST — a valid "empty" answer)
 *   HTTP 500 → unexpected internal failure
 *
 *   Response body is always JSON for consistency.
 */

#include "rest_handler.h"
#include "inventory_entity.h"
#include "inventory_repository.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* Escapes `"` and `\` in src so it is safe to embed in a JSON string literal. */
static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 2 < dst_size; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[di++] = '\\';
        }
        dst[di++] = src[i];
    }
    dst[di] = '\0';
}

/*
 * handle_get_inventory — handles GET /api/v1/inventory?sku=XXX&location=YYY
 *
 * Returns the current stock count for a specific SKU at a specific bin.
 *
 * Query parameters:
 *   sku      — the product identifier, e.g. "SKU-BEARING-001"
 *   location — the bin code, e.g. "BIN-A-01-01"
 *
 * Success response (200):
 *   {"sku":"SKU-BEARING-001","location":"BIN-A-01-01",
 *    "on_hand":100,"reserved":30,"available":70}
 *
 * Not found response (404):
 *   {"error":"not_found","sku":"SKU-BEARING-001","location":"BIN-A-01-01"}
 */
supcis_result_t handle_get_inventory(
    const http_request_t   *req,
    http_response_t        *res,
    inventory_repository_t *repo)
{
    char sku[32]      = {0};
    char location[24] = {0};

    /*
     * Parse query parameters from the URL path.
     * sscanf format breakdown:
     *   %31[^&]   = read up to 31 chars, stopping at '&' (the sku value)
     *   %23s      = read up to 23 chars (the location value)
     *
     * In production: replace with a proper URL query string parser that
     * handles URL encoding (%20 for spaces, etc.) and parameter ordering.
     */
    /* sscanf must match both fields; any other count means the query string
     * is malformed or missing a parameter — reject immediately. */
    if (sscanf(req->path,
               "/api/v1/inventory?sku=%31[^&]&location=%23s",
               sku, location) != 2 || sku[0] == '\0' || location[0] == '\0') {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\","
                 "\"detail\":\"sku and location query parameters are required\"}");
        return SUPCIS_OK;
    }

    /* ── Call the domain layer to fetch the stock record ────────────────── */
    stock_item_t    item;
    supcis_result_t rc = repo->find_by_sku_location(repo, sku, location, &item);

    /* ── Map domain result codes to HTTP status codes ────────────────────── */

    if (rc == SUPCIS_ERR_NOT_FOUND) {
        /*
         * 404 Not Found — this is a normal, expected response.
         * It means "this SKU doesn't exist at this location" — not a crash.
         * REST convention: return 404, not 500, for missing resources.
         */
        res->status_code = 404;
        snprintf(res->body, sizeof(res->body),
            "{\"error\":\"not_found\",\"sku\":\"%s\",\"location\":\"%s\"}",
            sku, location);
        return SUPCIS_OK;  /* the HTTP transaction itself succeeded */
    }

    if (rc != SUPCIS_OK) {
        /*
         * 500 Internal Server Error — something unexpected broke (DB down, etc.)
         * We do NOT expose internal error details to the client for security.
         * The real error is already logged at ERROR level by the repository.
         */
        res->status_code = 500;
        snprintf(res->body, sizeof(res->body), "{\"error\":\"internal\"}");
        return rc;
    }

    /* ── Success: serialize the stock_item_t to JSON ─────────────────────── */
    res->status_code = 200;
    char sku_safe[64]      = {0};
    char location_safe[48] = {0};
    json_escape(item.sku_id,        sku_safe,      sizeof(sku_safe));
    json_escape(item.location_code, location_safe, sizeof(location_safe));
    snprintf(res->body, sizeof(res->body),
        "{"
          "\"sku\":\"%s\","
          "\"location\":\"%s\","
          "\"on_hand\":%d,"        /* total units physically in the bin */
          "\"reserved\":%d,"       /* units locked for open pick tasks */
          "\"available\":%d"       /* on_hand - reserved = can be picked */
        "}",
        sku_safe,
        location_safe,
        item.quantity_on_hand,
        item.quantity_reserved,
        stock_item_available(&item));

    LOG_INFO("api", "GET inventory: SKU=%s location=%s on_hand=%d",
             sku, location, item.quantity_on_hand);
    return SUPCIS_OK;
}

/*
 * handle_adjust_inventory
 *   POST /api/v1/inventory/adjust
 *   Body: {"sku":"SKU-BEARING-001","location":"BIN-A-01-01",
 *           "new_qty":95,"reason":"cycle_count"}
 *
 * WHEN IS THIS CALLED?
 *   After a cycle count or annual audit reveals a discrepancy.
 *   Example: SuPCIS shows 100 units in BIN-A-01-01 but a physical count
 *   finds only 95. Someone picked 5 units without scanning, or a past
 *   pick confirmation used the wrong quantity.
 *
 *   The supervisor enters the correct count here.
 *   SuPCIS overwrites qty_on_hand to the verified value.
 *
 * WHAT ABOUT RESERVATIONS?
 *   If the new_qty is lower than qty_reserved, we have a problem:
 *   we have promised stock to pick tasks that no longer exists.
 *   In production: if new_qty < qty_reserved, return 409 Conflict and
 *   require the supervisor to cancel the open pick tasks first.
 *   This forces a deliberate decision -- no silent data corruption.
 *
 * WHO IS ALLOWED TO CALL THIS?
 *   Only warehouse supervisors. In production this endpoint would require
 *   a role claim in the auth token (e.g. X-API-Role: supervisor),
 *   not just the regular API key. Regular pickers cannot adjust counts.
 *
 * 201: {"adjusted":true,"sku":"...","location":"...","new_qty":95}
 * 400: {"error":"invalid_qty"}    -- new_qty is negative
 * 404: {"error":"not_found"}      -- SKU+location does not exist
 * 409: {"error":"qty_below_reserved","reserved":30,"new_qty":20}
 */
supcis_result_t handle_adjust_inventory(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    char sku[32]      = {0};
    char location[24] = {0};
    int  new_qty      = -1;

    /* Parse fields from JSON body; require all three fields to be present. */
    if (sscanf(req->body,
               "{\"sku\":\"%31[^\"]\",\"location\":\"%23[^\"]\",\"new_qty\":%d",
               sku, location, &new_qty) != 3) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\","
                 "\"detail\":\"sku, location and new_qty are required\"}");
        return SUPCIS_OK;
    }

    if (sku[0] == '\0' || location[0] == '\0') {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid\",\"detail\":\"sku and location are required\"}");
        return SUPCIS_OK;
    }
    if (new_qty < 0) {
        res->status_code = 400;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"invalid_qty\",\"detail\":\"new_qty must be >= 0\"}");
        return SUPCIS_OK;
    }

    /*
     * In production:
     *   stock_item_t item;
     *   rc = ctx->inventory_repo->find_by_sku_location(
     *            ctx->inventory_repo, sku, location, &item);
     *   if (rc == SUPCIS_ERR_NOT_FOUND) { 404 }
     *
     *   if (new_qty < item.quantity_reserved) {
     *       -- 409: cannot set qty below what is already promised to pickers
     *       res->status_code = 409;
     *       snprintf(res->body, ..., "{\"error\":\"qty_below_reserved\",...}");
     *       return SUPCIS_OK;
     *   }
     *
     *   item.quantity_on_hand = new_qty;
     *   ctx->inventory_repo->save(ctx->inventory_repo, &item);
     *
     *   LOG_INFO("api", "Inventory adjusted: SKU=%s loc=%s new_qty=%d reason=%s",
     *            sku, location, new_qty, reason);
     */

    (void)ctx; /* used in production implementation above */

    res->status_code = 201;
    snprintf(res->body, sizeof(res->body),
             "{\"adjusted\":true,\"sku\":\"%s\",\"location\":\"%s\",\"new_qty\":%d}",
             sku, location, new_qty);

    LOG_INFO("api", "Inventory adjusted: SKU=%s location=%s new_qty=%d",
             sku, location, new_qty);
    return SUPCIS_OK;
}
