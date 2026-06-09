#ifndef ERP_ADAPTER_H
#define ERP_ADAPTER_H

/*
 * erp_adapter.h — integration with customer ERP systems (SAP, etc.)
 *
 * ── WHAT IS AN ERP? ──────────────────────────────────────────────────────────
 *   ERP = Enterprise Resource Planning. SAP is the most common in manufacturing.
 *   The ERP is the system of record for orders, invoices, and inventory levels
 *   at the business level.
 *
 *   SuPCIS-L8 is the OPERATIONAL system — it manages the physical warehouse.
 *   The two systems must stay in sync:
 *     ERP → SuPCIS: new orders arrive as IDocs (SAP documents)
 *     SuPCIS → ERP: shipment confirmations, stock adjustments sent back
 *
 * ── COMMUNICATION METHOD ─────────────────────────────────────────────────────
 *   SAP uses RFC (Remote Function Call) — its own RPC protocol.
 *   The SAP RFC library for C is called NW RFC SDK (NetWeaver RFC SDK).
 *   Alternatively: REST-based SAP APIs (newer SAP versions support this).
 *
 * ── THIS IS A STUB ────────────────────────────────────────────────────────────
 *   Real implementation links against sapnwrfc.h and libsapnwrfc.so.
 */

#include "types.h"
#include "order_entity.h"

/*
 * erp_adapter_t — adapter state, holds the RFC connection handle.
 * Callers create one at startup and pass it to each function.
 */
typedef struct erp_adapter erp_adapter_t; /* opaque — RFC internals hidden */

supcis_result_t erp_adapter_connect(
    const char    *host,
    const char    *client,
    const char    *user,
    const char    *password,
    erp_adapter_t **out);

/*
 * erp_adapter_notify_picking_started — tell the ERP that picking began.
 *   ERP updates the order status on its side (e.g. SAP delivery status → "picking").
 */
supcis_result_t erp_adapter_notify_picking_started(
    erp_adapter_t *adapter,
    const char    *order_id);

/*
 * erp_adapter_confirm_shipment — send goods issue confirmation to ERP.
 *   ERP posts the inventory reduction and closes the delivery document.
 */
supcis_result_t erp_adapter_confirm_shipment(
    erp_adapter_t *adapter,
    const char    *order_id);

void erp_adapter_disconnect(erp_adapter_t *adapter);

#endif /* ERP_ADAPTER_H */
