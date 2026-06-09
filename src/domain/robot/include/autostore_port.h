#ifndef AUTOSTORE_PORT_H
#define AUTOSTORE_PORT_H

#include "types.h"

/*
 * autostore_port: adapter that speaks to the AutoStore Controller REST API.
 *
 * AutoStore robots retrieve bins from the grid when commanded.
 * The controller exposes an HTTP API — we POST commands and poll status.
 */

typedef enum {
    ROBOT_CMD_FETCH_BIN   = 1,  /* bring a bin to a port */
    ROBOT_CMD_RETURN_BIN  = 2,  /* return a bin to the grid */
    ROBOT_CMD_MOVE_TO_PORT= 3   /* move robot to a specific port */
} robot_cmd_type_t;

typedef struct {
    char             command_id[37];
    robot_cmd_type_t type;
    char             bin_id[24];
    char             target_port[16];
    bool             acknowledged;
} robot_command_t;

typedef struct {
    char  controller_url[128]; /* e.g. "http://192.168.1.100:8080" */
    char  api_key[64];
    int   timeout_ms;
} autostore_config_t;

supcis_result_t autostore_send_command(
    const autostore_config_t *cfg,
    const robot_command_t    *cmd);

supcis_result_t autostore_poll_status(
    const autostore_config_t *cfg,
    const char               *command_id,
    bool                     *out_complete);

#endif /* AUTOSTORE_PORT_H */
