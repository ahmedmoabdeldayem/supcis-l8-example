#ifndef PICKING_REPOSITORY_H
#define PICKING_REPOSITORY_H

/*
 * picking_repository.h -- abstract persistence interface for waves and tasks
 *
 * Oracle implementation: src/infrastructure/database/src/picking_repository.c
 *
 * A wave spans two tables:
 *   pick_wave  -- one row per wave (wave_id, order_id, released_at, is_closed)
 *   pick_task  -- N rows per wave  (task_id, wave_id, sku_id, location, qty, status)
 *
 * Loading a wave requires two queries; saving a wave requires inserts into both.
 * Tasks can also be saved individually (one UPDATE) when a picker confirms.
 */

#include "picking_entity.h"

typedef struct picking_repository {

    /*
     * save_wave -- persist a new wave and all its tasks to Oracle.
     *
     *   Called once when app_release_order creates the wave.
     *   Always called inside a db_begin/db_commit transaction so that
     *   the wave and all its tasks appear atomically -- no partial waves.
     *
     *   INSERT INTO pick_wave (wave_id, order_id, released_at, is_closed)
     *   VALUES (:1, :2, SYSTIMESTAMP, 0)
     *
     *   For each task in wave->tasks[0..task_count-1]:
     *   INSERT INTO pick_task
     *     (task_id, wave_id, order_line_id, sku_id, location_code,
     *      qty_to_pick, qty_picked, status)
     *   VALUES (:1, :2, :3, :4, :5, :6, 0, 0)
     */
    supcis_result_t (*save_wave)(
        struct picking_repository *self,
        const pick_wave_t *wave);

    /*
     * find_wave_by_id -- load a complete wave (header + all tasks) by UUID.
     *
     *   Two queries:
     *   1. SELECT wave_id, order_id, released_at, is_closed
     *      FROM pick_wave WHERE wave_id = :1
     *
     *   2. SELECT task_id, order_line_id, sku_id, location_code,
     *             qty_to_pick, qty_picked, status
     *      FROM pick_task WHERE wave_id = :1 ORDER BY task_id
     *      (fetches rows in a loop until OCI_NO_DATA)
     *
     *   Used by: app_confirm_pick_task to check if the wave is now done.
     *   Used by: handle_get_wave for the supervisor dashboard.
     */
    supcis_result_t (*find_wave_by_id)(
        struct picking_repository *self,
        const char  *wave_id,
        pick_wave_t *out);

    /*
     * find_task_by_id -- load a single pick task by UUID.
     *
     *   SELECT task_id, order_line_id, sku_id, location_code,
     *          qty_to_pick, qty_picked, status
     *   FROM   pick_task WHERE task_id = :1
     *
     *   Used by: app_confirm_pick_task as its first step.
     */
    supcis_result_t (*find_task_by_id)(
        struct picking_repository *self,
        const char  *task_id,
        pick_task_t *out);

    /*
     * save_task -- update a single task after confirmation or cancellation.
     *
     *   UPDATE pick_task
     *   SET    status = :1, qty_picked = :2
     *   WHERE  task_id = :3
     *
     *   Called by app_confirm_pick_task after pick_task_confirm() succeeds.
     *   Also called when a task is cancelled individually.
     */
    supcis_result_t (*save_task)(
        struct picking_repository *self,
        const pick_task_t *task);

    /*
     * find_tasks_by_order -- load all open tasks belonging to an order.
     *
     *   Used by app_cancel_order to release stock reservations:
     *   it needs to know every task (and its reserved qty) for the order.
     *
     *   SELECT pt.task_id, pt.order_line_id, pt.sku_id, pt.location_code,
     *          pt.qty_to_pick, pt.qty_picked, pt.status
     *   FROM   pick_task pt
     *   JOIN   pick_wave pw   ON pt.wave_id      = pw.wave_id
     *   JOIN   order_line ol  ON pt.order_line_id = ol.line_id
     *   WHERE  ol.order_id = :1
     *   AND    pt.status IN (0, 1)   -- 0=PENDING, 1=ASSIGNED (open only)
     *   ORDER  BY pt.task_id
     *
     *   out_tasks  : caller-provided array large enough for max tasks
     *   out_count  : receives actual number of rows returned
     */
    supcis_result_t (*find_tasks_by_order)(
        struct picking_repository *self,
        const char  *order_id,
        pick_task_t *out_tasks,
        int          max_tasks,
        int         *out_count);

} picking_repository_t;

#endif /* PICKING_REPOSITORY_H */
