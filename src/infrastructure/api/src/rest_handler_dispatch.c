/*
 * rest_handler_dispatch.c -- routes incoming HTTP requests to handler functions
 *
 * This file contains two things:
 *   1. Path-matching helper functions
 *   2. rest_handler_dispatch() -- the routing switch
 *
 * HOW ROUTING WORKS:
 *   The HTTP server (libmicrohttpd) calls rest_handler_dispatch() for every
 *   incoming request. Dispatch checks method + path in order of specificity
 *   (most specific patterns first) and calls the matching handler.
 *
 *   Order matters: POST /api/v1/orders/:id/release must be matched before
 *   GET /api/v1/orders/:id, otherwise the more general pattern would fire first.
 *
 * AUTHENTICATION:
 *   The API key is validated here, before any handler is called.
 *   This centralises auth so individual handlers don't need to repeat it.
 */
#include "rest_handler.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Path-matching helpers
 * -------------------------------------------------------------------------*/

/*
 * starts_with -- returns 1 if path begins with prefix, 0 otherwise.
 * Example: starts_with("/api/v1/orders/ORD-1/release", "/api/v1/orders/") -> 1
 */
static int starts_with(const char *path, const char *prefix)
{
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

/*
 * ends_with -- returns 1 if path ends with suffix, 0 otherwise.
 * Example: ends_with("/api/v1/orders/ORD-1/release", "/release") -> 1
 *
 * Used to distinguish /orders/:id/release from /orders/:id/cancel and
 * /orders/:id (no suffix).
 */
static int ends_with(const char *path, const char *suffix)
{
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

/*
 * extract_id -- copies the path segment between prefix and the next '/', '?',
 * or end-of-string into out_id.
 *
 * Example:
 *   extract_id("/api/v1/orders/ORD-001/release", "/api/v1/orders/", id, 64)
 *   -> id = "ORD-001", returns 1
 *
 * Returns 1 on success, 0 if prefix not found or segment is empty.
 * Used by handlers that need to extract a path parameter (e.g. order ID).
 * Not all handlers use this helper -- some inline the extraction for clarity.
 */
static int extract_id(const char *path, const char *prefix,
                       char *out_id, size_t id_size)
{
    size_t prefix_len = strlen(prefix);
    if (!starts_with(path, prefix)) return 0;

    const char *start = path + prefix_len;

    /* find where the ID ends: at '/', '?', or end of string */
    size_t len = strcspn(start, "/?");
    if (len == 0 || len >= id_size) return 0;

    memcpy(out_id, start, len);
    out_id[len] = '\0';
    return 1;
}


/* -------------------------------------------------------------------------
 * api_key_eq -- constant-time API key comparison
 * -------------------------------------------------------------------------
 * strcmp() short-circuits on the first mismatched byte, leaking timing
 * information that an attacker can use to brute-force the key one character
 * at a time. This function always compares all 128 bytes regardless of where
 * a mismatch occurs, so response time reveals nothing about the key.
 *
 * Both auth_token and api_key are char[128] in their structs, so iterating
 * the full fixed length is safe.
 */
static int api_key_eq(const char *a, const char *b)
{
    /* volatile prevents the compiler from optimising away the loop */
    volatile uint8_t diff = 0;
    size_t i;
    for (i = 0; i < 128; i++) {
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    }
    return diff == 0;
}

/* -------------------------------------------------------------------------
 * rest_handler_dispatch
 * -------------------------------------------------------------------------
 * Single entry point from the HTTP server. Validates auth, then routes.
 */
supcis_result_t rest_handler_dispatch(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx)
{
    if (!req || !res || !ctx) return SUPCIS_ERR_INVALID_ARG;

    /* extract_id is available to all handlers via this header; suppress the
     * unused-function warning since handlers call it as needed. */
    (void)extract_id;

    /* ---- Authentication check ----------------------------------------
     * All API calls require X-API-Key header to match the configured key.
     * This is validated once here so no handler can accidentally skip it.
     */
    if (!api_key_eq(req->auth_token, ctx->api_key)) {
        res->status_code = 401;
        snprintf(res->body, sizeof(res->body),
                 "{\"error\":\"unauthorized\"}");
        /* LOG_ERROR so failed auth attempts stand out clearly in log monitoring */
        LOG_ERROR("api", "Rejected request to %s: invalid API key", req->path);
        return SUPCIS_OK;
    }

    /* ---- Routing -------------------------------------------------------
     * Rules are checked in order of specificity.
     * More specific patterns (longer paths with sub-segments like /release)
     * must come BEFORE general patterns (like /orders/:id).
     */

    /* --- Health check ------------------------------------------------- */

    if (req->method == HTTP_GET && strcmp(req->path, "/api/v1/health") == 0) {
        return handle_health_check(req, res, ctx);
    }

    /* --- Inventory ----------------------------------------------------- */

    /* POST /api/v1/inventory/adjust -- before the GET so paths don't collide */
    if (req->method == HTTP_POST &&
        strcmp(req->path, "/api/v1/inventory/adjust") == 0) {
        return handle_adjust_inventory(req, res, ctx);
    }

    /* GET /api/v1/inventory?sku=X&location=Y */
    if (req->method == HTTP_GET &&
        starts_with(req->path, "/api/v1/inventory")) {
        return handle_get_inventory(req, res, ctx->inventory_repo);
    }

    /* --- Orders -------------------------------------------------------- */

    /* POST /api/v1/orders/:id/release -- check sub-path BEFORE generic :id */
    if (req->method == HTTP_POST &&
        starts_with(req->path, "/api/v1/orders/") &&
        ends_with(req->path, "/release")) {
        return handle_release_order(req, res, ctx);
    }

    /* POST /api/v1/orders/:id/cancel */
    if (req->method == HTTP_POST &&
        starts_with(req->path, "/api/v1/orders/") &&
        ends_with(req->path, "/cancel")) {
        return handle_cancel_order(req, res, ctx);
    }

    /* POST /api/v1/orders -- create (exact match, no trailing slash or ID) */
    if (req->method == HTTP_POST &&
        strcmp(req->path, "/api/v1/orders") == 0) {
        return handle_create_order(req, res, ctx);
    }

    /* GET /api/v1/orders/:id */
    if (req->method == HTTP_GET &&
        starts_with(req->path, "/api/v1/orders/")) {
        return handle_get_order(req, res, ctx);
    }

    /* --- Picking ------------------------------------------------------- */

    /* POST /api/v1/picking/tasks/:id/confirm */
    if (req->method == HTTP_POST &&
        starts_with(req->path, "/api/v1/picking/tasks/") &&
        ends_with(req->path, "/confirm")) {
        return handle_confirm_task(req, res, ctx);
    }

    /* GET /api/v1/picking/waves/:id */
    if (req->method == HTTP_GET &&
        starts_with(req->path, "/api/v1/picking/waves/")) {
        return handle_get_wave(req, res, ctx);
    }

    /* --- Robots -------------------------------------------------------- */

    /* GET /api/v1/robots/commands/:id/status -- before the POST on /commands */
    if (req->method == HTTP_GET &&
        starts_with(req->path, "/api/v1/robots/commands/") &&
        ends_with(req->path, "/status")) {
        return handle_robot_status(req, res, ctx);
    }

    /* POST /api/v1/robots/commands */
    if (req->method == HTTP_POST &&
        strcmp(req->path, "/api/v1/robots/commands") == 0) {
        return handle_robot_command(req, res, ctx);
    }

    /* ---- No route matched --------------------------------------------- */

    res->status_code = 404;
    snprintf(res->body, sizeof(res->body),
             "{\"error\":\"not_found\",\"path\":\"%s\"}", req->path);
    LOG_WARN("api", "No route for %s %s", req->method == HTTP_GET ? "GET" : "POST",
             req->path);
    return SUPCIS_OK;
}
