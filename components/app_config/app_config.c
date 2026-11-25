#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "cfg";

#define NVS_NAMESPACE "dexcfg"
#define NVS_SERVER    "srv_url"
#define NVS_STD_KEY   "std_key"
#define NVS_DYN_SLUG  "dyn_slug"
#define NVS_DYN_KEY   "dyn_key"
#define NVS_DYN_STATE "dyn_state"
#define NVS_LAST_RID  "last_rid"
#define NVS_LAST_STAT "last_stat"
#define NVS_LAST_RES  "last_res"
#define NVS_LAST_ERR  "last_err"
#define NVS_LAST_UPD  "last_upd"

#define DEFAULT_STATE "Voice note from dex"

static app_config_t s_config;
static app_result_t s_result;
static SemaphoreHandle_t s_lock;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src ? src : "");
}

/* Strip a single trailing slash so URL joins are predictable. */
static void normalise_url(char *url)
{
    size_t len = strlen(url);
    while (len > 0 && (url[len - 1] == '/' || url[len - 1] == ' ')) {
        url[--len] = '\0';
    }
}

static void set_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    copy_str(s_config.state, sizeof(s_config.state), DEFAULT_STATE);
}

static esp_err_t read_str(nvs_handle_t handle, const char *key, char *out,
                          size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static void load(void)
{
    set_defaults();

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    read_str(handle, NVS_SERVER, s_config.server_url, sizeof(s_config.server_url));
    read_str(handle, NVS_STD_KEY, s_config.standard_key, sizeof(s_config.standard_key));
    read_str(handle, NVS_DYN_SLUG, s_config.dynamic_slug, sizeof(s_config.dynamic_slug));
    read_str(handle, NVS_DYN_KEY, s_config.dynamic_key, sizeof(s_config.dynamic_key));
    read_str(handle, NVS_DYN_STATE, s_config.state, sizeof(s_config.state));
    if (s_config.state[0] == '\0') {
        copy_str(s_config.state, sizeof(s_config.state), DEFAULT_STATE);
    }
    normalise_url(s_config.server_url);

    memset(&s_result, 0, sizeof(s_result));
    read_str(handle, NVS_LAST_RID, s_result.request_id, sizeof(s_result.request_id));
    read_str(handle, NVS_LAST_STAT, s_result.status, sizeof(s_result.status));
    read_str(handle, NVS_LAST_RES, s_result.result, sizeof(s_result.result));
    read_str(handle, NVS_LAST_ERR, s_result.error, sizeof(s_result.error));
    uint64_t updated = 0;
    if (nvs_get_u64(handle, NVS_LAST_UPD, &updated) == ESP_OK) {
        s_result.updated_ms = (int64_t)updated;
    }
    s_result.present = s_result.request_id[0] != '\0' || s_result.status[0] != '\0';

    nvs_close(handle);
}

esp_err_t app_config_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    load();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "server=%s todo=%s hook=%s",
             s_config.server_url[0] ? s_config.server_url : "(unset)",
             app_config_todo_ready(&s_config) ? "yes" : "no",
             app_config_hook_ready(&s_config) ? "yes" : "no");
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        *out = s_config;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t clean = *cfg;
    normalise_url(clean.server_url);
    if (clean.state[0] == '\0') {
        copy_str(clean.state, sizeof(clean.state), DEFAULT_STATE);
    }

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "nvs open");
    esp_err_t err = nvs_set_str(handle, NVS_SERVER, clean.server_url);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_STD_KEY, clean.standard_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_DYN_SLUG, clean.dynamic_slug);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_DYN_KEY, clean.dynamic_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_DYN_STATE, clean.state);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_config = clean;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "settings saved");
    return ESP_OK;
}

bool app_config_todo_ready(const app_config_t *cfg)
{
    return cfg != NULL && cfg->server_url[0] != '\0' &&
           cfg->standard_key[0] != '\0';
}

bool app_config_hook_ready(const app_config_t *cfg)
{
    return cfg != NULL && cfg->server_url[0] != '\0' &&
           cfg->dynamic_slug[0] != '\0' && cfg->dynamic_key[0] != '\0';
}

bool app_config_any_ready(const app_config_t *cfg)
{
    return app_config_todo_ready(cfg) || app_config_hook_ready(cfg);
}

const char *app_config_default_state(void)
{
    return DEFAULT_STATE;
}

void app_config_get_result(app_result_t *out)
{
    if (out == NULL) {
        return;
    }

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = s_result;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t app_config_set_result(const app_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_result_t clean = *result;
    clean.updated_ms = esp_timer_get_time() / 1000;
    clean.present = true;
    clean.result[sizeof(clean.result) - 1] = '\0';
    clean.error[sizeof(clean.error) - 1] = '\0';

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "nvs open");
    esp_err_t err = nvs_set_str(handle, NVS_LAST_RID, clean.request_id);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_LAST_STAT, clean.status);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_LAST_RES, clean.result);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_LAST_ERR, clean.error);
    }
    if (err == ESP_OK) {
        err = nvs_set_u64(handle, NVS_LAST_UPD, (uint64_t)clean.updated_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_result = clean;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return err;
}
