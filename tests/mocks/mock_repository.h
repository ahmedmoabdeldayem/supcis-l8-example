#ifndef MOCK_REPOSITORY_H
#define MOCK_REPOSITORY_H

/*
 * mock_repository.h — reusable mock inventory repository for tests
 *
 * Instead of each test file defining its own mock, they include this header
 * and call mock_repo_reset() between tests to start fresh.
 *
 * The mock stores up to 10 stock items in a local array.
 * Tests add items via mock_repo_add_item() then pass &mock_inventory_repo
 * to any function that needs an inventory_repository_t.
 *
 * WHY MOCK AND NOT A REAL DB?
 *   Unit tests must be fast (< 1ms per test) and runnable without infrastructure.
 *   A real Oracle DB would make tests take seconds and require a live server.
 *   Mocks isolate the domain logic from external dependencies.
 */

#include "inventory_repository.h"

/* The mock repository instance — pass its address to domain functions */
extern inventory_repository_t mock_inventory_repo;

/* Reset the mock: clear all stored items. Call before each test. */
void mock_repo_reset(void);

/* Add a stock item to the mock's backing store */
void mock_repo_add_item(const stock_item_t *item);

/* Retrieve saved items (to assert what was persisted during a test) */
const stock_item_t *mock_repo_get_saved(int index);
int                 mock_repo_save_count(void);

#endif /* MOCK_REPOSITORY_H */
