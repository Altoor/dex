#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    bool sta_connected;
    bool ap_active;
    bool has_credentials;
    char ssid[33];
    char ip[16];
    char ap_ssid[33];
    char ap_ip[16];
} wifi_status_t;

typedef void (*wifi_event_cb_t)(void);

esp_err_t wifi_manager_init(void);

/* Called from the Wi-Fi event task; must not block. */
void wifi_manager_set_event_callback(wifi_event_cb_t callback);

void wifi_manager_get_status(wifi_status_t *status);
bool wifi_manager_sta_connected(void);

/* Bring up the provisioning SoftAP if it is not already running. */
esp_err_t wifi_manager_enable_ap(void);

/*
 * Tear down the SoftAP and captive DNS (called when the QR/settings screen
 * closes). With credentials the station stays up; without credentials Wi-Fi is
 * stopped completely to save power.
 */
esp_err_t wifi_manager_disable_ap(void);

/*
 * Blocking scan. On success returns an allocated array that the caller must
 * release with wifi_manager_free_scan().
 */
esp_err_t wifi_manager_scan(wifi_ap_record_t **records, uint16_t *count);
void wifi_manager_free_scan(wifi_ap_record_t *records);

/* Persist credentials to NVS. The caller is responsible for rebooting. */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_forget_credentials(void);

/* Erase the whole NVS partition (all device settings). Reboot afterwards. */
esp_err_t wifi_manager_factory_reset(void);

/* Best URL for the settings page: STA IP if connected, otherwise SoftAP IP. */
esp_err_t wifi_manager_get_settings_url(char *buffer, size_t len);
