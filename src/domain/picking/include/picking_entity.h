#ifndef PICKING_ENTITY_H
#define PICKING_ENTITY_H

/*
 * picking_entity.h — PickTask and PickWave entities (DDD: Domain Layer)
 *
 * ── WHAT IS A PICK WAVE? ────────────────────────────────────────────────────
 *   When orders are released, the system groups their lines into a "wave".
 *   A wave is a batch of pick tasks assigned to one or more pickers.
 *
 *   Why batch? Efficiency.
 *   If 10 orders all need SKU-A from BIN-A-01, one picker visits that bin
 *   once and picks for all 10 orders, instead of 10 pickers visiting it.
 *   This is called "wave picking" or "batch picking".
 *
 * ── WHAT IS A PICK TASK? ────────────────────────────────────────────────────
 *   One atomic instruction: "go to location X and pick Y units of SKU Z".
 *   A picker's mobile scanner shows these tasks one by one.
 *   When done, they scan a confirmation → pick_task_confirm() is called.
 *
 * ── TASK LIFECYCLE ──────────────────────────────────────────────────────────
 *
 *   PENDING   → task created, not yet shown to any picker
 *   ASSIGNED  → task shown to / taken by a specific picker
 *   CONFIRMED → picker scanned confirmation, qty_picked recorded
 *   CANCELLED → task was dropped (e.g. stock ran out, order cancelled)
 *
 * ── WAVE LIFECYCLE ──────────────────────────────────────────────────────────
 *   A wave is open until all tasks are CONFIRMED or CANCELLED.
 *   Once pick_wave_pending_count() == 0, the wave can be closed.
 *   Closing the wave triggers order status updates (PICKING → PACKING).
 */

#include "types.h"

/* Maximum tasks a single wave can hold.
 * Sized for a busy shift: 500 tasks ≈ processing one large customer order batch. */
#define WAVE_MAX_TASKS 500

/*
 * pick_task_status_t — lifecycle state of one pick task.
 * Stored as integer in Oracle pick_task.status column.
 */
typedef enum {
    PICK_TASK_PENDING    = 0,  /* created, waiting to be assigned to a picker */
    PICK_TASK_ASSIGNED   = 1,  /* a picker's scanner has received this task */
    PICK_TASK_CONFIRMED  = 2,  /* picker confirmed pick — qty_picked is now valid */
    PICK_TASK_CANCELLED  = 3   /* dropped — no longer expected to be executed */
} pick_task_status_t;

/*
 * pick_task_t — a single picking instruction for one picker.
 *
 *   qty_to_pick  : how many units the picker should take from the location
 *   qty_picked   : how many they actually took (may differ in short-pick scenarios)
 */
typedef struct {
    supcis_id_t        task_id;        /* UUID primary key for this task */
    supcis_id_t        order_line_id;  /* which order line this task fulfils */
    char               sku_id[32];     /* product to pick */
    char               location_code[24]; /* bin to go to, e.g. "BIN-A-01-01" */
    quantity_t         qty_to_pick;    /* target quantity */
    quantity_t         qty_picked;     /* actual quantity picked (set on confirm) */
    pick_task_status_t status;
} pick_task_t;

/*
 * pick_wave_t — a batch of tasks released together to the warehouse floor.
 *
 *   tasks[]     : fixed array of all tasks in this wave
 *   task_count  : how many entries in tasks[] are actually used
 *   is_closed   : true once all tasks are done — set by pick_wave_close()
 */
typedef struct {
    supcis_id_t wave_id;
    int         task_count;
    pick_task_t tasks[WAVE_MAX_TASKS];
    timestamp_t released_at;   /* when the wave was sent to the floor */
    bool        is_closed;     /* true = all tasks complete, wave finished */
} pick_wave_t;

/*
 * pick_task_confirm — record that a picker completed a task.
 *   qty_picked  : how many items they actually took (≤ qty_to_pick)
 *   Returns SUPCIS_ERR_CONFLICT if task is not in ASSIGNED state.
 *   Returns SUPCIS_ERR_INVALID_ARG if qty_picked is 0 or exceeds qty_to_pick.
 *
 *   Short-picks (qty_picked < qty_to_pick) are allowed — the remaining
 *   quantity is handled by backorder logic at the application layer.
 */
supcis_result_t pick_task_confirm(pick_task_t *task, quantity_t qty_picked);

/*
 * pick_wave_close — marks a wave as finished.
 *   Returns SUPCIS_ERR_CONFLICT if any task is still PENDING or ASSIGNED.
 *   All tasks must be CONFIRMED or CANCELLED before a wave can close.
 */
supcis_result_t pick_wave_close(pick_wave_t *wave);

/*
 * pick_wave_pending_count — counts tasks still needing action.
 *   Returns the number of PENDING + ASSIGNED tasks.
 *   When this returns 0, the wave is ready to close.
 */
int pick_wave_pending_count(const pick_wave_t *wave);

#endif /* PICKING_ENTITY_H */
