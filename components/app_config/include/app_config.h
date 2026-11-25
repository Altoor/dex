#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Rolodex connection settings plus the last dynamic-API result. */

#define APP_CONFIG_URL_MAX    160
#define APP_CONFIG_KEY_MAX    192
#define APP_CONFIG_SLUG_MAX   64
#define APP_CONFIG_STATE_MAX  256
#define APP_CONFIG_RESULT_MAX 512
#define APP_CONFIG_ERROR_MAX  192
#define APP_CONFIG_ID_MAX     48
#define APP_CONFIG_STATUS_MAX 24

typedef struct {
    char server_url[APP_CONFIG_URL_MAX];
    char standard_key[APP_CONFIG_KEY_MAX];
    char dynamic_slug[APP_CONFIG_SLUG_MAX];
    char dynamic_key[APP_CONFIG_KEY_MAX];
    char state[APP_CONFIG_STATE_MAX];
} app_config_t;

typedef struct {
    bool present;
    char request_id[APP_CONFIG_ID_MAX];
    char status[APP_CONFIG_STATUS_MAX];
    char result[APP_CONFIG_RESULT_MAX];
    char error[APP_CONFIG_ERROR_MAX];
    int64_t updated_ms;
} app_result_t;

esp_err_t app_config_init(void);

/* Thread-safe copies. */
void app_config_get(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);

bool app_config_todo_ready(const app_config_t *cfg);
bool app_config_hook_ready(const app_config_t *cfg);
bool app_config_any_ready(const app_config_t *cfg);

const char *app_config_default_state(void);

void app_config_get_result(app_result_t *out);
esp_err_t app_config_set_result(const app_result_t *result);
