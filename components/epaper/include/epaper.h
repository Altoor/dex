#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define EPD_WIDTH  200
#define EPD_HEIGHT 200

/* 1 bit per pixel, 1 = white, 0 = black. */
#define EPD_FRAMEBUFFER_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

esp_err_t epd_init(void);

/*
 * Push a complete 1-bpp frame to the panel and perform a full refresh.
 * The buffer must be exactly EPD_FRAMEBUFFER_SIZE bytes.
 */
esp_err_t epd_display(const uint8_t *framebuffer, size_t len);
