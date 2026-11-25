#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gfx.h"
#include "qrcodegen.h"
#include "touch.h"
#include "wifi_manager.h"

static const char *TAG = "ui";

#define UI_QUEUE_DEPTH 8
#define UI_TASK_STACK  8192
#define QR_MAX_VERSION 10
#define GEAR_HITBOX    40

typedef enum {
    UI_EVENT_HOME = 0,
    UI_EVENT_TOGGLE,
    UI_EVENT_WIFI_CHANGED,
    UI_EVENT_TAP,
    UI_EVENT_FACTORY_RESET,
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int x;
    int y;
} ui_event_t;

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_SETTINGS,
    SCREEN_RESETTING,
} screen_t;

static QueueHandle_t s_queue;
static screen_t s_screen = SCREEN_HOME;

static uint8_t s_framebuffer[EPD_FRAMEBUFFER_SIZE];
static gfx_canvas_t s_canvas;

static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static uint8_t s_qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

static void post_event(ui_event_type_t type, int x, int y)
{
    if (s_queue == NULL) {
        return;
    }
    ui_event_t event = {
        .type = type,
        .x = x,
        .y = y,
    };
    xQueueSend(s_queue, &event, 0);
}

static void present(void)
{
    esp_err_t err = epd_display(s_framebuffer, sizeof(s_framebuffer));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display refresh failed: %s", esp_err_to_name(err));
    }
}

static void short_text(char *out, size_t out_len, const char *prefix, const char *value)
{
    snprintf(out, out_len, "%s%s", prefix ? prefix : "", value ? value : "");
    if (strlen(out) > 33) {
        out[33] = '\0';
    }
}

static void draw_home(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);

    gfx_draw_gear(&s_canvas, 18, 18, 9, GFX_BLACK);
    gfx_draw_text(&s_canvas, 38, 14, "settings", 1, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 0, 33, BOARD_EPD_WIDTH, 1, GFX_BLACK);

    gfx_draw_text_centered(&s_canvas, 58, "hello", 4, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 92, "world", 4, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 76, 130, 48, 1, GFX_BLACK);

    wifi_status_t status;
    wifi_manager_get_status(&status);
    char line[64];

    if (status.sta_connected) {
        short_text(line, sizeof(line), "WiFi: ", status.ssid);
        gfx_draw_text_centered(&s_canvas, 143, line, 1, GFX_BLACK);
        gfx_draw_text_centered(&s_canvas, 157, status.ip, 1, GFX_BLACK);
    } else if (status.ap_active) {
        gfx_draw_text_centered(&s_canvas, 143, "setup mode", 1, GFX_BLACK);
        short_text(line, sizeof(line), "join ", status.ap_ssid);
        gfx_draw_text_centered(&s_canvas, 157, line, 1, GFX_BLACK);
    } else {
        gfx_draw_text_centered(&s_canvas, 143, "no wifi configured", 1, GFX_BLACK);
        gfx_draw_text_centered(&s_canvas, 157, "press PWR to set up", 1, GFX_BLACK);
    }

    gfx_draw_text_centered(&s_canvas, 179, "PWR or tap gear = settings", 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 190, "hold both 15s = reset", 1, GFX_BLACK);

    present();
}

static int draw_qr(const char *text, int top, int max_px)
{
    bool ok = qrcodegen_encodeText(text, s_qr_temp, s_qr, qrcodegen_Ecc_MEDIUM, 1,
                                   QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
    if (!ok) {
        gfx_draw_text_centered(&s_canvas, top + 20, "QR encode failed", 1, GFX_BLACK);
        return top + 34;
    }

    int size = qrcodegen_getSize(s_qr);
    int modules = size + 8; /* 4-module quiet zone on each side */
    int scale = max_px / modules;
    if (scale < 1) {
        scale = 1;
    }

    int pixels = modules * scale;
    int left = (BOARD_EPD_WIDTH - pixels) / 2;

    gfx_fill_rect(&s_canvas, left, top, pixels, pixels, GFX_WHITE);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(s_qr, x, y)) {
                gfx_fill_rect(&s_canvas, left + (x + 4) * scale, top + (y + 4) * scale,
                              scale, scale, GFX_BLACK);
            }
        }
    }

    return top + pixels;
}

static void draw_settings(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);

    gfx_draw_gear(&s_canvas, 18, 18, 9, GFX_BLACK);
    gfx_draw_text(&s_canvas, 38, 11, "Settings", 2, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 0, 32, BOARD_EPD_WIDTH, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 36, "scan to configure", 1, GFX_BLACK);

    char url[64];
    wifi_manager_get_settings_url(url, sizeof(url));
    int qr_bottom = draw_qr(url, 44, 140);

    wifi_status_t status;
    wifi_manager_get_status(&status);
    char line[64];

    if (status.sta_connected) {
        snprintf(line, sizeof(line), "http://%s", status.ip);
    } else {
        short_text(line, sizeof(line), "join WiFi: ", status.ap_ssid);
    }

    gfx_draw_text_centered(&s_canvas, qr_bottom + 5, line, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, qr_bottom + 16, "tap gear or PWR = close", 1, GFX_BLACK);

    present();
}

static void draw_resetting(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);
    gfx_draw_text_centered(&s_canvas, 68, "Factory reset", 2, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 44, 90, 112, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 102, "erasing all settings", 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 118, "restarting...", 1, GFX_BLACK);
    present();
}

static void open_settings(void)
{
    wifi_status_t status;
    wifi_manager_get_status(&status);

    if (!status.sta_connected) {
        esp_err_t err = wifi_manager_enable_ap();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not enable AP: %s", esp_err_to_name(err));
        }
    }

    s_screen = SCREEN_SETTINGS;
    draw_settings();
}

static void go_home(void)
{
    s_screen = SCREEN_HOME;
    draw_home();
}

static void handle_factory_reset(void)
{
    s_screen = SCREEN_RESETTING;
    draw_resetting();

    /* BOOT held across reset selects the ROM download mode; wait for release. */
    board_wait_buttons_released(10000);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = wifi_manager_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
    }
    esp_restart();
}

static void ui_task(void *arg)
{
    (void)arg;

    for (;;) {
        ui_event_t event;
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UI_EVENT_HOME:
            go_home();
            break;

        case UI_EVENT_TOGGLE:
            if (s_screen == SCREEN_HOME) {
                open_settings();
            } else if (s_screen == SCREEN_SETTINGS) {
                go_home();
            }
            break;

        case UI_EVENT_WIFI_CHANGED:
            if (s_screen == SCREEN_HOME) {
                draw_home();
            } else if (s_screen == SCREEN_SETTINGS) {
                draw_settings();
            }
            break;

        case UI_EVENT_TAP:
            if (event.x < GEAR_HITBOX && event.y < GEAR_HITBOX) {
                if (s_screen == SCREEN_HOME) {
                    open_settings();
                } else if (s_screen == SCREEN_SETTINGS) {
                    go_home();
                }
            }
            break;

        case UI_EVENT_FACTORY_RESET:
            handle_factory_reset();
            break;

        default:
            break;
        }
    }
}

static void on_button(board_button_t button)
{
    if (button == BOARD_BUTTON_BOTH_LONG) {
        post_event(UI_EVENT_FACTORY_RESET, 0, 0);
    } else {
        post_event(UI_EVENT_TOGGLE, 0, 0);
    }
}

static void on_touch(int x, int y)
{
    post_event(UI_EVENT_TAP, x, y);
}

void ui_notify_wifi_changed(void)
{
    post_event(UI_EVENT_WIFI_CHANGED, 0, 0);
}

esp_err_t ui_start(void)
{
    s_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gfx_init(&s_canvas, s_framebuffer, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT);

    if (xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = board_buttons_start(on_button);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "buttons unavailable: %s", esp_err_to_name(err));
    }

    err = touch_start(on_touch);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(err));
    }

    post_event(UI_EVENT_HOME, 0, 0);
    return ESP_OK;
}
