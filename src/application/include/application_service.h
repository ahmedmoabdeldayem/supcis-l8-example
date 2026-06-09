#ifndef APPLICATION_SERVICE_H
#define APPLICATION_SERVICE_H

/*
 * application_service.h -- orchestration layer (DDD: Application Services)
 *
 * The application layer sits between the REST API and the domain.
 * It does NOT contain business rules -- those live in the domain.
 * It DOES contain workflow steps: load entity, call domain function,
 * persist result, notify external systems.
 *
 * Functions in this file:
 *   app_create_order             -- receive an order from ERP, store as NEW
 *   app_check_order_availability -- verify stock for all lines before release
 *   app_release_order            -- check -> release -> wave -> commit -> notify SAP
 *   app_confirm_pick_task        -- picker confirms: deduct stock, close wave if done
 *   app_cancel_order             -- cancel order, release all stock reservations
 */

#include "types.h"
#include "order_entity.h"
#include "picking_entity.h"
#include "inventory_repository.h"

/* -------------------------------------------------------------------------
 * Order creation
 * -------------------------------------------------------------------------
 *
 * app_create_order -- receive a new order from the ERP and store it in Oracle.
 *
 * Called when SAP sends an IDoc, or when the REST handler receives
 * POST /api/v1/orders.
 *
 * The order starts in status NEW. It is not released to the warehouse floor
 * until app_release_order() is called separately. This separation lets
 * operators review orders, check stock, and batch releases.
 *
 * ID ownership: SuPCIS generates its own UUID (stored in order_id).
 * SAP's own document number is stored in customer_ref. Both are needed:
 * SuPCIS uses order_id internally; when calling back to SAP, SuPCIS
 * always passes customer_ref so SAP recognises its own document.
 *
 * Parameters:
 *   customer_ref  -- SAP document number, e.g. "SAP-4500123"
 *   lines         -- array of lines: each has sku_id + qty_ordered
 *   line_count    -- how many lines are in the array
 *   out_order_id  -- receives the UUID that SuPCIS assigned
 *
 * Returns:
 *   SUPCIS_OK              -- order stored, out_order_id populated
 *   SUPCIS_ERR_INVALID_ARG -- customer_ref empty or line_count <= 0
 *   SUPCIS_ERR_CONFLICT    -- an order with this customer_ref already exists
 *   SUPCIS_ERR_DB          -- Oracle write failed
 */
supcis_result_t app_create_order(
    const char        *customer_ref,
    const order_line_t *lines,
    int                line_count,
    supcis_id_t        *out_order_id);

/* -------------------------------------------------------------------------
 * Availability check
 * -------------------------------------------------------------------------
 *
 * order_line_availability_t -- availability result for one order line.
 *
 * qty_available is the total pickable stock across ALL bins for this SKU:
 *   SELECT SUM(qty_on_hand - qty_reserved) FROM stock_item WHERE sku_id = :1
 * This aggregates across all bin locations, so a line with qty_ordered=10
 * can be satisfied even if the stock is split across multiple bins.
 */
typedef struct {
    char       sku_id[32];
    quantity_t qty_ordered;    /* what the order line asks for         */
    quantity_t qty_available;  /* total available right now            */
    int        can_fulfil;     /* 1 = qty_available >= qty_ordered     */
} order_line_availability_t;

/*
 * order_availability_t -- availability result for the whole order.
 *
 * all_lines_ok = 1: every line can be fully satisfied right now.
 * all_lines_ok = 0: at least one line is short.
 *
 * Even when all_lines_ok = 0, the lines[] array shows exactly which SKUs
 * are short and by how much. The operator can then decide:
 *   - hold the order and wait for stock to arrive
 *   - release anyway and accept short picks on the short lines
 *   - split: ship what is available now, backorder the rest
 */
typedef struct {
    int                       all_lines_ok;
    int                       line_count;
    order_line_availability_t lines[ORDER_MAX_LINES];
} order_availability_t;

/*
 * app_check_order_availability -- check stock for every line before release.
 *
 * This should be called BEFORE app_release_order. If any line is short,
 * the REST handler returns 409 Conflict with the full shortage report in the
 * response body, so the operator sees exactly which SKUs are missing.
 *
 * The function itself always returns SUPCIS_OK if the check ran successfully
 * (even if stock is short). The caller must inspect out_result->all_lines_ok.
 *
 * Important: this check is a point-in-time snapshot. Stock can change between
 * the check and the actual reservation inside app_release_order (because other
 * orders may be released in parallel). The wave creation step reserves stock
 * atomically, so the worst outcome is a short pick -- not a double-booking.
 *
 * Returns:
 *   SUPCIS_OK           -- check completed, inspect out_result->all_lines_ok
 *   SUPCIS_ERR_NOT_FOUND -- order_id does not exist in Oracle
 *   SUPCIS_ERR_DB        -- Oracle query failed
 */
supcis_result_t app_check_order_availability(
    const char             *order_id,
    inventory_repository_t *inventory_repo,
    order_availability_t   *out_result);

/* -------------------------------------------------------------------------
 * Order workflows
 * -------------------------------------------------------------------------
 *
 * app_release_order -- full workflow to release an order to the warehouse floor.
 *
 * Step 0: app_check_order_availability -- block if any line is short
 * Step 1: load order from Oracle
 * Step 2: order_release() -- domain enforces NEW -> RELEASED
 * Step 3: picking_service_create_wave() -- reserve stock, build pick tasks
 * Step 4: db_begin -> save wave + tasks + order status -> db_commit (atomic)
 * Step 5: erp_adapter_notify_picking_started() -- tell SAP (fire-and-forget)
 *
 * Returns:
 *   SUPCIS_OK              -- wave created, out_wave populated
 *   SUPCIS_ERR_NOT_FOUND   -- order_id does not exist
 *   SUPCIS_ERR_CONFLICT    -- order already released, OR stock shortage detected
 *   SUPCIS_ERR_DB          -- Oracle transaction failed (everything rolled back)
 */
supcis_result_t app_release_order(
    const char             *order_id,
    inventory_repository_t *inventory_repo,
    pick_wave_t            *out_wave);

/*
 * app_confirm_pick_task -- picker scans task completion on their handheld device.
 *
 * Step 1: load pick_task from Oracle
 * Step 2: pick_task_confirm(task, qty_picked) -- domain validates state + qty
 * Step 3: load stock_item for the task's bin
 * Step 4: stock_item_deduct(stock, qty_picked) -- physically remove from inventory
 * Step 5: save task + stock_item in one transaction
 * Step 6: if pick_wave_pending_count(wave) == 0 -> pick_wave_close(wave)
 * Step 7: if order_is_fully_picked(order) -> advance order to PACKING, notify SAP
 *
 * Returns:
 *   SUPCIS_OK              -- task confirmed, wave/order status updated if needed
 *   SUPCIS_ERR_NOT_FOUND   -- task_id does not exist
 *   SUPCIS_ERR_CONFLICT    -- task not in ASSIGNED state (already confirmed/cancelled)
 *   SUPCIS_ERR_INVALID_ARG -- qty_picked is zero or negative
 */
supcis_result_t app_confirm_pick_task(const char *task_id, quantity_t qty_picked);

/*
 * app_cancel_order -- cancel an order and release all its stock reservations.
 *
 * Step 1: load all open pick_tasks for this order
 * Step 2: for each task: stock_item_release(stock, task.qty_to_pick)
 * Step 3: cancel each task (status -> CANCELLED)
 * Step 4: order_cancel(order) -- domain enforces it is not already SHIPPED
 * Step 5: persist everything in one transaction
 * Step 6: notify ERP the order is cancelled
 *
 * Returns:
 *   SUPCIS_OK            -- order cancelled, stock released
 *   SUPCIS_ERR_NOT_FOUND -- order_id does not exist
 *   SUPCIS_ERR_CONFLICT  -- order is already SHIPPED (cannot cancel)
 */
supcis_result_t app_cancel_order(const char *order_id);

#endif /* APPLICATION_SERVICE_H */
