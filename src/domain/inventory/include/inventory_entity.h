#ifndef INVENTORY_ENTITY_H
#define INVENTORY_ENTITY_H

/*
 * inventory_entity.h — the StockItem entity (DDD: Domain Layer)
 *
 * ── WHAT IS A DDD ENTITY? ───────────────────────────────────────────────────
 *   In Domain-Driven Design an "Entity" is an object with a stable identity.
 *   A StockItem is the same entity even if its quantity changes — because it
 *   is identified by (sku_id + location_code), not by its current values.
 *
 *   Contrast with a "Value Object": Quantity(5) has no identity — two
 *   Quantity(5) objects are interchangeable. Here quantity_t plays that role.
 *
 * ── WHAT DOES A STOCK ITEM REPRESENT? ───────────────────────────────────────
 *   One SKU stored at one warehouse location.
 *   Example: "50 units of SKU-BEARING-001 are in bin BIN-A-01-01".
 *
 *   The same SKU can exist at multiple locations (multiple stock_item_t rows).
 *   The WMS tracks each separately so it can route pickers to the closest bin.
 *
 * ── QUANTITY SPLIT: on_hand vs reserved ─────────────────────────────────────
 *   quantity_on_hand   = total physically present in the bin
 *   quantity_reserved  = portion already promised to open pick tasks
 *   available          = on_hand - reserved  (call stock_item_available())
 *
 *   Why separate reserved instead of just decrementing on_hand immediately?
 *   Because a pick task might be cancelled before the picker reaches the bin.
 *   If we had already decremented on_hand we would lose track of those units.
 *   Reservation is a soft lock; deduction is the final commit after picking.
 */

#include "types.h"

typedef struct {
    supcis_id_t  stock_id;           /* UUID primary key — unique per row in DB */
    char         sku_id[32];         /* product identifier, e.g. "SKU-BEARING-001" */
    char         location_code[24];  /* bin address, e.g. "BIN-A-01-01" */
    quantity_t   quantity_on_hand;   /* total units physically in the bin */
    quantity_t   quantity_reserved;  /* units locked for open pick tasks (cannot be picked for other orders) */
    timestamp_t  last_updated;       /* epoch seconds — set on every DB write */
} stock_item_t;

/*
 * stock_item_available — returns how many units can still be reserved.
 *   = quantity_on_hand - quantity_reserved
 *   This is the number a new pick task may take.
 */
quantity_t stock_item_available(const stock_item_t *item);

/*
 * stock_item_reserve — locks `qty` units for a pick task.
 *   Increments quantity_reserved by qty.
 *   Returns SUPCIS_ERR_CONFLICT if available < qty (not enough free stock).
 *   Returns SUPCIS_ERR_INVALID_ARG if qty <= 0.
 *
 *   Call this when creating a pick task so the same stock is not
 *   double-allocated to two different orders.
 */
supcis_result_t stock_item_reserve(stock_item_t *item, quantity_t qty);

/*
 * stock_item_release — undoes a reservation (e.g. pick task was cancelled).
 *   Decrements quantity_reserved by qty.
 *   Returns SUPCIS_ERR_INVALID_ARG if qty > current reserved.
 */
supcis_result_t stock_item_release(stock_item_t *item, quantity_t qty);

/*
 * stock_item_deduct — physically removes stock after a confirmed pick.
 *   Decrements BOTH quantity_on_hand AND quantity_reserved by qty.
 *   Called after the picker confirms "I picked this item".
 *   Returns SUPCIS_ERR_INVALID_ARG if qty > current reserved
 *   (you can only deduct what was reserved — never more).
 */
supcis_result_t stock_item_deduct(stock_item_t *item, quantity_t qty);

#endif /* INVENTORY_ENTITY_H */
