/*
 * Minimal SSD1681 / GDEY0154D67 driver for the Waveshare
 * ESP32-S3-ePaper-1.54.
 *
 * The initialisation sequence and the waveform LUTs are adapted from
 * Waveshare's official ESP-IDF example for this board (epaper_driver_bsp),
 * which is published in their hardware example repository.
 */

#include "epaper.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

static spi_device_handle_t s_spi;

static const uint8_t k_lut_full[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x08, 0x01, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0x00, 0x32, 0x20,
};

static esp_err_t wait_busy(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (gpio_get_level(BOARD_EPD_PIN_BUSY) == 1) {
        if ((xTaskGetTickCount() - start) > timeout) {
            ESP_LOGE(TAG, "busy timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static esp_err_t send_byte(uint8_t value)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t send_data(uint8_t value)
{
    gpio_set_level(BOARD_EPD_PIN_DC, 1);
    gpio_set_level(BOARD_EPD_PIN_CS, 0);
    esp_err_t err = send_byte(value);
    gpio_set_level(BOARD_EPD_PIN_CS, 1);
    return err;
}

static esp_err_t send_cmd(uint8_t value)
{
    gpio_set_level(BOARD_EPD_PIN_DC, 0);
    gpio_set_level(BOARD_EPD_PIN_CS, 0);
    esp_err_t err = send_byte(value);
    gpio_set_level(BOARD_EPD_PIN_CS, 1);
    return err;
}

static esp_err_t send_buffer(const uint8_t *buffer, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buffer,
    };
    gpio_set_level(BOARD_EPD_PIN_DC, 1);
    gpio_set_level(BOARD_EPD_PIN_CS, 0);
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    gpio_set_level(BOARD_EPD_PIN_CS, 1);
    return err;
}

static esp_err_t set_window(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end)
{
    ESP_RETURN_ON_ERROR(send_cmd(0x44), TAG, "window x cmd");
    ESP_RETURN_ON_ERROR(send_data((x_start >> 3) & 0xFF), TAG, "window x start");
    ESP_RETURN_ON_ERROR(send_data((x_end >> 3) & 0xFF), TAG, "window x end");

    ESP_RETURN_ON_ERROR(send_cmd(0x45), TAG, "window y cmd");
    ESP_RETURN_ON_ERROR(send_data(y_start & 0xFF), TAG, "window y start lo");
    ESP_RETURN_ON_ERROR(send_data((y_start >> 8) & 0xFF), TAG, "window y start hi");
    ESP_RETURN_ON_ERROR(send_data(y_end & 0xFF), TAG, "window y end lo");
    ESP_RETURN_ON_ERROR(send_data((y_end >> 8) & 0xFF), TAG, "window y end hi");
    return ESP_OK;
}

static esp_err_t set_cursor(uint16_t x, uint16_t y)
{
    ESP_RETURN_ON_ERROR(send_cmd(0x4E), TAG, "cursor x cmd");
    ESP_RETURN_ON_ERROR(send_data(x & 0xFF), TAG, "cursor x");

    ESP_RETURN_ON_ERROR(send_cmd(0x4F), TAG, "cursor y cmd");
    ESP_RETURN_ON_ERROR(send_data(y & 0xFF), TAG, "cursor y lo");
    ESP_RETURN_ON_ERROR(send_data((y >> 8) & 0xFF), TAG, "cursor y hi");
    return ESP_OK;
}

static esp_err_t load_lut(const uint8_t *lut)
{
    ESP_RETURN_ON_ERROR(send_cmd(0x32), TAG, "lut cmd");
    ESP_RETURN_ON_ERROR(send_buffer(lut, 153), TAG, "lut data");
    ESP_RETURN_ON_ERROR(wait_busy(5000), TAG, "lut busy");

    ESP_RETURN_ON_ERROR(send_cmd(0x3F), TAG, "lut 3f");
    ESP_RETURN_ON_ERROR(send_data(lut[153]), TAG, "lut 3f data");

    ESP_RETURN_ON_ERROR(send_cmd(0x03), TAG, "lut 03");
    ESP_RETURN_ON_ERROR(send_data(lut[154]), TAG, "lut 03 data");

    ESP_RETURN_ON_ERROR(send_cmd(0x04), TAG, "lut 04");
    ESP_RETURN_ON_ERROR(send_data(lut[155]), TAG, "lut 04 a");
    ESP_RETURN_ON_ERROR(send_data(lut[156]), TAG, "lut 04 b");
    ESP_RETURN_ON_ERROR(send_data(lut[157]), TAG, "lut 04 c");

    ESP_RETURN_ON_ERROR(send_cmd(0x2C), TAG, "lut 2c");
    ESP_RETURN_ON_ERROR(send_data(lut[158]), TAG, "lut 2c data");
    return ESP_OK;
}

static esp_err_t gpio_init(void)
{
    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PIN_RST) |
                        (1ULL << BOARD_EPD_PIN_DC) |
                        (1ULL << BOARD_EPD_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "epd output GPIO config");

    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "epd busy GPIO config");

    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    gpio_set_level(BOARD_EPD_PIN_CS, 1);
    return ESP_OK;
}

static esp_err_t spi_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_EPD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_EPD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_FRAMEBUFFER_SIZE,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi),
                        TAG, "spi device add");
    return ESP_OK;
}

esp_err_t epd_init(void)
{
    board_epd_power(true);

    ESP_RETURN_ON_ERROR(gpio_init(), TAG, "gpio init");
    ESP_RETURN_ON_ERROR(spi_init(), TAG, "spi init");

    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(wait_busy(5000), TAG, "reset busy");

    ESP_RETURN_ON_ERROR(send_cmd(0x12), TAG, "sw reset");
    ESP_RETURN_ON_ERROR(wait_busy(5000), TAG, "sw reset busy");

    ESP_RETURN_ON_ERROR(send_cmd(0x01), TAG, "driver output");
    ESP_RETURN_ON_ERROR(send_data(0xC7), TAG, "driver output a");
    ESP_RETURN_ON_ERROR(send_data(0x00), TAG, "driver output b");
    ESP_RETURN_ON_ERROR(send_data(0x01), TAG, "driver output c");

    ESP_RETURN_ON_ERROR(send_cmd(0x11), TAG, "data entry");
    ESP_RETURN_ON_ERROR(send_data(0x01), TAG, "data entry mode");

    ESP_RETURN_ON_ERROR(set_window(0, EPD_WIDTH - 1, EPD_HEIGHT - 1, 0), TAG, "window");

    ESP_RETURN_ON_ERROR(send_cmd(0x3C), TAG, "border");
    ESP_RETURN_ON_ERROR(send_data(0x01), TAG, "border data");

    ESP_RETURN_ON_ERROR(send_cmd(0x18), TAG, "temp sensor");
    ESP_RETURN_ON_ERROR(send_data(0x80), TAG, "temp sensor data");

    ESP_RETURN_ON_ERROR(send_cmd(0x22), TAG, "load temp");
    ESP_RETURN_ON_ERROR(send_data(0xB1), TAG, "load temp data");
    ESP_RETURN_ON_ERROR(send_cmd(0x20), TAG, "activate temp");
    ESP_RETURN_ON_ERROR(wait_busy(5000), TAG, "temp busy");

    ESP_RETURN_ON_ERROR(set_cursor(0, EPD_HEIGHT - 1), TAG, "cursor");
    ESP_RETURN_ON_ERROR(load_lut(k_lut_full), TAG, "load full lut");

    ESP_LOGI(TAG, "initialised (%dx%d)", EPD_WIDTH, EPD_HEIGHT);
    return ESP_OK;
}

esp_err_t epd_display(const uint8_t *framebuffer, size_t len)
{
    ESP_RETURN_ON_FALSE(framebuffer != NULL, ESP_ERR_INVALID_ARG, TAG, "null framebuffer");
    ESP_RETURN_ON_FALSE(len == EPD_FRAMEBUFFER_SIZE, ESP_ERR_INVALID_SIZE, TAG,
                        "bad framebuffer length %u", (unsigned)len);

    ESP_RETURN_ON_ERROR(send_cmd(0x24), TAG, "write ram");
    ESP_RETURN_ON_ERROR(send_buffer(framebuffer, len), TAG, "write ram data");

    ESP_RETURN_ON_ERROR(send_cmd(0x22), TAG, "display update");
    ESP_RETURN_ON_ERROR(send_data(0xC7), TAG, "display update seq");
    ESP_RETURN_ON_ERROR(send_cmd(0x20), TAG, "activate display");
    ESP_RETURN_ON_ERROR(wait_busy(20000), TAG, "display busy");

    ESP_LOGI(TAG, "frame displayed");
    return ESP_OK;
}
