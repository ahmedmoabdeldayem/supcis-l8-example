/*
 * picking_entity.c — PickTask and PickWave business logic
 *
 * These functions enforce the rules for how pickers interact with tasks:
 *   - A task can only be confirmed if it was assigned first
 *   - A wave can only close when all its tasks are done
 *
 * No database access here — purely in-memory domain logic.
 */

#include "picking_entity.h"
#include "logger.h"

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t pick_task_confirm(pick_task_t *task, quantity_t qty_picked)
{
    /*
     * Business rule: only ASSIGNED tasks can be confirmed.
     *
     * Why? Because the system assigns tasks to specific pickers.
     * Confirming a PENDING task (one not yet assigned) would mean bypassing
     * the assignment step — the system would not know which picker did it.
     * Confirming an already-CONFIRMED task would be a double-confirmation.
     */
    if (task->status != PICK_TASK_ASSIGNED) {
        LOG_WARN("picking", "Task %s not in ASSIGNED state", task->task_id);
        return SUPCIS_ERR_CONFLICT;
    }

    /*
     * Guard: qty_picked must be between 1 and qty_to_pick.
     *   - 0 means the picker picked nothing → they should cancel, not confirm
     *   - > qty_to_pick means they picked more than asked — physically impossible
     */
    if (qty_picked <= 0 || qty_picked > task->qty_to_pick) {
        return SUPCIS_ERR_INVALID_ARG;
    }

    /*
     * Record what was actually picked and mark as done.
     * qty_picked may be less than qty_to_pick (a "short pick"):
     *   e.g. asked for 10, bin only had 7 → qty_picked = 7
     * The application layer handles short picks (backorder, alert, re-wave).
     */
    task->qty_picked = qty_picked;
    task->status     = PICK_TASK_CONFIRMED;

    LOG_INFO("picking", "Task %s confirmed: picked %d of %d",
             task->task_id, qty_picked, task->qty_to_pick);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int pick_wave_pending_count(const pick_wave_t *wave)
{
    /*
     * Count tasks that still need action: PENDING or ASSIGNED.
     * CONFIRMED and CANCELLED tasks are done — they do not count.
     *
     * This is a simple linear scan. With WAVE_MAX_TASKS = 500 this is
     * O(500) = effectively O(1) — no optimization needed.
     */
    int count = 0;
    for (int i = 0; i < wave->task_count; i++) {
        if (wave->tasks[i].status == PICK_TASK_PENDING ||
            wave->tasks[i].status == PICK_TASK_ASSIGNED) {
            count++;
        }
    }
    return count;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t pick_wave_close(pick_wave_t *wave)
{
    /*
     * A wave can only close when ALL tasks are done.
     *
     * Why enforce this? Closing a wave triggers order status changes upstream
     * (PICKING → PACKING) and releases pickers to receive new work.
     * If we allowed closing with pending tasks, those tasks would become
     * orphaned — no wave to belong to, no picker working on them.
     */
    if (pick_wave_pending_count(wave) > 0) {
        LOG_WARN("picking", "Wave %s cannot close: %d tasks still pending",
                 wave->wave_id, pick_wave_pending_count(wave));
        return SUPCIS_ERR_CONFLICT;
    }

    wave->is_closed = true;
    return SUPCIS_OK;
}
