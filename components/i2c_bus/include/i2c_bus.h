#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * One I2C master bus shared by the FT6336 touch controller and the ES8311
 * codec. Create it once at boot before initialising either device.
 */
esp_err_t i2c_bus_init(void);

/* Returns NULL until i2c_bus_init() has succeeded. */
i2c_master_bus_handle_t i2c_bus_handle(void);
