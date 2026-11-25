#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * FT6336 touch panel on the ESP32-S3-Touch-ePaper-1.54.
 * Coordinates are in display pixels (0..199 on both axes).
 */

typedef void (*touch_cb_t)(int x, int y);

/*
 * Initialise the I2C bus and touch controller, then start a polling task.
 * Returns ESP_ERR_NOT_FOUND when no controller answers (non-touch board);
 * callers should treat that as non-fatal.
 */
esp_err_t touch_start(touch_cb_t callback);
