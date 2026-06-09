#ifndef TEST_HELPER_H
#define TEST_HELPER_H

/*
 * test_helper.h — shared factory functions for building test data
 *
 * Include this in any test file that needs pre-built entities.
 */
#include "inventory_entity.h"
#include "order_entity.h"

/* Create a stock_item_t with the given values */
stock_item_t test_make_stock_item(const char *sku, const char *location,
                                   quantity_t on_hand, quantity_t reserved);

/* Create an order_t with N lines each having qty_per_line ordered */
order_t test_make_order(order_status_t status, int line_count, quantity_t qty_per_line);

#endif /* TEST_HELPER_H */
