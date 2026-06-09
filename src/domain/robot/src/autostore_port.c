/*
 * autostore_port.c — HTTP adapter to the AutoStore robot controller
 *
 * ── CONTEXT ──────────────────────────────────────────────────────────────────
 *   AutoStore is a physical robotic system. Robots (small wheeled machines)
 *   drive on top of a dense grid of storage bins stacked vertically.
 *   To retrieve a bin, a robot digs down through the stack, grabs it,
 *   and brings it to a "port" where a human picker takes items from it.
 *
 *   The AutoStore controller is a separate server (IP-based) that manages
 *   the robot fleet. SuPCIS-L8 sends it commands via HTTP POST requests.
 *
 * ── WHY libcurl? ─────────────────────────────────────────────────────────────
 *   libcurl is the standard HTTP client library for C.
 *   It handles TCP connections, TLS, timeouts, redirects, headers — everything
 *   you'd need to write yourself for raw socket communication.
 *
 * ── INTEGRATION PATTERN: Command + Poll ──────────────────────────────────────
 *   1. autostore_send_command() — POST the command, get a command_id back
 *   2. autostore_poll_status()  — GET the status of that command_id
 *   This is an async pattern: the robot takes time to execute,
 *   so we don't block and wait — we poll until it reports "complete".
 */

#include "autostore_port.h"
#include "logger.h"
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * build_command_json — serialize a robot_command_t to JSON for the HTTP body.
 *
 * Why manual snprintf instead of a JSON library?
 *   The payload is small and fixed in structure. Using jansson or cJSON for
 *   this 4-field object would add a dependency and complexity for no benefit.
 *   For larger or dynamic payloads, json_serializer.c uses jansson.
 *
 * Output example:
 *   {"commandId":"abc-123","type":1,"binId":"BIN-42","targetPort":"PORT-01"}
 */
static void build_command_json(const robot_command_t *cmd, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size,
        "{\"commandId\":\"%s\",\"type\":%d,\"binId\":\"%s\",\"targetPort\":\"%s\"}",
        cmd->command_id,
        cmd->type,       /* 1=FETCH_BIN, 2=RETURN_BIN, 3=MOVE_TO_PORT */
        cmd->bin_id,
        cmd->target_port);
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t autostore_send_command(
    const autostore_config_t *cfg,
    const robot_command_t    *cmd)
{
    CURL    *curl;
    CURLcode res;
    char     url[256];
    char     json[512];

    /* Build the target URL: http://<controller>/api/v1/commands */
    snprintf(url, sizeof(url), "%s/api/v1/commands", cfg->controller_url);

    /* Serialize the command struct to a JSON string */
    build_command_json(cmd, json, sizeof(json));

    /* Initialize a libcurl "easy" session (one request per session) */
    curl = curl_easy_init();
    if (!curl) return SUPCIS_ERR_EXTERNAL;  /* curl failed to allocate — rare */

    /* Build HTTP headers:
     *   Content-Type: application/json  — tells the controller we're sending JSON
     *   X-API-Key: <key>                — authentication (bearer token alternative) */
    struct curl_slist *headers = NULL;
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "X-API-Key: %s", cfg->api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    /* Configure the request:
     *   CURLOPT_URL        — the target endpoint
     *   CURLOPT_POSTFIELDS — makes this a POST request with the JSON body
     *   CURLOPT_HTTPHEADER — attach our custom headers
     *   CURLOPT_TIMEOUT_MS — abort if no response within timeout_ms milliseconds */
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     (long)cfg->timeout_ms);
    /* Enforce TLS certificate verification to prevent MITM attacks.
     * VERIFYPEER=1: reject invalid/self-signed/expired certificates.
     * VERIFYHOST=2: reject certificates where the hostname doesn't match.
     * If the controller uses a private CA, set CURLOPT_CAINFO to its cert path. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Execute the HTTP request (blocking until response or timeout) */
    res = curl_easy_perform(curl);

    /* Always clean up — curl_slist is heap-allocated, must be freed */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        /* CURLE_OK = 0. Any other value = network/timeout/TLS error.
         * curl_easy_strerror() returns a human-readable description. */
        LOG_ERROR("autostore", "Command send failed: %s", curl_easy_strerror(res));
        return SUPCIS_ERR_EXTERNAL;
    }

    LOG_INFO("autostore", "Command %s sent to controller", cmd->command_id);
    return SUPCIS_OK;
}

/* ─────────────────────────────────────────────────────────────────────────── */

supcis_result_t autostore_poll_status(
    const autostore_config_t *cfg,
    const char               *command_id,
    bool                     *out_complete)
{
    /*
     * In production this sends:
     *   GET <controller_url>/api/v1/commands/<command_id>/status
     * And parses the JSON response to check if the robot has finished.
     *
     * The caller (robot_service) calls this in a loop with a small sleep
     * between iterations until out_complete becomes true or a timeout expires.
     *
     * Simplified stub here — a full implementation would use curl to GET
     * and parse {"status":"COMPLETE"} or {"status":"IN_PROGRESS"}.
     */
    (void)cfg;        /* suppress "unused parameter" compiler warning */
    (void)command_id;

    *out_complete = false;  /* stub always returns "not done yet" */

    LOG_DEBUG("autostore", "Polling status for command %s", command_id);
    return SUPCIS_OK;
}
