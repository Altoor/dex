#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    int millivolts;   /* pack voltage, VBAT */
    int percent;      /* 0..100 */
    bool usb_present; /* USB data connection detected */
    bool charging;    /* best-effort: USB present */
} battery_status_t;

esp_err_t battery_init(void);
void battery_read(battery_status_t *out);
