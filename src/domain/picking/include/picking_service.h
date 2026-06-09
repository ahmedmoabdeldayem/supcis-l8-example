#ifndef PICKING_SERVICE_H
#define PICKING_SERVICE_H

/*
 * picking_service.h — domain service for pick wave creation (DDD: Domain Service)
 *
 * ── WHAT IS A DDD DOMAIN SERVICE? ───────────────────────────────────────────
 *   A Domain Service contains business logic that does NOT naturally belong
 *   to a single entity. Creating a pick wave requires:
 *     - Reading from the Order entity
 *     - Querying the inventory repository
 *     - Building PickTask entities inside a PickWave entity
 *
 *   That crosses multiple entities, so it lives here as a standalone service.
 *
 * ── WHAT THIS SERVICE DOES ──────────────────────────────────────────────────
 *   Given a RELEASED order and access to inventory, it:
 *     1. Iterates the order's lines
 *     2. Finds available stock for each SKU via the repository
 *     3. Reserves the stock (stock_item_reserve)
 *     4. Persists the reservation (repo->save)
 *     5. Creates a pick_task_t for each line
 *     6. Packages all tasks into a pick_wave_t
 *
 * ── WHAT IT DOES NOT DO ─────────────────────────────────────────────────────
 *   - Does not persist the wave itself (caller's responsibility)
 *   - Does not update the order status (application layer does that)
 *   - Does not know about HTTP, JSON, or Oracle internals
 *
 * ── RETURN VALUES ───────────────────────────────────────────────────────────
 *   SUPCIS_OK          — wave created, out_wave is populated
 *   SUPCIS_ERR_CONFLICT  — order is not in RELEASED status
 *   SUPCIS_ERR_NOT_FOUND — no stock available for any of the lines
 *   SUPCIS_ERR_DB        — repository call failed
 */

#include "picking_entity.h"
#include "order_entity.h"
#include "inventory_repository.h"

/*
 * picking_service_create_wave
 *
 *   order    : the RELEASED order whose lines we are converting to pick tasks
 *   repo     : inventory repository to query available stock and reserve it
 *   out_wave : caller-allocated wave struct that will be populated on success
 */
supcis_result_t picking_service_create_wave(
    const order_t          *order,
    inventory_repository_t *repo,
    pick_wave_t            *out_wave);

#endif /* PICKING_SERVICE_H */
