#include "board.h"
#include "epaper.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
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

void app_main(void)
{
    ESP_LOGI(TAG, "dex starting");

    init_nvs();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(epd_init());

    wifi_manager_set_event_callback(ui_notify_wifi_changed);

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
    }

    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(ui_start());

    ESP_LOGI(TAG, "ready");
}
