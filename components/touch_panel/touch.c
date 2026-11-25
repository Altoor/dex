#include "touch.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "touch";

#define FT6336_REG_TOUCH_COUNT 0x02
#define FT6336_REG_TOUCH_DATA  0x03
#define I2C_TIMEOUT_MS         1000

static i2c_master_dev_handle_t s_dev;

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static bool read_point(int *x, int *y)
{
    uint8_t count = 0;
    if (read_reg(FT6336_REG_TOUCH_COUNT, &count, 1) != ESP_OK) {
        return false;
    }
    if (count == 0) {
        return false;
    }

    uint8_t data[4] = {0};
    if (read_reg(FT6336_REG_TOUCH_DATA, data, sizeof(data)) != ESP_OK) {
        return false;
    }

    int px = (((int)data[0] & 0x0F) << 8) | data[1];
    int py = (((int)data[2] & 0x0F) << 8) | data[3];

    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    if (px > BOARD_EPD_WIDTH - 1) {
        px = BOARD_EPD_WIDTH - 1;
    }
    if (py > BOARD_EPD_HEIGHT - 1) {
        py = BOARD_EPD_HEIGHT - 1;
    }

    *x = px;
    *y = py;
    return true;
}

static void touch_task(void *arg)
{
    touch_cb_t callback = (touch_cb_t)arg;
    bool was_down = false;

    for (;;) {
        int x = 0;
        int y = 0;
        bool down = read_point(&x, &y);

        if (down && !was_down) {
            ESP_LOGI(TAG, "tap at %d,%d", x, y);
            if (callback) {
                callback(x, y);
            }
        }
        was_down = down;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t touch_start(touch_cb_t callback)
{
    i2c_master_bus_handle_t bus = i2c_bus_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus missing");

    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "touch reset GPIO");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG,
                        "touch device");

    /* Reset the controller. */
    gpio_set_level(BOARD_TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t probe = i2c_master_probe(bus, BOARD_TOUCH_ADDR, 250);
    if (probe != ESP_OK) {
        ESP_LOGW(TAG, "no FT6336 found (non-touch board?): %s", esp_err_to_name(probe));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    if (xTaskCreate(touch_task, "touch", 3072, (void *)callback, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "FT6336 ready");
    return ESP_OK;
}
