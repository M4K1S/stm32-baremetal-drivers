#include "ssd1306.h"

uint8_t OLED_FB[OLED_FB_SIZE] = {0};

uint8_t ssd1306_write_command(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t cmd) {
    return I2C_WriteBytes(port, tim_port, SSD1306_ADDR, SSD1306_CMD_MODE, &cmd, 1);
}

uint8_t ssd1306_write_data(I2C_TypeDef *port, TIM_TypeDef *tim_port, uint8_t *data, uint8_t len) {
    return I2C_WriteBytes(port, tim_port, SSD1306_ADDR, SSD1306_DATA_MODE, data, len);
}

uint8_t ssd1306_init(I2C_TypeDef *port, TIM_TypeDef *tim_port) {
    uint8_t ok = 1;

    ok &= ssd1306_write_command(port, tim_port, 0x8D); // enable charge pump
    ok &= ssd1306_write_command(port, tim_port, 0x14);

    ok &= ssd1306_write_command(port, tim_port, 0xA8); // set MUX ratio
    ok &= ssd1306_write_command(port, tim_port, 0x3F);

    ok &= ssd1306_write_command(port, tim_port, 0xD3); // set display offset
    ok &= ssd1306_write_command(port, tim_port, 0x00);

    ok &= ssd1306_write_command(port, tim_port, 0x40); // set display start line

    ok &= ssd1306_write_command(port, tim_port, 0xA1); // set segment re-map

    ok &= ssd1306_write_command(port, tim_port, 0xC8); // set COM output scan direction

    ok &= ssd1306_write_command(port, tim_port, 0x81); // set contrast control
    ok &= ssd1306_write_command(port, tim_port, 0x7F);

    ok &= ssd1306_write_command(port, tim_port, 0xA4); // disable entire display on

    ok &= ssd1306_write_command(port, tim_port, 0xA6); // set normal display

    ok &= ssd1306_write_command(port, tim_port, 0xD5); // set oscillator frequency
    ok &= ssd1306_write_command(port, tim_port, 0x80);

    ok &= ssd1306_write_command(port, tim_port, 0xDA); // set COM pins hardware config
    ok &= ssd1306_write_command(port, tim_port, 0x12);

    ok &= ssd1306_write_command(port, tim_port, 0x20); // memory addressing mode
    ok &= ssd1306_write_command(port, tim_port, 0x00); // horizontal addressing

    ok &= ssd1306_write_command(port, tim_port, 0xAF); // display on

    return ok;
}

void oled_set_pixel(int x, int y, uint8_t state) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return; // bounds check

    int page = y / 8; // Each page 8px tall
    int byte_index = x + (page * OLED_WIDTH); // Calculates the specific byte
    uint8_t bit_mask = (1 << (y % 8)); // Mask pixel

    if (state) {
        OLED_FB[byte_index] |= bit_mask;
    } else {
        OLED_FB[byte_index] &= ~bit_mask;
    }
}

uint8_t oled_get_pixel(int x, int y) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return 0; // bounds check

    int page = y / 8; // Each page 8px tall
    int byte_index = x + (page * OLED_WIDTH); // Calculates the specific byte
    uint8_t bit_mask = (1 << (y % 8)); // Mask pixel

    return (OLED_FB[byte_index] & bit_mask) != 0;
}

void oled_clear(void) {
    for(int i = 0; i < OLED_FB_SIZE; i++) {
        OLED_FB[i] = 0;
    }
}

uint8_t oled_update(I2C_TypeDef *port, TIM_TypeDef *tim_port) {
    uint8_t ok = 1;

    ok &= ssd1306_write_command(port, tim_port, 0x21); // set column address range
    ok &= ssd1306_write_command(port, tim_port, 0);            // start column
    ok &= ssd1306_write_command(port, tim_port, OLED_WIDTH - 1); // end column

    ok &= ssd1306_write_command(port, tim_port, 0x22); // set page address range
    ok &= ssd1306_write_command(port, tim_port, 0);    // start page
    ok &= ssd1306_write_command(port, tim_port, 7);    // end page


    // Send in chunks of 128 byte each (uint8_t's range of 0-255)
    for (uint8_t chunk = 0; chunk < 8; chunk++) {
        uint8_t *chunk_data = &OLED_FB[chunk * OLED_WIDTH];
        ok &= ssd1306_write_data(port, tim_port, chunk_data, OLED_WIDTH);
    }

    return ok;
}

#define ICON_W 14
#define ICON_H 11
#define SMALL_GLYPH_W 8
#define SMALL_GLYPH_H 11
#define SMALL_CHAR_SPACING 7

// Icons are stored page-major (one byte per column per 8-row page), same
// layout as OLED_FB, so each bit maps directly to a pixel.
void oled_draw_icon(int x, int y, const uint8_t *icon) {
    uint8_t pages = (ICON_H + 7) / 8;
    for (uint8_t page = 0; page < pages; page++) {
        for (uint8_t col = 0; col < ICON_W; col++) {
            uint8_t byte = icon[col + page * ICON_W];
            for (uint8_t bit = 0; bit < 8; bit++) {
                int row = page * 8 + bit;
                if (row >= ICON_H) break;
                if ((byte >> bit) & 0x1) oled_set_pixel(x + col, y + row, 1);
            }
        }
    }
}

// Small digits/colon are stored row-major, LSB-first (bit 0 = leftmost
// column), one byte per row.
static void oled_draw_small_glyph(int x, int y, const uint8_t *glyph) {
    for (int row = 0; row < SMALL_GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < SMALL_GLYPH_W; col++) {
            if ((bits >> col) & 0x1) oled_set_pixel(x + col, y + row, 1);
        }
    }
}

void oled_draw_small_time(int x, int y, uint8_t hh, uint8_t mm) {
    oled_draw_small_glyph(x, y, OLED_SmallDigits[hh / 10]); x += SMALL_CHAR_SPACING;
    oled_draw_small_glyph(x, y, OLED_SmallDigits[hh % 10]); x += SMALL_CHAR_SPACING;
    oled_draw_small_glyph(x, y, OLED_SmallColon);           x += SMALL_CHAR_SPACING;
    oled_draw_small_glyph(x, y, OLED_SmallDigits[mm / 10]); x += SMALL_CHAR_SPACING;
    oled_draw_small_glyph(x, y, OLED_SmallDigits[mm % 10]);
}

// NOTE: OLED_BigDigits/OLED_BigC/OLED_BigMinus/OLED_BigDegree in oled_fonts.c
// are declared as 24x36 but only ever have pixels set in an 8-wide strip -
// they're not actually drawn as big glyphs yet. Rather than render that
// half-finished art, the big temp reading below is built by scaling the
// small 8x11 digit font up (giving a legible enlarged digit) and drawing the
// sign/point/degree marks procedurally. Swap this out once real big glyph
// bitmaps exist. Scale is 2x (not 3x) so a sign + 3 integer digits + point +
// 2 fraction digits + degree mark all still fit across the 128px panel.
#define BIG_SCALE 2
#define BIG_DIGIT_W (SMALL_GLYPH_W * BIG_SCALE)
#define BIG_DIGIT_H (SMALL_GLYPH_H * BIG_SCALE)
#define BIG_GAP 2
#define BIG_SIGN_W (BIG_DIGIT_W / 2)
#define BIG_POINT_SIZE 3
#define DEGREE_SIZE 5

static void oled_draw_big_digit(int x, int y, uint8_t digit) {
    const uint8_t *glyph = OLED_SmallDigits[digit];
    for (int row = 0; row < SMALL_GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < SMALL_GLYPH_W; col++) {
            if (!((bits >> col) & 0x1)) continue;
            for (int sy = 0; sy < BIG_SCALE; sy++) {
                for (int sx = 0; sx < BIG_SCALE; sx++) {
                    oled_set_pixel(x + col * BIG_SCALE + sx, y + row * BIG_SCALE + sy, 1);
                }
            }
        }
    }
}

static void oled_draw_big_minus(int x, int y) {
    int bar_y = y + BIG_DIGIT_H / 2;
    for (int dy = 0; dy < BIG_SCALE; dy++) {
        for (int dx = 0; dx < BIG_SIGN_W; dx++) {
            oled_set_pixel(x + dx, bar_y + dy, 1);
        }
    }
}

// Small filled square sitting on the baseline, next to the fraction digits.
static void oled_draw_big_point(int x, int y) {
    for (int dy = 0; dy < BIG_POINT_SIZE; dy++) {
        for (int dx = 0; dx < BIG_POINT_SIZE; dx++) {
            oled_set_pixel(x + dx, y + BIG_DIGIT_H - BIG_POINT_SIZE + dy, 1);
        }
    }
}

// Small square ring next to the number - reads as a degree mark without
// needing a dedicated glyph bitmap.
static void oled_draw_degree(int x, int y) {
    for (int i = 0; i < DEGREE_SIZE; i++) {
        oled_set_pixel(x + i, y, 1);
        oled_set_pixel(x + i, y + DEGREE_SIZE - 1, 1);
        oled_set_pixel(x, y + i, 1);
        oled_set_pixel(x + DEGREE_SIZE - 1, y + i, 1);
    }
}

// Draws the full status screen into the framebuffer (does not push it to the
// panel - call oled_update() afterwards). Icons are only drawn when their
// "ready" flag is set, so e.g. a disconnected wifi/absent SD card just shows
// nothing rather than a stale icon. temp_centi is the temperature in
// hundredths of a degree (e.g. -1234 -> "-12.34").
void oled_draw_status_screen(uint8_t wifi_connected, uint8_t sd_ready,
                              uint8_t have_refresh, uint8_t refresh_hh, uint8_t refresh_mm,
                              uint8_t have_temp, int32_t temp_centi) {
    oled_clear();

    if (wifi_connected) oled_draw_icon(0, 0, OLED_Icon_Wifi);
    if (sd_ready)        oled_draw_icon(ICON_W + 2, 0, OLED_Icon_SD);

    oled_draw_icon(OLED_WIDTH - ICON_W, 0, OLED_Icon_Refresh);
    if (have_refresh) {
        int time_w = 5 * SMALL_CHAR_SPACING;
        oled_draw_small_time(OLED_WIDTH - ICON_W - 2 - time_w, 0, refresh_hh, refresh_mm);
    }

    if (have_temp) {
        uint8_t neg = temp_centi < 0;
        int32_t mag = neg ? -temp_centi : temp_centi;
        int32_t whole = mag / 100;
        int32_t frac = mag % 100;

        uint8_t int_digits[3];
        uint8_t n_int = 0;
        if (whole >= 100) int_digits[n_int++] = (uint8_t)((whole / 100) % 10);
        if (whole >= 10)  int_digits[n_int++] = (uint8_t)((whole / 10) % 10);
        int_digits[n_int++] = (uint8_t)(whole % 10);

        uint8_t frac_digits[2] = { (uint8_t)(frac / 10), (uint8_t)(frac % 10) };

        // Elements drawn left to right: [sign] digit* point digit digit degree,
        // with BIG_GAP between every pair of them.
        uint8_t n_elements = (neg ? 1 : 0) + n_int + 1 + 2 + 1;
        int total_w = (neg ? BIG_SIGN_W : 0) + n_int * BIG_DIGIT_W + BIG_POINT_SIZE
                    + 2 * BIG_DIGIT_W + DEGREE_SIZE + BIG_GAP * (n_elements - 1);

        int x = (OLED_WIDTH - total_w) / 2;
        int y = (OLED_HEIGHT - BIG_DIGIT_H) / 2;

        if (neg) {
            oled_draw_big_minus(x, y);
            x += BIG_SIGN_W + BIG_GAP;
        }
        for (uint8_t i = 0; i < n_int; i++) {
            oled_draw_big_digit(x, y, int_digits[i]);
            x += BIG_DIGIT_W + BIG_GAP;
        }
        oled_draw_big_point(x, y);
        x += BIG_POINT_SIZE + BIG_GAP;
        for (uint8_t i = 0; i < 2; i++) {
            oled_draw_big_digit(x, y, frac_digits[i]);
            x += BIG_DIGIT_W + BIG_GAP;
        }
        oled_draw_degree(x, y);
    }
}
