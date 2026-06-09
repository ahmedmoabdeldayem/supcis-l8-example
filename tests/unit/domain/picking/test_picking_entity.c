/*
 * test_picking_entity.c — unit tests for picking_entity.c
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "picking_entity.h"

static pick_task_t make_task(pick_task_status_t status, quantity_t qty)
{
    pick_task_t t = {0};
    t.status      = status;
    t.qty_to_pick = qty;
    return t;
}

/* ── pick_task_confirm ────────────────────────────────────────────────────── */

static void test_confirm_succeeds_when_assigned(void **state)
{
    (void)state;
    pick_task_t t = make_task(PICK_TASK_ASSIGNED, 10);
    assert_int_equal(pick_task_confirm(&t, 10), SUPCIS_OK);
    assert_int_equal(t.status,     PICK_TASK_CONFIRMED);
    assert_int_equal(t.qty_picked, 10);
}

static void test_confirm_allows_short_pick(void **state)
{
    (void)state;
    /* Picker found 7 instead of 10 — short pick is allowed */
    pick_task_t t = make_task(PICK_TASK_ASSIGNED, 10);
    assert_int_equal(pick_task_confirm(&t, 7), SUPCIS_OK);
    assert_int_equal(t.qty_picked, 7);
}

static void test_confirm_fails_if_not_assigned(void **state)
{
    (void)state;
    pick_task_t t = make_task(PICK_TASK_PENDING, 10);
    assert_int_equal(pick_task_confirm(&t, 10), SUPCIS_ERR_CONFLICT);
}

static void test_confirm_rejects_zero_qty(void **state)
{
    (void)state;
    pick_task_t t = make_task(PICK_TASK_ASSIGNED, 10);
    assert_int_equal(pick_task_confirm(&t, 0), SUPCIS_ERR_INVALID_ARG);
}

static void test_confirm_rejects_qty_above_to_pick(void **state)
{
    (void)state;
    pick_task_t t = make_task(PICK_TASK_ASSIGNED, 10);
    assert_int_equal(pick_task_confirm(&t, 11), SUPCIS_ERR_INVALID_ARG);
}

/* ── pick_wave_close ──────────────────────────────────────────────────────── */

static void test_wave_closes_when_all_confirmed(void **state)
{
    (void)state;
    pick_wave_t w = {0};
    w.task_count = 2;
    w.tasks[0].status = PICK_TASK_CONFIRMED;
    w.tasks[1].status = PICK_TASK_CONFIRMED;
    assert_int_equal(pick_wave_close(&w), SUPCIS_OK);
    assert_true(w.is_closed);
}

static void test_wave_cannot_close_with_pending_tasks(void **state)
{
    (void)state;
    pick_wave_t w = {0};
    w.task_count = 2;
    w.tasks[0].status = PICK_TASK_CONFIRMED;
    w.tasks[1].status = PICK_TASK_PENDING;  /* still open */
    assert_int_equal(pick_wave_close(&w), SUPCIS_ERR_CONFLICT);
    assert_false(w.is_closed);
}

static void test_wave_closes_when_mix_of_confirmed_and_cancelled(void **state)
{
    (void)state;
    pick_wave_t w = {0};
    w.task_count = 3;
    w.tasks[0].status = PICK_TASK_CONFIRMED;
    w.tasks[1].status = PICK_TASK_CANCELLED;  /* cancelled is "done" */
    w.tasks[2].status = PICK_TASK_CONFIRMED;
    assert_int_equal(pick_wave_close(&w), SUPCIS_OK);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_confirm_succeeds_when_assigned),
        cmocka_unit_test(test_confirm_allows_short_pick),
        cmocka_unit_test(test_confirm_fails_if_not_assigned),
        cmocka_unit_test(test_confirm_rejects_zero_qty),
        cmocka_unit_test(test_confirm_rejects_qty_above_to_pick),
        cmocka_unit_test(test_wave_closes_when_all_confirmed),
        cmocka_unit_test(test_wave_cannot_close_with_pending_tasks),
        cmocka_unit_test(test_wave_closes_when_mix_of_confirmed_and_cancelled),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
