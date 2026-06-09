/*
 * test_order_entity.c — unit tests for order_entity.c
 *
 * Tests the order state machine:
 *   - release transitions NEW → RELEASED
 *   - release rejects non-NEW orders
 *   - cancel is blocked on SHIPPED
 *   - order_is_fully_picked works correctly
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "order_entity.h"

/* ── Fixtures ─────────────────────────────────────────────────────────────── */

static order_t make_order_with_lines(order_status_t status, int line_count)
{
    order_t o = {0};
    o.status     = status;
    o.line_count = line_count;
    for (int i = 0; i < line_count; i++) {
        o.lines[i].qty_ordered = 10;
        o.lines[i].qty_picked  = 0;
    }
    return o;
}

/* ── Tests: order_release ─────────────────────────────────────────────────── */

static void test_release_transitions_new_to_released(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_NEW, 1);
    assert_int_equal(order_release(&o), SUPCIS_OK);
    assert_int_equal(o.status, ORDER_STATUS_RELEASED);
}

static void test_release_fails_if_already_released(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_RELEASED, 1);
    assert_int_equal(order_release(&o), SUPCIS_ERR_CONFLICT);
    assert_int_equal(o.status, ORDER_STATUS_RELEASED); /* unchanged */
}

static void test_release_fails_if_picking(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_PICKING, 1);
    assert_int_equal(order_release(&o), SUPCIS_ERR_CONFLICT);
}

/* ── Tests: order_cancel ──────────────────────────────────────────────────── */

static void test_cancel_succeeds_from_new(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_NEW, 1);
    assert_int_equal(order_cancel(&o), SUPCIS_OK);
    assert_int_equal(o.status, ORDER_STATUS_CANCELLED);
}

static void test_cancel_fails_if_shipped(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_SHIPPED, 1);
    assert_int_equal(order_cancel(&o), SUPCIS_ERR_CONFLICT);
    assert_int_equal(o.status, ORDER_STATUS_SHIPPED); /* unchanged */
}

/* ── Tests: order_is_fully_picked ─────────────────────────────────────────── */

static void test_fully_picked_returns_true_when_all_lines_done(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_PICKING, 3);
    /* Set all lines as fully picked */
    for (int i = 0; i < 3; i++) o.lines[i].qty_picked = 10;
    assert_true(order_is_fully_picked(&o));
}

static void test_fully_picked_returns_false_when_one_line_incomplete(void **state)
{
    (void)state;
    order_t o = make_order_with_lines(ORDER_STATUS_PICKING, 3);
    o.lines[0].qty_picked = 10;
    o.lines[1].qty_picked = 10;
    o.lines[2].qty_picked = 5;  /* only 5 of 10 picked */
    assert_false(order_is_fully_picked(&o));
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_release_transitions_new_to_released),
        cmocka_unit_test(test_release_fails_if_already_released),
        cmocka_unit_test(test_release_fails_if_picking),
        cmocka_unit_test(test_cancel_succeeds_from_new),
        cmocka_unit_test(test_cancel_fails_if_shipped),
        cmocka_unit_test(test_fully_picked_returns_true_when_all_lines_done),
        cmocka_unit_test(test_fully_picked_returns_false_when_one_line_incomplete),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
