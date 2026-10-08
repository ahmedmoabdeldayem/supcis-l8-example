/*
 * picking_service.c — pick wave creation logic
 *
 * This is the most important domain service in SuPCIS-L8.
 * It bridges the Order domain and the Inventory domain to produce
 * actionable work instructions (pick tasks) for warehouse staff.
 *
 * ALGORITHM OVERVIEW:
 *   For each order line:
 *     1. Ask the inventory repository: "what stock exists for this SKU?"
 *     2. Calculate how much to pick: min(qty_ordered, qty_available)
 *     3. Reserve that quantity so no other wave can take it
 *     4. Build a pick_task pointing to that bin
 *   Return the completed wave.
 *
 * DESIGN DECISION — short picks:
 *   If stock is available but less than ordered, we pick what we can.
 *   The wave is still created. The application layer handles the gap
 *   (backorder, notify ERP, create a second wave when stock arrives).
 *   This avoids holding up ALL orders just because one SKU is short.
 */

#include "picking_service.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

supcis_result_t picking_service_create_wave(
    const order_t          *order,
    inventory_repository_t *repo,
    pick_wave_t            *out_wave)
{
    /* Validate all inputs before touching anything */
    if (!order || !repo || !out_wave) return SUPCIS_ERR_INVALID_ARG;

    /*
     * Only RELEASED orders can generate pick tasks.
     * NEW orders haven't been checked yet.
     * PICKING/PACKING orders already have tasks — generating again would duplicate them.
     */
    if (order->status != ORDER_STATUS_RELEASED) {
        LOG_WARN("picking_service", "Order %s is not RELEASED (status=%d)",
                 order->order_id, order->status);
        return SUPCIS_ERR_CONFLICT;
    }

    /* Zero-initialise the output wave — no garbage values in any field */
    memset(out_wave, 0, sizeof(*out_wave));

    /* Build a deterministic wave ID from the order ID so it's traceable in logs */
    snprintf(out_wave->wave_id, sizeof(out_wave->wave_id), "WAVE-%s", order->order_id);

    /* Record when this wave was created (Unix timestamp) */
    out_wave->released_at = (timestamp_t)time(NULL);

    /* ── Main loop: one iteration per order line ────────────────────────── */
    for (int i = 0; i < order->line_count; i++) {
        const order_line_t *line = &order->lines[i];

        /*
         * Safety cap: if somehow more than WAVE_MAX_TASKS lines are processed,
         * stop rather than overflow the fixed-size tasks[] array.
         * In production this would trigger an alert and split into a second wave.
         */
        if (out_wave->task_count >= WAVE_MAX_TASKS) {
            /* Return an error rather than silently dropping remaining lines.
             * The caller (app_release_order) must roll back the DB transaction
             * to undo any reservations already made, then either split the order
             * into multiple waves or raise WAVE_MAX_TASKS. */
            LOG_ERROR("picking_service",
                      "Order %s has more lines than WAVE_MAX_TASKS (%d). "
                      "Split the order or increase the limit.",
                      order->order_id, WAVE_MAX_TASKS);
            return SUPCIS_ERR_CONFLICT;
        }

        /* ── Step 1: find available stock for this SKU ───────────────────── */
        stock_item_t stock;
        supcis_result_t rc = repo->find_by_sku_location(
            repo,
            line->sku_id,
            NULL,   /* NULL = "any location" — repository picks the best bin */
            &stock);

        if (rc == SUPCIS_ERR_NOT_FOUND) {
            /* No stock at all for this SKU — skip this line, continue with others */
            LOG_WARN("picking_service", "No stock found for SKU %s — skipping line",
                     line->sku_id);
            continue;
        }
        if (rc != SUPCIS_OK) {
            /* Actual DB error — abort the whole wave creation */
            return rc;
        }

        /* ── Step 2: check available quantity ───────────────────────────── */
        quantity_t available = stock_item_available(&stock);
        if (available <= 0) {
            LOG_WARN("picking_service", "SKU %s has 0 available — skipping", line->sku_id);
            continue;
        }

        /*
         * Pick the smaller of: what was ordered vs what is available.
         * Example: ordered=10, available=7 → to_pick=7 (short pick)
         * Example: ordered=5,  available=50 → to_pick=5 (full fill)
         */
        quantity_t to_pick = (line->qty_ordered < available)
                             ? line->qty_ordered
                             : available;

        /* ── Step 3: reserve the stock ───────────────────────────────────── */
        rc = stock_item_reserve(&stock, to_pick);
        if (rc != SUPCIS_OK) return rc;

        /*
         * Persist the reservation immediately so concurrent wave creation
         * for other orders doesn't see these units as available.
         * In production this is inside a database transaction.
         */
        rc = repo->save(repo, &stock);
        if (rc != SUPCIS_OK) {
            LOG_ERROR("picking_service",
                      "Failed to persist reservation for SKU %s — "
                      "%d reservation(s) already saved. Caller must rollback.",
                      line->sku_id, out_wave->task_count);
            return rc;
        }

        /* ── Step 4: build the pick task ─────────────────────────────────── */
        pick_task_t *task = &out_wave->tasks[out_wave->task_count++];

        snprintf(task->task_id,       sizeof(task->task_id),
                 "TASK-%s-%d", order->order_id, i);
        snprintf(task->order_line_id, sizeof(task->order_line_id),
                 "%s", line->line_id);
        snprintf(task->sku_id,        sizeof(task->sku_id),
                 "%s", line->sku_id);
        snprintf(task->location_code, sizeof(task->location_code),
                 "%s", stock.location_code); /* where the picker should go */

        task->qty_to_pick = to_pick;
        task->status      = PICK_TASK_PENDING; /* not yet assigned to any picker */
    }

    LOG_INFO("picking_service", "Wave %s created with %d tasks",
             out_wave->wave_id, out_wave->task_count);

    /*
     * Return NOT_FOUND if the entire order had no pickable stock.
     * Return OK even if only some lines were filled (partial wave).
     */
    return (out_wave->task_count > 0) ? SUPCIS_OK : SUPCIS_ERR_NOT_FOUND;
}
