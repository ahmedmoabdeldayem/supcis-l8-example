/*
 * test_rest_handler.c — unit tests for REST API handlers
 *
 * Tests that HTTP handlers correctly:
 *   - Map domain results to HTTP status codes
 *   - Format JSON responses correctly
 *   - Return 404 (not 500) when a resource is not found
 *
 * Uses a mock inventory_repository so no database is needed.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include "rest_handler.h"
#include "inventory_entity.h"
#include "inventory_repository.h"

/* ── Mock repository: always returns NOT_FOUND ───────────────────────────── */

static supcis_result_t mock_find_not_found(
    inventory_repository_t *self,
    const char *sku_id, const char *location_code, stock_item_t *out)
{
    (void)self; (void)sku_id; (void)location_code; (void)out;
    return SUPCIS_ERR_NOT_FOUND;
}

static supcis_result_t mock_save_ok(inventory_repository_t *self, const stock_item_t *item)
{
    (void)self; (void)item;
    return SUPCIS_OK;
}

/* ── Mock repository: returns a stock item ───────────────────────────────── */

static supcis_result_t mock_find_with_stock(
    inventory_repository_t *self,
    const char *sku_id, const char *location_code, stock_item_t *out)
{
    (void)self; (void)sku_id; (void)location_code;
    memset(out, 0, sizeof(*out));
    strncpy(out->sku_id,        "SKU-001",    sizeof(out->sku_id)        - 1);
    strncpy(out->location_code, "BIN-A-01",   sizeof(out->location_code) - 1);
    out->quantity_on_hand  = 100;
    out->quantity_reserved = 30;
    return SUPCIS_OK;
}

/* Declare the function we're testing (defined in inventory_rest_handlers.c) */
extern supcis_result_t handle_get_inventory(
    const http_request_t *, http_response_t *, inventory_repository_t *);

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_get_inventory_returns_404_when_not_found(void **state)
{
    (void)state;
    inventory_repository_t repo = { .find_by_sku_location = mock_find_not_found,
                                    .save = mock_save_ok };
    http_request_t  req = { .method = HTTP_GET };
    http_response_t res = {0};
    strncpy(req.path, "/api/v1/inventory?sku=UNKNOWN&location=BIN-X", sizeof(req.path) - 1);

    handle_get_inventory(&req, &res, &repo);

    assert_int_equal(res.status_code, 404);
    /* Response body must contain "not_found" */
    assert_non_null(strstr(res.body, "not_found"));
}

static void test_get_inventory_returns_200_with_correct_fields(void **state)
{
    (void)state;
    inventory_repository_t repo = { .find_by_sku_location = mock_find_with_stock,
                                    .save = mock_save_ok };
    http_request_t  req = { .method = HTTP_GET };
    http_response_t res = {0};
    strncpy(req.path, "/api/v1/inventory?sku=SKU-001&location=BIN-A-01", sizeof(req.path) - 1);

    handle_get_inventory(&req, &res, &repo);

    assert_int_equal(res.status_code, 200);
    assert_non_null(strstr(res.body, "\"on_hand\":100"));
    assert_non_null(strstr(res.body, "\"reserved\":30"));
    assert_non_null(strstr(res.body, "\"available\":70"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_get_inventory_returns_404_when_not_found),
        cmocka_unit_test(test_get_inventory_returns_200_with_correct_fields),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
