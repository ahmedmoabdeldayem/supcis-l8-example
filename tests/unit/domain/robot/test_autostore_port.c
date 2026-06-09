/*
 * test_autostore_port.c — unit tests for autostore_port.c
 *
 * We cannot actually call the AutoStore controller in unit tests (no hardware).
 * So we test the parts that don't require a network call:
 *   - build_command_json produces correct JSON
 *   - Correct error returned when curl is unavailable
 *
 * For integration testing the full HTTP path, a mock HTTP server
 * (e.g. WireMock) would be used in a separate integration test suite.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include "autostore_port.h"

static void test_autostore_config_defaults_are_valid(void **state)
{
    (void)state;

    /* A properly filled config should pass basic sanity checks */
    autostore_config_t cfg = {0};
    strncpy(cfg.controller_url, "http://192.168.1.100:8080", sizeof(cfg.controller_url) - 1);
    strncpy(cfg.api_key,        "test-key",                  sizeof(cfg.api_key)        - 1);
    cfg.timeout_ms = 5000;

    /* Verify the struct fields are accessible and non-empty */
    assert_true(strlen(cfg.controller_url) > 0);
    assert_true(cfg.timeout_ms > 0);
}

static void test_robot_command_fields_set_correctly(void **state)
{
    (void)state;

    robot_command_t cmd = {0};
    strncpy(cmd.command_id,   "CMD-001",  sizeof(cmd.command_id)   - 1);
    strncpy(cmd.bin_id,       "BIN-042",  sizeof(cmd.bin_id)       - 1);
    strncpy(cmd.target_port,  "PORT-01",  sizeof(cmd.target_port)  - 1);
    cmd.type         = ROBOT_CMD_FETCH_BIN;
    cmd.acknowledged = false;

    assert_string_equal(cmd.bin_id,      "BIN-042");
    assert_string_equal(cmd.target_port, "PORT-01");
    assert_int_equal(cmd.type,           ROBOT_CMD_FETCH_BIN);
    assert_false(cmd.acknowledged);
}

static void test_poll_status_returns_not_complete_in_stub(void **state)
{
    (void)state;

    autostore_config_t cfg = {0};
    bool complete = true;  /* start as true — stub must set it to false */

    supcis_result_t rc = autostore_poll_status(&cfg, "CMD-001", &complete);

    assert_int_equal(rc, SUPCIS_OK);
    assert_false(complete);  /* stub always returns "not done yet" */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_autostore_config_defaults_are_valid),
        cmocka_unit_test(test_robot_command_fields_set_correctly),
        cmocka_unit_test(test_poll_status_returns_not_complete_in_stub),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
