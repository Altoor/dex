#pragma once

#include "esp_err.h"

/*
 * Create the UI queue, start the render task, and start the button and touch
 * input tasks. Call after the display, Wi-Fi manager and network client are
 * initialised.
 */
esp_err_t ui_start(void);

/* Safe to call from the Wi-Fi event task; posts a refresh to the UI queue. */
void ui_notify_wifi_changed(void);

/* Safe to call from the network worker. */
void ui_notify_net_changed(void);

/* Safe to call from the recorder task. */
void ui_notify_recorder_changed(void);
