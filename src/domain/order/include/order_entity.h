#ifndef ORDER_ENTITY_H
#define ORDER_ENTITY_H

/*
 * order_entity.h — Order and OrderLine entities (DDD: Domain Layer)
 *
 * ── WHAT IS AN ORDER IN THIS WMS? ───────────────────────────────────────────
 *   An Order is a customer's request to pick and ship a set of products.
 *   It contains one or more OrderLines, each asking for a specific SKU + qty.
 *
 *   Example:
 *     Order ORD-001  (customer: "Amazon DE")
 *       Line 1: 5 × SKU-BEARING-001
 *       Line 2: 3 × SKU-MOTOR-007
 *
 *   The WMS processes the order by:
 *     NEW → RELEASED → PICKING → PACKING → SHIPPED
 *
 * ── ORDER LIFECYCLE (state machine) ─────────────────────────────────────────
 *
 *   NEW         Order received from ERP (e.g. SAP), not yet sent to the floor.
 *               Waiting for stock check or manual release.
 *
 *   RELEASED    Released to the warehouse floor. Pick tasks will be generated.
 *               Stock is reserved as soon as wave is created.
 *
 *   PICKING     At least one pick task exists and pickers are working on it.
 *
 *   PACKING     All items picked, now being packed into boxes/pallets.
 *
 *   SHIPPED     Order left the warehouse. Terminal state — cannot be cancelled.
 *
 *   CANCELLED   Order was cancelled before shipping (e.g. customer withdrew).
 *               Reserved stock is released back to available.
 *
 * ── WHY EMBED LINES IN THE STRUCT? ──────────────────────────────────────────
 *   lines[ORDER_MAX_LINES] is a fixed-size array inside the struct.
 *   This avoids heap allocation (malloc) for the common case of <200 lines.
 *   It makes the struct easy to copy, pass by value, and test.
 *   For customers with >200 lines per order, ORDER_MAX_LINES would be raised.
 */

#include "types.h"

/* Maximum number of SKU lines a single order can contain */
#define ORDER_MAX_LINES 200

/*
 * order_status_t — the order's current position in the warehouse workflow.
 * Values are numbered 0-5 so they can be stored as integers in Oracle.
 */
typedef enum {
    ORDER_STATUS_NEW        = 0,  /* received, not yet released to floor */
    ORDER_STATUS_RELEASED   = 1,  /* released — pick tasks can be created */
    ORDER_STATUS_PICKING    = 2,  /* pickers are actively working on this order */
    ORDER_STATUS_PACKING    = 3,  /* picked, now being packed */
    ORDER_STATUS_SHIPPED    = 4,  /* left the warehouse — final state */
    ORDER_STATUS_CANCELLED  = 5   /* cancelled — stock reservations released */
} order_status_t;

/*
 * order_line_t — one line in an order: "pick X units of SKU Y"
 *
 *   qty_ordered  = what the customer asked for
 *   qty_picked   = what has actually been physically collected so far
 *   When qty_picked == qty_ordered the line is complete.
 */
typedef struct {
    supcis_id_t line_id;       /* UUID identifying this specific line */
    char        sku_id[32];    /* product to pick, e.g. "SKU-BEARING-001" */
    quantity_t  qty_ordered;   /* how many the customer wants */
    quantity_t  qty_picked;    /* how many have been physically picked so far */
} order_line_t;

/*
 * order_t — the aggregate root for an order (DDD: Aggregate)
 *
 *   In DDD an "Aggregate Root" is the only entry point into a cluster of
 *   related entities. You never modify an order_line_t directly from outside —
 *   you always go through the order_t functions (order_release, etc.).
 *   This ensures the order's invariants (rules) are always enforced.
 */
typedef struct {
    supcis_id_t    order_id;          /* UUID primary key */
    char           customer_ref[64];  /* reference from the customer's ERP system, e.g. "SAP-4500123" */
    order_status_t status;            /* current lifecycle state */
    timestamp_t    created_at;        /* when the order arrived from ERP */
    int            line_count;        /* number of active lines (0 to ORDER_MAX_LINES) */
    order_line_t   lines[ORDER_MAX_LINES]; /* the items to pick */
} order_t;

/*
 * order_release — transitions order from NEW → RELEASED.
 *   This makes the order eligible for pick wave generation.
 *   Returns SUPCIS_ERR_CONFLICT if the order is not in NEW status
 *   (you cannot re-release a picking or shipped order).
 */
supcis_result_t order_release(order_t *order);

/*
 * order_cancel — cancels the order (any status except SHIPPED).
 *   Transitions to CANCELLED. The picking service must separately
 *   release any stock reservations tied to this order's pick tasks.
 *   Returns SUPCIS_ERR_CONFLICT if order is already SHIPPED.
 */
supcis_result_t order_cancel(order_t *order);

/*
 * order_is_fully_picked — returns true if every line has qty_picked == qty_ordered.
 *   Used to decide whether to move the order to PACKING status.
 */
bool order_is_fully_picked(const order_t *order);

#endif /* ORDER_ENTITY_H */
