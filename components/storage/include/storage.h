#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Optional microSD card on the shared board (1-bit SDMMC). */

#define STORAGE_ROOT "/sdcard"

esp_err_t storage_init(void);

/* False when no card is mounted; all other features keep working. */
bool storage_available(void);

const char *storage_root(void);
