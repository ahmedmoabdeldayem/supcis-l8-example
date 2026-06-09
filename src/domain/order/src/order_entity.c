/*
 * order_entity.c — Order state-machine transitions
 *
 * Orders move through a strict sequence of states. These functions enforce
 * the rules about which transitions are allowed and which are not.
 *
 * State machine diagram:
 *
 *   NEW ──── release() ──→ RELEASED ──→ PICKING ──→ PACKING ──→ SHIPPED
 *    │                         │            │           │
 *    └──── cancel() ──────────→└────────────┘───────────┘
 *                         CANCELLED (from any state except SHIPPED)
 *
 * Only order_release() and order_cancel() are domain operations here.
 * The PICKING/PACKING/SHIPPED transitions are set by the application layer
 * when external events happen (wave confirmed, packing complete, shipment scanned).
 */

#include "order_entity.h"
#include "logger.h"

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t order_release(order_t *order)
{
    /*
     * Only a NEW order can be released.
     *
     * Why? Because releasing means "start generating pick tasks and reserving
     * stock". Doing this twice on the same order would create duplicate tasks
     * and double-reserve inventory — a serious data corruption bug.
     *
     * If an operator tries to release a PICKING order the system returns
     * CONFLICT and the UI shows "Order already in progress".
     */
    if (order->status != ORDER_STATUS_NEW) {
        LOG_WARN("order", "Cannot release order %s in status %d",
                 order->order_id, order->status);
        return SUPCIS_ERR_CONFLICT;
    }

    order->status = ORDER_STATUS_RELEASED;
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t order_cancel(order_t *order)
{
    /*
     * Cannot cancel a SHIPPED order — it has already left the building.
     * Every other status can be cancelled (NEW, RELEASED, PICKING, PACKING).
     *
     * Note: the caller (application layer) is responsible for:
     *   - Cancelling any open pick tasks
     *   - Releasing reserved inventory back to available
     * This function only updates the order status — it does not touch stock.
     */
    if (order->status == ORDER_STATUS_SHIPPED) {
        return SUPCIS_ERR_CONFLICT;
    }

    order->status = ORDER_STATUS_CANCELLED;
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

bool order_is_fully_picked(const order_t *order)
{
    /*
     * Walk all lines and check whether each one has been fully picked.
     * Returns false as soon as one incomplete line is found (early exit).
     * Returns true only if every single line is complete.
     *
     * This is called after each task confirmation to decide whether to
     * advance the order to PACKING status.
     */
    for (int i = 0; i < order->line_count; i++) {
        if (order->lines[i].qty_picked < order->lines[i].qty_ordered)
            return false;  /* found an incomplete line — stop checking */
    }
    return true;  /* all lines complete */
}
