#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Waveshare ESP32-S3-Touch-ePaper-1.54 (V1 and V2) hardware definitions. */

#define BOARD_EPD_WIDTH  200
#define BOARD_EPD_HEIGHT 200

#define BOARD_EPD_PIN_PWR   6
#define BOARD_EPD_PIN_BUSY  8
#define BOARD_EPD_PIN_RST   9
#define BOARD_EPD_PIN_DC    10
#define BOARD_EPD_PIN_CS    11
#define BOARD_EPD_PIN_SCK   12
#define BOARD_EPD_PIN_MOSI  13

#define BOARD_AUDIO_PIN_PWR 42 /* active low */
#define BOARD_PA_PIN        46
#define BOARD_VBAT_PIN_PWR  17 /* high keeps the battery rail latched */

#define BOARD_BUTTON_BOOT_PIN 0
#define BOARD_BUTTON_PWR_PIN  18

/* FT6336 capacitive touch (Touch-ePaper variant). */
#define BOARD_TOUCH_I2C_SDA 47
#define BOARD_TOUCH_I2C_SCL 48
#define BOARD_TOUCH_RST_PIN 7
#define BOARD_TOUCH_INT_PIN 21
#define BOARD_TOUCH_ADDR    0x38

/* ES8311 codec, on the shared I2C bus (7-bit address). */
#define BOARD_CODEC_I2C_ADDR 0x18

/* I2S microphone path. */
#define BOARD_I2S_MCLK 14
#define BOARD_I2S_BCLK 15
#define BOARD_I2S_WS   38
#define BOARD_I2S_DIN  16
#define BOARD_I2S_DOUT 45

/* microSD, 1-bit SDMMC. */
#define BOARD_SD_CLK 39
#define BOARD_SD_CMD 41
#define BOARD_SD_D0  40

/* Battery measurement (200K/200K divider). */
#define BOARD_BATTERY_ADC_PIN 4

typedef enum {
    BOARD_EVENT_BOOT_CLICK = 0,
    BOARD_EVENT_PWR_SINGLE,
    BOARD_EVENT_PWR_DOUBLE,
    BOARD_EVENT_PWR_LONG,
    /* Both buttons held together for BOARD_FACTORY_RESET_HOLD_MS. */
    BOARD_EVENT_FACTORY_RESET,
} board_event_t;

#define BOARD_FACTORY_RESET_HOLD_MS 15000
#define BOARD_PWR_LONG_MS           3000
#define BOARD_DOUBLE_CLICK_MS       350
#define BOARD_CLICK_MAX_MS          1000

typedef void (*board_event_cb_t)(board_event_t event);

/* Configure power rails and button GPIOs. Raises the battery power latch. */
esp_err_t board_init(void);

/* E-paper rail is active low on this board. */
void board_epd_power(bool on);

/* Audio codec / amplifier rail is active low on this board. */
void board_audio_power(bool on);
void board_pa_enable(bool on);

/* Start the debounced button poll task. Callback runs in that task's context. */
esp_err_t board_buttons_start(board_event_cb_t callback);

/*
 * Lower the battery power latch and enter deep sleep. On battery this cuts
 * power; on USB the chip parks in deep sleep until PWR is pressed.
 */
void board_power_off(void);

/*
 * Block until both side buttons are released, or the timeout expires.
 * Used before rebooting so a held BOOT button does not enter download mode.
 */
void board_wait_buttons_released(uint32_t timeout_ms);
