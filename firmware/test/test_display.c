/* SSD1309 driver: init sequence and flush over a mock SPI, framebuffer primitives, both fonts, UTF-8 glyphs. */
#include "display.h"

#include "harness.h"

/* mock bus: records every command / data byte in order */
static uint8_t g_cmd[256], g_data[4096];
static size_t g_ncmd, g_ndata, g_resets;
static void mock_cmd(const uint8_t *b, size_t n) { if (g_ncmd + n <= sizeof g_cmd) memcpy(g_cmd + g_ncmd, b, n); g_ncmd += n; }
static void mock_data(const uint8_t *b, size_t n) { if (g_ndata + n <= sizeof g_data) memcpy(g_data + g_ndata, b, n); g_ndata += n; }
static void mock_reset(void) { g_resets++; }
static const display_hw_t HW = {mock_cmd, mock_data, mock_reset};

static void mock_clear(void) { g_ncmd = g_ndata = g_resets = 0; memset(g_cmd, 0, sizeof g_cmd); memset(g_data, 0, sizeof g_data); }

static void test_init_sequence(void)
{
    display_t d;
    mock_clear();
    display_init(&d, &HW);
    CHECK(display_present(&d));
    CHECK_EQ(g_resets, 1);
    CHECK(g_ncmd > 20);
    CHECK_EQ(g_cmd[0], 0xAE);              /* off first */
    CHECK_EQ(g_cmd[g_ncmd - 1], 0xAF);     /* on last */
    CHECK_EQ(g_cmd[3], 0xA8); CHECK_EQ(g_cmd[4], 0x3F); /* 64 lines */
    CHECK_EQ(g_cmd[8], 0x20); CHECK_EQ(g_cmd[9], 0x00); /* horizontal addressing */
    {
        size_t i; bool pump = false;
        for (i = 0; i < g_ncmd; i++) if (g_cmd[i] == 0x8D) pump = true;
        CHECK(!pump); /* no SSD1306 charge-pump command for an SSD1309 */
    }
    CHECK_EQ(g_ndata, 0);
    /* no panel: everything still works, nothing goes out */
    display_init(&d, NULL);
    CHECK(!display_present(&d));
    display_text(&d, 0, 0, "x", false);
    CHECK(!display_flush(&d));
    CHECK_EQ(g_resets, 1);
}

static void test_framebuffer_and_flush(void)
{
    display_t d;
    size_t i;
    mock_clear();
    display_init(&d, &HW);
    mock_clear();
    display_clear(&d);
    display_pixel(&d, 0, 0, true);
    display_pixel(&d, 127, 63, true);
    display_pixel(&d, 5, 9, true);
    display_pixel(&d, 128, 0, true); display_pixel(&d, 0, 64, true); display_pixel(&d, -1, 0, true); /* clipped */
    CHECK(display_get_pixel(&d, 0, 0));
    CHECK(display_get_pixel(&d, 127, 63));
    CHECK(display_get_pixel(&d, 5, 9));
    CHECK(!display_get_pixel(&d, 5, 8));
    CHECK(!display_get_pixel(&d, 128, 0));
    CHECK_EQ(d.fb[0], 0x01);
    CHECK_EQ(d.fb[7 * 128 + 127], 0x80);
    CHECK_EQ(d.fb[1 * 128 + 5], 0x02);
    display_pixel(&d, 0, 0, false);
    CHECK_EQ(d.fb[0], 0x00);
    /* flush: set window then the whole 1024-byte framebuffer, exactly once until something changes */
    CHECK(display_flush(&d));
    CHECK_EQ(g_ncmd, 6);
    CHECK_EQ(g_cmd[0], 0x21); CHECK_EQ(g_cmd[1], 0); CHECK_EQ(g_cmd[2], 127);
    CHECK_EQ(g_cmd[3], 0x22); CHECK_EQ(g_cmd[4], 0); CHECK_EQ(g_cmd[5], 7);
    CHECK_EQ(g_ndata, DISPLAY_FB_SIZE);
    CHECK_MEM(g_data, d.fb, DISPLAY_FB_SIZE);
    CHECK(!display_flush(&d)); /* not dirty */
    display_hline(&d, 0, 127, 20, true);
    CHECK(display_flush(&d));
    for (i = 0; i < 128; i++) CHECK(display_get_pixel(&d, (int)i, 20));
    display_rect(&d, 10, 30, 20, 10, true);
    CHECK(display_get_pixel(&d, 10, 30) && display_get_pixel(&d, 29, 39) && display_get_pixel(&d, 10, 39) && display_get_pixel(&d, 29, 30));
    CHECK(!display_get_pixel(&d, 15, 35));
    display_fill_rect(&d, 40, 40, 4, 4, true);
    CHECK(display_get_pixel(&d, 41, 41) && display_get_pixel(&d, 43, 43) && !display_get_pixel(&d, 44, 44));
}

/* Column-major glyph check: 'I' is a full centre column, 'T' a full centre column + a top bar. */
static void test_font_5x7(void)
{
    display_t d;
    const uint8_t *g;
    int x, y;
    display_init(&d, NULL);
    g = display_glyph('I');
    CHECK_EQ(g[2], 0x7F);
    CHECK_EQ(g[0], 0x00);
    g = display_glyph('T');
    CHECK_EQ(g[0], 0x01); CHECK_EQ(g[2], 0x7F);
    CHECK_EQ(display_glyph('A')[0], 0x7E);
    CHECK_EQ(display_glyph('0')[0], 0x3E);
    CHECK_MEM(display_glyph(0x19), display_glyph('?'), 5); /* control char -> '?' */
    CHECK_MEM(display_glyph(0xff), display_glyph('?'), 5);
    /* render "I" at (6, 8): pixels at x=8, y=8..14 lit, nothing at y=15 or x=6 */
    CHECK_EQ(display_text(&d, 6, 8, "I", false), 1);
    for (y = 8; y < 15; y++) CHECK(display_get_pixel(&d, 8, y));
    CHECK(!display_get_pixel(&d, 8, 15));
    CHECK(!display_get_pixel(&d, 6, 10));
    /* inverted: the 6x8 cell is lit except the glyph */
    display_clear(&d);
    display_text(&d, 0, 0, "I", true);
    CHECK(!display_get_pixel(&d, 2, 3)); /* glyph column dark */
    CHECK(display_get_pixel(&d, 0, 3));  /* background lit */
    CHECK(display_get_pixel(&d, 5, 7));  /* gap column and row 7 lit */
    CHECK(!display_get_pixel(&d, 6, 3)); /* next cell untouched */
    /* 21 cells per line, clipped at the right edge */
    display_clear(&d);
    CHECK_EQ(display_text(&d, 0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", false), 21);
    CHECK_EQ(display_text(&d, 120, 0, "AB", false), 1);
    /* UTF-8 middle dot and ellipsis are one cell each; other non-ASCII draws '?' */
    CHECK_EQ(display_text_cells("a\xC2\xB7" "b"), 3);
    CHECK_EQ(display_text_cells("\xE2\x80\xA6"), 1);
    CHECK_EQ(display_text_cells("\xC3\xA9x"), 2);
    display_clear(&d);
    display_text(&d, 0, 0, "\xC2\xB7", false);
    CHECK(display_get_pixel(&d, 2, 3) && display_get_pixel(&d, 2, 4) && !display_get_pixel(&d, 2, 2) && !display_get_pixel(&d, 1, 3));
    display_clear(&d);
    display_text(&d, 0, 0, "\xE2\x80\xA6", false);
    CHECK(display_get_pixel(&d, 0, 6) && display_get_pixel(&d, 2, 6) && display_get_pixel(&d, 4, 6) && !display_get_pixel(&d, 1, 6) && !display_get_pixel(&d, 0, 5));
    /* every printable glyph fits 5 columns x 7 rows (bit 7 never set) */
    for (x = 0x20; x <= 0x7E; x++) {
        const uint8_t *gl = display_glyph((uint8_t)x);
        for (y = 0; y < 5; y++) CHECK((gl[y] & 0x80) == 0);
    }
}

/* The 8x16 face is the 5x7 face with columns 1..3 doubled and rows doubled, one blank row above and below. */
static void test_font_8x16(void)
{
    display_t d;
    int y;
    display_init(&d, NULL);
    CHECK_EQ(display_text_big(&d, 0, 0, "I"), 1);
    /* source column 2 (full) lands on x=3 and x=4; rows 0..6 -> y 1..14 */
    for (y = 1; y <= 14; y++) { CHECK(display_get_pixel(&d, 3, y)); CHECK(display_get_pixel(&d, 4, y)); }
    CHECK(!display_get_pixel(&d, 3, 0)); CHECK(!display_get_pixel(&d, 3, 15));
    CHECK(!display_get_pixel(&d, 0, 5)); CHECK(!display_get_pixel(&d, 7, 5)); /* columns 0 and 4 of 'I' are empty */
    display_clear(&d);
    display_text_big(&d, 0, 0, "T");
    CHECK(display_get_pixel(&d, 0, 1) && display_get_pixel(&d, 7, 1) && display_get_pixel(&d, 0, 2)); /* top bar, two rows thick */
    CHECK(!display_get_pixel(&d, 0, 3));
    CHECK_EQ(display_text_big(&d, 0, 16, "0123456789ABCDEFGHIJ"), 16); /* 16 cells per line */
    CHECK_EQ(display_text_big(&d, 124, 32, "1"), 0);
}

int main(void)
{
    RUN(test_init_sequence);
    RUN(test_framebuffer_and_flush);
    RUN(test_font_5x7);
    RUN(test_font_8x16);
    TEST_END();
}
