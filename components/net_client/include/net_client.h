#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NET_TODO_ID_MAX   40
#define NET_TODO_NAME_MAX 200
#define NET_TODO_DUE_MAX  40
#define NET_STATUS_MAX    64
#define NET_ERROR_MAX     96

typedef struct {
    bool valid;
    bool empty;
    char id[NET_TODO_ID_MAX];
    char name[NET_TODO_NAME_MAX];
    char due[NET_TODO_DUE_MAX];
    bool done;
} net_todo_t;

typedef struct {
    bool configured;
    bool busy;
    bool connected;
    bool todo_ready;
    bool hook_ready;
    net_todo_t todo;
    uint32_t queued;
    char message[NET_STATUS_MAX];
    char error[NET_ERROR_MAX];
} net_status_t;

typedef enum {
    /* Boot, PWR click or other explicit refresh: drain, poll, fetch todo. */
    NET_KICK_ACTION = 0,
    /* Wi-Fi came up: same as an action. */
    NET_KICK_WIFI,
    /* A recording was queued: send it right away. */
    NET_KICK_RECORDING,
} net_kick_reason_t;

typedef void (*net_event_cb_t)(void);

esp_err_t net_client_init(void);
void net_client_set_event_callback(net_event_cb_t cb);

/* Non-blocking. Safe from any task. */
void net_client_kick(net_kick_reason_t reason);

/* Debounced PATCH from the UI; the worker performs it asynchronously. */
esp_err_t net_client_patch_done(const char *todo_id, bool done);

void net_client_get_todo(net_todo_t *out);
bool net_client_is_busy(void);
void net_client_get_status(net_status_t *out);
