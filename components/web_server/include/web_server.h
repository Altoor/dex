#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Create the settings HTTP server resources. Does not listen yet. */
esp_err_t web_server_init(void);

/* Start/stop listening. Called by the UI while the QR/settings screen is up. */
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
bool web_server_running(void);
