#ifndef DB_CONNECTION_H
#define DB_CONNECTION_H

#include "types.h"
#include <stddef.h>   /* size_t */

/*
 * Oracle OCI-based connection pool.
 * SuPCIS-L8 uses Oracle as its primary database.
 * Connection strings use Oracle TNS aliases defined in tnsnames.ora.
 */

typedef struct db_handle db_handle_t; /* opaque — OCI internals hidden */

supcis_result_t db_connect(
    const char  *tns_alias,
    const char  *username,
    const char  *password,
    db_handle_t **out_handle);

supcis_result_t db_execute(
    db_handle_t *db,
    const char  *sql,
    int          param_count,
    ...);   /* variadic: param type + value pairs */

supcis_result_t db_query_one(
    db_handle_t *db,
    const char  *sql,
    void        *out_row,
    size_t       row_size);

supcis_result_t db_begin(db_handle_t *db);
supcis_result_t db_commit(db_handle_t *db);
supcis_result_t db_rollback(db_handle_t *db);
void            db_disconnect(db_handle_t *db);

#endif /* DB_CONNECTION_H */
