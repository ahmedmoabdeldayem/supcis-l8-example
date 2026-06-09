/*
 * order_app_service.c -- orchestrates order workflows
 *
 * Each function here is an APPLICATION SERVICE: it loads entities from
 * the database, calls domain functions (which enforce business rules),
 * persists the results, and notifies external systems.
 *
 * These are stubs. Comments show exactly what each step does in production.
 */
#include "application_service.h"
#include "order_entity.h"
#include "picking_service.h"
#include "logger.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * app_create_order
 * -------------------------------------------------------------------------
 * Receives an order from SAP (via IDoc or REST) and stores it in Oracle
 * with status NEW. SuPCIS generates its own UUID; the ERP's reference
 * number is stored in customer_ref for callbacks.
 */
supcis_result_t app_create_order(
    const char        *customer_ref,
    const order_line_t *lines,
    int                line_count,
    supcis_id_t        *out_order_id)
{
    if (!customer_ref || !lines || line_count <= 0 || !out_order_id)
        return SUPCIS_ERR_INVALID_ARG;
    if (line_count > ORDER_MAX_LINES) {
        LOG_WARN("app", "Rejected order from ref %s: %d lines exceeds ORDER_MAX_LINES (%d)",
                 customer_ref, line_count, ORDER_MAX_LINES);
        return SUPCIS_ERR_INVALID_ARG;
    }

    LOG_INFO("app", "Creating order from ERP ref %s (%d lines)", customer_ref, line_count);

    /*
     * Step 1: Check for duplicate
     *   rc = order_repository_find_by_customer_ref(repo, customer_ref, &existing);
     *   if (rc == SUPCIS_OK) return SUPCIS_ERR_CONFLICT;  -- already exists
     *
     * Step 2: Generate a new UUID using Oracle's SYS_GUID()
     *   The UUID comes from Oracle (not from C code) so it is guaranteed unique
     *   even if two application servers create orders at the same millisecond.
     *   SQL: "SELECT LOWER(RAWTOHEX(SYS_GUID())) FROM dual"
     *   result -> out_order_id
     *
     * Step 3: Build order_t from parameters
     *   order_t order = {0};
     *   strncpy(order.order_id,      *out_order_id, sizeof(order.order_id)-1);
     *   strncpy(order.customer_ref,  customer_ref,  sizeof(order.customer_ref)-1);
     *   order.status     = ORDER_STATUS_NEW;
     *   order.created_at = time(NULL);
     *   order.line_count = line_count;
     *   memcpy(order.lines, lines, line_count * sizeof(order_line_t));
     *
     * Step 4: Persist in Oracle
     *   INSERT INTO order_header (order_id, customer_ref, status, created_at)
     *   VALUES (:1, :2, :3, :4)
     *   For each line:
     *   INSERT INTO order_line (line_id, order_id, sku_id, qty_ordered, qty_picked)
     *   VALUES (SYS_GUID(), :1, :2, :3, 0)
     */

    /* Stub: write a fake UUID so callers can see the pattern */
    strncpy(*out_order_id, "00000000-0000-0000-0000-000000000001",
            sizeof(supcis_id_t) - 1);

    LOG_INFO("app", "Order created: id=%s ref=%s", *out_order_id, customer_ref);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * app_check_order_availability
 * -------------------------------------------------------------------------
 * Checks whether we have enough stock for every line BEFORE releasing the
 * order to the floor. Returns a per-line breakdown so the operator can see
 * exactly which SKUs are short.
 *
 * WHY THIS CHECK IS NEEDED:
 *   SAP runs its own ATP (Available to Promise) check, but SAP's inventory
 *   view lags behind SuPCIS by the time it takes to post goods movements back.
 *   This check uses the live physical stock from SuPCIS's own Oracle tables,
 *   which is always the authoritative view of what is actually in the warehouse.
 */
supcis_result_t app_check_order_availability(
    const char             *order_id,
    inventory_repository_t *inventory_repo,
    order_availability_t   *out_result)
{
    if (!order_id || !inventory_repo || !out_result)
        return SUPCIS_ERR_INVALID_ARG;

    (void)inventory_repo; /* used in real implementation below */

    LOG_INFO("app", "Checking stock availability for order %s", order_id);

    /*
     * Step 1: Load the order from Oracle to get its lines
     *   order_t order;
     *   rc = order_repository_find_by_id(repo, order_id, &order);
     *   if (rc != SUPCIS_OK) return rc;
     *
     * Step 2: For each order line, query total available stock across all bins
     *
     *   This requires a SUM query, not the per-bin find_by_sku_location.
     *   In production, inventory_repository would have an extra method:
     *
     *   supcis_result_t (*find_total_available)(
     *       struct inventory_repository *self,
     *       const char *sku_id,
     *       quantity_t *out_available);
     *
     *   Which executes:
     *   SELECT SUM(qty_on_hand - qty_reserved) AS available
     *   FROM   stock_item
     *   WHERE  sku_id = :1
     *
     *   For each line:
     *     quantity_t available = 0;
     *     inventory_repo->find_total_available(inventory_repo,
     *         order.lines[i].sku_id, &available);
     *
     *     out_result->lines[i].qty_ordered   = order.lines[i].qty_ordered;
     *     out_result->lines[i].qty_available = available;
     *     out_result->lines[i].can_fulfil    =
     *         (available >= order.lines[i].qty_ordered) ? 1 : 0;
     *     strncpy(out_result->lines[i].sku_id, order.lines[i].sku_id, 32);
     *
     *     if (!out_result->lines[i].can_fulfil)
     *         out_result->all_lines_ok = 0;
     *
     * Step 3: Set out_result->all_lines_ok = 1 only if no line was short
     *
     * IMPORTANT: this is a snapshot, not a lock. Stock can change between
     * this check and the actual reservation in app_release_order. The wave
     * creation step handles that -- it takes only what is actually available
     * (short picks), so the warehouse never over-commits.
     */

    /* Stub: always reports all lines OK */
    out_result->all_lines_ok = 1;
    out_result->line_count   = 0;

    LOG_INFO("app", "Availability check done for order %s: all_ok=%d",
             order_id, out_result->all_lines_ok);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * app_release_order
 * -------------------------------------------------------------------------
 * Releases an order to the warehouse floor:
 *   1. Blocks release if stock is short
 *   2. Transitions order NEW -> RELEASED
 *   3. Creates a pick wave (reserves stock, builds task list)
 *   4. Persists everything in one Oracle transaction
 *   5. Notifies SAP that picking has started
 */
supcis_result_t app_release_order(
    const char             *order_id,
    inventory_repository_t *inventory_repo,
    pick_wave_t            *out_wave)
{
    (void)out_wave;        /* stub -- populated when implementation is complete */
    (void)inventory_repo;  /* stub -- passed to app_check_order_availability and picking_service in production */

    LOG_INFO("app", "Releasing order %s", order_id);

    /*
     * Step 0: Check stock availability before doing anything else.
     *
     * If any line is short we return SUPCIS_ERR_CONFLICT immediately.
     * The REST handler will turn this into a 409 response with the
     * availability report so the operator can see what is missing.
     *
     *   order_availability_t avail;
     *   rc = app_check_order_availability(order_id, inventory_repo, &avail);
     *   if (rc != SUPCIS_OK) return rc;
     *
     *   if (!avail.all_lines_ok) {
     *       LOG_WARN("app", "Order %s blocked: stock shortage on %d line(s)",
     *                order_id, avail.line_count);
     *       return SUPCIS_ERR_CONFLICT;
     *   }
     *
     * Step 1: Load order from Oracle
     *   order_t order;
     *   rc = order_repository_find_by_id(repo, order_id, &order);
     *   if (rc != SUPCIS_OK) return rc;
     *
     * Step 2: Call domain rule -- transitions NEW -> RELEASED
     *   rc = order_release(&order);
     *   if (rc != SUPCIS_OK) return rc;  -- CONFLICT if already released
     *
     * Step 3: Create pick wave -- reserves stock and builds tasks
     *   rc = picking_service_create_wave(&order, inventory_repo, out_wave);
     *   if (rc != SUPCIS_OK) return rc;
     *
     * Step 4: Persist wave and updated order in one Oracle transaction
     *   db_begin(db);
     *   picking_repository_save(picking_repo, out_wave);
     *   order_repository_save(order_repo, &order);
     *   db_commit(db);
     *   -- If any save fails: db_rollback(db); return SUPCIS_ERR_DB;
     *
     * Step 5: Notify ERP (SAP) that picking has started -- fire-and-forget
     *   erp_adapter_notify_picking_started(erp, order_id);
     *   -- We log a warning if this fails but do not abort the workflow.
     *   -- The order is already released; SAP will catch up on next sync.
     */

    LOG_INFO("app", "Order %s released successfully", order_id);
    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * app_confirm_pick_task
 * -------------------------------------------------------------------------
 * Called when a picker scans their handheld device after physically taking
 * items from a bin. Deducts inventory, checks if the whole wave is done.
 */
supcis_result_t app_confirm_pick_task(const char *task_id, quantity_t qty_picked)
{
    LOG_INFO("app", "Confirming task %s, qty=%d", task_id, qty_picked);

    /*
     * Step 1: Load pick task from Oracle
     * Step 2: pick_task_confirm(task, qty_picked) -- domain validates state + qty
     * Step 3: Load stock_item for the task's SKU + location
     * Step 4: stock_item_deduct(stock, qty_picked) -- items physically left the bin
     * Step 5: Save both task and stock_item in one transaction
     * Step 6: Check if the whole wave is done: pick_wave_pending_count(wave) == 0
     * Step 7: If wave done -> pick_wave_close(wave)
     * Step 8: Check if the order is fully picked: order_is_fully_picked(order)
     * Step 9: If order fully picked -> advance to PACKING, notify SAP
     */

    return SUPCIS_OK;
}

/* -------------------------------------------------------------------------
 * app_cancel_order
 * -------------------------------------------------------------------------
 * Cancels an order that has not yet shipped. Releases all stock that was
 * reserved for its pick tasks so other orders can use it.
 */
supcis_result_t app_cancel_order(const char *order_id)
{
    LOG_INFO("app", "Cancelling order %s", order_id);

    /*
     * Step 1: Load all open pick tasks for this order
     * Step 2: For each task: stock_item_release(stock, task.qty_to_pick)
     *   -- This undoes the soft-lock created when the wave was built.
     *   -- The items never left the bin, so only qty_reserved decreases.
     * Step 3: Cancel each task (status -> CANCELLED)
     * Step 4: order_cancel(order) -- domain blocks if status == SHIPPED
     * Step 5: Persist everything in one transaction
     * Step 6: Notify ERP that order was cancelled
     */

    return SUPCIS_OK;
}
