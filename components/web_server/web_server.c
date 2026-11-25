#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "wifi_manager.h"

#define DEX_VERSION "0.1.0"
#define MAX_BODY_SIZE 1024

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static esp_timer_handle_t s_reboot_timer;
static httpd_handle_t s_server;

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

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);

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

esp_err_t web_server_start(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = reboot_cb,
        .name = "web_reboot",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reboot_timer), TAG, "reboot timer");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

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
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &scan), TAG, "scan handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &wifi), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &reset), TAG, "reset handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &captive), TAG, "captive handler");

    ESP_LOGI(TAG, "settings page ready");
    return ESP_OK;
}
