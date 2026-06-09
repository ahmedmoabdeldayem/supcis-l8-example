/*
 * erp_adapter.c — SAP ERP integration stub
 *
 * Real implementation calls SAP RFC functions via the NW RFC SDK:
 *   RfcOpenConnection() → establish RFC connection to SAP
 *   RfcGetFunctionDesc() → describe an RFC function module
 *   RfcCreateFunction()  → create a call object
 *   RfcSetChars()        → set input parameters
 *   RfcInvoke()          → execute the RFC call
 *   RfcDestroyFunction() → clean up
 *   RfcCloseConnection() → close connection
 *
 * The RFC function modules called are customer-specific:
 *   ZSUPCIS_NOTIFY_PICKING  — custom function created by SAP team at the customer
 *   ZSUPCIS_GOODS_ISSUE     — posts goods movement in SAP
 */
#include "erp_adapter.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

struct erp_adapter {
    int  connected;
    char host[128];
    char client[8];
};

supcis_result_t erp_adapter_connect(
    const char    *host,
    const char    *client,
    const char    *user,
    const char    *password,
    erp_adapter_t **out)
{
    (void)user; (void)password;

    erp_adapter_t *a = calloc(1, sizeof(erp_adapter_t));
    if (!a) return SUPCIS_ERR_OUT_OF_MEM;

    /*
     * Real: RfcConnectionParameter params[] = {
     *     { cU("ASHOST"), host }, { cU("CLIENT"), client },
     *     { cU("USER"),   user }, { cU("PASSWD"), password }
     * };
     * a->rfc_handle = RfcOpenConnection(params, 4, &error_info);
     */

    strncpy(a->host,   host,   sizeof(a->host)   - 1);
    strncpy(a->client, client, sizeof(a->client) - 1);
    a->connected = 1;

    LOG_INFO("erp", "Connected to SAP: %s client %s", host, client);
    *out = a;
    return SUPCIS_OK;
}

supcis_result_t erp_adapter_notify_picking_started(
    erp_adapter_t *adapter,
    const char    *order_id)
{
    if (!adapter || !adapter->connected) return SUPCIS_ERR_INVALID_ARG;

    /*
     * Real: invoke RFC function ZSUPCIS_NOTIFY_PICKING with:
     *   ORDERID = order_id
     *   STATUS  = "PICKING"
     * SAP updates delivery document status on its side.
     */

    LOG_INFO("erp", "Notified SAP: picking started for order %s", order_id);
    return SUPCIS_OK; /* stub */
}

supcis_result_t erp_adapter_confirm_shipment(
    erp_adapter_t *adapter,
    const char    *order_id)
{
    if (!adapter || !adapter->connected) return SUPCIS_ERR_INVALID_ARG;

    /*
     * Real: invoke RFC function ZSUPCIS_GOODS_ISSUE with:
     *   ORDERID     = order_id
     *   POSTING_DATE = today
     * SAP posts goods issue — inventory officially leaves SAP stock.
     */

    LOG_INFO("erp", "Confirmed shipment to SAP for order %s", order_id);
    return SUPCIS_OK; /* stub */
}

void erp_adapter_disconnect(erp_adapter_t *adapter)
{
    if (!adapter) return;
    /* Real: RfcCloseConnection(adapter->rfc_handle, &error_info) */
    LOG_INFO("erp", "Disconnected from SAP: %s", adapter->host);
    free(adapter);
}
