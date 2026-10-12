/* Pincer1 - trusted display: 2.42" 128x64 monochrome OLED, SSD1309 controller, 4-wire SPI, driven by the
 * consent controller only. The daemon has no path to this panel: what it shows is rendered from the
 * controller's own parse of a request (trusted_ui.c), never from host-supplied text.
 *
 * PURE render-to-framebuffer functions over a 1 KB page-addressed framebuffer (8 pages x 128 columns, bit 0
 * of a byte = top row of its page), plus a tiny hardware interface (command / data / reset) that main.c
 * implements over SPI0 and the host tests mock. A NULL hardware interface (PINCER_NO_DISPLAY bench builds)
 * keeps every render working and turns the bus traffic into no-ops; display_present() then says false.
 *
 * Fonts: a 5x7 ASCII face (96 glyphs, 5 column bytes each, embedded below) and an 8x16 face derived from it
 * at render time (columns 1..3 doubled, every row doubled, one blank row above and below) for amounts. Two
 * UTF-8 sequences are rendered as extra glyphs: U+00B7 "·" (middle dot) and U+2026 "…" (ellipsis). */
#ifndef PINCER_DISPLAY_H
#define PINCER_DISPLAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_W 128
#define DISPLAY_H 64
#define DISPLAY_PAGES (DISPLAY_H / 8)
#define DISPLAY_FB_SIZE (DISPLAY_W * DISPLAY_PAGES)
#define DISPLAY_FONT_W 5
#define DISPLAY_FONT_H 7
#define DISPLAY_CELL_W 6  /* 5 px glyph + 1 px gap: 21 cells per line */
#define DISPLAY_COLS (DISPLAY_W / DISPLAY_CELL_W)
#define DISPLAY_BIG_W 8
#define DISPLAY_BIG_H 16
#define DISPLAY_BIG_COLS (DISPLAY_W / DISPLAY_BIG_W)

typedef struct {
    void (*cmd)(const uint8_t *bytes, size_t n);  /* D/C low, chip selected, write */
    void (*data)(const uint8_t *bytes, size_t n); /* D/C high */
    void (*reset)(void);                          /* RST low >= 3 us, high, wait */
} display_hw_t;

typedef struct {
    const display_hw_t *hw; /* NULL = no panel */
    uint8_t fb[DISPLAY_FB_SIZE];
    bool dirty;
} display_t;

/* Resets and initialises the panel (SSD1309 command sequence), clears the framebuffer. `hw` may be NULL. */
void display_init(display_t *d, const display_hw_t *hw);
bool display_present(const display_t *d);
/* Sends the whole framebuffer (column 0..127, page 0..7) when dirty. Returns true when bytes went out. */
bool display_flush(display_t *d);

/* ---- render to framebuffer (pure) ---- */
void display_clear(display_t *d);
void display_pixel(display_t *d, int x, int y, bool on);
bool display_get_pixel(const display_t *d, int x, int y);
void display_hline(display_t *d, int x0, int x1, int y, bool on);
void display_vline(display_t *d, int x, int y0, int y1, bool on);
void display_rect(display_t *d, int x, int y, int w, int h, bool on);        /* 1 px outline */
void display_fill_rect(display_t *d, int x, int y, int w, int h, bool on);
/* 5x7 text at pixel (x, y); `inverted` draws dark glyphs on a lit 6x8 cell. Returns the number of cells drawn.
 * Clips at the right edge. UTF-8 "·" and "…" are one cell each; other non-ASCII bytes draw as '?'. */
int display_text(display_t *d, int x, int y, const char *s, bool inverted);
/* 8x16 text (the scaled face). Returns cells drawn. */
int display_text_big(display_t *d, int x, int y, const char *s);
/* Number of cells `s` occupies (UTF-8 aware for the two extra glyphs). */
size_t display_text_cells(const char *s);
/* Glyph columns for an ASCII code (0x20..0x7E) or the extra glyphs ('·' = DISPLAY_GLYPH_DOT, '…' = DISPLAY_GLYPH_ELLIPSIS). */
#define DISPLAY_GLYPH_DOT 0x7f
#define DISPLAY_GLYPH_ELLIPSIS 0x80
const uint8_t *display_glyph(uint8_t code);
#endif
