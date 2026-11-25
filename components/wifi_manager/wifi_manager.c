#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE "dex"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASS "wifi_pass"
#define MAX_RETRIES 5
#define AP_DEFAULT_IP "192.168.4.1"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_retry_timer;

static bool s_wifi_started;
static bool s_sta_connected;
static bool s_ap_active;
static bool s_has_credentials;
static bool s_give_up;
static volatile bool s_dns_run;
static volatile int s_dns_sock = -1;
static int s_retry_count;

static char s_ssid[33];
static char s_password[65];
static char s_ip[16];
static char s_ap_ssid[33];

static wifi_event_cb_t s_event_cb;

static void notify(void)
{
    if (s_event_cb) {
        s_event_cb();
    }
}

static void copy_ip(esp_netif_t *netif, char *out, size_t len)
{
    out[0] = '\0';
    if (netif == NULL) {
        return;
    }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        esp_ip4addr_ntoa(&info.ip, out, (int)len);
    }
}

static bool load_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t len = sizeof(s_ssid);
    esp_err_t err = nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &len);
    if (err != ESP_OK || s_ssid[0] == '\0') {
        nvs_close(handle);
        return false;
    }

    len = sizeof(s_password);
    if (nvs_get_str(handle, NVS_KEY_PASS, s_password, &len) != ESP_OK) {
        s_password[0] = '\0';
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "stored credentials for \"%s\"", s_ssid);
    return true;
}

static void retry_cb(void *arg)
{
    (void)arg;

    if (s_give_up) {
        s_give_up = false;
        ESP_LOGW(TAG, "could not join \"%s\"; starting provisioning AP", s_ssid);
        wifi_manager_enable_ap();
        notify();
        return;
    }

    if (s_has_credentials && !s_sta_connected) {
        ESP_LOGI(TAG, "retrying connection to \"%s\"", s_ssid);
        esp_wifi_connect();
    }
}

static void schedule_retry(uint32_t delay_ms)
{
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
    }
}

static void apply_ap_config(void)
{
    wifi_config_t config = {0};
    size_t ssid_len = strlen(s_ap_ssid);
    if (ssid_len > sizeof(config.ap.ssid)) {
        ssid_len = sizeof(config.ap.ssid);
    }

    memcpy(config.ap.ssid, s_ap_ssid, ssid_len);
    config.ap.ssid_len = (uint8_t)ssid_len;
    config.ap.channel = 6;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_has_credentials) {
                ESP_LOGI(TAG, "station started, connecting to \"%s\"", s_ssid);
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "disconnected (reason %d)", event ? event->reason : -1);
            s_sta_connected = false;
            s_ip[0] = '\0';
            notify();

            if (!s_has_credentials) {
                break;
            }

            if (s_retry_count < MAX_RETRIES) {
                s_retry_count++;
                schedule_retry(2000);
            } else {
                s_give_up = true;
                schedule_retry(100);
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            s_ap_active = true;
            notify();
            break;

        case WIFI_EVENT_AP_STOP:
            s_ap_active = false;
            notify();
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        s_give_up = false;
        ESP_LOGI(TAG, "connected to \"%s\", IP %s", s_ssid, s_ip);
        notify();
    }
}

static void dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "captive DNS socket failed");
        s_dns_run = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        close(sock);
        s_dns_run = false;
        vTaskDelete(NULL);
        return;
    }

    s_dns_sock = sock;
    ESP_LOGI(TAG, "captive DNS listening on :53");

    while (s_dns_run) {
        uint8_t query[512];
        uint8_t response[512];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
        if (!s_dns_run) {
            break;
        }
        if (len < 12) {
            continue;
        }

        int pos = 12;
        bool done = false;
        while (pos < len && !done) {
            uint8_t label = query[pos];
            if (label == 0) {
                pos++;
                done = true;
            } else if ((label & 0xC0) == 0xC0) {
                pos += 2;
                done = true;
            } else {
                pos += label + 1;
            }
        }

        if (!done || pos + 4 > len) {
            continue;
        }

        uint16_t qtype = (uint16_t)((query[pos] << 8) | query[pos + 1]);
        int question_end = pos + 4;

        memcpy(response, query, (size_t)question_end);
        response[2] = 0x81; /* QR + RD */
        response[3] = 0x80; /* RA */
        response[6] = 0;
        response[7] = 0;
        response[8] = 0;
        response[9] = 0;
        response[10] = 0;
        response[11] = 0;

        int out_len = question_end;

        if (qtype == 1 && out_len + 16 <= (int)sizeof(response)) {
            response[6] = 0;
            response[7] = 1; /* one answer */
            response[out_len++] = 0xC0;
            response[out_len++] = 0x0C; /* pointer to question name */
            response[out_len++] = 0x00;
            response[out_len++] = 0x01; /* A */
            response[out_len++] = 0x00;
            response[out_len++] = 0x01; /* IN */
            response[out_len++] = 0x00;
            response[out_len++] = 0x00;
            response[out_len++] = 0x00;
            response[out_len++] = 0x3C; /* TTL 60s */
            response[out_len++] = 0x00;
            response[out_len++] = 0x04;
            response[out_len++] = 192;
            response[out_len++] = 168;
            response[out_len++] = 4;
            response[out_len++] = 1;
        }

        sendto(sock, response, (size_t)out_len, 0, (struct sockaddr *)&from, from_len);
    }

    s_dns_sock = -1;
    close(sock);
    ESP_LOGI(TAG, "captive DNS stopped");
    vTaskDelete(NULL);
}

static void start_dns(void)
{
    if (s_dns_run) {
        return;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task, "captive_dns", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start captive DNS task");
        s_dns_run = false;
    }
}

static void stop_dns(void)
{
    if (!s_dns_run) {
        return;
    }
    s_dns_run = false;

    /* Unblock recvfrom() from this task; dns_task closes the descriptor. */
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
    }
}

esp_err_t wifi_manager_enable_ap(void)
{
    if (s_ap_active) {
        return ESP_OK;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "get wifi mode");
    }

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode");
        apply_ap_config();
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");
        s_wifi_started = true;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else if (mode == WIFI_MODE_STA) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "switch to APSTA");
        apply_ap_config();
    } else {
        apply_ap_config();
    }

    s_ap_active = true;
    start_dns();
    ESP_LOGI(TAG, "provisioning AP \"%s\" up at %s", s_ap_ssid, AP_DEFAULT_IP);
    return ESP_OK;
}

esp_err_t wifi_manager_disable_ap(void)
{
    if (!s_ap_active) {
        return ESP_OK;
    }

    stop_dns();

    if (s_wifi_started) {
        if (s_has_credentials) {
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "could not switch back to STA: %s",
                         esp_err_to_name(err));
            }
        } else {
            esp_wifi_stop();
            s_wifi_started = false;
            s_sta_connected = false;
            s_ip[0] = '\0';
            s_retry_count = 0;
        }
    }

    s_ap_active = false;
    ESP_LOGI(TAG, "provisioning AP stopped");
    notify();
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop");
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif && s_ap_netif, ESP_FAIL, TAG, "create netifs");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
                        TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
                        TAG, "ip event handler");

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read MAC");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "dex-%02x%02x", mac[4], mac[5]);

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_retry_timer), TAG, "retry timer");

    s_has_credentials = load_credentials();

    if (s_has_credentials) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi");
        s_wifi_started = true;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        /* Keep Wi-Fi (and its radio) off until the QR/settings screen opens. */
        ESP_LOGI(TAG, "no credentials yet; Wi-Fi idle until setup");
    }

    ESP_LOGI(TAG, "ready (AP SSID %s)", s_ap_ssid);
    return ESP_OK;
}

void wifi_manager_set_event_callback(wifi_event_cb_t callback)
{
    s_event_cb = callback;
}

void wifi_manager_get_status(wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->sta_connected = s_sta_connected;
    status->ap_active = s_ap_active;
    status->has_credentials = s_has_credentials;

    if (s_has_credentials) {
        snprintf(status->ssid, sizeof(status->ssid), "%s", s_ssid);
    }
    if (s_sta_connected) {
        snprintf(status->ip, sizeof(status->ip), "%s", s_ip);
    }

    snprintf(status->ap_ssid, sizeof(status->ap_ssid), "%s", s_ap_ssid);
    copy_ip(s_ap_netif, status->ap_ip, sizeof(status->ap_ip));
    if (status->ap_ip[0] == '\0') {
        snprintf(status->ap_ip, sizeof(status->ap_ip), "%s", AP_DEFAULT_IP);
    }
}

bool wifi_manager_sta_connected(void)
{
    return s_sta_connected;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count)
{
    ESP_RETURN_ON_FALSE(records && count, ESP_ERR_INVALID_ARG, TAG, "invalid scan args");
    *records = NULL;
    *count = 0;

    ESP_RETURN_ON_FALSE(s_wifi_started, ESP_ERR_INVALID_STATE, TAG, "wifi not started");

    const wifi_scan_config_t config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&found), TAG, "scan count");

    if (found == 0) {
        return ESP_OK;
    }
    if (found > 32) {
        found = 32;
    }

    wifi_ap_record_t *list = calloc(found, sizeof(*list));
    ESP_RETURN_ON_FALSE(list != NULL, ESP_ERR_NO_MEM, TAG, "scan alloc");

    uint16_t got = found;
    err = esp_wifi_scan_get_ap_records(&got, list);
    if (err != ESP_OK) {
        free(list);
        return err;
    }

    *records = list;
    *count = got;
    return ESP_OK;
}

void wifi_manager_free_scan(wifi_ap_record_t *records)
{
    free(records);
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "missing ssid");
    size_t ssid_len = strlen(ssid);
    size_t pass_len = password ? strlen(password) : 0;
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= 32, ESP_ERR_INVALID_ARG, TAG,
                        "ssid must be 1..32 bytes");
    ESP_RETURN_ON_FALSE(pass_len <= 64, ESP_ERR_INVALID_ARG, TAG,
                        "password too long");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs open");
    esp_err_t err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_forget_credentials(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs open");

    esp_err_t err = nvs_erase_key(handle, NVS_KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        esp_err_t pass_err = nvs_erase_key(handle, NVS_KEY_PASS);
        if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
            err = pass_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_factory_reset(void)
{
    ESP_LOGW(TAG, "erasing all NVS settings");
    return nvs_flash_erase();
}

esp_err_t wifi_manager_get_settings_url(char *buffer, size_t len)
{
    ESP_RETURN_ON_FALSE(buffer && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid url buffer");

    if (s_sta_connected && s_ip[0] != '\0') {
        snprintf(buffer, len, "http://%s/", s_ip);
        return ESP_OK;
    }

    char ap_ip[16] = AP_DEFAULT_IP;
    copy_ip(s_ap_netif, ap_ip, sizeof(ap_ip));
    if (ap_ip[0] == '\0') {
        snprintf(ap_ip, sizeof(ap_ip), "%s", AP_DEFAULT_IP);
    }

    snprintf(buffer, len, "http://%s/", ap_ip);
    return ESP_OK;
}
