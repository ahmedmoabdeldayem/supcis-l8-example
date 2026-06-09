#ifndef INVENTORY_REPOSITORY_H
#define INVENTORY_REPOSITORY_H

/*
 * inventory_repository.h -- abstract persistence interface (DDD: Repository)
 *
 * A Repository hides HOW data is stored. Domain code only knows:
 *   "find me a stock item by SKU and location"
 *   "save this updated stock item"
 * It has no idea whether the data comes from Oracle, a test mock, or a file.
 *
 * C INTERFACE PATTERN (simulating abstract class with function pointers):
 *   The struct IS the interface. Each field is a method.
 *   The `self` pointer is C's equivalent of `this` in C++/Java.
 *   It lets the function reach back into the concrete struct for state (e.g. db).
 *
 * TWO IMPLEMENTATIONS:
 *   Oracle (production)  -- src/infrastructure/database/src/stock_item_repository.c
 *   Mock array (tests)   -- tests/unit/... and tests/integration/...
 */

#include "inventory_entity.h"

typedef struct inventory_repository {

    /*
     * find_by_sku_location -- look up stock by product + bin address.
     *
     *   sku_id        : product to find, e.g. "SKU-BEARING-001"
     *   location_code : specific bin, e.g. "BIN-A-01-01"
     *                   Pass NULL to get the bin with the most available stock
     *                   for this SKU (used by wave creation).
     *   out           : caller-allocated; populated on SUPCIS_OK
     *
     *   SUPCIS_OK           -- found, out is populated
     *   SUPCIS_ERR_NOT_FOUND -- no matching row
     *   SUPCIS_ERR_DB        -- Oracle query failed
     */
    supcis_result_t (*find_by_sku_location)(
        struct inventory_repository *self,
        const char   *sku_id,
        const char   *location_code,
        stock_item_t *out);

    /*
     * save -- persist changes to a stock item (INSERT or UPDATE).
     *
     *   Uses Oracle MERGE: UPDATE if stock_id exists, INSERT if not.
     *   This lets callers always call save() without tracking whether
     *   the row is new or existing.
     *
     *   SUPCIS_OK    -- written successfully
     *   SUPCIS_ERR_DB -- write failed (transaction should be rolled back)
     */
    supcis_result_t (*save)(
        struct inventory_repository *self,
        const stock_item_t *item);

    /*
     * find_total_available_by_sku -- sum available stock across ALL bins.
     *
     *   Used by app_check_order_availability before releasing an order.
     *   Executes: SELECT SUM(qty_on_hand - qty_reserved) FROM stock_item
     *             WHERE sku_id = :1
     *
     *   This answers: "how much of this SKU can we actually pick right now,
     *   considering stock spread across multiple bins?"
     *
     *   out_available : receives the total available (0 if none)
     *
     *   Always returns SUPCIS_OK (even if result is zero).
     *   Returns SUPCIS_ERR_DB if the query failed.
     *
     *   NOTE: Mock implementations may leave this pointer NULL.
     *   Call sites must check: if (repo->find_total_available_by_sku) { ... }
     */
    supcis_result_t (*find_total_available_by_sku)(
        struct inventory_repository *self,
        const char *sku_id,
        quantity_t *out_available);

} inventory_repository_t;

#endif /* INVENTORY_REPOSITORY_H */
