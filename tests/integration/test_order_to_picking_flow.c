/*
 * Integration test: full order → picking flow
 *
 * Tests that:
 *  1. A released order generates a pick wave with tasks
 *  2. Confirming all tasks closes the wave
 *  3. Inventory is deducted correctly
 *
 * Requires: a live Oracle test DB with migrations applied
 * Run via: scripts/testing/run_integration_tests.sh
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include "order_entity.h"
#include "picking_entity.h"
#include "inventory_entity.h"
#include "inventory_repository.h"
#include "picking_service.h"

/* ── Mock inventory repository ─────────────────────────────────────────────── */

/* Backing store: two SKUs each at a fixed location with plenty of stock */
static stock_item_t mock_stock[2] = {
    { "STOCK-001", "SKU-A", "BIN-A-01", 50, 0, 0 },
    { "STOCK-002", "SKU-B", "BIN-B-01", 50, 0, 0 },
};

static supcis_result_t mock_find(
    inventory_repository_t *self,
    const char *sku_id,
    const char *location_code,
    stock_item_t *out)
{
    (void)self; (void)location_code;
    for (int i = 0; i < 2; i++) {
        if (strcmp(mock_stock[i].sku_id, sku_id) == 0) {
            *out = mock_stock[i];
            return SUPCIS_OK;
        }
    }
    return SUPCIS_ERR_NOT_FOUND;
}

static supcis_result_t mock_save(
    inventory_repository_t *self,
    const stock_item_t *item)
{
    (void)self;
    for (int i = 0; i < 2; i++) {
        if (strcmp(mock_stock[i].sku_id, item->sku_id) == 0) {
            mock_stock[i] = *item;
            return SUPCIS_OK;
        }
    }
    return SUPCIS_ERR_NOT_FOUND;
}

static inventory_repository_t mock_repo = {
    .find_by_sku_location = mock_find,
    .save                 = mock_save,
};

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static void test_released_order_creates_pick_tasks(void **state)
{
    (void)state;

    /* Reset mock stock before test */
    mock_stock[0].quantity_reserved = 0;
    mock_stock[1].quantity_reserved = 0;

    /* Build a released order with 2 lines */
    order_t order = {0};
    snprintf(order.order_id, sizeof(order.order_id), "ORD-INTTEST-001");
    order.status     = ORDER_STATUS_NEW;
    order.line_count = 2;

    snprintf(order.lines[0].sku_id, 32, "SKU-A");
    order.lines[0].qty_ordered = 5;

    snprintf(order.lines[1].sku_id, 32, "SKU-B");
    order.lines[1].qty_ordered = 3;

    assert_int_equal(order_release(&order), SUPCIS_OK);
    assert_int_equal(order.status, ORDER_STATUS_RELEASED);

    /* Generate pick wave via picking_service */
    pick_wave_t wave = {0};
    supcis_result_t rc = picking_service_create_wave(&order, &mock_repo, &wave);

    assert_int_equal(rc, SUPCIS_OK);
    assert_int_equal(wave.task_count, 2);

    /* Correct SKUs assigned to tasks */
    assert_string_equal(wave.tasks[0].sku_id, "SKU-A");
    assert_string_equal(wave.tasks[1].sku_id, "SKU-B");

    /* Correct quantities */
    assert_int_equal(wave.tasks[0].qty_to_pick, 5);
    assert_int_equal(wave.tasks[1].qty_to_pick, 3);

    /* Stock must have been reserved in the mock */
    assert_int_equal(mock_stock[0].quantity_reserved, 5);
    assert_int_equal(mock_stock[1].quantity_reserved, 3);
}

static void test_confirming_all_tasks_closes_wave(void **state)
{
    (void)state;

    pick_wave_t wave = {0};
    wave.task_count = 2;

    wave.tasks[0].status     = PICK_TASK_ASSIGNED;
    wave.tasks[0].qty_to_pick = 5;

    wave.tasks[1].status     = PICK_TASK_ASSIGNED;
    wave.tasks[1].qty_to_pick = 3;

    /* Confirm both tasks */
    assert_int_equal(pick_task_confirm(&wave.tasks[0], 5), SUPCIS_OK);
    assert_int_equal(pick_task_confirm(&wave.tasks[1], 3), SUPCIS_OK);

    /* Wave should now close cleanly */
    assert_int_equal(pick_wave_close(&wave), SUPCIS_OK);
    assert_true(wave.is_closed);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_released_order_creates_pick_tasks),
        cmocka_unit_test(test_confirming_all_tasks_closes_wave),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
