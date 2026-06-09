-- v_wave_progress — how far along each active pick wave is
-- Used by supervisors to see floor activity in real time.

CREATE OR REPLACE VIEW v_wave_progress AS
SELECT
    pw.wave_id,
    pw.released_at,
    COUNT(pt.task_id)                                                    AS total_tasks,
    SUM(CASE WHEN pt.status = 'CONFIRMED'  THEN 1 ELSE 0 END)           AS confirmed_tasks,
    SUM(CASE WHEN pt.status = 'CANCELLED'  THEN 1 ELSE 0 END)           AS cancelled_tasks,
    SUM(CASE WHEN pt.status IN ('PENDING','ASSIGNED') THEN 1 ELSE 0 END) AS pending_tasks,
    -- Completion percentage
    ROUND(
        SUM(CASE WHEN pt.status IN ('CONFIRMED','CANCELLED') THEN 1 ELSE 0 END)
        * 100.0 / NULLIF(COUNT(pt.task_id), 0), 1
    ) AS pct_complete,
    pw.is_closed
FROM pick_wave pw
JOIN pick_task pt ON pt.wave_id = pw.wave_id
WHERE pw.is_closed = 0   -- only show active waves
GROUP BY pw.wave_id, pw.released_at, pw.is_closed;
