#ifndef REST_HANDLER_H
#define REST_HANDLER_H

/*
 * rest_handler.h -- HTTP request/response types, routing, and handler declarations
 *
 * STRUCTURE OF THE REST LAYER:
 *
 *   rest_handler_dispatch()       -- the single entry point from the HTTP server
 *       |
 *       +-- handle_get_inventory()       GET  /api/v1/inventory
 *       +-- handle_adjust_inventory()    POST /api/v1/inventory/adjust
 *       |
 *       +-- handle_create_order()        POST /api/v1/orders
 *       +-- handle_get_order()           GET  /api/v1/orders/:id
 *       +-- handle_release_order()       POST /api/v1/orders/:id/release
 *       +-- handle_cancel_order()        POST /api/v1/orders/:id/cancel
 *       |
 *       +-- handle_get_wave()            GET  /api/v1/picking/waves/:id
 *       +-- handle_confirm_task()        POST /api/v1/picking/tasks/:id/confirm
 *       |
 *       +-- handle_robot_command()       POST /api/v1/robots/commands
 *       +-- handle_robot_status()        GET  /api/v1/robots/commands/:id/status
 *       |
 *       +-- handle_health_check()        GET  /api/v1/health
 *
 * HANDLER PATTERN:
 *   Each handler follows the same structure:
 *     1. Parse parameters from req->path or req->body (URL params or JSON)
 *     2. Call application service or repository via ctx
 *     3. Map domain result codes to HTTP status codes
 *     4. Serialize response to JSON in res->body
 *
 * AUTHENTICATION:
 *   All requests must include: X-API-Key: <configured key>
 *   The dispatch function validates the key before routing.
 *   Missing or wrong key -> 401 Unauthorized.
 *
 * DEPENDENCIES (server_context_t):
 *   Each handler needs different dependencies (db, repo, erp, autostore).
 *   Instead of passing them individually to every handler, they are bundled
 *   into server_context_t which is initialized once at startup and passed
 *   through dispatch to all handlers.
 */

#include "types.h"
#include "db_connection.h"
#include "inventory_repository.h"
#include "erp_adapter.h"
#include "autostore_port.h"

/* -------------------------------------------------------------------------
 * HTTP types
 * -------------------------------------------------------------------------*/

/* Supported HTTP methods */
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_PATCH,
    HTTP_DELETE
} http_method_t;

/*
 * http_request_t -- everything the HTTP server gives us about an incoming request.
 *
 * path       -- URL path + query string, e.g. "/api/v1/inventory?sku=X&location=Y"
 * body       -- raw request body (JSON for POST/PUT/PATCH requests)
 * auth_token -- value of the X-API-Key header (validated in dispatch)
 */
typedef struct {
    http_method_t method;
    char          path[256];
    char          body[4096];
    char          auth_token[128];
} http_request_t;

/*
 * http_response_t -- what the handler fills in to send back to the client.
 *
 * status_code -- HTTP status (200, 201, 400, 401, 404, 409, 500)
 * body        -- JSON response body (always JSON, even for errors)
 */
typedef struct {
    int  status_code;
    char body[8192];
} http_response_t;

/* -------------------------------------------------------------------------
 * Dependency container
 * -------------------------------------------------------------------------
 *
 * server_context_t -- all initialized dependencies bundled into one struct.
 *
 * Initialized once in main() after startup sequence:
 *   config loaded -> Oracle connected -> adapters connected -> context created
 *
 * Passed through dispatch to every handler. Handlers pull what they need:
 *   ctx->inventory_repo for inventory queries
 *   ctx->db             for transactions (begin/commit/rollback)
 *   ctx->erp            for SAP notifications
 *   ctx->autostore_cfg  for robot commands
 */
typedef struct {
    db_handle_t            *db;             /* Oracle connection           */
    inventory_repository_t *inventory_repo; /* stock queries + saves       */
    erp_adapter_t          *erp;            /* SAP RFC connection          */
    autostore_config_t     *autostore_cfg;  /* AutoStore controller config */
    char                    api_key[128];   /* expected X-API-Key value    */
} server_context_t;

/* -------------------------------------------------------------------------
 * Dispatcher
 * -------------------------------------------------------------------------
 *
 * rest_handler_dispatch -- routes an incoming HTTP request to the correct handler.
 *
 * Called once per incoming request by the HTTP server (libmicrohttpd in production).
 * Validates the API key, matches path + method to a handler, calls it.
 *
 * Returns SUPCIS_OK if the HTTP transaction completed (even for 404/409 responses).
 * Returns SUPCIS_ERR_INVALID_ARG if req or res is NULL.
 *
 * The HTTP status code is always in res->status_code, not in the return value.
 * The return value only signals internal errors (e.g. NULL pointer passed in).
 */
supcis_result_t rest_handler_dispatch(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/* -------------------------------------------------------------------------
 * Inventory handlers  (src/infrastructure/api/src/inventory_rest_handlers.c)
 * -------------------------------------------------------------------------
 *
 * handle_get_inventory
 *   GET /api/v1/inventory?sku=SKU-BEARING-001&location=BIN-A-01-01
 *
 *   Returns current stock at a specific bin.
 *   Used by: ERP dashboards, mobile picking apps, stock audit tools.
 *
 *   200: {"sku":"...","location":"...","on_hand":100,"reserved":30,"available":70}
 *   404: {"error":"not_found","sku":"...","location":"..."}
 *   500: {"error":"internal"}
 */
supcis_result_t handle_get_inventory(
    const http_request_t   *req,
    http_response_t        *res,
    inventory_repository_t *repo);

/*
 * handle_adjust_inventory
 *   POST /api/v1/inventory/adjust
 *   Body: {"sku":"SKU-001","location":"BIN-A-01","new_qty":95,"reason":"cycle_count"}
 *
 *   Overwrites the on_hand quantity after a physical count reveals a discrepancy.
 *   This is called after a cycle count or annual inventory audit.
 *
 *   When would you use this?
 *     Pickers report they found 95 units but Oracle shows 100. Someone may have
 *     taken items without scanning, or a past confirm confirmed the wrong quantity.
 *     The supervisor enters the correct number here and SuPCIS updates the record.
 *
 *   201: {"adjusted":true,"sku":"...","location":"...","new_qty":95}
 *   400: {"error":"invalid_qty"} -- new_qty is negative
 *   404: {"error":"not_found"}   -- SKU/location combo does not exist
 */
supcis_result_t handle_adjust_inventory(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/* -------------------------------------------------------------------------
 * Order handlers  (src/infrastructure/api/src/order_rest_handlers.c)
 * -------------------------------------------------------------------------
 *
 * handle_create_order
 *   POST /api/v1/orders
 *   Body: {"customer_ref":"SAP-4500123","lines":[{"sku":"SKU-A","qty":5},...]}
 *
 *   Receives a new order from an ERP system or integration layer.
 *   In SAP-based setups this endpoint may not be called directly -- SAP sends
 *   IDocs which the ERP adapter ingests in the background. But non-SAP partners
 *   (custom ERPs, marketplace integrators) use this REST endpoint.
 *
 *   201: {"order_id":"<uuid>","customer_ref":"SAP-4500123","status":"NEW"}
 *   409: {"error":"conflict"} -- order with this customer_ref already exists
 *   400: {"error":"invalid"}  -- missing fields or zero lines
 */
supcis_result_t handle_create_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_get_order
 *   GET /api/v1/orders/:id
 *
 *   Returns the current status and line details for one order.
 *   Used by: operator dashboards, ERP polling for status updates.
 *
 *   200: {"order_id":"...","status":"PICKING","line_count":3,
 *          "lines":[{"sku":"SKU-A","qty_ordered":5,"qty_picked":3},...]}
 *   404: {"error":"not_found"}
 */
supcis_result_t handle_get_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_release_order
 *   POST /api/v1/orders/:id/release
 *
 *   Releases the order to the warehouse floor:
 *     1. Checks stock availability for all lines
 *     2. If any line is short: returns 409 with the shortage report
 *     3. If all OK: calls app_release_order(), returns the new wave info
 *
 *   This endpoint is the START of physical warehouse activity.
 *   Before this call: the order exists in Oracle but nobody is picking it.
 *   After this call: pick tasks exist, stock is reserved, robots may be commanded.
 *
 *   200: {"order_id":"...","status":"RELEASED","wave_id":"...","task_count":3}
 *   404: {"error":"not_found"}
 *   409: {"error":"conflict","reason":"already_released"}
 *   409: {"error":"stock_shortage","lines":[{"sku":"SKU-A","need":10,"available":3}]}
 */
supcis_result_t handle_release_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_cancel_order
 *   POST /api/v1/orders/:id/cancel
 *
 *   Cancels an order and releases all its stock reservations.
 *   Can be called by ERP when a customer cancels before shipment.
 *   Blocked if the order is already SHIPPED.
 *
 *   200: {"order_id":"...","status":"CANCELLED"}
 *   404: {"error":"not_found"}
 *   409: {"error":"conflict","reason":"already_shipped"}
 */
supcis_result_t handle_cancel_order(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/* -------------------------------------------------------------------------
 * Picking handlers  (src/infrastructure/api/src/picking_rest_handlers.c)
 * -------------------------------------------------------------------------
 *
 * handle_get_wave
 *   GET /api/v1/picking/waves/:id
 *
 *   Returns the progress of a pick wave: how many tasks are pending,
 *   confirmed, or cancelled.
 *   Used by: supervisor dashboard to monitor floor activity in real time.
 *
 *   200: {"wave_id":"...","order_id":"...","task_count":3,
 *          "pending":1,"confirmed":2,"cancelled":0,"is_closed":false}
 *   404: {"error":"not_found"}
 */
supcis_result_t handle_get_wave(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_confirm_task
 *   POST /api/v1/picking/tasks/:id/confirm
 *   Body: {"qty_picked": 7}
 *
 *   Called by the picker's handheld scanner when they physically take
 *   items from a bin. qty_picked may be less than qty_to_pick (short pick).
 *
 *   After this call:
 *     - Stock is deducted from inventory
 *     - Task status -> CONFIRMED
 *     - If all tasks done: wave is closed, order may advance to PACKING
 *
 *   200: {"task_id":"...","status":"CONFIRMED","qty_picked":7}
 *   400: {"error":"invalid_qty"}  -- qty_picked is zero or negative
 *   404: {"error":"not_found"}    -- task_id does not exist
 *   409: {"error":"conflict"}     -- task is not in ASSIGNED state
 */
supcis_result_t handle_confirm_task(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/* -------------------------------------------------------------------------
 * Robot handlers  (src/infrastructure/api/src/robot_rest_handlers.c)
 * -------------------------------------------------------------------------
 *
 * handle_robot_command
 *   POST /api/v1/robots/commands
 *   Body: {"bin_id":"BIN-042","target_port":"PORT-01","type":"FETCH_BIN"}
 *
 *   Sends a command to the AutoStore controller.
 *   The controller assigns a robot and handles navigation -- SuPCIS only
 *   says WHAT it wants (fetch bin X to port Y), never HOW.
 *
 *   After the command is sent, the caller should poll handle_robot_status()
 *   with the returned command_id until the robot reports "COMPLETE".
 *
 *   Command types:
 *     FETCH_BIN    -- robot fetches a bin from the grid to a port
 *     RETURN_BIN   -- robot returns a bin from a port back to the grid
 *     MOVE_TO_PORT -- move a robot to a specific port (maintenance use)
 *
 *   201: {"command_id":"<uuid>","status":"SENT"}
 *   503: {"error":"controller_unreachable"} -- AutoStore controller offline
 */
supcis_result_t handle_robot_command(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_robot_status
 *   GET /api/v1/robots/commands/:id/status
 *
 *   Polls whether a previously sent robot command has completed.
 *   The caller loops: send command -> poll every 500ms -> act when COMPLETE.
 *
 *   This async pattern avoids holding an HTTP connection open while the
 *   robot (which may need to dig through several stacked bins) does its work.
 *
 *   200: {"command_id":"...","complete":true}
 *   200: {"command_id":"...","complete":false}  -- still in progress
 *   404: {"error":"not_found"}
 *   503: {"error":"controller_unreachable"}
 */
supcis_result_t handle_robot_status(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

/*
 * handle_health_check
 *   GET /api/v1/health
 *
 *   Returns 200 if the server is running and can reach Oracle.
 *   Used by: deployment script, load balancer health probes, monitoring.
 *
 *   200: {"status":"ok","db":"connected"}
 *   503: {"status":"degraded","db":"unreachable"}
 */
supcis_result_t handle_health_check(
    const http_request_t *req,
    http_response_t      *res,
    server_context_t     *ctx);

#endif /* REST_HANDLER_H */
