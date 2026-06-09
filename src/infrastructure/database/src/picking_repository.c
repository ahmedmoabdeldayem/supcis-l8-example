/*
 * picking_repository.c -- Oracle implementation of picking_repository_t
 *
 * A pick wave spans two tables:
 *   pick_wave  (wave_id, order_id, released_at, is_closed)
 *   pick_task  (task_id, wave_id, order_line_id, sku_id, location_code,
 *               qty_to_pick, qty_picked, status)
 *
 * Status codes stored in pick_task.status:
 *   0 = PENDING   -- created, not yet assigned to a picker
 *   1 = ASSIGNED  -- a picker has scanned and claimed this task
 *   2 = DONE      -- picker confirmed; qty_picked >= qty_to_pick
 *   3 = CANCELLED -- order was cancelled before this task was completed
 *   (See picking_entity.h for the PICK_TASK_STATUS_* enum.)
 *
 * Same struct-embedding pattern as stock_item_repository.c:
 *   oracle_picking_repo_t embeds picking_repository_t as its first field.
 */
#include "picking_repository.h"
#include "db_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * SQL QUERY STRINGS
 * =========================================================================*/

/*
 * Insert a wave header row.
 * :1 = wave_id, :2 = order_id
 * is_closed starts at 0 (open). released_at is set by Oracle SYSTIMESTAMP.
 */
static const char *SQL_INSERT_WAVE =
    "INSERT INTO pick_wave (wave_id, order_id, released_at, is_closed) "
    "VALUES (:1, :2, SYSTIMESTAMP, 0)";

/*
 * Insert one pick task.
 * :1 = task_id, :2 = wave_id, :3 = order_line_id
 * :4 = sku_id,  :5 = location_code, :6 = qty_to_pick
 * qty_picked starts at 0; status starts at 0 (PENDING).
 */
static const char *SQL_INSERT_TASK =
    "INSERT INTO pick_task "
    "    (task_id, wave_id, order_line_id, sku_id, location_code, "
    "     qty_to_pick, qty_picked, status) "
    "VALUES (:1, :2, :3, :4, :5, :6, 0, 0)";

/*
 * Load the wave header by UUID.
 * :1 = wave_id
 *
 * is_closed is stored as NUMBER(1): 0=open, 1=closed.
 */
static const char *SQL_FIND_WAVE_BY_ID =
    "SELECT wave_id, order_id, "
    "       CAST(released_at AS NUMBER) AS released_epoch, "
    "       is_closed "
    "FROM   pick_wave "
    "WHERE  wave_id = :1";

/*
 * Load all tasks for a wave (used when loading a complete wave).
 * :1 = wave_id
 * ORDER BY task_id keeps tasks in creation order.
 */
static const char *SQL_FIND_TASKS_BY_WAVE =
    "SELECT task_id, order_line_id, sku_id, location_code, "
    "       qty_to_pick, qty_picked, status "
    "FROM   pick_task "
    "WHERE  wave_id = :1 "
    "ORDER  BY task_id";

/*
 * Load a single task by its UUID.
 * :1 = task_id
 * Used by app_confirm_pick_task as its first step.
 */
static const char *SQL_FIND_TASK_BY_ID =
    "SELECT task_id, order_line_id, sku_id, location_code, "
    "       qty_to_pick, qty_picked, status "
    "FROM   pick_task "
    "WHERE  task_id = :1";

/*
 * Update a single task's status and qty_picked after confirmation.
 * :1 = new status, :2 = qty_picked, :3 = task_id
 *
 * Also used when cancelling a task (status -> 3 CANCELLED, qty_picked = 0).
 */
static const char *SQL_UPDATE_TASK =
    "UPDATE pick_task "
    "SET    status     = :1, "
    "       qty_picked = :2 "
    "WHERE  task_id = :3";

/*
 * Close a wave after all its tasks are done.
 * :1 = wave_id
 * Called by app_confirm_pick_task when pick_wave_pending_count(wave) == 0.
 */
static const char *SQL_CLOSE_WAVE =
    "UPDATE pick_wave "
    "SET    is_closed = 1 "
    "WHERE  wave_id = :1";

/*
 * Load all OPEN tasks for an order (status PENDING or ASSIGNED).
 *
 * Used by app_cancel_order to find every task whose reservation must be
 * released. The JOIN walks: pick_task -> pick_wave -> order_line -> order.
 *
 * :1 = order_id
 *
 * WHY JOIN THROUGH pick_wave?
 *   pick_task does not store order_id directly -- it stores wave_id.
 *   The wave links back to the order. The JOIN chain reconstructs that path.
 *
 * "AND pt.status IN (0, 1)" excludes DONE (2) and CANCELLED (3) tasks because:
 *   - DONE tasks already had their stock deducted (not reserved anymore).
 *   - CANCELLED tasks already had their reservations released.
 *   Only PENDING/ASSIGNED tasks still hold a qty_reserved lock on stock_item.
 */
static const char *SQL_FIND_TASKS_BY_ORDER =
    "SELECT pt.task_id, pt.order_line_id, pt.sku_id, pt.location_code, "
    "       pt.qty_to_pick, pt.qty_picked, pt.status "
    "FROM   pick_task  pt "
    "JOIN   pick_wave  pw  ON pt.wave_id       = pw.wave_id "
    "JOIN   order_line ol  ON pt.order_line_id = ol.line_id "
    "WHERE  ol.order_id = :1 "
    "AND    pt.status IN (0, 1) "
    "ORDER  BY pt.task_id";

/* =========================================================================
 * CONCRETE REPOSITORY STRUCT
 * =========================================================================*/

typedef struct {
    picking_repository_t base;   /* MUST be first */
    db_handle_t         *db;
} oracle_picking_repo_t;

/* =========================================================================
 * IMPLEMENTATION FUNCTIONS
 * =========================================================================*/

/*
 * impl_save_wave
 *
 * Inserts the wave header and then all tasks in a loop.
 * Always called inside a db_begin/db_commit transaction (started by the
 * application service) so that a partial write never leaves orphan tasks.
 *
 * OCI BINDING FOR WAVE HEADER:
 *   OCIBindByPos(stmt, &b1, err, 1, wave->wave_id,  37, SQLT_STR, ...)
 *   OCIBindByPos(stmt, &b2, err, 2, wave->order_id, 37, SQLT_STR, ...)
 *   OCIStmtExecute(svc, stmt, err, 1, ...)
 *
 * OCI BINDING FOR EACH TASK (loop over wave->tasks[0..task_count-1]):
 *   OCIBindByPos(stmt2, &b1, err, 1, task->task_id,      37, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b2, err, 2, wave->wave_id,      37, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b3, err, 3, task->order_line_id,37, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b4, err, 4, task->sku_id,       strlen+1, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b5, err, 5, task->location_code,strlen+1, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b6, err, 6, &task->qty_to_pick, sizeof(int), SQLT_INT, ...)
 *   OCIStmtExecute(svc, stmt2, err, 1, ...)
 *
 * PERFORMANCE NOTE:
 *   In production, the task insert statement is prepared once and re-executed
 *   with different bind values for each task -- no re-prepare per iteration.
 *   OCIStmtPrepare2 is called once before the loop; only OCIBindByPos +
 *   OCIStmtExecute run inside the loop.
 */
static supcis_result_t impl_save_wave(
    struct picking_repository *self,
    const pick_wave_t *wave)
{
    oracle_picking_repo_t *repo = (oracle_picking_repo_t *)self;

    /*
     * NOTE: pick_wave_t does not store order_id directly -- the real
     * implementation would receive it as an extra parameter or look it up
     * via the tasks' order_line_id. This stub uses an empty string.
     * In production: add order_id to pick_wave_t or pass it separately.
     */
    LOG_DEBUG("picking_repo", "save_wave: wave=%s tasks=%d",
              wave->wave_id, wave->task_count);

    /* Insert wave header */
    const char *order_id_placeholder = "";  /* see NOTE above */
    supcis_result_t rc = db_execute(repo->db, SQL_INSERT_WAVE, 2,
        /* :1 wave_id  */ wave->wave_id,
        /* :2 order_id */ order_id_placeholder);

    if (rc != SUPCIS_OK) {
        LOG_ERROR("picking_repo", "insert wave failed: wave=%s", wave->wave_id);
        return rc;
    }

    /* Insert each task */
    for (int i = 0; i < wave->task_count; i++) {
        const pick_task_t *t = &wave->tasks[i];

        rc = db_execute(repo->db, SQL_INSERT_TASK, 6,
            /* :1 task_id       */ t->task_id,
            /* :2 wave_id       */ wave->wave_id,
            /* :3 order_line_id */ t->order_line_id,
            /* :4 sku_id        */ t->sku_id,
            /* :5 location_code */ t->location_code,
            /* :6 qty_to_pick   */ &t->qty_to_pick);

        if (rc != SUPCIS_OK) {
            LOG_ERROR("picking_repo", "insert task[%d] failed: wave=%s", i, wave->wave_id);
            return rc;
        }
    }

    LOG_DEBUG("picking_repo", "wave saved: %d tasks inserted", wave->task_count);
    return SUPCIS_OK;
}

/*
 * impl_find_wave_by_id
 *
 * Two-query load: wave header, then all tasks in a fetch loop.
 *
 * OCI COLUMN MAPPING FOR WAVE HEADER:
 *   char      col_wave_id[37]  = {0};
 *   char      col_order_id[37] = {0};
 *   long long col_released     = 0;
 *   int       col_is_closed    = 0;
 *
 *   OCIDefineByPos(stmt, &def, err, 1, col_wave_id,   37, SQLT_STR,  ...)
 *   OCIDefineByPos(stmt, &def, err, 2, col_order_id,  37, SQLT_STR,  ...)
 *   OCIDefineByPos(stmt, &def, err, 3, &col_released,  sizeof(long long), SQLT_INT, ...)
 *   OCIDefineByPos(stmt, &def, err, 4, &col_is_closed, sizeof(int),       SQLT_INT, ...)
 *
 * OCI COLUMN MAPPING FOR TASK FETCH LOOP:
 *   char col_task_id[37]      = {0};
 *   char col_line_id[37]      = {0};
 *   char col_sku_id[32]       = {0};
 *   char col_location[24]     = {0};
 *   int  col_qty_to_pick      = 0;
 *   int  col_qty_picked       = 0;
 *   int  col_status           = 0;
 *
 *   int i = 0;
 *   while (OCIStmtFetch2(...) != OCI_NO_DATA && i < PICK_WAVE_MAX_TASKS) {
 *       [copy columns into out->tasks[i]]
 *       i++;
 *   }
 *   out->task_count = i;
 */
static supcis_result_t impl_find_wave_by_id(
    struct picking_repository *self,
    const char  *wave_id,
    pick_wave_t *out)
{
    oracle_picking_repo_t *repo = (oracle_picking_repo_t *)self;

    LOG_DEBUG("picking_repo", "find_wave: wave=%s", wave_id);

    supcis_result_t rc = db_query_one(repo->db, SQL_FIND_WAVE_BY_ID,
                                      out, sizeof(*out));
    if (rc != SUPCIS_OK) {
        LOG_DEBUG("picking_repo", "wave not found: %s", wave_id);
        return rc;
    }
    /* In the real implementation, SQL_FIND_TASKS_BY_WAVE fills out->tasks[] here. */
    (void)SQL_FIND_TASKS_BY_WAVE;
    return SUPCIS_OK;
}

/*
 * impl_find_task_by_id
 *
 * Single-row SELECT -- the simplest repository query.
 *
 * OCI BINDING:
 *   OCIBindByPos(stmt, &b1, err, 1, task_id, strlen(task_id)+1, SQLT_STR, ...)
 *
 * OCI COLUMN MAPPING (same as task columns in the wave loop above):
 *   OCIDefineByPos for 7 columns -> copy into out->task_id, sku_id, etc.
 */
static supcis_result_t impl_find_task_by_id(
    struct picking_repository *self,
    const char  *task_id,
    pick_task_t *out)
{
    oracle_picking_repo_t *repo = (oracle_picking_repo_t *)self;

    LOG_DEBUG("picking_repo", "find_task: task=%s", task_id);

    supcis_result_t rc = db_query_one(repo->db, SQL_FIND_TASK_BY_ID,
                                      out, sizeof(*out));
    if (rc == SUPCIS_ERR_NOT_FOUND)
        LOG_DEBUG("picking_repo", "task not found: %s", task_id);
    return rc;
}

/*
 * impl_save_task
 *
 * UPDATE the status and qty_picked for a single task.
 * Called after pick_task_confirm() or pick_task_cancel() succeeds in the domain.
 *
 * WHY NOT MERGE?
 *   Tasks are always pre-created by save_wave. We never INSERT a task here --
 *   we only update one that already exists. A plain UPDATE is clearer than MERGE.
 *
 * OCI BINDING:
 *   OCIBindByPos(stmt, &b1, err, 1, &task->status,     sizeof(int), SQLT_INT, ...)
 *   OCIBindByPos(stmt, &b2, err, 2, &task->qty_picked, sizeof(int), SQLT_INT, ...)
 *   OCIBindByPos(stmt, &b3, err, 3, task->task_id,     37, SQLT_STR, ...)
 *   OCIStmtExecute(svc, stmt, err, 1, ...)
 */
static supcis_result_t impl_save_task(
    struct picking_repository *self,
    const pick_task_t *task)
{
    oracle_picking_repo_t *repo = (oracle_picking_repo_t *)self;

    LOG_DEBUG("picking_repo", "save_task: task=%s status=%d qty_picked=%d",
              task->task_id, task->status, task->qty_picked);

    supcis_result_t rc = db_execute(repo->db, SQL_UPDATE_TASK, 3,
        /* :1 status     */ &task->status,
        /* :2 qty_picked */ &task->qty_picked,
        /* :3 task_id    */ task->task_id);

    if (rc != SUPCIS_OK)
        LOG_ERROR("picking_repo", "save_task failed: task=%s", task->task_id);

    /* Suppress unused warning for SQL_CLOSE_WAVE (used by close_wave, not shown here) */
    (void)SQL_CLOSE_WAVE;
    return rc;
}

/*
 * impl_find_tasks_by_order
 *
 * Loads all OPEN tasks (PENDING + ASSIGNED) for a given order_id.
 * Uses a three-table JOIN because pick_task does not store order_id directly.
 *
 * OCI BINDING:
 *   OCIBindByPos(stmt, &b1, err, 1, order_id, strlen(order_id)+1, SQLT_STR, ...)
 *
 * OCI COLUMN MAPPING (same 7 columns as impl_find_task_by_id):
 *   Define 7 columns, then fetch in a loop:
 *
 *   int i = 0;
 *   while (OCIStmtFetch2(...) != OCI_NO_DATA && i < max_tasks) {
 *       [copy into out_tasks[i]]
 *       i++;
 *   }
 *   *out_count = i;
 *
 * NOTE ON max_tasks:
 *   The caller passes the size of their stack-allocated array. If the DB
 *   returns more rows than max_tasks, we stop and return SUPCIS_ERR_OVERFLOW
 *   in production (not shown here in the stub).
 */
static supcis_result_t impl_find_tasks_by_order(
    struct picking_repository *self,
    const char  *order_id,
    pick_task_t *out_tasks,
    int          max_tasks,
    int         *out_count)
{
    oracle_picking_repo_t *repo = (oracle_picking_repo_t *)self;

    LOG_DEBUG("picking_repo", "find_tasks_by_order: order=%s max=%d",
              order_id, max_tasks);

    /*
     * Real OCI steps:
     *   1. Prepare + bind SQL_FIND_TASKS_BY_ORDER (:1 = order_id)
     *   2. Define 7 output columns
     *   3. OCIStmtExecute (rows=0 for SELECT)
     *   4. Loop:  OCIStmtFetch2 until OCI_NO_DATA or i >= max_tasks
     *      copy task columns into out_tasks[i], i++
     *   5. *out_count = i
     */

    /* Stub: no actual rows returned */
    (void)repo;
    *out_count = 0;

    (void)out_tasks;
    (void)SQL_FIND_TASKS_BY_ORDER;

    return SUPCIS_OK;
}

/* =========================================================================
 * CONSTRUCTOR / DESTRUCTOR
 * =========================================================================*/

picking_repository_t *oracle_picking_repo_create(db_handle_t *db)
{
    oracle_picking_repo_t *r = calloc(1, sizeof(oracle_picking_repo_t));
    if (!r) return NULL;

    r->base.save_wave           = impl_save_wave;
    r->base.find_wave_by_id     = impl_find_wave_by_id;
    r->base.find_task_by_id     = impl_find_task_by_id;
    r->base.save_task           = impl_save_task;
    r->base.find_tasks_by_order = impl_find_tasks_by_order;
    r->db = db;

    LOG_DEBUG("picking_repo", "Oracle picking repository initialized");
    return &r->base;
}

void oracle_picking_repo_destroy(picking_repository_t *repo)
{
    free((oracle_picking_repo_t *)repo);
}
