#include "web_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "battery.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "net_client.h"
#include "storage.h"
#include "wifi_manager.h"

#define DEX_VERSION "0.2.0"
#define MAX_BODY_SIZE 4096

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_timer_handle_t s_reboot_timer;
static httpd_handle_t s_server;
static bool s_running;

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rebooting");
    esp_restart();
}

static void schedule_reboot(void)
{
    if (s_reboot_timer) {
        esp_timer_stop(s_reboot_timer);
        esp_timer_start_once(s_reboot_timer, 800 * 1000);
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "error", message);

    httpd_resp_set_status(req, status);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static void add_key_fields(cJSON *root, const char *prefix, const char *key)
{
    char name[32];
    char hint[32];

    snprintf(name, sizeof(name), "%s_set", prefix);
    cJSON_AddBoolToObject(root, name, key[0] != '\0');

    size_t len = strlen(key);
    if (len == 0) {
        hint[0] = '\0';
    } else if (len <= 8) {
        snprintf(hint, sizeof(hint), "(set)");
    } else {
        snprintf(hint, sizeof(hint), "%.4s...%.4s", key, key + len - 4);
    }

    snprintf(name, sizeof(name), "%s_hint", prefix);
    cJSON_AddStringToObject(root, name, hint);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "server_url", cfg.server_url);
    cJSON_AddStringToObject(root, "dynamic_slug", cfg.dynamic_slug);
    cJSON_AddStringToObject(root, "state", cfg.state);
    cJSON_AddStringToObject(root, "default_state", app_config_default_state());
    add_key_fields(root, "standard_key", cfg.standard_key);
    add_key_fields(root, "dynamic_key", cfg.dynamic_key);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);

    battery_status_t battery;
    battery_read(&battery);

    net_status_t net;
    net_client_get_status(&net);

    app_result_t result;
    app_config_get_result(&result);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    cJSON_AddBoolToObject(root, "has_credentials", status.has_credentials);
    cJSON_AddStringToObject(root, "ssid", status.ssid);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddStringToObject(root, "ap_ssid", status.ap_ssid);
    cJSON_AddStringToObject(root, "ap_ip", status.ap_ip);
    cJSON_AddStringToObject(root, "version", DEX_VERSION);

    cJSON *bat = cJSON_AddObjectToObject(root, "battery");
    if (bat) {
        cJSON_AddBoolToObject(bat, "valid", battery.valid);
        cJSON_AddNumberToObject(bat, "millivolts", battery.millivolts);
        cJSON_AddNumberToObject(bat, "percent", battery.percent);
        cJSON_AddBoolToObject(bat, "usb_present", battery.usb_present);
        cJSON_AddBoolToObject(bat, "charging", battery.charging);
    }

    cJSON_AddBoolToObject(root, "sd_mounted", storage_available());
    cJSON_AddNumberToObject(root, "queue", net.queued);

    cJSON *net_obj = cJSON_AddObjectToObject(root, "net");
    if (net_obj) {
        cJSON_AddBoolToObject(net_obj, "configured", net.configured);
        cJSON_AddBoolToObject(net_obj, "busy", net.busy);
        cJSON_AddBoolToObject(net_obj, "connected", net.connected);
        cJSON_AddBoolToObject(net_obj, "todo_ready", net.todo_ready);
        cJSON_AddBoolToObject(net_obj, "hook_ready", net.hook_ready);
        cJSON_AddStringToObject(net_obj, "message", net.message);
        cJSON_AddStringToObject(net_obj, "error", net.error);

        cJSON *todo = cJSON_AddObjectToObject(net_obj, "todo");
        if (todo) {
            cJSON_AddBoolToObject(todo, "valid", net.todo.valid);
            cJSON_AddBoolToObject(todo, "empty", net.todo.empty);
            cJSON_AddStringToObject(todo, "id", net.todo.id);
            cJSON_AddStringToObject(todo, "name", net.todo.name);
            cJSON_AddStringToObject(todo, "due", net.todo.due);
            cJSON_AddBoolToObject(todo, "done", net.todo.done);
        }
    }

    cJSON *res = cJSON_AddObjectToObject(root, "result");
    if (res) {
        cJSON_AddBoolToObject(res, "present", result.present);
        cJSON_AddStringToObject(res, "request_id", result.request_id);
        cJSON_AddStringToObject(res, "status", result.status);
        cJSON_AddStringToObject(res, "result", result.result);
        cJSON_AddStringToObject(res, "error", result.error);
        cJSON_AddNumberToObject(res, "updated_ms", (double)result.updated_ms);
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *records = NULL;
    uint16_t count = 0;

    esp_err_t err = wifi_manager_scan(&records, &count);
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    if (root == NULL || networks == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(networks);
        wifi_manager_free_scan(records);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "networks", networks);

    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            break;
        }

        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "channel", records[i].primary);
        cJSON_AddBoolToObject(item, "secure", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, item);
    }

    err = send_json(req, root);
    cJSON_Delete(root);
    wifi_manager_free_scan(records);
    return err;
}

static esp_err_t read_json_body(httpd_req_t *req, cJSON **out)
{
    *out = NULL;

    int total = req->content_len;
    if (total <= 0 || total > MAX_BODY_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buffer = malloc((size_t)total + 1);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buffer + received, (size_t)(total - received));
        if (ret <= 0) {
            free(buffer);
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[total] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = root;
    return ESP_OK;
}

static void copy_json_string(cJSON *root, const char *name, char *dst,
                             size_t dst_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item)) {
        snprintf(dst, dst_len, "%s", item->valuestring);
    }
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, &root);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }

    app_config_t cfg;
    app_config_get(&cfg);

    copy_json_string(root, "server_url", cfg.server_url, sizeof(cfg.server_url));
    copy_json_string(root, "dynamic_slug", cfg.dynamic_slug, sizeof(cfg.dynamic_slug));
    copy_json_string(root, "state", cfg.state, sizeof(cfg.state));

    cJSON *std_key = cJSON_GetObjectItemCaseSensitive(root, "standard_key");
    if (cJSON_IsString(std_key) && std_key->valuestring[0] != '\0') {
        snprintf(cfg.standard_key, sizeof(cfg.standard_key), "%s",
                 std_key->valuestring);
    }
    cJSON *dyn_key = cJSON_GetObjectItemCaseSensitive(root, "dynamic_key");
    if (cJSON_IsString(dyn_key) && dyn_key->valuestring[0] != '\0') {
        snprintf(cfg.dynamic_key, sizeof(cfg.dynamic_key), "%s",
                 dyn_key->valuestring);
    }

    cJSON *clear_std = cJSON_GetObjectItemCaseSensitive(root, "clear_standard_key");
    if (cJSON_IsTrue(clear_std)) {
        cfg.standard_key[0] = '\0';
    }
    cJSON *clear_dyn = cJSON_GetObjectItemCaseSensitive(root, "clear_dynamic_key");
    if (cJSON_IsTrue(clear_dyn)) {
        cfg.dynamic_key[0] = '\0';
    }

    cJSON_Delete(root);

    /* Light validation: only absolute HTTP(S) URLs make sense. */
    if (cfg.server_url[0] != '\0' &&
        strncmp(cfg.server_url, "http://", 7) != 0 &&
        strncmp(cfg.server_url, "https://", 8) != 0) {
        return send_error(req, "400 Bad Request",
                          "server_url must start with http:// or https://");
    }

    err = app_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save settings failed: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "could not save settings");
    }

    net_client_kick(NET_KICK_ACTION);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    err = send_json(req, response);
    cJSON_Delete(response);
    return err;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *root = NULL;
    esp_err_t err = read_json_body(req, &root);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");

    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "ssid is required");
    }

    if (password != NULL && !cJSON_IsString(password)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "password must be a string");
    }

    err = wifi_manager_save_credentials(ssid->valuestring,
                                        password ? password->valuestring : "");
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save credentials failed: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "could not save credentials");
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    err = send_json(req, response);
    cJSON_Delete(response);

    schedule_reboot();
    return err;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    /* Drain any body so the connection closes cleanly. */
    char scratch[64];
    int remaining = req->content_len;
    while (remaining > 0) {
        int chunk = remaining > (int)sizeof(scratch) ? (int)sizeof(scratch) : remaining;
        int ret = httpd_req_recv(req, scratch, (size_t)chunk);
        if (ret <= 0) {
            break;
        }
        remaining -= ret;
    }

    esp_err_t err = wifi_manager_forget_credentials();
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not forget network");
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    err = send_json(req, response);
    cJSON_Delete(response);

    schedule_reboot();
    return err;
}

static esp_err_t captive_redirect(httpd_req_t *req)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);

    if (!status.ap_active) {
        return httpd_resp_send_404(req);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_server_init(void)
{
    if (s_reboot_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reboot_cb,
        .name = "web_reboot",
    };
    return esp_timer_create(&timer_args, &s_reboot_timer);
}

esp_err_t web_server_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 14;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "http server start");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    const httpd_uri_t settings = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_get,
    };
    const httpd_uri_t settings_save = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_post,
    };
    const httpd_uri_t scan = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = scan_get,
    };
    const httpd_uri_t wifi = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_post,
    };
    const httpd_uri_t reset = {
        .uri = "/api/reset",
        .method = HTTP_POST,
        .handler = reset_post,
    };
    const httpd_uri_t captive = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_redirect,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status), TAG, "status handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &settings), TAG, "settings handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &settings_save), TAG, "settings save handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &scan), TAG, "scan handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &wifi), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &reset), TAG, "reset handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &captive), TAG, "captive handler");

    s_running = true;
    ESP_LOGI(TAG, "settings page ready");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_running || s_server == NULL) {
        return ESP_OK;
    }

    httpd_stop(s_server);
    s_server = NULL;
    s_running = false;
    ESP_LOGI(TAG, "settings page stopped");
    return ESP_OK;
}

bool web_server_running(void)
{
    return s_running;
}
