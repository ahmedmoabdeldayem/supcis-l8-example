-- v_inventory_on_hand — current stock summary per SKU and location
-- Used by reporting dashboards and inventory queries.
--
-- WHY A VIEW AND NOT A DIRECT TABLE QUERY?
--   Views encapsulate complex joins. The REST handler calls this view
--   with a simple SELECT — no join logic in C code.
--   If the table structure changes, only the view needs updating,
--   not every place in the application that reads inventory.

CREATE OR REPLACE VIEW v_inventory_on_hand AS
SELECT
    si.sku_id,
    si.location_code,
    wl.aisle,
    wl.level,
    wl.location_type,
    si.quantity_on_hand,
    si.quantity_reserved,
    (si.quantity_on_hand - si.quantity_reserved) AS quantity_available,
    si.last_updated
FROM stock_item si
JOIN warehouse_location wl ON wl.location_code = si.location_code
WHERE si.quantity_on_hand > 0     -- exclude empty bins from the view
  AND wl.is_active = 1;           -- exclude decommissioned locations
