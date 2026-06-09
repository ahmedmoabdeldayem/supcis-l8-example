#ifndef ORDER_REPOSITORY_H
#define ORDER_REPOSITORY_H

/*
 * order_repository.h -- abstract persistence interface for orders
 *
 * Same pattern as inventory_repository.h: function pointer struct.
 * Oracle implementation: src/infrastructure/database/src/order_repository.c
 *
 * An order spans two database tables:
 *   order_header  -- one row per order (id, customer_ref, status, created_at)
 *   order_line    -- N rows per order  (line_id, sku_id, qty_ordered, qty_picked)
 *
 * Loading an order requires two SELECT queries.
 * Saving an order requires UPDATE on order_header + UPDATE on each changed line.
 */

#include "order_entity.h"

typedef struct order_repository {

    /*
     * find_by_id -- load a complete order (header + all lines) by UUID.
     *
     *   Executes two queries:
     *   1. SELECT * FROM order_header WHERE order_id = :1
     *   2. SELECT * FROM order_line WHERE order_id = :1 ORDER BY line_id
     *
     *   SUPCIS_OK           -- found, out is populated with header + lines
     *   SUPCIS_ERR_NOT_FOUND -- no order with this UUID
     *   SUPCIS_ERR_DB        -- query failed
     */
    supcis_result_t (*find_by_id)(
        struct order_repository *self,
        const char *order_id,
        order_t    *out);

    /*
     * find_by_customer_ref -- look up an order by the ERP's document number.
     *
     *   Used by app_create_order to check for duplicates before inserting.
     *   Also used when SAP sends a status query referencing its own number.
     *
     *   SELECT * FROM order_header WHERE customer_ref = :1
     *
     *   SUPCIS_ERR_NOT_FOUND if no order has this customer_ref (not an error
     *   in the duplicate-check use case -- it means the order is new).
     */
    supcis_result_t (*find_by_customer_ref)(
        struct order_repository *self,
        const char *customer_ref,
        order_t    *out);

    /*
     * save -- persist an order and all its lines.
     *
     *   For new orders (not yet in DB):
     *     INSERT INTO order_header (order_id, customer_ref, status, created_at)
     *     For each line:
     *     INSERT INTO order_line (line_id, order_id, sku_id, qty_ordered, qty_picked)
     *
     *   For existing orders (status update, qty_picked update):
     *     UPDATE order_header SET status = :1 WHERE order_id = :2
     *     For each line with changed qty_picked:
     *     UPDATE order_line SET qty_picked = :1 WHERE line_id = :2
     *
     *   The implementation detects new vs existing by querying order_header first,
     *   or the caller can use a separate create function for new orders.
     */
    supcis_result_t (*save)(
        struct order_repository *self,
        const order_t *order);

} order_repository_t;

#endif /* ORDER_REPOSITORY_H */
