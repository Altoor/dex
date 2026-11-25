#include "gfx.h"

#include <math.h>
#include <stdlib.h>
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

void gfx_draw_line(gfx_canvas_t *canvas, int x0, int y0, int x1, int y1,
                   gfx_color_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        gfx_set_pixel(canvas, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int gfx_draw_text_wrapped(gfx_canvas_t *canvas, int x, int y, int max_width,
                          const char *text, int scale, int max_lines,
                          gfx_color_t color)
{
    if (canvas == NULL || text == NULL || scale < 1 || max_lines <= 0 ||
        max_width <= 0) {
        return 0;
    }

    int advance = FONT_ADVANCE * scale;
    int max_chars = max_width / advance;
    if (max_chars < 1) {
        max_chars = 1;
    }

    char line[160];
    const char *p = text;
    int lines = 0;

    while (*p != '\0' && lines < max_lines) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        int len = (int)strlen(p);
        int take = len < max_chars ? len : max_chars;

        if (take < len) {
            int cut = take;
            while (cut > 0 && p[cut - 1] != ' ') {
                cut--;
            }
            if (cut > 0) {
                take = cut - 1;
            }
        }
        if (take <= 0) {
            take = 1;
        }
        if (take > (int)sizeof(line) - 1) {
            take = (int)sizeof(line) - 1;
        }

        memcpy(line, p, (size_t)take);
        line[take] = '\0';
        while (take > 0 && line[take - 1] == ' ') {
            line[--take] = '\0';
        }

        gfx_draw_text(canvas, x, y, line, scale, color);
        y += FONT_HEIGHT * scale + 3 * scale;
        p += take;
        lines++;
    }

    return lines;
}

void gfx_draw_hourglass(gfx_canvas_t *canvas, int x, int y, int w, int h,
                        gfx_color_t color)
{
    if (w < 4 || h < 4) {
        return;
    }

    gfx_fill_rect(canvas, x, y, w, 1, color);
    gfx_fill_rect(canvas, x, y + h - 1, w, 1, color);

    int half = h / 2;
    for (int r = 0; r < half; r++) {
        int width = w - 2 - 2 * r;
        if (width <= 0) {
            break;
        }
        gfx_fill_rect(canvas, x + 1 + r, y + 1 + r, width, 1, color);
        gfx_fill_rect(canvas, x + 1 + r, y + h - 2 - r, width, 1, color);
    }
}

void gfx_draw_battery(gfx_canvas_t *canvas, int x, int y, int percent,
                      bool charging, gfx_color_t color)
{
    const int w = 18;
    const int h = 9;

    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    gfx_draw_rect(canvas, x, y, w, h, color);
    gfx_fill_rect(canvas, x + w, y + 2, 2, 4, color);

    int inner = w - 4;
    int fill = inner * percent / 100;
    if (fill > 0) {
        gfx_fill_rect(canvas, x + 2, y + 2, fill, h - 4, color);
    }

    if (charging) {
        /* Small lightning bolt to the left of the pack. */
        int bx = x - 7;
        gfx_fill_rect(canvas, bx + 3, y, 2, 2, color);
        gfx_fill_rect(canvas, bx + 2, y + 2, 3, 2, color);
        gfx_fill_rect(canvas, bx + 1, y + 4, 2, 2, color);
        gfx_fill_rect(canvas, bx + 0, y + 6, 2, 2, color);
    }
}

void gfx_draw_checkbox(gfx_canvas_t *canvas, int x, int y, int size, bool checked,
                       gfx_color_t color)
{
    gfx_draw_rect(canvas, x, y, size, size, color);
    gfx_draw_rect(canvas, x + 1, y + 1, size - 2, size - 2, color);

    if (!checked) {
        return;
    }

    int x0 = x + size / 4;
    int y0 = y + size / 2;
    int x1 = x + size / 2;
    int y1 = y + 3 * size / 4;
    int x2 = x + 3 * size / 4;
    int y2 = y + size / 4;

    gfx_draw_line(canvas, x0, y0, x1, y1, color);
    gfx_draw_line(canvas, x1, y1, x2, y2, color);
    gfx_draw_line(canvas, x0 + 1, y0, x1 + 1, y1, color);
    gfx_draw_line(canvas, x1, y1 - 1, x2, y2 - 1, color);
}
