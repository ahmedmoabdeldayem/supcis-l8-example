-- v_open_orders — all orders not yet shipped or cancelled
-- Used by the picking service to find work and by dashboards.

CREATE OR REPLACE VIEW v_open_orders AS
SELECT
    oh.order_id,
    oh.customer_ref,
    oh.status,
    oh.created_at,
    COUNT(ol.line_id)                           AS total_lines,
    SUM(ol.qty_ordered)                         AS total_qty_ordered,
    SUM(ol.qty_picked)                          AS total_qty_picked,
    -- How complete is this order? (percentage)
    ROUND(SUM(ol.qty_picked) * 100.0 / NULLIF(SUM(ol.qty_ordered), 0), 1) AS pct_picked
FROM order_header oh
JOIN order_line ol ON ol.order_id = oh.order_id
WHERE oh.status NOT IN ('SHIPPED', 'CANCELLED')
GROUP BY oh.order_id, oh.customer_ref, oh.status, oh.created_at
ORDER BY oh.created_at ASC;  -- oldest orders first (FIFO processing)
