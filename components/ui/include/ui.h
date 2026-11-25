#pragma once

#include "esp_err.h"

/*
 * Create the UI queue, start the render task, and start the button and touch
 * input tasks. Call after the display and Wi-Fi manager are initialised.
 */
esp_err_t ui_start(void);

/* Safe to call from the Wi-Fi event task; posts a refresh to the UI queue. */
void ui_notify_wifi_changed(void);
