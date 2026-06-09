/*
 * inventory_entity.c — StockItem business logic
 *
 * This file contains the RULES that govern how stock quantities change.
 * It lives in the DOMAIN LAYER — it has zero knowledge of:
 *   - How data is stored  (no SQL, no Oracle calls)
 *   - How requests arrive (no HTTP, no JSON parsing)
 *   - Which customer's warehouse this is
 *
 * Every function follows the same pattern:
 *   1. Validate inputs — reject obviously wrong calls early
 *   2. Check business rules — is this allowed in the current state?
 *   3. Mutate the in-memory struct
 *   4. Return SUPCIS_OK or an error code
 *
 * IMPORTANT: These functions only change the in-memory struct.
 * The CALLER is responsible for persisting the result via inventory_repository.save().
 */

#include "inventory_entity.h"
#include "logger.h"
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────── */

quantity_t stock_item_available(const stock_item_t *item)
{
    /*
     * Available = what is physically there MINUS what is already promised.
     *
     * Example:
     *   on_hand=100, reserved=30  →  available=70
     *
     * Those 70 can be given to new pick tasks.
     * The 30 reserved units belong to existing open pick tasks —
     * they are physically still in the bin but not allocatable.
     */
    return item->quantity_on_hand - item->quantity_reserved;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t stock_item_reserve(stock_item_t *item, quantity_t qty)
{
    /* Guard: reserving zero or negative is always a caller bug */
    if (qty <= 0) return SUPCIS_ERR_INVALID_ARG;

    /* Overflow guard: reserved + qty must not exceed INT32_MAX.
     * In practice on_hand <= INT32_MAX and the availability check below
     * ensures reserved + qty <= on_hand, but we check explicitly to be safe
     * if data is ever corrupted at the storage layer. */
    if (qty > INT32_MAX - item->quantity_reserved) return SUPCIS_ERR_INVALID_ARG;

    /*
     * Business rule: you can only reserve what is actually available.
     * CONFLICT (not DB error) because this is an expected business situation —
     * stock ran out. The caller decides what to do (skip, backorder, alert).
     */
    if (stock_item_available(item) < qty) {
        LOG_WARN("inventory", "Reserve failed for SKU %s: requested %d, available %d",
                 item->sku_id, qty, stock_item_available(item));
        return SUPCIS_ERR_CONFLICT;
    }

    /*
     * Soft lock: only reserved goes up — on_hand stays unchanged.
     * The items are still physically in the bin; we just marked them as spoken for.
     * on_hand only decreases later, after the picker physically takes the items
     * (via stock_item_deduct).
     */
    item->quantity_reserved += qty;

    LOG_DEBUG("inventory", "Reserved %d of SKU %s at %s",
              qty, item->sku_id, item->location_code);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t stock_item_release(stock_item_t *item, quantity_t qty)
{
    /*
     * Guard: cannot release more than currently reserved.
     * This would mean the caller lost track of its own reservations.
     */
    if (qty <= 0 || qty > item->quantity_reserved)
        return SUPCIS_ERR_INVALID_ARG;

    /* Undo the soft lock — these units are free again for other orders */
    item->quantity_reserved -= qty;
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t stock_item_deduct(stock_item_t *item, quantity_t qty)
{
    /*
     * Guard: you can only deduct what was previously reserved.
     *
     * The correct workflow is always:
     *   1. stock_item_reserve()  — when a pick task is created
     *   2. stock_item_deduct()   — when the picker confirms they picked it
     *
     * Deducting more than reserved would corrupt the inventory count.
     */
    if (qty <= 0 || qty > item->quantity_reserved)
        return SUPCIS_ERR_INVALID_ARG;

    /* Underflow guard: on_hand must not go below zero.
     * With valid state on_hand >= reserved >= qty, so this should never
     * trigger, but we defend against corrupted storage data. */
    if (qty > item->quantity_on_hand) return SUPCIS_ERR_INVALID_ARG;

    /*
     * Both counters decrease together:
     *   on_hand   decreases — items physically left the bin
     *   reserved  decreases — the lock is no longer needed
     *
     * After this call the inventory is accurate: the bin has fewer items,
     * and no ghost reservation remains.
     */
    item->quantity_on_hand  -= qty;
    item->quantity_reserved -= qty;
    return SUPCIS_OK;
}
