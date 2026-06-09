/*
 * db_connection_oci_real.c — PRODUCTION Oracle OCI implementation
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * THIS FILE IS FOR STUDY ONLY — IT IS NOT COMPILED ON MAC.
 *
 * It requires:
 *   - Oracle Instant Client installed on the build machine
 *   - The official Oracle header: #include <oci.h>
 *   - Linker flag: -loci  (or -lclntsh on newer Oracle versions)
 *
 * On a production RedHat/Oracle Linux server all of these are available.
 * On a Mac development machine they are not — use db_connection.c (stub) instead.
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * WHAT IS OCI?
 *   Oracle Call Interface (OCI) is the official C library for talking to Oracle.
 *   Think of it like POSIX file I/O but for a database:
 *     fopen()  → OCILogon2()     — open a connection
 *     fwrite() → OCIStmtExecute() — execute DML (INSERT/UPDATE/DELETE)
 *     fread()  → OCIStmtFetch2() — read rows from a SELECT
 *     fclose() → OCILogoff()     — close the connection
 *
 * KEY OCI HANDLE TYPES:
 *   OCIEnv     — one per process. Sets up memory, threading, encoding.
 *   OCIError   — one per thread. After any OCI call fails, call OCIErrorGet()
 *                on this handle to get the Oracle error code and message text.
 *   OCISvcCtx  — the actual database session/connection.
 *   OCIStmt    — a prepared SQL statement (like a FILE* but for SQL).
 *   OCIBind    — binds a C variable to an :input placeholder in SQL.
 *   OCIDefine  — maps an output column to a C variable in a SELECT result.
 *
 * HOW ORACLE TRANSACTIONS WORK (different from PostgreSQL/MySQL):
 *   Oracle does NOT have a BEGIN statement.
 *   Any INSERT/UPDATE/DELETE automatically starts an implicit transaction.
 *   The transaction stays open until you call COMMIT or ROLLBACK.
 *   If the process crashes, Oracle automatically rolls back open transactions.
 *   We use db->in_txn as a flag so our application code knows when to commit.
 */

/*
 * To compile this file on a Linux server with Oracle Instant Client installed:
 *
 *   gcc -Wall -Wextra -std=c99 \
 *       -I$ORACLE_HOME/sdk/include \
 *       -Isrc/common/include \
 *       -Isrc/infrastructure/database/include \
 *       -c src/infrastructure/database/src/db_connection_oci_real.c \
 *       -o build/obj/infrastructure/database/src/db_connection_oci_real.o
 *
 * Then link with:   -loci -lpthread -lm
 */

#include <oci.h>           /* Oracle OCI — only on servers with Instant Client */
#include "db_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ─────────────────────────────────────────────────────────────────────────── */
/* Internal handle struct — opaque to all callers (hidden behind db_handle_t) */
/* ─────────────────────────────────────────────────────────────────────────── */

struct db_handle {
    OCIEnv    *env;      /* OCI environment  — initialized once at startup    */
    OCIError  *err;      /* error handle     — call OCIErrorGet() after fails */
    OCISvcCtx *svc;      /* service context  — represents the DB connection   */
    int        connected;
    int        in_txn;
    char       tns[64];
};

/* ─────────────────────────────────────────────────────────────────────────── */
/* Internal helper: extract Oracle error message after a failed OCI call.     */
/* Call this whenever an OCI function returns OCI_ERROR.                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static void log_oci_error(OCIError *err, const char *context)
{
    sb4  error_code = 0;
    text error_msg[512];

    /*
     * OCIErrorGet extracts the Oracle error code (e.g. ORA-00942: table not found)
     * and puts the human-readable message text into error_msg.
     *
     * Parameters:
     *   hndlp    — the OCI error handle
     *   recordno — error record number (1 = most recent)
     *   sqlstate — not used for OCI (set NULL)
     *   errcodep — receives the Oracle error number (e.g. 942)
     *   bufp     — receives the error message string
     *   bufsiz   — size of bufp
     *   type     — OCI_HTYPE_ERROR because we're passing an error handle
     */
    OCIErrorGet(err, 1, NULL, &error_code, error_msg, sizeof(error_msg),
                OCI_HTYPE_ERROR);

    LOG_ERROR("db", "[%s] ORA-%05d: %s", context, (int)error_code, (char*)error_msg);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_connect — initialize OCI environment and open a database session        */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_connect(
    const char   *tns_alias,
    const char   *username,
    const char   *password,
    db_handle_t **out_handle)
{
    if (!tns_alias || !username || !password || !out_handle)
        return SUPCIS_ERR_INVALID_ARG;

    db_handle_t *h = calloc(1, sizeof(db_handle_t));
    if (!h) return SUPCIS_ERR_OUT_OF_MEM;

    sword rc; /* OCI return type — OCI_SUCCESS=0, OCI_ERROR=-1, etc. */

    /* ── Step 1: Create the OCI environment ─────────────────────────────────
     *
     * OCIEnvCreate initializes the OCI library for this process.
     * Must be called exactly once before any other OCI call.
     *
     * OCI_THREADED  — multiple threads may use this environment concurrently
     * OCI_OBJECT    — enables Oracle object type support (needed for some types)
     *
     * The three NULL, NULL, NULL, 0, NULL arguments are for custom memory
     * allocators. Passing NULL tells OCI to use the system malloc/free.
     */
    rc = OCIEnvCreate(&h->env,
                      OCI_THREADED | OCI_OBJECT,
                      NULL, NULL, NULL, NULL, 0, NULL);
    if (rc != OCI_SUCCESS) {
        LOG_ERROR("db", "OCIEnvCreate failed: rc=%d", rc);
        free(h);
        return SUPCIS_ERR_DB;
    }

    /* ── Step 2: Allocate the error handle ──────────────────────────────────
     *
     * Every thread that uses OCI needs its own error handle.
     * After any OCI call returns OCI_ERROR, call OCIErrorGet() on this
     * handle to get the Oracle error number and text message.
     *
     * OCIHandleAlloc allocates any OCI handle type. Second argument is the
     * pointer-to-pointer that receives the allocated handle.
     * OCI_HTYPE_ERROR tells it we want an error handle.
     */
    rc = OCIHandleAlloc(h->env, (dvoid**)&h->err, OCI_HTYPE_ERROR, 0, NULL);
    if (rc != OCI_SUCCESS) {
        LOG_ERROR("db", "OCIHandleAlloc(err) failed: rc=%d", rc);
        OCIHandleFree(h->env, OCI_HTYPE_ENV);
        free(h);
        return SUPCIS_ERR_DB;
    }

    /* ── Step 3: Open the database session ──────────────────────────────────
     *
     * OCILogon2 is the simplified connection call (vs. the lower-level
     * OCIServerAttach + OCISessionBegin).
     *
     * Parameters:
     *   env       — the environment handle we just created
     *   err       — error handle (receives error details if call fails)
     *   svcp      — output: the service context (the actual connection)
     *   username  — Oracle schema name (e.g. "SUPCIS_PROD")
     *   uname_len — strlen(username)
     *   password  — Oracle password
     *   pass_len  — strlen(password)
     *   tns_alias — TNS alias from tnsnames.ora (e.g. "PROD_DB")
     *               tnsnames.ora resolves this to a host:port/service
     *   tns_len   — strlen(tns_alias)
     *   mode      — OCI_DEFAULT = synchronous; OCI_CPOOL = connection pool
     *
     * The (text*) cast is because OCI uses its own "text" typedef for strings
     * to support Unicode — it's typedef'd to unsigned char.
     */
    rc = OCILogon2(h->env, h->err, &h->svc,
                   (const OraText*)username, (ub4)strlen(username),
                   (const OraText*)password, (ub4)strlen(password),
                   (const OraText*)tns_alias, (ub4)strlen(tns_alias),
                   OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(h->err, "OCILogon2");
        OCIHandleFree(h->err, OCI_HTYPE_ERROR);
        OCIHandleFree(h->env, OCI_HTYPE_ENV);
        free(h);
        return SUPCIS_ERR_DB;
    }

    strncpy(h->tns, tns_alias, sizeof(h->tns) - 1);
    h->connected = 1;

    LOG_INFO("db", "Connected to Oracle: %s@%s", username, tns_alias);
    *out_handle = h;
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_begin — mark start of a transaction in our own tracking                 */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_begin(db_handle_t *db)
{
    /*
     * Oracle starts transactions implicitly on the first DML statement.
     * There is NO "BEGIN TRANSACTION" command in Oracle SQL.
     *
     * We track in_txn ourselves so the application code can:
     *   1. Call db_begin() to mark intent
     *   2. Do several db_execute() calls
     *   3. Call db_commit() or db_rollback() at the end
     *
     * If in_txn is already 1 when db_begin() is called, it means someone
     * forgot to commit/rollback the previous transaction — log a warning.
     */
    if (!db || !db->connected) return SUPCIS_ERR_INVALID_ARG;
    if (db->in_txn) {
        LOG_WARN("db", "db_begin called while already in a transaction — check for missing commit/rollback");
    }
    db->in_txn = 1;
    LOG_DEBUG("db", "Transaction started on %s", db->tns);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_execute — run an INSERT, UPDATE, or DELETE with bound parameters        */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_execute(db_handle_t *db, const char *sql, int param_count, ...)
{
    /*
     * OVERVIEW OF THE OCI EXECUTE PATTERN:
     *
     * SQL:   INSERT INTO stock_item (stock_id, sku_id, location_code, qty_on_hand)
     *        VALUES (:1, :2, :3, :4)
     *
     * The :1, :2, :3, :4 are "bind variables" — they act as typed placeholders.
     * OCI fills them in at execute time from C variables we "bind" to them.
     *
     * Why bind variables instead of sprintf-ing values into the SQL?
     *   1. SQL injection prevention — values never interpreted as SQL
     *   2. Oracle can reuse the execution plan for the same SQL shape
     *   3. Handles type conversion (int → NUMBER, char* → VARCHAR2) automatically
     *
     * VARIADIC CONVENTION for this function:
     *   param_count pairs of (type_code, pointer_to_value):
     *     db_execute(db, sql, 2,
     *                SQLT_STR, sku_id,        ← string
     *                SQLT_INT, &qty_on_hand);  ← int pointer
     *
     * OCI TYPE CODES (SQLT_xxx):
     *   SQLT_STR  — null-terminated C string → Oracle VARCHAR2
     *   SQLT_INT  — int (4 bytes) → Oracle NUMBER
     *   SQLT_FLT  — double → Oracle FLOAT
     *   SQLT_DAT  — 7-byte Oracle date buffer
     */

    OCIStmt *stmt = NULL;
    sword    rc;

    /* ── Step 1: Prepare the SQL statement ──────────────────────────────────
     *
     * OCIStmtPrepare2 compiles the SQL text and creates a statement handle.
     * Oracle parses and caches the execution plan at this point.
     *
     * OCI_NTV_SYNTAX tells Oracle to use its native SQL syntax (not ANSI mode).
     */
    rc = OCIStmtPrepare2(db->svc, &stmt, db->err,
                         (const OraText*)sql, (ub4)strlen(sql),
                         NULL, 0,
                         OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCIStmtPrepare2");
        return SUPCIS_ERR_DB;
    }

    /* ── Step 2: Bind each input parameter ──────────────────────────────────
     *
     * We iterate the variadic argument list and bind each value by position.
     * OCIBindByPos maps :1, :2, :3, etc. to C variables.
     *
     * OCIBindByPos parameters (simplified):
     *   stmtp    — the statement handle
     *   bindpp   — output: the bind handle (we don't use it after this call)
     *   errhp    — error handle
     *   position — which placeholder (:1=1, :2=2, ...)
     *   valuep   — pointer to the C variable holding the value
     *   value_sz — size in bytes of the C variable
     *   dty      — OCI type code (SQLT_STR, SQLT_INT, etc.)
     *   indp     — NULL indicator (NULL means "this value is not NULL")
     *   alenp    — actual data length array (NULL = use value_sz always)
     *   rcodep   — return code array (NULL = we don't need per-bind codes)
     *   maxarr_len — 0 for non-array binds
     *   curelep  — 0 for non-array binds
     *   mode     — OCI_DEFAULT
     */
    va_list args;
    va_start(args, param_count);

    for (int i = 1; i <= param_count; i++) {
        ub2      type_code = (ub2)va_arg(args, int);   /* SQLT_STR, SQLT_INT, ... */
        void    *value_ptr = va_arg(args, void*);        /* pointer to the value   */
        OCIBind *bind      = NULL;

        sb4 value_sz;
        if (type_code == SQLT_STR) {
            value_sz = (sb4)strlen((const char*)value_ptr) + 1;
        } else if (type_code == SQLT_INT) {
            value_sz = sizeof(int);
        } else {
            value_sz = sizeof(double); /* fallback for SQLT_FLT */
        }

        rc = OCIBindByPos(stmt, &bind, db->err,
                          (ub4)i,
                          value_ptr, value_sz, type_code,
                          NULL, NULL, NULL, 0, NULL,
                          OCI_DEFAULT);
        if (rc != OCI_SUCCESS) {
            log_oci_error(db->err, "OCIBindByPos");
            va_end(args);
            OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
            return SUPCIS_ERR_DB;
        }
    }
    va_end(args);

    /* ── Step 3: Execute the statement ──────────────────────────────────────
     *
     * OCIStmtExecute runs the SQL.
     *
     * iters=1 means "execute once" (vs. array DML where iters > 1).
     * For SELECT, iters must be 0 (fetch rows separately with OCIStmtFetch2).
     *
     * OCI_DEFAULT = synchronous mode. OCI_COMMIT_ON_SUCCESS would auto-commit,
     * but we manage transactions ourselves so we use OCI_DEFAULT.
     */
    rc = OCIStmtExecute(db->svc, stmt, db->err,
                        1,            /* iters — execute once */
                        0,            /* rowoff — start at row 0 */
                        NULL, NULL,   /* snap_in/out — not used */
                        OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCIStmtExecute");
        OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
        return SUPCIS_ERR_DB;
    }

    /* ── Step 4: Release the statement handle ───────────────────────────────
     *
     * OCIStmtRelease frees the statement handle.
     * Always release — even on success. Failure to release is a handle leak.
     */
    OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);

    LOG_DEBUG("db", "Execute OK: %s", sql);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_query_one — run a SELECT that returns exactly one row                   */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_query_one(
    db_handle_t *db,
    const char  *sql,
    void        *out_row,
    size_t       row_size)
{
    /*
     * This function is a simplified version: it selects into a single output
     * buffer. For production code you'd use a db_query_t abstraction that
     * maps individual column values to struct fields (see below).
     *
     * For a realistic example, let's say out_row points to a stock_item_t and
     * the SQL is:
     *   SELECT stock_id, sku_id, location_code, qty_on_hand, qty_reserved,
     *          EXTRACT(EPOCH FROM last_updated)
     *   FROM   stock_item
     *   WHERE  sku_id = :1 AND location_code = :2
     *
     * The Define step maps each SELECT column to a field inside stock_item_t.
     *
     * In production this would be a typed repository function:
     *   static supcis_result_t map_row_to_stock_item(OCIStmt *stmt,
     *       OCIError *err, stock_item_t *out)
     * ...defined alongside each repository, not in the generic db layer.
     * The generic db_query_one here just copies the raw bytes — it's a
     * simplification to show the OCI pattern.
     */

    OCIStmt   *stmt = NULL;
    OCIDefine *def  = NULL;
    sword      rc;
    char       buf[1024] = {0};  /* simplified: map entire row as text blob */

    /* ── Step 1: Prepare ─────────────────────────────────────────────────── */
    rc = OCIStmtPrepare2(db->svc, &stmt, db->err,
                         (const OraText*)sql, (ub4)strlen(sql),
                         NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "db_query_one/OCIStmtPrepare2");
        return SUPCIS_ERR_DB;
    }

    /* ── Step 2: Define output column mapping ────────────────────────────────
     *
     * OCIDefineByPos maps SELECT column N to a C variable.
     * Called once per column in the SELECT list, before executing.
     *
     * Parameters:
     *   stmtp    — statement handle
     *   defnpp   — output: define handle
     *   errhp    — error handle
     *   position — column index (1-based, left-to-right in SELECT list)
     *   valuep   — pointer to C buffer that will receive the column value
     *   value_sz — size of that buffer
     *   dty      — OCI type code (SQLT_STR for VARCHAR2, SQLT_INT for NUMBER, etc.)
     *   indp     — NULL indicator pointer (receives -1 if column is NULL, 0 otherwise)
     *   rlenp    — receives actual data length after fetch
     *   rcodep   — per-column return code (NULL if we don't need it)
     *   mode     — OCI_DEFAULT
     *
     * Real repository code would define one OCIDefine per column:
     *   char stock_id[37]; OCIDefineByPos(stmt, &def, err, 1, stock_id, 37, SQLT_STR, ...)
     *   char sku_id[32];   OCIDefineByPos(stmt, &def, err, 2, sku_id,   32, SQLT_STR, ...)
     *   int  qty;          OCIDefineByPos(stmt, &def, err, 3, &qty, sizeof(qty), SQLT_INT, ...)
     */
    rc = OCIDefineByPos(stmt, &def, db->err,
                        1,
                        buf, (sb4)sizeof(buf),
                        SQLT_STR,
                        NULL, NULL, NULL,
                        OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCIDefineByPos");
        OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
        return SUPCIS_ERR_DB;
    }

    /* ── Step 3: Execute (iters=0 for SELECT — data comes from OCIStmtFetch2) */
    rc = OCIStmtExecute(db->svc, stmt, db->err,
                        0, 0, NULL, NULL, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "db_query_one/OCIStmtExecute");
        OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
        return SUPCIS_ERR_DB;
    }

    /* ── Step 4: Fetch one row ───────────────────────────────────────────────
     *
     * OCIStmtFetch2 retrieves rows into the buffers defined with OCIDefineByPos.
     * nrows=1 fetches exactly one row.
     *
     * OCI_FETCH_NEXT  — standard sequential fetch
     * Returns OCI_NO_DATA if the SELECT returned no rows at all.
     */
    rc = OCIStmtFetch2(stmt, db->err,
                       1,               /* nrows — fetch 1 row */
                       OCI_FETCH_NEXT,  /* orientation */
                       0,               /* fetch offset (not used for NEXT) */
                       OCI_DEFAULT);

    if (rc == OCI_NO_DATA) {
        OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
        return SUPCIS_ERR_NOT_FOUND;  /* zero rows — caller handles this */
    }
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCIStmtFetch2");
        OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
        return SUPCIS_ERR_DB;
    }

    /* Copy fetched data to out_row. In production this would be field-by-field. */
    size_t copy_size = row_size < sizeof(buf) ? row_size : sizeof(buf);
    memcpy(out_row, buf, copy_size);

    OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
    LOG_DEBUG("db", "Query one row OK: %s", sql);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_commit — make all pending DML changes permanent and visible             */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_commit(db_handle_t *db)
{
    if (!db || !db->in_txn) return SUPCIS_ERR_INVALID_ARG;

    /*
     * OCITransCommit makes all DML since the last commit permanent.
     * After this call:
     *   - All row locks are released
     *   - Other sessions immediately see the changes
     *   - The transaction is complete — there is nothing to roll back
     *
     * The third argument (flags) is OCI_DEFAULT for a standard commit.
     * OCI_TRANS_TWOPHASE enables two-phase commit for distributed transactions
     * (across multiple Oracle databases) — not needed here.
     */
    sword rc = OCITransCommit(db->svc, db->err, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCITransCommit");
        db->in_txn = 0;  /* reset anyway — transaction is in unknown state */
        return SUPCIS_ERR_DB;
    }

    db->in_txn = 0;
    LOG_DEBUG("db", "Transaction committed on %s", db->tns);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_rollback — undo all DML since the last commit                           */
/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t db_rollback(db_handle_t *db)
{
    if (!db) return SUPCIS_ERR_INVALID_ARG;

    /*
     * OCITransRollback undoes all DML changes since the last COMMIT.
     * Called whenever an error occurs mid-transaction to keep the database
     * in a consistent state.
     *
     * Example scenario that requires rollback:
     *   db_begin()
     *   db_execute("INSERT INTO pick_wave ...")           ← succeeds
     *   db_execute("INSERT INTO pick_task ...")           ← succeeds
     *   db_execute("UPDATE stock_item SET reserved ...")  ← FAILS (OCI error)
     *   db_rollback()   ← undoes both the wave AND the task inserts
     *
     * Without rollback, the wave and tasks would exist in Oracle but the
     * stock would not be reserved — an inconsistent state that would cause
     * items to be over-picked.
     */
    sword rc = OCITransRollback(db->svc, db->err, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        log_oci_error(db->err, "OCITransRollback");
    }

    db->in_txn = 0;
    LOG_WARN("db", "Transaction rolled back on %s", db->tns);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* db_disconnect — close the session and free all OCI handles                 */
/* ─────────────────────────────────────────────────────────────────────────── */

void db_disconnect(db_handle_t *db)
{
    if (!db) return;

    if (db->in_txn) {
        /*
         * If we are disconnecting while a transaction is open, that's a bug.
         * Oracle will roll it back automatically when the session closes,
         * but we log a warning so developers can find the code path that
         * failed to commit or rollback.
         */
        LOG_WARN("db", "Disconnecting with an open transaction — changes will be rolled back");
    }

    if (db->svc) {
        /*
         * OCILogoff closes the session and releases the service context handle.
         * For connection-pool sessions (OCI_CPOOL mode), use OCISessionRelease instead.
         */
        OCILogoff(db->svc, db->err);
        db->svc = NULL;
    }

    /*
     * Free handles in the reverse order they were allocated:
     *   error handle  (allocated after env)
     *   env handle    (allocated first)
     *
     * Always pass the correct OCI_HTYPE_xxx constant matching the handle type.
     * Passing the wrong type causes silent memory corruption in the OCI library.
     */
    if (db->err) {
        OCIHandleFree(db->err, OCI_HTYPE_ERROR);
        db->err = NULL;
    }
    if (db->env) {
        OCIHandleFree(db->env, OCI_HTYPE_ENV);
        db->env = NULL;
    }

    LOG_INFO("db", "Disconnected from Oracle: %s", db->tns);
    free(db);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────
 * HOW A REAL REPOSITORY USES THESE FUNCTIONS
 *
 * The generic db_execute / db_query_one above are a simplification.
 * In a real codebase, each repository module has its own typed query functions.
 * Here is what stock_item_repository.c would look like:
 *
 *   supcis_result_t stock_repo_find_by_sku_location(
 *       db_handle_t *db,
 *       const char  *sku_id,
 *       const char  *location_code,
 *       stock_item_t *out)
 *   {
 *       OCIStmt *stmt = NULL;
 *
 *       // Separate C buffers for each output column
 *       char  col_stock_id[37]       = {0};
 *       char  col_sku_id[32]         = {0};
 *       char  col_location_code[24]  = {0};
 *       int   col_qty_on_hand        = 0;
 *       int   col_qty_reserved       = 0;
 *       sb2   ind_qty_on_hand        = 0;  // NULL indicator: -1 if column is NULL
 *
 *       const char *sql =
 *           "SELECT stock_id, sku_id, location_code, "
 *           "       qty_on_hand, qty_reserved "
 *           "FROM   stock_item "
 *           "WHERE  sku_id = :1 AND location_code = :2";
 *
 *       // Prepare
 *       OCIStmtPrepare2(db->svc, &stmt, db->err,
 *                       (text*)sql, strlen(sql), NULL, 0,
 *                       OCI_NTV_SYNTAX, OCI_DEFAULT);
 *
 *       // Bind WHERE inputs (:1 and :2)
 *       OCIBind *b1 = NULL, *b2 = NULL;
 *       OCIBindByPos(stmt, &b1, db->err, 1,
 *                   (void*)sku_id, strlen(sku_id)+1, SQLT_STR,
 *                   NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
 *       OCIBindByPos(stmt, &b2, db->err, 2,
 *                   (void*)location_code, strlen(location_code)+1, SQLT_STR,
 *                   NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
 *
 *       // Define output columns (one OCIDefine per SELECT column)
 *       OCIDefine *d1,*d2,*d3,*d4,*d5;
 *       OCIDefineByPos(stmt,&d1,db->err,1, col_stock_id,    37, SQLT_STR, NULL,NULL,NULL,OCI_DEFAULT);
 *       OCIDefineByPos(stmt,&d2,db->err,2, col_sku_id,      32, SQLT_STR, NULL,NULL,NULL,OCI_DEFAULT);
 *       OCIDefineByPos(stmt,&d3,db->err,3, col_location_code,24,SQLT_STR, NULL,NULL,NULL,OCI_DEFAULT);
 *       OCIDefineByPos(stmt,&d4,db->err,4, &col_qty_on_hand, sizeof(int), SQLT_INT, &ind_qty_on_hand,NULL,NULL,OCI_DEFAULT);
 *       OCIDefineByPos(stmt,&d5,db->err,5, &col_qty_reserved,sizeof(int), SQLT_INT, NULL,NULL,NULL,OCI_DEFAULT);
 *
 *       // Execute (iters=0 for SELECT)
 *       OCIStmtExecute(db->svc, stmt, db->err, 0, 0, NULL, NULL, OCI_DEFAULT);
 *
 *       // Fetch one row
 *       sword rc = OCIStmtFetch2(stmt, db->err, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
 *       if (rc == OCI_NO_DATA) {
 *           OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
 *           return SUPCIS_ERR_NOT_FOUND;
 *       }
 *
 *       // Map columns → struct fields
 *       strncpy(out->stock_id,       col_stock_id,      sizeof(out->stock_id)-1);
 *       strncpy(out->sku_id,         col_sku_id,        sizeof(out->sku_id)-1);
 *       strncpy(out->location_code,  col_location_code, sizeof(out->location_code)-1);
 *       out->quantity_on_hand  = (ind_qty_on_hand == -1) ? 0 : col_qty_on_hand;
 *       out->quantity_reserved = col_qty_reserved;
 *
 *       OCIStmtRelease(stmt, db->err, NULL, 0, OCI_DEFAULT);
 *       return SUPCIS_OK;
 *   }
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */
