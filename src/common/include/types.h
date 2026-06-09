#ifndef SUPCIS_TYPES_H
#define SUPCIS_TYPES_H

/*
 * types.h — shared primitive types used across ALL modules
 *
 * WHY THIS FILE EXISTS:
 *   In a large C codebase every module needs the same basic building blocks:
 *   result codes, IDs, quantities, timestamps. Defining them once here avoids
 *   each module inventing its own versions and then having mismatches when
 *   modules talk to each other.
 *
 * INCLUDE ORDER:
 *   This file should be included first (directly or via a module's own header).
 *   Nothing in this file depends on any other SuPCIS header.
 */

#include <stdint.h>   /* gives us int32_t, int64_t — exact-width integers that
                         are the same size on every platform (32-bit, 64-bit, ARM) */
#include <stdbool.h>  /* gives us bool, true, false — C99 standard */

/* ─────────────────────────────────────────────────────────────────────────────
 * RESULT CODES
 *
 * Convention: every function that can fail returns supcis_result_t.
 * SUPCIS_OK (0) means success. Any other value is an error.
 * Callers MUST check the return value — ignoring it is a bug.
 *
 * Why named codes instead of plain -1 / 0?
 *   `if (rc == SUPCIS_ERR_NOT_FOUND)` is immediately readable.
 *   `if (rc == -2)` requires looking up what -2 means.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef enum {
    SUPCIS_OK              = 0,  /* success — operation completed without error */
    SUPCIS_ERR_INVALID_ARG = 1,  /* caller passed NULL, negative qty, or bad format */
    SUPCIS_ERR_NOT_FOUND   = 2,  /* record does not exist (DB row missing, unknown SKU, etc.) */
    SUPCIS_ERR_DB          = 3,  /* Oracle OCI returned an error (connection lost, SQL failed) */
    SUPCIS_ERR_OUT_OF_MEM  = 4,  /* malloc/calloc returned NULL */
    SUPCIS_ERR_CONFLICT    = 5,  /* business rule violated — e.g. reserving more stock than available,
                                    or trying to release an already-shipped order */
    SUPCIS_ERR_EXTERNAL    = 6   /* an external system failed — AutoStore controller,
                                    SAP RFC call, or any HTTP request to a third party */
} supcis_result_t;

/* ─────────────────────────────────────────────────────────────────────────────
 * ID TYPE
 *
 * All entities (orders, stock items, pick tasks, robots) are identified by
 * UUID strings: "550e8400-e29b-41d4-a716-446655440000"
 *
 * Format: 8-4-4-4-12 hex digits separated by dashes = 36 characters.
 * +1 for the null terminator '\0' at the end = 37 bytes total.
 *
 * In Oracle these map to VARCHAR2(36) columns.
 * Generated with SYS_GUID() in PL/SQL or a UUID library in C.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef char supcis_id_t[37];

/* ─────────────────────────────────────────────────────────────────────────────
 * QUANTITY TYPE
 *
 * Whole-number unit count. Why integer and NOT float/double?
 *   You never have 2.5 boxes. Floating-point arithmetic introduces rounding
 *   errors (0.1 + 0.2 = 0.30000000000000004 in IEEE 754) which are
 *   dangerous in inventory: stock counts must always be exact.
 *   int32_t handles up to ~2.1 billion units — more than enough per location.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef int32_t quantity_t;

/* ─────────────────────────────────────────────────────────────────────────────
 * TIMESTAMP TYPE
 *
 * Unix epoch: seconds since 1970-01-01 00:00:00 UTC.
 *
 * Why int64_t and not int32_t?
 *   32-bit Unix timestamps overflow on 2038-01-19 (the "Year 2038 problem").
 *   int64_t can represent dates until the year 292 billion — safe forever.
 *   In Oracle these map to TIMESTAMP columns.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef int64_t timestamp_t;

#endif /* SUPCIS_TYPES_H */
