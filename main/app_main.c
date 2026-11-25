#include "app_config.h"
#include "battery.h"
#include "board.h"
#include "epaper.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "i2c_bus.h"
#include "net_client.h"
#include "nvs_flash.h"
#include "recorder.h"
#include "storage.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "app";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing incompatible NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void on_wifi_event(void)
{
    ui_notify_wifi_changed();
    if (wifi_manager_sta_connected()) {
        net_client_kick(NET_KICK_WIFI);
    }
}

static void on_net_event(void)
{
    ui_notify_net_changed();
}

static void on_recorder_event(void)
{
    ui_notify_recorder_changed();

    recorder_status_t status;
    recorder_get_status(&status);
    if (!status.recording) {
        net_client_kick(NET_KICK_RECORDING);
    }
}

static void configure_power(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "dynamic frequency scaling unavailable: %s",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "dex starting");

    /* Latch battery power before anything else can take time. */
    ESP_ERROR_CHECK(board_init());

    init_nvs();
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(epd_init());

    wifi_manager_set_event_callback(on_wifi_event);
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
    }

    if (recorder_init() != ESP_OK) {
        ESP_LOGE(TAG, "recorder init failed");
    }
    recorder_set_event_callback(on_recorder_event);

    if (net_client_init() != ESP_OK) {
        ESP_LOGE(TAG, "network client init failed");
    }
    net_client_set_event_callback(on_net_event);

    err = web_server_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server init failed: %s", esp_err_to_name(err));
    }

    configure_power();

    ESP_ERROR_CHECK(ui_start());

    /* Boot-time checks: the UI redraws when they finish. */
    net_client_kick(NET_KICK_ACTION);
    if (wifi_manager_sta_connected()) {
        net_client_kick(NET_KICK_WIFI);
    }

    ESP_LOGI(TAG, "ready");
}
