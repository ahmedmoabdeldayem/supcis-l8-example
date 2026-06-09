/*
 * Unit tests for inventory_entity.c
 * Framework: cmocka  (https://cmocka.org/)
 * Run via: make test
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "inventory_entity.h"

/* ── Fixtures ──────────────────────────────────────────────────────────────── */

static stock_item_t make_item(quantity_t on_hand, quantity_t reserved)
{
    stock_item_t item = {0};
    snprintf(item.sku_id, sizeof(item.sku_id), "SKU-TEST-001");
    snprintf(item.location_code, sizeof(item.location_code), "BIN-A-01-01");
    item.quantity_on_hand  = on_hand;
    item.quantity_reserved = reserved;
    return item;
}

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static void test_available_quantity_is_on_hand_minus_reserved(void **state)
{
    (void)state;
    stock_item_t item = make_item(100, 30);
    assert_int_equal(stock_item_available(&item), 70);
}

static void test_reserve_succeeds_when_enough_available(void **state)
{
    (void)state;
    stock_item_t item = make_item(100, 0);
    supcis_result_t rc = stock_item_reserve(&item, 50);
    assert_int_equal(rc, SUPCIS_OK);
    assert_int_equal(item.quantity_reserved, 50);
}

static void test_reserve_fails_when_insufficient_stock(void **state)
{
    (void)state;
    stock_item_t item = make_item(10, 8);  /* only 2 available */
    supcis_result_t rc = stock_item_reserve(&item, 5);
    assert_int_equal(rc, SUPCIS_ERR_CONFLICT);
    assert_int_equal(item.quantity_reserved, 8); /* unchanged */
}

static void test_deduct_reduces_on_hand_and_reserved(void **state)
{
    (void)state;
    stock_item_t item = make_item(100, 40);
    supcis_result_t rc = stock_item_deduct(&item, 40);
    assert_int_equal(rc, SUPCIS_OK);
    assert_int_equal(item.quantity_on_hand,  60);
    assert_int_equal(item.quantity_reserved, 0);
}

static void test_reserve_rejects_zero_quantity(void **state)
{
    (void)state;
    stock_item_t item = make_item(100, 0);
    supcis_result_t rc = stock_item_reserve(&item, 0);
    assert_int_equal(rc, SUPCIS_ERR_INVALID_ARG);
}

/* ── Main ───────────────────────────────────────────────────────────────────── */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_available_quantity_is_on_hand_minus_reserved),
        cmocka_unit_test(test_reserve_succeeds_when_enough_available),
        cmocka_unit_test(test_reserve_fails_when_insufficient_stock),
        cmocka_unit_test(test_deduct_reduces_on_hand_and_reserved),
        cmocka_unit_test(test_reserve_rejects_zero_quantity),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
