/*
 * Anykernel OS — Framebuffer Text Console
 *
 * Minimal 8x16 built-in font. 32-bit pixel direct rendering.
 * Scrolling by memory copy when cursor reaches bottom.
 */

#include "console.h"
#include "font8x16.h"
#include "string.h"
#include <stdint.h>

/* ── State ───────────────────────────────────────────────────── */

static uint32_t *fb = 0;
static uint64_t fb_width = 0;
static uint64_t fb_height = 0;
static uint64_t fb_pitch = 0;  /* bytes per row */
static int      fb_ready = 0;

static uint32_t cursor_x = 0;  /* character column */
static uint32_t cursor_y = 0;  /* character row */
static uint32_t cols = 0;      /* max columns */
static uint32_t rows = 0;      /* max rows */

#define FONT_W        8
#define FONT_H        16
#define GLYPH_MSB     0x80   /* MSB of glyph row byte (leftmost pixel) */

/* ── ASCII constants ────────────────────────────────────────── */
#define ASCII_PRINTABLE_FIRST   32   /* Space — first printable char    */
#define ASCII_PRINTABLE_LAST   126   /* '~' — last printable char       */
#define ASCII_PRINTABLE_COUNT   (ASCII_PRINTABLE_LAST - ASCII_PRINTABLE_FIRST + 1)  /* 95 */
#define ASCII_ESC              0x1B  /* Escape character                */
#define TAB_STOP               4     /* Tab stop width in characters    */

/* ── Color palette (ARGB32) ──────────────────────────────────── */
#define COL_BG       0x00101010  /* Near-black background */
#define COL_WHITE    0x00E0E0E0  /* Light gray (default fg)  */
#define COL_RED      0x00FF4444
#define COL_GREEN    0x0044FF44
#define COL_YELLOW   0x00FFFF44
#define COL_BLUE     0x006666FF
#define COL_MAGENTA  0x00FF44FF
#define COL_CYAN     0x0044FFFF

/* Current foreground color (changeable via ANSI codes) */
static uint32_t fg_color = COL_WHITE;

/* ANSI escape sequence state machine */
#define ANSI_STATE_NORMAL  0
#define ANSI_STATE_ESC     1  /* Got ESC */
#define ANSI_STATE_BRACKET 2  /* Got ESC[ */

static int  ansi_state = ANSI_STATE_NORMAL;
static int  ansi_param = 0;

/* ── Built-in 8x16 font (printable ASCII 32-126) ────────────── */
/*    Each glyph = 16 bytes (one byte per row, MSB = leftmost) */

/* ── Internal: draw a glyph at pixel position ────────────────── */

static inline void draw_glyph(uint32_t px, uint32_t py, char c) {
    int idx = (int)c - ASCII_PRINTABLE_FIRST;
    if (idx < 0 || idx >= ASCII_PRINTABLE_COUNT) idx = 0; /* fallback to space */

    const uint8_t *glyph = font_data[idx];
    uint32_t ppitch = fb_pitch / 4;  /* pitch in uint32_t */

    for (int y = 0; y < FONT_H; y++) {
        uint8_t row = glyph[y];
        uint32_t *line = fb + (py + y) * ppitch + px;
        for (int x = 0; x < FONT_W; x++) {
            line[x] = (row & (GLYPH_MSB >> x)) ? fg_color : COL_BG;
        }
    }
}

/* ── Scroll one line up ──────────────────────────────────────── */

static void scroll_up(void) {
    uint32_t line_bytes = fb_pitch * FONT_H;
    uint32_t total = fb_pitch * fb_height;

    /* Move everything up by one text line */
    memmove(fb, (uint8_t *)fb + line_bytes, total - line_bytes);

    /* Clear the last line with exact COL_BG pixels */
    uint32_t ppitch = fb_pitch / 4;
    uint32_t start_y = (rows - 1) * FONT_H;
    for (uint32_t y = start_y; y < start_y + FONT_H; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb[y * ppitch + x] = COL_BG;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void console_init(void *fb_addr, uint64_t width, uint64_t height,
                  uint64_t pitch, uint16_t bpp) {
    if (!fb_addr || bpp != 32) return;

    fb = (uint32_t *)fb_addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    cols = width / FONT_W;
    rows = height / FONT_H;
    cursor_x = 0;
    cursor_y = 0;
    fb_ready = 1;

    console_clear();
}

void console_clear(void) {
    if (!fb_ready) return;
    /* Fill entire framebuffer with background color */
    uint32_t ppitch = fb_pitch / 4;
    for (uint64_t y = 0; y < fb_height; y++) {
        for (uint64_t x = 0; x < fb_width; x++) {
            fb[y * ppitch + x] = COL_BG;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void ansi_set_color(int code) {
    switch (code) {
        case 0:  fg_color = COL_WHITE;   break;  /* Reset */
        case 31: fg_color = COL_RED;     break;
        case 32: fg_color = COL_GREEN;   break;
        case 33: fg_color = COL_YELLOW;  break;
        case 34: fg_color = COL_BLUE;    break;
        case 35: fg_color = COL_MAGENTA; break;
        case 36: fg_color = COL_CYAN;    break;
        case 37: fg_color = COL_WHITE;   break;
        default: break;
    }
}

void console_putchar(char c) {
    if (!fb_ready) return;

    /* ANSI escape sequence parser */
    if (ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            ansi_state = ANSI_STATE_BRACKET;
            ansi_param = 0;
            return;
        }
        ansi_state = ANSI_STATE_NORMAL;
        /* Fall through to print the char */
    } else if (ansi_state == ANSI_STATE_BRACKET) {
        if (c >= '0' && c <= '9') {
            ansi_param = ansi_param * 10 + (c - '0');
            return;
        }
        if (c == 'm') {
            ansi_set_color(ansi_param);
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
        /* Unknown sequence end — ignore */
        ansi_state = ANSI_STATE_NORMAL;
        return;
    }

    if (c == ASCII_ESC) {
        ansi_state = ANSI_STATE_ESC;
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            draw_glyph(cursor_x * FONT_W, cursor_y * FONT_H, ' ');
        }
    } else if (c == '\t') {
        cursor_x = (cursor_x + TAB_STOP) & ~(TAB_STOP - 1);
    } else if (c >= ASCII_PRINTABLE_FIRST && c <= ASCII_PRINTABLE_LAST) {
        draw_glyph(cursor_x * FONT_W, cursor_y * FONT_H, c);
        cursor_x++;
    }

    /* Wrap */
    if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
    }

    /* Scroll */
    if (cursor_y >= rows) {
        scroll_up();
        cursor_y = rows - 1;
    }
}

void console_puts(const char *s) {
    while (*s) console_putchar(*s++);
}

int console_available(void) {
    return fb_ready;
}
