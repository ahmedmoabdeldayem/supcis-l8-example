/*
 * stock_item_repository.c -- Oracle implementation of inventory_repository_t
 *
 * This file contains:
 *   - The SQL query strings used at runtime
 *   - The concrete struct that embeds the abstract interface + a DB handle
 *   - One C function per repository method
 *   - A constructor that wires them together
 *
 * HOW THE CONCRETE STRUCT WORKS:
 *
 *   inventory_repository_t (abstract -- just function pointers)
 *         ^
 *         | "inherits" via embedding as first field
 *         |
 *   oracle_stock_item_repo_t (concrete -- adds db_handle_t *)
 *
 *   When a caller does:
 *     repo->find_by_sku_location(repo, sku, loc, &item)
 *
 *   The function receives `self` = repo (type: inventory_repository_t *).
 *   It casts self to oracle_stock_item_repo_t * to reach the db field.
 *   This cast is safe ONLY because oracle_stock_item_repo_t.base is first.
 */
#include "inventory_repository.h"
#include "db_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * SQL QUERY STRINGS
 * =========================================================================
 * Defined as static constants so they are easy to find, read, and change.
 * :1, :2, ... are Oracle bind variable placeholders (positional).
 * They are bound with OCIBindByPos() in the real OCI implementation.
 *
 * WHY BIND VARIABLES INSTEAD OF snprintf INTO THE SQL?
 *   SQL injection: if location_code came from user input and was sprintf'd
 *   into the query, a value like "' OR '1'='1" would change the query logic.
 *   With bind variables, Oracle treats the value as data, never as SQL.
 *   Also: Oracle caches the execution plan for a SQL string. If the string
 *   changes every call (because values are inlined), the plan cache fills up
 *   with thousands of one-time queries. Bind variables keep the cache small.
 */

/*
 * Find stock at a specific bin.
 * :1 = sku_id, :2 = location_code
 */
static const char *SQL_FIND_BY_SKU_AND_LOCATION =
    "SELECT stock_id, sku_id, location_code, qty_on_hand, qty_reserved, "
    "       CAST(last_updated AS NUMBER) AS last_updated_epoch "
    "FROM   stock_item "
    "WHERE  sku_id        = :1 "
    "AND    location_code = :2";

/*
 * Find the best bin for a SKU when no specific bin is requested.
 * Returns the single bin with the most available stock.
 * :1 = sku_id
 *
 * "FETCH FIRST 1 ROW ONLY" is Oracle 12c+ syntax for LIMIT 1.
 */
static const char *SQL_FIND_BEST_BIN_FOR_SKU =
    "SELECT stock_id, sku_id, location_code, qty_on_hand, qty_reserved, "
    "       CAST(last_updated AS NUMBER) AS last_updated_epoch "
    "FROM   stock_item "
    "WHERE  sku_id = :1 "
    "AND    qty_on_hand > qty_reserved "
    "ORDER  BY (qty_on_hand - qty_reserved) DESC "
    "FETCH  FIRST 1 ROW ONLY";

/*
 * Upsert (INSERT or UPDATE) a stock item.
 *
 * MERGE is Oracle's combined INSERT + UPDATE statement.
 * It checks whether a matching row exists: if yes, UPDATE; if no, INSERT.
 * This lets callers always use save() without knowing if the row is new.
 *
 * :1 = stock_id  (used to match existing rows AND as INSERT value)
 * :2 = qty_on_hand
 * :3 = qty_reserved
 * :4 = sku_id         (INSERT only)
 * :5 = location_code  (INSERT only)
 */
static const char *SQL_MERGE_STOCK_ITEM =
    "MERGE INTO stock_item dst "
    "USING (SELECT :1 AS stock_id FROM dual) src "
    "ON    (dst.stock_id = src.stock_id) "
    "WHEN MATCHED THEN "
    "    UPDATE SET qty_on_hand   = :2, "
    "               qty_reserved  = :3, "
    "               last_updated  = SYSTIMESTAMP "
    "WHEN NOT MATCHED THEN "
    "    INSERT (stock_id, sku_id, location_code, qty_on_hand, qty_reserved, last_updated) "
    "    VALUES (:1, :4, :5, :2, :3, SYSTIMESTAMP)";

/*
 * Sum available stock across all bins for a SKU.
 * Used by the order availability check before release.
 * :1 = sku_id
 *
 * NVL(..., 0) converts NULL to 0 so the result is always a number
 * (SUM of zero rows returns NULL in Oracle, not 0).
 */
static const char *SQL_SUM_AVAILABLE_BY_SKU =
    "SELECT NVL(SUM(qty_on_hand - qty_reserved), 0) AS total_available "
    "FROM   stock_item "
    "WHERE  sku_id = :1";

/* =========================================================================
 * CONCRETE REPOSITORY STRUCT
 * =========================================================================*/

/*
 * oracle_stock_item_repo_t -- the concrete Oracle implementation.
 *
 * base MUST be the first field. This guarantees that a pointer to
 * oracle_stock_item_repo_t has the same address as a pointer to its base,
 * making the (inventory_repository_t *) <-> (oracle_stock_item_repo_t *)
 * cast safe in both directions.
 */
typedef struct {
    inventory_repository_t base;   /* MUST be first -- see comment above */
    db_handle_t           *db;     /* Oracle connection (from db_connect) */
} oracle_stock_item_repo_t;

/* =========================================================================
 * IMPLEMENTATION FUNCTIONS
 * =========================================================================*/

/*
 * impl_find_by_sku_location
 *
 * Selects one row from stock_item. If location_code is NULL, it selects
 * the bin with the most available stock (best-bin selection for wave creation).
 *
 * OCI COLUMN MAPPING (what the real implementation does):
 *
 *   char     col_stock_id[37]      = {0};
 *   char     col_sku_id[32]        = {0};
 *   char     col_location_code[24] = {0};
 *   int      col_qty_on_hand       = 0;
 *   int      col_qty_reserved      = 0;
 *   long long col_last_updated     = 0;
 *
 *   OCIDefineByPos(stmt, &def, err, 1, col_stock_id,      37, SQLT_STR, ...)
 *   OCIDefineByPos(stmt, &def, err, 2, col_sku_id,        32, SQLT_STR, ...)
 *   OCIDefineByPos(stmt, &def, err, 3, col_location_code, 24, SQLT_STR, ...)
 *   OCIDefineByPos(stmt, &def, err, 4, &col_qty_on_hand,  sizeof(int), SQLT_INT, ...)
 *   OCIDefineByPos(stmt, &def, err, 5, &col_qty_reserved, sizeof(int), SQLT_INT, ...)
 *   OCIDefineByPos(stmt, &def, err, 6, &col_last_updated, sizeof(long long), SQLT_INT, ...)
 *
 *   After OCIStmtFetch2:
 *   strncpy(out->stock_id,       col_stock_id,      sizeof(out->stock_id)-1);
 *   strncpy(out->sku_id,         col_sku_id,        sizeof(out->sku_id)-1);
 *   strncpy(out->location_code,  col_location_code, sizeof(out->location_code)-1);
 *   out->quantity_on_hand  = col_qty_on_hand;
 *   out->quantity_reserved = col_qty_reserved;
 *   out->last_updated      = col_last_updated;
 */
static supcis_result_t impl_find_by_sku_location(
    inventory_repository_t *self,
    const char   *sku_id,
    const char   *location_code,
    stock_item_t *out)
{
    oracle_stock_item_repo_t *repo = (oracle_stock_item_repo_t *)self;
    const char *sql = location_code
                      ? SQL_FIND_BY_SKU_AND_LOCATION
                      : SQL_FIND_BEST_BIN_FOR_SKU;

    LOG_DEBUG("stock_repo", "find: sku=%s location=%s",
              sku_id, location_code ? location_code : "(best bin)");

    /*
     * Real OCI call:
     *   OCIStmtPrepare2(repo->db->svc, &stmt, err, sql, strlen(sql), ...)
     *   OCIBindByPos(stmt, &b1, err, 1, sku_id, strlen(sku_id)+1, SQLT_STR, ...)
     *   if (location_code)
     *     OCIBindByPos(stmt, &b2, err, 2, location_code, ..., SQLT_STR, ...)
     *   [OCIDefineByPos calls -- see comment above]
     *   OCIStmtExecute(svc, stmt, err, 0, ...)
     *   rc = OCIStmtFetch2(stmt, err, 1, OCI_FETCH_NEXT, ...)
     *   if (rc == OCI_NO_DATA) return SUPCIS_ERR_NOT_FOUND;
     *   [copy column buffers into out struct]
     *   OCIStmtRelease(stmt, ...)
     */
    supcis_result_t rc = db_query_one(repo->db, sql, out, sizeof(*out));
    if (rc == SUPCIS_ERR_NOT_FOUND) {
        LOG_DEBUG("stock_repo", "not found: sku=%s", sku_id);
    }
    return rc;
}

/*
 * impl_save_stock_item
 *
 * Uses MERGE to handle both new and existing rows in one statement.
 *
 * OCI BINDING (what the real implementation does):
 *
 *   OCIStmtPrepare2(svc, &stmt, err, SQL_MERGE_STOCK_ITEM, ...)
 *   OCIBindByPos(stmt, &b1, err, 1, item->stock_id, 37, SQLT_STR, ...)
 *   OCIBindByPos(stmt, &b2, err, 2, &item->quantity_on_hand,  sizeof(int), SQLT_INT, ...)
 *   OCIBindByPos(stmt, &b3, err, 3, &item->quantity_reserved, sizeof(int), SQLT_INT, ...)
 *   OCIBindByPos(stmt, &b4, err, 4, item->sku_id,        strlen(item->sku_id)+1,       SQLT_STR, ...)
 *   OCIBindByPos(stmt, &b5, err, 5, item->location_code, strlen(item->location_code)+1, SQLT_STR, ...)
 *   OCIStmtExecute(svc, stmt, err, 1, ...)   -- iters=1 for DML
 *   OCIStmtRelease(stmt, ...)
 *   -- Commit happens outside, in db_commit(), after all saves for this transaction
 */
static supcis_result_t impl_save_stock_item(
    inventory_repository_t *self,
    const stock_item_t     *item)
{
    oracle_stock_item_repo_t *repo = (oracle_stock_item_repo_t *)self;

    LOG_DEBUG("stock_repo", "save: sku=%s location=%s on_hand=%d reserved=%d",
              item->sku_id, item->location_code,
              item->quantity_on_hand, item->quantity_reserved);

    supcis_result_t rc = db_execute(repo->db, SQL_MERGE_STOCK_ITEM, 5,
        /* :1 stock_id      */ item->stock_id,
        /* :2 qty_on_hand   */ &item->quantity_on_hand,
        /* :3 qty_reserved  */ &item->quantity_reserved,
        /* :4 sku_id        */ item->sku_id,
        /* :5 location_code */ item->location_code);

    if (rc != SUPCIS_OK)
        LOG_ERROR("stock_repo", "save failed for sku=%s", item->sku_id);
    return rc;
}

/*
 * impl_find_total_available_by_sku
 *
 * Aggregates available stock across all bins for this SKU.
 * Returns 0 (not NOT_FOUND) when no stock exists anywhere.
 *
 * OCI BINDING:
 *   OCIBindByPos(stmt, &b1, err, 1, sku_id, strlen(sku_id)+1, SQLT_STR, ...)
 *
 * OCI COLUMN MAPPING:
 *   int col_total = 0;
 *   sb2 ind = 0;   -- NULL indicator
 *   OCIDefineByPos(stmt, &def, err, 1, &col_total, sizeof(int), SQLT_INT, &ind, ...)
 *   After fetch: if (ind == -1) col_total = 0;   -- NVL handles this in SQL
 *   *out_available = col_total;
 */
static supcis_result_t impl_find_total_available_by_sku(
    inventory_repository_t *self,
    const char *sku_id,
    quantity_t *out_available)
{
    oracle_stock_item_repo_t *repo = (oracle_stock_item_repo_t *)self;
    int result = 0;

    LOG_DEBUG("stock_repo", "total_available query: sku=%s", sku_id);

    supcis_result_t rc = db_query_one(repo->db, SQL_SUM_AVAILABLE_BY_SKU,
                                      &result, sizeof(result));
    if (rc == SUPCIS_OK)
        *out_available = (quantity_t)result;
    return rc;
}

/* =========================================================================
 * CONSTRUCTOR / DESTRUCTOR
 * =========================================================================*/

/*
 * oracle_stock_item_repo_create -- allocates and wires up the concrete repo.
 *
 * Called once at startup (in main.c or a factory function) after db_connect().
 * The returned pointer is cast to inventory_repository_t * for use by domain code.
 *
 * Example:
 *   db_handle_t *db;
 *   db_connect(tns, user, pass, &db);
 *   inventory_repository_t *inv_repo = oracle_stock_item_repo_create(db);
 *
 *   // Pass to domain services:
 *   picking_service_create_wave(&order, inv_repo, &wave);
 */
inventory_repository_t *oracle_stock_item_repo_create(db_handle_t *db)
{
    oracle_stock_item_repo_t *r = calloc(1, sizeof(oracle_stock_item_repo_t));
    if (!r) return NULL;

    /* Wire the function pointers (the "vtable") */
    r->base.find_by_sku_location      = impl_find_by_sku_location;
    r->base.save                       = impl_save_stock_item;
    r->base.find_total_available_by_sku = impl_find_total_available_by_sku;
    r->db = db;

    LOG_DEBUG("stock_repo", "Oracle stock_item repository initialized");
    return &r->base; /* return pointer to base -- callers use it as interface */
}

void oracle_stock_item_repo_destroy(inventory_repository_t *repo)
{
    /* Cast back to concrete type to free the full struct */
    free((oracle_stock_item_repo_t *)repo);
}
