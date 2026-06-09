/*
 * test_helper.c — shared setup/teardown and factory functions for all tests
 *
 * Instead of duplicating `make_item()`, `make_order()` etc. in every test file,
 * they live here and are shared. This keeps individual test files focused
 * on what they test, not on how to build test data.
 */
#include "test_helper.h"
#include <string.h>
#include <stdio.h>

stock_item_t test_make_stock_item(const char *sku, const char *location,
                                   quantity_t on_hand, quantity_t reserved)
{
    stock_item_t item = {0};
    snprintf(item.stock_id, sizeof(item.stock_id), "TEST-STOCK-%s", sku);
    strncpy(item.sku_id,        sku,      sizeof(item.sku_id)        - 1);
    strncpy(item.location_code, location, sizeof(item.location_code) - 1);
    item.quantity_on_hand  = on_hand;
    item.quantity_reserved = reserved;
    return item;
}

order_t test_make_order(order_status_t status, int line_count, quantity_t qty_per_line)
{
    order_t o = {0};
    snprintf(o.order_id,     sizeof(o.order_id),     "ORD-TEST-001");
    snprintf(o.customer_ref, sizeof(o.customer_ref), "SAP-TEST-REF");
    o.status     = status;
    o.line_count = line_count;

    for (int i = 0; i < line_count; i++) {
        snprintf(o.lines[i].line_id, sizeof(o.lines[i].line_id), "LINE-%03d", i);
        snprintf(o.lines[i].sku_id,  sizeof(o.lines[i].sku_id),  "SKU-TEST-%03d", i);
        o.lines[i].qty_ordered = qty_per_line;
        o.lines[i].qty_picked  = 0;
    }
    return o;
}
