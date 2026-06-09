/*
 * order_repository.c -- Oracle implementation of order_repository_t
 *
 * An order spans two tables:
 *   order_header  (order_id, customer_ref, status, created_at)
 *   order_line    (line_id, order_id, sku_id, qty_ordered, qty_picked)
 *
 * Loading requires two SELECT queries (header, then lines in a fetch loop).
 * Saving requires:
 *   - New order:      INSERT header + INSERT each line
 *   - Existing order: UPDATE header status + UPDATE each changed line's qty_picked
 *
 * Same struct-embedding pattern as stock_item_repository.c:
 *   oracle_order_repo_t embeds order_repository_t as its first field.
 */
#include "order_repository.h"
#include "db_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * SQL QUERY STRINGS
 * =========================================================================*/

/*
 * Load the header row for a given order UUID.
 * :1 = order_id
 *
 * CAST(created_at AS NUMBER) converts Oracle TIMESTAMP to a Unix epoch
 * integer so we can store it in order_t.created_at (time_t / long long).
 */
static const char *SQL_FIND_ORDER_BY_ID =
    "SELECT order_id, customer_ref, status, "
    "       CAST(created_at AS NUMBER) AS created_epoch "
    "FROM   order_header "
    "WHERE  order_id = :1";

/*
 * Load all lines for an order.
 * :1 = order_id
 *
 * ORDER BY line_id keeps lines in insertion order so index 0 always maps to
 * the first line SAP sent. The loop in impl_find_by_id fetches rows one at
 * a time until OCI_NO_DATA.
 */
static const char *SQL_FIND_ORDER_LINES =
    "SELECT line_id, sku_id, qty_ordered, qty_picked "
    "FROM   order_line "
    "WHERE  order_id = :1 "
    "ORDER  BY line_id";

/*
 * Look up an order by the ERP's own document number.
 * :1 = customer_ref
 *
 * Used in two scenarios:
 *   1. Duplicate detection in app_create_order -- if found, return CONFLICT.
 *   2. Status queries from SAP that reference its own order number.
 */
static const char *SQL_FIND_ORDER_BY_CUSTOMER_REF =
    "SELECT order_id, customer_ref, status, "
    "       CAST(created_at AS NUMBER) AS created_epoch "
    "FROM   order_header "
    "WHERE  customer_ref = :1";

/*
 * Insert a new order header row.
 * :1 = order_id, :2 = customer_ref, :3 = status (int)
 * SYSTIMESTAMP is set by Oracle -- no clock skew from the app server.
 */
static const char *SQL_INSERT_ORDER_HEADER =
    "INSERT INTO order_header (order_id, customer_ref, status, created_at) "
    "VALUES (:1, :2, :3, SYSTIMESTAMP)";

/*
 * Insert one order line.
 * :1 = line_id, :2 = order_id, :3 = sku_id, :4 = qty_ordered
 * qty_picked starts at 0 -- nothing has been picked yet.
 */
static const char *SQL_INSERT_ORDER_LINE =
    "INSERT INTO order_line (line_id, order_id, sku_id, qty_ordered, qty_picked) "
    "VALUES (:1, :2, :3, :4, 0)";

/*
 * Update only the status field on an existing header.
 * :1 = new status (int), :2 = order_id
 *
 * This is the most frequent update path: NEW -> RELEASED -> PACKING -> SHIPPED.
 * qty fields live on the lines, not the header.
 */
static const char *SQL_UPDATE_ORDER_STATUS =
    "UPDATE order_header "
    "SET    status = :1 "
    "WHERE  order_id = :2";

/*
 * Update the qty_picked counter on one line after confirming a pick task.
 * :1 = new qty_picked, :2 = line_id
 *
 * Called for each line whose qty_picked has changed.
 * The application layer tracks which lines changed; we don't update all lines
 * on every save to avoid unnecessary I/O.
 */
static const char *SQL_UPDATE_ORDER_LINE_QTY =
    "UPDATE order_line "
    "SET    qty_picked = :1 "
    "WHERE  line_id = :2";

/* =========================================================================
 * CONCRETE REPOSITORY STRUCT
 * =========================================================================*/

/*
 * oracle_order_repo_t -- concrete Oracle implementation.
 * base MUST be first (see stock_item_repository.c for full explanation).
 */
typedef struct {
    order_repository_t base;   /* MUST be first */
    db_handle_t       *db;
} oracle_order_repo_t;

/* =========================================================================
 * IMPLEMENTATION FUNCTIONS
 * =========================================================================*/

/*
 * impl_find_by_id
 *
 * Two-query load: first fetch the header row, then fetch all lines in a loop.
 *
 * WHY TWO QUERIES INSTEAD OF A JOIN?
 *   A JOIN would repeat the header columns on every line row, wasting network
 *   bandwidth and making the OCI mapping more complex (duplicate column names).
 *   Two simple queries are easier to bind, define, and reason about.
 *
 * OCI COLUMN MAPPING FOR HEADER:
 *   char      col_order_id[37]     = {0};
 *   char      col_customer_ref[64] = {0};
 *   int       col_status           = 0;
 *   long long col_created_at       = 0;
 *
 *   OCIDefineByPos(stmt, &def, err, 1, col_order_id,     37, SQLT_STR, ...)
 *   OCIDefineByPos(stmt, &def, err, 2, col_customer_ref, 64, SQLT_STR, ...)
 *   OCIDefineByPos(stmt, &def, err, 3, &col_status,      sizeof(int),       SQLT_INT, ...)
 *   OCIDefineByPos(stmt, &def, err, 4, &col_created_at,  sizeof(long long), SQLT_INT, ...)
 *
 * OCI COLUMN MAPPING FOR LINE FETCH LOOP:
 *   char col_line_id[37]  = {0};
 *   char col_sku_id[32]   = {0};
 *   int  col_qty_ordered  = 0;
 *   int  col_qty_picked   = 0;
 *
 *   OCIDefineByPos(stmt2, &def, err, 1, col_line_id,    37, SQLT_STR, ...)
 *   OCIDefineByPos(stmt2, &def, err, 2, col_sku_id,     32, SQLT_STR, ...)
 *   OCIDefineByPos(stmt2, &def, err, 3, &col_qty_ordered, sizeof(int), SQLT_INT, ...)
 *   OCIDefineByPos(stmt2, &def, err, 4, &col_qty_picked,  sizeof(int), SQLT_INT, ...)
 *
 *   int i = 0;
 *   while (OCIStmtFetch2(stmt2, err, 1, OCI_FETCH_NEXT, ...) != OCI_NO_DATA
 *          && i < ORDER_MAX_LINES) {
 *       strncpy(out->lines[i].line_id, col_line_id, sizeof(out->lines[i].line_id)-1);
 *       strncpy(out->lines[i].sku_id,  col_sku_id,  sizeof(out->lines[i].sku_id)-1);
 *       out->lines[i].qty_ordered = col_qty_ordered;
 *       out->lines[i].qty_picked  = col_qty_picked;
 *       i++;
 *   }
 *   out->line_count = i;
 */
static supcis_result_t impl_find_by_id(
    struct order_repository *self,
    const char *order_id,
    order_t    *out)
{
    oracle_order_repo_t *repo = (oracle_order_repo_t *)self;

    LOG_DEBUG("order_repo", "find_by_id: order=%s", order_id);

    /*
     * Real OCI steps:
     *   1. Prepare + bind + execute SQL_FIND_ORDER_BY_ID (:1 = order_id)
     *   2. Fetch one row; if OCI_NO_DATA return SUPCIS_ERR_NOT_FOUND
     *   3. Copy header columns into out->order_id, out->customer_ref, out->status, out->created_at
     *   4. Prepare + bind + execute SQL_FIND_ORDER_LINES (:1 = order_id)
     *   5. Fetch rows in loop until OCI_NO_DATA, filling out->lines[]
     *   6. Set out->line_count to the number of lines fetched
     */
    supcis_result_t rc = db_query_one(repo->db, SQL_FIND_ORDER_BY_ID,
                                      out, sizeof(*out));
    if (rc != SUPCIS_OK) {
        LOG_DEBUG("order_repo", "not found: order=%s", order_id);
        return rc;
    }
    /* In the real implementation, a second query fills out->lines[] here. */
    return SUPCIS_OK;
}

/*
 * impl_find_by_customer_ref
 *
 * Same as find_by_id but keyed on the ERP's document number.
 * If the order is found, we also fetch its lines (same loop as above).
 * If not found, SUPCIS_ERR_NOT_FOUND is returned -- which is expected in the
 * duplicate-check path (means the order is new and we can proceed).
 *
 * OCI BINDING:
 *   OCIBindByPos(stmt, &b1, err, 1, customer_ref, strlen(customer_ref)+1, SQLT_STR, ...)
 */
static supcis_result_t impl_find_by_customer_ref(
    struct order_repository *self,
    const char *customer_ref,
    order_t    *out)
{
    oracle_order_repo_t *repo = (oracle_order_repo_t *)self;

    LOG_DEBUG("order_repo", "find_by_customer_ref: ref=%s", customer_ref);

    supcis_result_t rc = db_query_one(repo->db, SQL_FIND_ORDER_BY_CUSTOMER_REF,
                                      out, sizeof(*out));
    if (rc == SUPCIS_ERR_NOT_FOUND)
        LOG_DEBUG("order_repo", "not found by customer_ref=%s", customer_ref);
    return rc;
}

/*
 * impl_save
 *
 * Detects whether the order is new (no header row yet) or existing,
 * then executes the appropriate INSERT or UPDATE statements.
 *
 * NEW ORDER PATH (called by app_create_order):
 *   1. INSERT order_header with status=NEW, SYSTIMESTAMP
 *   2. For each line: INSERT order_line with qty_picked=0
 *
 * EXISTING ORDER PATH (called when status changes or qty_picked changes):
 *   1. UPDATE order_header SET status = :1 WHERE order_id = :2
 *   2. For each line where qty_picked changed:
 *      UPDATE order_line SET qty_picked = :1 WHERE line_id = :2
 *
 * DETECTING NEW vs EXISTING:
 *   The real implementation can check order->order_id against the DB first,
 *   or the application layer can explicitly call different repo methods for
 *   create vs update. The single save() method keeps the interface simple.
 *
 * TRANSACTION CONTEXT:
 *   This function does NOT commit. The caller (application service) wraps
 *   multiple saves in db_begin/db_commit so that a wave + order save is atomic.
 *
 * OCI BINDING FOR INSERT HEADER:
 *   OCIBindByPos(stmt, &b1, err, 1, order->order_id,     37, SQLT_STR, ...)
 *   OCIBindByPos(stmt, &b2, err, 2, order->customer_ref, strlen(...)+1, SQLT_STR, ...)
 *   OCIBindByPos(stmt, &b3, err, 3, &order->status,      sizeof(int), SQLT_INT, ...)
 *   OCIStmtExecute(svc, stmt, err, 1, ...)  -- iters=1 for DML
 *
 * OCI BINDING FOR INSERT LINE (repeated for each line):
 *   OCIBindByPos(stmt2, &b1, err, 1, line->line_id,    37, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b2, err, 2, order->order_id,  37, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b3, err, 3, line->sku_id,     strlen(...)+1, SQLT_STR, ...)
 *   OCIBindByPos(stmt2, &b4, err, 4, &line->qty_ordered, sizeof(int), SQLT_INT, ...)
 *   OCIStmtExecute(svc, stmt2, err, 1, ...)
 */
static supcis_result_t impl_save(
    struct order_repository *self,
    const order_t *order)
{
    oracle_order_repo_t *repo = (oracle_order_repo_t *)self;

    LOG_DEBUG("order_repo", "save: order=%s status=%d lines=%d",
              order->order_id, order->status, order->line_count);

    /*
     * Real OCI path for UPDATE (most common case -- status change):
     *   rc = db_execute(repo->db, SQL_UPDATE_ORDER_STATUS, 2,
     *                   &order->status, order->order_id);
     *   if (rc != SUPCIS_OK) return rc;
     *
     *   for (int i = 0; i < order->line_count; i++) {
     *       db_execute(repo->db, SQL_UPDATE_ORDER_LINE_QTY, 2,
     *                  &order->lines[i].qty_picked, order->lines[i].line_id);
     *   }
     *
     * For INSERT (new order), use SQL_INSERT_ORDER_HEADER + SQL_INSERT_ORDER_LINE.
     */
    supcis_result_t rc = db_execute(repo->db, SQL_UPDATE_ORDER_STATUS, 2,
        /* :1 status   */ &order->status,
        /* :2 order_id */ order->order_id);

    if (rc != SUPCIS_OK) {
        LOG_ERROR("order_repo", "save failed for order=%s", order->order_id);
    }

    /* Suppress unused-variable warnings for the other SQL constants in this stub */
    (void)SQL_FIND_ORDER_LINES;
    (void)SQL_INSERT_ORDER_HEADER;
    (void)SQL_INSERT_ORDER_LINE;
    (void)SQL_UPDATE_ORDER_LINE_QTY;

    return rc;
}

/* =========================================================================
 * CONSTRUCTOR / DESTRUCTOR
 * =========================================================================*/

order_repository_t *oracle_order_repo_create(db_handle_t *db)
{
    oracle_order_repo_t *r = calloc(1, sizeof(oracle_order_repo_t));
    if (!r) return NULL;

    r->base.find_by_id           = impl_find_by_id;
    r->base.find_by_customer_ref = impl_find_by_customer_ref;
    r->base.save                 = impl_save;
    r->db = db;

    LOG_DEBUG("order_repo", "Oracle order repository initialized");
    return &r->base;
}

void oracle_order_repo_destroy(order_repository_t *repo)
{
    free((oracle_order_repo_t *)repo);
}
