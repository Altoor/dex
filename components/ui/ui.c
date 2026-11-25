#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "battery.h"
#include "board.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gfx.h"
#include "net_client.h"
#include "qrcodegen.h"
#include "recorder.h"
#include "storage.h"
#include "touch.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "ui";

#define UI_QUEUE_DEPTH   16
#define UI_TASK_STACK    8192
#define QR_MAX_VERSION   10
#define GEAR_HITBOX      48
#define DEBOUNCE_US      (3LL * 1000 * 1000)

#define TOP_BAR_H        31
#define CHECK_X          168
#define CHECK_Y          40
#define CHECK_SIZE       24

typedef enum {
    UI_EVENT_REFRESH = 0,
    UI_EVENT_BOOT,
    UI_EVENT_PWR_SINGLE,
    UI_EVENT_PWR_DOUBLE,
    UI_EVENT_PWR_LONG,
    UI_EVENT_FACTORY_RESET,
    UI_EVENT_TAP,
    UI_EVENT_WIFI_CHANGED,
    UI_EVENT_NET_CHANGED,
    UI_EVENT_RECORDER_CHANGED,
    UI_EVENT_DEBOUNCE,
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int x;
    int y;
} ui_event_t;

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_SETTINGS,
    SCREEN_POWERING_OFF,
    SCREEN_RESETTING,
} screen_t;

static QueueHandle_t s_queue;
static screen_t s_screen = SCREEN_HOME;

static uint8_t s_framebuffer[EPD_FRAMEBUFFER_SIZE];
static uint8_t s_last_framebuffer[EPD_FRAMEBUFFER_SIZE];
static gfx_canvas_t s_canvas;
static bool s_have_last;

static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static uint8_t s_qr_temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

static esp_timer_handle_t s_debounce_timer;
static bool s_debounce_active;
static bool s_override_done;
static bool s_patch_inflight;
static char s_patch_todo_id[NET_TODO_ID_MAX];
static bool s_force_next_render;

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

static void present(bool force)
{
    if (!force && s_have_last &&
        memcmp(s_framebuffer, s_last_framebuffer, sizeof(s_framebuffer)) == 0) {
        return;
    }

    esp_err_t err = epd_display(s_framebuffer, sizeof(s_framebuffer));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display refresh failed: %s", esp_err_to_name(err));
        return;
    }

    memcpy(s_last_framebuffer, s_framebuffer, sizeof(s_framebuffer));
    s_have_last = true;
}

/* ------------------------------------------------------------- home ------ */

static void draw_home(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);

    gfx_draw_gear(&s_canvas, 16, 15, 8, GFX_BLACK);
    gfx_draw_text(&s_canvas, 31, 11, "dex", 1, GFX_BLACK);

    recorder_status_t rec;
    recorder_get_status(&rec);

    net_status_t net;
    net_client_get_status(&net);
    bool busy = net_client_is_busy();

    if (busy) {
        gfx_draw_hourglass(&s_canvas, 140, 9, 11, 14, GFX_BLACK);
    }

    battery_status_t battery;
    battery_read(&battery);
    if (battery.usb_present) {
        gfx_draw_battery(&s_canvas, 176, 12, battery.percent, battery.charging,
                         GFX_BLACK);
    }

    gfx_fill_rect(&s_canvas, 0, TOP_BAR_H, BOARD_EPD_WIDTH, 1, GFX_BLACK);

    net_todo_t todo;
    net_client_get_todo(&todo);

    gfx_draw_text(&s_canvas, 8, 38, "NEXT TODO", 1, GFX_BLACK);

    if (todo.valid) {
        gfx_draw_text_wrapped(&s_canvas, 8, 50, 152, todo.name, 2, 4, GFX_BLACK);
    } else if (todo.empty) {
        gfx_draw_text(&s_canvas, 8, 56, "All clear", 2, GFX_BLACK);
        gfx_draw_text(&s_canvas, 8, 80, "Nothing left for today.", 1, GFX_BLACK);
    } else if (net.todo_ready) {
        gfx_draw_text(&s_canvas, 8, 56, "Checking...", 2, GFX_BLACK);
        gfx_draw_text(&s_canvas, 8, 80, "syncing todos", 1, GFX_BLACK);
    } else {
        gfx_draw_text(&s_canvas, 8, 56, "Not configured", 2, GFX_BLACK);
        gfx_draw_text(&s_canvas, 8, 80, "Open settings with", 1, GFX_BLACK);
        gfx_draw_text(&s_canvas, 8, 92, "2x PWR and set the", 1, GFX_BLACK);
        gfx_draw_text(&s_canvas, 8, 104, "server and keys.", 1, GFX_BLACK);
    }

    if (todo.valid) {
        char due[40];
        if (todo.due[0] != '\0') {
            snprintf(due, sizeof(due), "Due %.10s", todo.due);
        } else {
            snprintf(due, sizeof(due), "No due date");
        }
        gfx_draw_text(&s_canvas, 8, 132, due, 1, GFX_BLACK);

        bool checked = todo.done;
        if (s_debounce_active) {
            checked = s_override_done;
        }
        if (s_patch_inflight) {
            checked = true;
        }
        gfx_draw_checkbox(&s_canvas, CHECK_X, CHECK_Y, CHECK_SIZE, checked,
                          GFX_BLACK);
    }

    gfx_fill_rect(&s_canvas, 0, 143, BOARD_EPD_WIDTH, 1, GFX_BLACK);

    char status[48];
    status[0] = '\0';

    if (rec.recording) {
        snprintf(status, sizeof(status), "RECORDING - BOOT to stop");
    } else if (s_debounce_active) {
        snprintf(status, sizeof(status), "done in 3s - tap to undo");
    } else if (s_patch_inflight) {
        snprintf(status, sizeof(status), "updating...");
    } else if (rec.queued > 0 && !net.connected) {
        snprintf(status, sizeof(status), "%u queued - offline",
                 (unsigned)rec.queued);
    } else if (busy) {
        snprintf(status, sizeof(status), "syncing...");
    } else if (net.error[0] != '\0') {
        snprintf(status, sizeof(status), "%.34s", net.error);
    } else if (rec.queued > 0) {
        snprintf(status, sizeof(status), "%u queued", (unsigned)rec.queued);
    } else if (!net.configured) {
        snprintf(status, sizeof(status), "setup needed");
    }

    if (status[0] != '\0') {
        gfx_draw_text_centered(&s_canvas, 148, status, 1, GFX_BLACK);
    }

    gfx_draw_text_centered(&s_canvas, 163,
                           rec.available ? "BOOT: record" : "microphone unavailable",
                           1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 175, "PWR: sync   2x PWR: settings", 1,
                           GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 187, "hold both 15s: reset", 1, GFX_BLACK);
}

/* --------------------------------------------------------- settings ------ */

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
                gfx_fill_rect(&s_canvas, left + (x + 4) * scale,
                              top + (y + 4) * scale, scale, scale, GFX_BLACK);
            }
        }
    }

    return top + pixels;
}

static void draw_settings(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);

    gfx_draw_gear(&s_canvas, 16, 15, 8, GFX_BLACK);
    gfx_draw_text(&s_canvas, 31, 11, "Settings", 1, GFX_BLACK);

    if (net_client_is_busy()) {
        gfx_draw_hourglass(&s_canvas, 140, 9, 11, 14, GFX_BLACK);
    }

    gfx_fill_rect(&s_canvas, 0, TOP_BAR_H, BOARD_EPD_WIDTH, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 35, "scan to configure", 1, GFX_BLACK);

    char url[64];
    wifi_manager_get_settings_url(url, sizeof(url));
    int qr_bottom = draw_qr(url, 44, 112);

    wifi_status_t status;
    wifi_manager_get_status(&status);

    char line[64];
    if (status.sta_connected) {
        snprintf(line, sizeof(line), "http://%s", status.ip);
    } else {
        snprintf(line, sizeof(line), "join %s", status.ap_ssid);
    }
    gfx_draw_text_centered(&s_canvas, qr_bottom + 3, line, 1, GFX_BLACK);

    recorder_status_t rec;
    recorder_get_status(&rec);
    app_result_t result;
    app_config_get_result(&result);

    char footer[64];
    if (result.present && result.status[0] != '\0') {
        snprintf(footer, sizeof(footer), "queued %u - last: %.10s",
                 (unsigned)rec.queued, result.status);
    } else {
        snprintf(footer, sizeof(footer), "queued %u - no calls yet",
                 (unsigned)rec.queued);
    }
    gfx_draw_text_centered(&s_canvas, 171, footer, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 184, "gear / 2x PWR = close", 1, GFX_BLACK);
}

static void draw_powering_off(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);
    gfx_draw_text_centered(&s_canvas, 74, "Power off", 3, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 44, 108, 112, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 118, "press PWR to wake", 1, GFX_BLACK);
}

static void draw_resetting(void)
{
    gfx_clear(&s_canvas, GFX_WHITE);
    gfx_draw_text_centered(&s_canvas, 68, "Factory reset", 2, GFX_BLACK);
    gfx_fill_rect(&s_canvas, 44, 90, 112, 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 102, "erasing all settings", 1, GFX_BLACK);
    gfx_draw_text_centered(&s_canvas, 118, "restarting...", 1, GFX_BLACK);
}

static void render_current(bool force)
{
    switch (s_screen) {
    case SCREEN_HOME:
        draw_home();
        break;
    case SCREEN_SETTINGS:
        draw_settings();
        break;
    case SCREEN_POWERING_OFF:
        draw_powering_off();
        break;
    case SCREEN_RESETTING:
        draw_resetting();
        break;
    default:
        return;
    }

    present(force);
}

/* ------------------------------------------------------------- actions ---- */

static void open_settings(void)
{
    if (s_screen == SCREEN_SETTINGS) {
        return;
    }

    wifi_status_t status;
    wifi_manager_get_status(&status);

    if (!status.sta_connected) {
        esp_err_t err = wifi_manager_enable_ap();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not enable AP: %s", esp_err_to_name(err));
        }
    }

    esp_err_t err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start settings server: %s", esp_err_to_name(err));
    }

    s_screen = SCREEN_SETTINGS;
    render_current(true);
}

static void close_settings(void)
{
    web_server_stop();
    wifi_manager_disable_ap();

    s_screen = SCREEN_HOME;
    render_current(false);
}

static void cancel_debounce(void)
{
    if (s_debounce_active && s_debounce_timer) {
        esp_timer_stop(s_debounce_timer);
    }
    s_debounce_active = false;
    s_override_done = false;
    s_patch_inflight = false;
}

static void handle_boot(void)
{
    recorder_status_t rec;
    recorder_get_status(&rec);

    if (rec.recording) {
        recorder_stop();
        return;
    }

    if (!rec.available) {
        return;
    }

    recorder_start();
}

static void handle_checkmark(void)
{
    net_todo_t todo;
    net_client_get_todo(&todo);

    if (!todo.valid || todo.id[0] == '\0' || s_patch_inflight) {
        return;
    }

    if (s_debounce_active) {
        /* Undo before the PATCH leaves the device. */
        if (s_debounce_timer) {
            esp_timer_stop(s_debounce_timer);
        }
        s_debounce_active = false;
        s_override_done = false;
        render_current(false);
        return;
    }

    snprintf(s_patch_todo_id, sizeof(s_patch_todo_id), "%s", todo.id);
    s_override_done = true;
    s_debounce_active = true;
    if (s_debounce_timer) {
        esp_timer_start_once(s_debounce_timer, DEBOUNCE_US);
    }
    render_current(false);
}

static void handle_tap(int x, int y)
{
    if (x < GEAR_HITBOX && y < GEAR_HITBOX) {
        if (s_screen == SCREEN_HOME) {
            open_settings();
        } else if (s_screen == SCREEN_SETTINGS) {
            close_settings();
        }
        return;
    }

    if (s_screen == SCREEN_HOME && x >= 156 && y >= 34 && y <= 78) {
        handle_checkmark();
    }
}

static void handle_factory_reset(void)
{
    cancel_debounce();
    web_server_stop();
    s_screen = SCREEN_RESETTING;
    render_current(true);

    /* BOOT held across reset selects the ROM download mode; wait for release. */
    board_wait_buttons_released(10000);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = wifi_manager_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
    }
    esp_restart();
}

static void handle_power_off(void)
{
    cancel_debounce();
    web_server_stop();
    s_screen = SCREEN_POWERING_OFF;
    render_current(true);

    vTaskDelay(pdMS_TO_TICKS(800));
    board_power_off();
}

/* --------------------------------------------------------------- task ----- */

static void debounce_cb(void *arg)
{
    (void)arg;
    post_event(UI_EVENT_DEBOUNCE, 0, 0);
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
        case UI_EVENT_REFRESH:
            render_current(s_force_next_render);
            s_force_next_render = false;
            break;

        case UI_EVENT_BOOT:
            if (s_screen == SCREEN_HOME) {
                handle_boot();
            }
            break;

        case UI_EVENT_PWR_SINGLE:
            if (s_screen == SCREEN_SETTINGS) {
                close_settings();
            } else if (s_screen == SCREEN_HOME) {
                recorder_status_t rec;
                recorder_get_status(&rec);
                if (rec.recording) {
                    recorder_cancel();
                } else {
                    /* API first: the net worker notifies when the checks finish. */
                    s_force_next_render = true;
                    net_client_kick(NET_KICK_ACTION);
                }
            }
            break;

        case UI_EVENT_PWR_DOUBLE:
            if (s_screen == SCREEN_HOME) {
                recorder_status_t rec;
                recorder_get_status(&rec);
                if (rec.recording) {
                    recorder_cancel();
                }
                open_settings();
            } else if (s_screen == SCREEN_SETTINGS) {
                close_settings();
            }
            break;

        case UI_EVENT_PWR_LONG:
            handle_power_off();
            break;

        case UI_EVENT_FACTORY_RESET:
            handle_factory_reset();
            break;

        case UI_EVENT_TAP:
            handle_tap(event.x, event.y);
            break;

        case UI_EVENT_WIFI_CHANGED:
            if (s_screen == SCREEN_HOME || s_screen == SCREEN_SETTINGS) {
                render_current(false);
            }
            break;

        case UI_EVENT_NET_CHANGED:
            if (s_patch_inflight) {
                s_patch_inflight = false;
                s_debounce_active = false;
                s_override_done = false;
            }
            if (s_screen == SCREEN_HOME || s_screen == SCREEN_SETTINGS) {
                render_current(s_force_next_render);
                s_force_next_render = false;
            }
            break;

        case UI_EVENT_RECORDER_CHANGED:
            if (s_screen == SCREEN_HOME || s_screen == SCREEN_SETTINGS) {
                render_current(false);
            }
            break;

        case UI_EVENT_DEBOUNCE:
            if (s_debounce_active) {
                s_debounce_active = false;
                s_patch_inflight = true;
                esp_err_t err =
                    net_client_patch_done(s_patch_todo_id, s_override_done);
                if (err != ESP_OK) {
                    /* Network worker not running: drop the optimistic check so
                     * the screen does not lie about the todo state. */
                    ESP_LOGW(TAG, "could not queue todo update: %s",
                             esp_err_to_name(err));
                    s_patch_inflight = false;
                    s_override_done = false;
                }
                render_current(false);
            }
            break;

        default:
            break;
        }
    }
}

/* -------------------------------------------------------------- inputs ---- */

static void on_board_event(board_event_t event)
{
    switch (event) {
    case BOARD_EVENT_BOOT_CLICK:
        post_event(UI_EVENT_BOOT, 0, 0);
        break;
    case BOARD_EVENT_PWR_SINGLE:
        post_event(UI_EVENT_PWR_SINGLE, 0, 0);
        break;
    case BOARD_EVENT_PWR_DOUBLE:
        post_event(UI_EVENT_PWR_DOUBLE, 0, 0);
        break;
    case BOARD_EVENT_PWR_LONG:
        post_event(UI_EVENT_PWR_LONG, 0, 0);
        break;
    case BOARD_EVENT_FACTORY_RESET:
        post_event(UI_EVENT_FACTORY_RESET, 0, 0);
        break;
    default:
        break;
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

void ui_notify_net_changed(void)
{
    post_event(UI_EVENT_NET_CHANGED, 0, 0);
}

void ui_notify_recorder_changed(void)
{
    post_event(UI_EVENT_RECORDER_CHANGED, 0, 0);
}

esp_err_t ui_start(void)
{
    s_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = debounce_cb,
        .name = "ui_debounce",
    };
    if (esp_timer_create(&timer_args, &s_debounce_timer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    gfx_init(&s_canvas, s_framebuffer, BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT);
    memset(s_last_framebuffer, 0, sizeof(s_last_framebuffer));

    if (xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, NULL, 5, NULL, 1) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = board_buttons_start(on_board_event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "buttons unavailable: %s", esp_err_to_name(err));
    }

    err = touch_start(on_touch);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "touch unavailable: %s", esp_err_to_name(err));
    }

    /* First paint happens immediately; the boot-time API check redraws it. */
    post_event(UI_EVENT_REFRESH, 0, 0);
    return ESP_OK;
}
