/*
 * db_connection.c — Oracle OCI connection implementation
 *
 * OCI = Oracle Call Interface — the official C library to talk to Oracle.
 * It is low-level: you initialize an environment, create a connection handle,
 * prepare statements, bind parameters, execute, fetch rows, then clean up.
 *
 * WHY OCI AND NOT ODBC OR JDBC?
 *   SuPCIS-L8 is a C application on RedHat Linux.
 *   OCI is Oracle's native C library — fastest and most feature-complete.
 *   JDBC is Java-only. ODBC adds a translation layer we don't need.
 *
 * THIS IS A STUB — it shows the OCI pattern without requiring Oracle headers.
 * Real implementation links against liboci and includes <oci.h>.
 */
#include "db_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/*
 * db_handle_t — the opaque struct that callers hold.
 * "Opaque" means the .h file declares it as `struct db_handle` without
 * showing the fields — callers can only use it via the functions in this file.
 * This hides OCI internals from the rest of the codebase.
 */
struct db_handle {
    /*
     * In a real OCI implementation these would be:
     *   OCIEnv     *env;      -- OCI environment (one per process)
     *   OCIError   *err;      -- error handle (captures last error code/message)
     *   OCISvcCtx  *svc;      -- service context (the actual connection)
     *   OCIStmt    *stmt;     -- reusable statement handle
     *   int         in_txn;   -- 1 if BEGIN was called, 0 otherwise
     */
    int  connected;   /* 1 = open, 0 = closed */
    int  in_txn;      /* 1 = inside a BEGIN/COMMIT block */
    char tns[64];     /* which Oracle service we connected to */
};

supcis_result_t db_connect(
    const char   *tns_alias,
    const char   *username,
    const char   *password,
    db_handle_t **out_handle)
{
    if (!tns_alias || !username || !password || !out_handle)
        return SUPCIS_ERR_INVALID_ARG;

    db_handle_t *handle = calloc(1, sizeof(db_handle_t));
    if (!handle) return SUPCIS_ERR_OUT_OF_MEM;

    /*
     * Real OCI sequence:
     *   OCIEnvCreate(&handle->env, OCI_THREADED, ...)
     *   OCIHandleAlloc(handle->env, &handle->err, OCI_HTYPE_ERROR, ...)
     *   OCILogon2(handle->env, handle->err, &handle->svc,
     *             username, strlen(username),
     *             password, strlen(password),
     *             tns_alias, strlen(tns_alias),
     *             OCI_DEFAULT)
     */

    strncpy(handle->tns, tns_alias, sizeof(handle->tns) - 1);
    handle->connected = 1;

    LOG_INFO("db", "Connected to Oracle: %s@%s", username, tns_alias);
    *out_handle = handle;
    return SUPCIS_OK;
}

supcis_result_t db_begin(db_handle_t *db)
{
    /*
     * Oracle does NOT have an explicit BEGIN statement like PostgreSQL.
     * Transactions begin implicitly with the first DML statement (INSERT/UPDATE/DELETE).
     * We track in_txn ourselves so we know to call COMMIT or ROLLBACK later.
     */
    if (!db || !db->connected) return SUPCIS_ERR_INVALID_ARG;
    db->in_txn = 1;
    LOG_DEBUG("db", "Transaction started");
    return SUPCIS_OK;
}

supcis_result_t db_commit(db_handle_t *db)
{
    if (!db || !db->in_txn) return SUPCIS_ERR_INVALID_ARG;
    /*
     * Real OCI: OCITransCommit(db->svc, db->err, OCI_DEFAULT)
     */
    db->in_txn = 0;
    LOG_DEBUG("db", "Transaction committed");
    return SUPCIS_OK;
}

supcis_result_t db_rollback(db_handle_t *db)
{
    if (!db) return SUPCIS_ERR_INVALID_ARG;
    /*
     * Real OCI: OCITransRollback(db->svc, db->err, OCI_DEFAULT)
     * Called on any error inside a transaction to undo partial changes.
     */
    db->in_txn = 0;
    LOG_WARN("db", "Transaction rolled back");
    return SUPCIS_OK;
}

void db_disconnect(db_handle_t *db)
{
    if (!db) return;
    /*
     * Real OCI:
     *   OCILogoff(db->svc, db->err)
     *   OCIHandleFree(db->err, OCI_HTYPE_ERROR)
     *   OCIHandleFree(db->env, OCI_HTYPE_ENV)
     */
    LOG_INFO("db", "Disconnected from Oracle: %s", db->tns);
    free(db);
}

supcis_result_t db_execute(db_handle_t *db, const char *sql, int param_count, ...)
{
    /*
     * Executes an INSERT, UPDATE, or DELETE.
     * Parameters are bound positionally using OCI bind variables (:1, :2, ...).
     *
     * Real OCI sequence:
     *   OCIStmtPrepare2(db->svc, &stmt, db->err, sql, strlen(sql), ...)
     *   For each param: OCIBindByPos(stmt, &bind, db->err, pos, value, ...)
     *   OCIStmtExecute(db->svc, stmt, db->err, 1, 0, NULL, NULL, OCI_DEFAULT)
     *   OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT)
     */
    (void)db; (void)sql; (void)param_count;
    return SUPCIS_OK; /* stub */
}

supcis_result_t db_query_one(db_handle_t *db, const char *sql, void *out_row, size_t row_size)
{
    /*
     * Executes a SELECT that returns exactly one row.
     * The row is fetched and its columns copied into out_row.
     *
     * Real OCI sequence:
     *   OCIStmtPrepare2 → OCIDefineByPos (for each column) → OCIStmtExecute
     *   → OCIStmtFetch2 → copy column values into out_row → OCIStmtRelease
     */
    (void)db; (void)sql; (void)out_row; (void)row_size;
    return SUPCIS_OK; /* stub */
}
