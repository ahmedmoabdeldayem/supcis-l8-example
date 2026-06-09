-- Creates a pick wave from RELEASED orders and assigns tasks
-- Called by the picking service when releasing a wave

CREATE OR REPLACE PROCEDURE sp_create_pick_wave(
    p_wave_id   OUT VARCHAR2,
    p_max_tasks IN  NUMBER DEFAULT 100
)
AS
    v_wave_id   VARCHAR2(36);
    v_task_id   VARCHAR2(36);
    v_count     NUMBER := 0;
BEGIN
    -- Generate wave ID
    v_wave_id := SYS_GUID();
    p_wave_id := v_wave_id;

    -- Create wave record
    INSERT INTO pick_wave (wave_id, released_at, is_closed)
    VALUES (v_wave_id, SYSTIMESTAMP, 0);

    -- Pull tasks from released order lines that have available stock
    FOR rec IN (
        SELECT ol.line_id, ol.sku_id, si.location_code,
               LEAST(ol.qty_ordered - ol.qty_picked, si.quantity_on_hand - si.quantity_reserved) AS qty
        FROM order_line ol
        JOIN order_header oh ON oh.order_id = ol.order_id
        JOIN stock_item   si ON si.sku_id   = ol.sku_id
        WHERE oh.status = 'RELEASED'
          AND ol.qty_picked < ol.qty_ordered
          AND (si.quantity_on_hand - si.quantity_reserved) > 0
          AND ROWNUM <= p_max_tasks
        ORDER BY oh.created_at ASC
    )
    LOOP
        v_task_id := SYS_GUID();

        INSERT INTO pick_task (task_id, wave_id, order_line_id, sku_id,
                               location_code, qty_to_pick, qty_picked, status)
        VALUES (v_task_id, v_wave_id, rec.line_id, rec.sku_id,
                rec.location_code, rec.qty, 0, 'PENDING');

        -- Reserve the stock
        UPDATE stock_item
        SET quantity_reserved = quantity_reserved + rec.qty,
            last_updated      = SYSTIMESTAMP
        WHERE sku_id = rec.sku_id AND location_code = rec.location_code;

        v_count := v_count + 1;
    END LOOP;

    COMMIT;
    DBMS_OUTPUT.PUT_LINE('Wave ' || v_wave_id || ' created with ' || v_count || ' tasks');

EXCEPTION
    WHEN OTHERS THEN
        ROLLBACK;
        RAISE;
END sp_create_pick_wave;
/
