#include "board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static const gpio_num_t k_button_gpio[] = {
    [0] = BOARD_BUTTON_BOOT_PIN,
    [1] = BOARD_BUTTON_PWR_PIN,
};
static const char *k_button_name[] = {
    [0] = "BOOT",
    [1] = "PWR",
};

static board_event_cb_t s_event_cb;

static void emit(board_event_t event)
{
    if (s_event_cb) {
        s_event_cb(event);
    }
}

void board_epd_power(bool on)
{
    gpio_set_level(BOARD_EPD_PIN_PWR, on ? 0 : 1);
}

void board_audio_power(bool on)
{
    gpio_set_level(BOARD_AUDIO_PIN_PWR, on ? 0 : 1);
}

void board_pa_enable(bool on)
{
    gpio_set_level(BOARD_PA_PIN, on ? 1 : 0);
}

esp_err_t board_init(void)
{
    const gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PIN_PWR) |
                        (1ULL << BOARD_AUDIO_PIN_PWR) |
                        (1ULL << BOARD_VBAT_PIN_PWR) |
                        (1ULL << BOARD_PA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "power GPIO config failed");

    /* Latch battery power first: the boost converter stays on until we let go. */
    gpio_set_level(BOARD_VBAT_PIN_PWR, 1);

    /* E-paper on; audio/PA off until recording. */
    board_epd_power(true);
    board_audio_power(false);
    board_pa_enable(false);

    ESP_LOGI(TAG, "power rails ready (EPD on, battery latched)");
    return ESP_OK;
}

void board_power_off(void)
{
    ESP_LOGW(TAG, "powering off");

    board_pa_enable(false);
    board_audio_power(false);
    board_epd_power(false);
    vTaskDelay(pdMS_TO_TICKS(120));

    gpio_set_level(BOARD_VBAT_PIN_PWR, 0);

    /* On USB the rail stays up, so park in deep sleep and wake on PWR. */
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BOARD_BUTTON_PWR_PIN,
                                      ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

static void button_task(void *arg)
{
    (void)arg;

    bool stable[2] = {false, false};
    TickType_t press_start[2] = {0, 0};

    bool pwr_pending_single = false;
    TickType_t pwr_pending_since = 0;
    bool pwr_second_press = false;
    bool pwr_long_fired = false;

    TickType_t both_start = 0;
    bool reset_fired = false;

    for (;;) {
        for (int i = 0; i < 2; i++) {
            bool raw = gpio_get_level(k_button_gpio[i]) == 0;

            if (raw != stable[i]) {
                vTaskDelay(pdMS_TO_TICKS(20));
                bool confirmed = gpio_get_level(k_button_gpio[i]) == 0;

                if (confirmed != raw) {
                    continue;
                }

                stable[i] = raw;
                TickType_t now = xTaskGetTickCount();

                if (raw) {
                    press_start[i] = now;
                    ESP_LOGI(TAG, "%s down", k_button_name[i]);

                    if (i == 1) {
                        /* A second PWR press inside the window cancels the
                         * still-pending single click. */
                        pwr_second_press = pwr_pending_single;
                        pwr_pending_single = false;
                        pwr_long_fired = false;
                    }
                } else {
                    TickType_t held = now - press_start[i];

                    if (i == 0) {
                        if (!reset_fired && held < pdMS_TO_TICKS(BOARD_CLICK_MAX_MS)) {
                            ESP_LOGI(TAG, "BOOT click");
                            emit(BOARD_EVENT_BOOT_CLICK);
                        }
                    } else {
                        bool was_second = pwr_second_press;
                        pwr_second_press = false;

                        if (reset_fired || pwr_long_fired) {
                            /* already handled */
                        } else if (held >= pdMS_TO_TICKS(BOARD_PWR_LONG_MS)) {
                            emit(BOARD_EVENT_PWR_LONG);
                        } else if (held < pdMS_TO_TICKS(BOARD_CLICK_MAX_MS)) {
                            if (was_second) {
                                ESP_LOGI(TAG, "PWR double click");
                                emit(BOARD_EVENT_PWR_DOUBLE);
                            } else {
                                pwr_pending_single = true;
                                pwr_pending_since = now;
                            }
                        }
                    }
                }
            }
        }

        TickType_t now = xTaskGetTickCount();

        /* Resolve a single PWR click after the double-click window. */
        if (pwr_pending_single &&
            (now - pwr_pending_since) >= pdMS_TO_TICKS(BOARD_DOUBLE_CLICK_MS)) {
            pwr_pending_single = false;
            ESP_LOGI(TAG, "PWR single click");
            emit(BOARD_EVENT_PWR_SINGLE);
        }

        /* Long PWR press fires while the button is still down. Suppress it
         * while BOOT is held too, so the 15 s factory-reset combo still wins. */
        if (stable[1] && !stable[0] && !reset_fired && !pwr_long_fired &&
            (now - press_start[1]) >= pdMS_TO_TICKS(BOARD_PWR_LONG_MS)) {
            pwr_long_fired = true;
            pwr_pending_single = false;
            ESP_LOGI(TAG, "PWR long press");
            emit(BOARD_EVENT_PWR_LONG);
        }

        if (stable[0] && stable[1]) {
            if (both_start == 0) {
                both_start = now;
                ESP_LOGI(TAG, "both buttons held; factory reset in %d s",
                         BOARD_FACTORY_RESET_HOLD_MS / 1000);
            } else if (!reset_fired &&
                       (now - both_start) >=
                           pdMS_TO_TICKS(BOARD_FACTORY_RESET_HOLD_MS)) {
                reset_fired = true;
                pwr_pending_single = false;
                ESP_LOGW(TAG, "factory reset combo triggered");
                emit(BOARD_EVENT_FACTORY_RESET);
            }
        } else {
            both_start = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

esp_err_t board_buttons_start(board_event_cb_t callback)
{
    const gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_BOOT_PIN) |
                        (1ULL << BOARD_BUTTON_PWR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "button GPIO config failed");

    s_event_cb = callback;
    BaseType_t ok = xTaskCreate(button_task, "buttons", 3072, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void board_wait_buttons_released(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while ((gpio_get_level(BOARD_BUTTON_BOOT_PIN) == 0 ||
            gpio_get_level(BOARD_BUTTON_PWR_PIN) == 0) &&
           (xTaskGetTickCount() - start) < timeout) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
