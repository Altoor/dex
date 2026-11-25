#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Tiny 1-bpp canvas.
 * Pixel value 1 = white, 0 = black, matching the e-paper controller RAM.
 */

typedef enum {
    GFX_BLACK = 0,
    GFX_WHITE = 1,
} gfx_color_t;

typedef struct {
    uint8_t *buffer;
    int width;
    int height;
    int stride;
} gfx_canvas_t;

void gfx_init(gfx_canvas_t *canvas, uint8_t *buffer, int width, int height);
void gfx_clear(gfx_canvas_t *canvas, gfx_color_t color);

void gfx_set_pixel(gfx_canvas_t *canvas, int x, int y, gfx_color_t color);
void gfx_fill_rect(gfx_canvas_t *canvas, int x, int y, int w, int h, gfx_color_t color);
void gfx_draw_rect(gfx_canvas_t *canvas, int x, int y, int w, int h, gfx_color_t color);
void gfx_fill_circle(gfx_canvas_t *canvas, int cx, int cy, int radius, gfx_color_t color);
void gfx_draw_gear(gfx_canvas_t *canvas, int cx, int cy, int radius, gfx_color_t color);

int gfx_text_width(const char *text, int scale);
void gfx_draw_text(gfx_canvas_t *canvas, int x, int y, const char *text, int scale,
                   gfx_color_t color);
void gfx_draw_text_centered(gfx_canvas_t *canvas, int y, const char *text, int scale,
                            gfx_color_t color);
