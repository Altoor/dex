#include "gfx.h"

#include <math.h>
#include <string.h>

#include "font5x7.h"

#define FONT_WIDTH  5
#define FONT_HEIGHT 7
#define FONT_ADVANCE (FONT_WIDTH + 1)

void gfx_init(gfx_canvas_t *canvas, uint8_t *buffer, int width, int height)
{
    canvas->buffer = buffer;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = (width + 7) / 8;
}

void gfx_set_pixel(gfx_canvas_t *canvas, int x, int y, gfx_color_t color)
{
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) {
        return;
    }

    uint8_t *byte = &canvas->buffer[y * canvas->stride + (x >> 3)];
    uint8_t mask = 1u << (7 - (x & 7));
    if (color == GFX_WHITE) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

void gfx_clear(gfx_canvas_t *canvas, gfx_color_t color)
{
    memset(canvas->buffer, color == GFX_WHITE ? 0xFF : 0x00,
           (size_t)(canvas->stride * canvas->height));
}

void gfx_fill_rect(gfx_canvas_t *canvas, int x, int y, int w, int h, gfx_color_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;

    if (x1 > canvas->width) {
        x1 = canvas->width;
    }
    if (y1 > canvas->height) {
        y1 = canvas->height;
    }

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            gfx_set_pixel(canvas, px, py, color);
        }
    }
}

void gfx_draw_rect(gfx_canvas_t *canvas, int x, int y, int w, int h, gfx_color_t color)
{
    gfx_fill_rect(canvas, x, y, w, 1, color);
    gfx_fill_rect(canvas, x, y + h - 1, w, 1, color);
    gfx_fill_rect(canvas, x, y, 1, h, color);
    gfx_fill_rect(canvas, x + w - 1, y, 1, h, color);
}

void gfx_fill_circle(gfx_canvas_t *canvas, int cx, int cy, int radius, gfx_color_t color)
{
    if (radius <= 0) {
        return;
    }

    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= r2) {
                gfx_set_pixel(canvas, cx + dx, cy + dy, color);
            }
        }
    }
}

void gfx_draw_gear(gfx_canvas_t *canvas, int cx, int cy, int radius, gfx_color_t color)
{
    const int teeth = 8;
    const float pi2 = 6.28318530718f;
    gfx_color_t hole = color == GFX_BLACK ? GFX_WHITE : GFX_BLACK;

    for (int i = 0; i < teeth; i++) {
        float angle = (pi2 * (float)i) / (float)teeth;
        int tx = cx + (int)lroundf(cosf(angle) * (float)radius * 0.98f);
        int ty = cy + (int)lroundf(sinf(angle) * (float)radius * 0.98f);
        gfx_fill_circle(canvas, tx, ty, radius / 3, color);
    }

    gfx_fill_circle(canvas, cx, cy, (radius * 4) / 5, color);
    gfx_fill_circle(canvas, cx, cy, (radius * 2) / 5, hole);
}

int gfx_text_width(const char *text, int scale)
{
    if (text == NULL || text[0] == '\0' || scale < 1) {
        return 0;
    }
    return (int)strlen(text) * FONT_ADVANCE * scale - scale;
}

void gfx_draw_text(gfx_canvas_t *canvas, int x, int y, const char *text, int scale,
                   gfx_color_t color)
{
    if (text == NULL || scale < 1) {
        return;
    }

    int cursor = x;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        unsigned char ch = *p;
        if (ch < 32 || ch > 126) {
            ch = '?';
        }

        for (int col = 0; col < FONT_WIDTH; col++) {
            uint8_t bits = font[ch * FONT_WIDTH + col];
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (bits & (1u << row)) {
                    gfx_fill_rect(canvas, cursor + col * scale, y + row * scale,
                                  scale, scale, color);
                }
            }
        }
        cursor += FONT_ADVANCE * scale;
    }
}

void gfx_draw_text_centered(gfx_canvas_t *canvas, int y, const char *text, int scale,
                            gfx_color_t color)
{
    int width = gfx_text_width(text, scale);
    gfx_draw_text(canvas, (canvas->width - width) / 2, y, text, scale, color);
}
