#include "display.h"
#include "app_config.h"
#include "queues.h"
#include "sample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"  // TODO: migrate to driver/i2c_master.h before ESP-IDF v7.0
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// ── SSD1306 constants ─────────────────────────────────────────────────────────
#define OLED_ADDR    APP_OLED_I2C_ADDR
#define I2C_PORT     I2C_NUM_0
#define OLED_W       128
#define OLED_H       64
#define OLED_PAGES   (OLED_H / 8)   // 8 pages of 8 rows each

// Framebuffer: 8 pages × 128 columns, 1 bit per pixel.
// Index: page * 128 + col. Bit position: row % 8 (bit 0 = top of page).
static uint8_t s_fb[OLED_PAGES * OLED_W];

static bool s_blink_on = false;  // toggled by display_task to drive low-battery blink

// ── SSD1306 init sequence ─────────────────────────────────────────────────────
static const uint8_t INIT_SEQ[] = {
    0xAE,        // display off
    0xD5, 0x80,  // clock divide ratio / oscillator freq
    0xA8, 0x3F,  // multiplex ratio = 63 (64 rows)
    0xD3, 0x00,  // display offset = 0
    0x40,        // start line = 0
    0x8D, 0x14,  // charge pump enable (internal VCC)
    0x20, 0x00,  // horizontal addressing mode
    0xA1,        // segment remap (col 127 → SEG0)
    0xC8,        // COM output scan remapped
    0xDA, 0x12,  // COM pins hardware config
    0x81, 0xCF,  // contrast
    0xD9, 0xF1,  // pre-charge period
    0xDB, 0x40,  // VCOMH deselect level
    0xA4,        // output follows RAM
    0xA6,        // normal display (not inverted)
    0xAF,        // display on
};

// Send a command stream (0x00 control byte = all following bytes are commands/args).
static void oled_cmd_seq(const uint8_t *data, size_t len) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    uint8_t ctrl = 0x00;
    i2c_master_write(h, &ctrl, 1, true);
    i2c_master_write(h, (uint8_t *)data, len, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(h);
}

// Send raw data bytes to GDDRAM (0x40 control byte = data stream).
static void oled_data(const uint8_t *data, size_t len) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    uint8_t dc = 0x40;
    i2c_master_write(h, &dc, 1, true);
    i2c_master_write(h, (uint8_t *)data, len, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_PORT, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
}

// Flush entire framebuffer to GDDRAM using horizontal addressing mode.
static void fb_flush(void) {
    const uint8_t col_cmd[]  = {0x21, 0x00, 0x7F};  // columns 0–127
    const uint8_t page_cmd[] = {0x22, 0x00, 0x07};  // pages 0–7
    oled_cmd_seq(col_cmd,  sizeof(col_cmd));
    oled_cmd_seq(page_cmd, sizeof(page_cmd));
    for (int page = 0; page < OLED_PAGES; page++) {
        oled_data(&s_fb[page * OLED_W], OLED_W);
    }
}

// Set or clear a single pixel at (x, y).
static void fb_pixel(int x, int y, uint8_t on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    int idx = (y / 8) * OLED_W + x;
    if (on) s_fb[idx] |=  (1 << (y % 8));
    else    s_fb[idx] &= ~(1 << (y % 8));
}

// Draw a 1-pixel rectangle outline from (x0,y0) to (x1,y1).
static void fb_rect(int x0, int y0, int x1, int y1) {
    for (int x = x0; x <= x1; x++) { fb_pixel(x, y0, 1); fb_pixel(x, y1, 1); }
    for (int y = y0 + 1; y < y1; y++) { fb_pixel(x0, y, 1); fb_pixel(x1, y, 1); }
}

// ── Minimal 5×7 font (ASCII 0x20–0x7E) ──────────────────────────────────────
// Each character is 5 columns of 7-bit rows (LSB = top row).
static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x03,0x04,0x78,0x04,0x03}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x00,0x7F,0x41,0x41}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x41,0x41,0x7F,0x00,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x08,0x14,0x54,0x54,0x3C}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x00,0x7F,0x10,0x28,0x44}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x08,0x04,0x08,0x10,0x08}, // '~'
};

// Draw a single character at pixel position (x, y) with scale factor.
static void fb_char(int x, int y, char c, int scale) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = FONT5X7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_pixel(x + col * scale + sx, y + row * scale + sy, 1);
            }
        }
    }
}

// Draw a null-terminated string.
static void fb_str(int x, int y, const char *s, int scale) {
    while (*s) {
        fb_char(x, y, *s++, scale);
        x += (5 + 1) * scale;  // 5-pixel glyph + 1-pixel gap
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void display_init(void) {
    // I2C driver already installed by sensor_init().
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    oled_cmd_seq(INIT_SEQ, sizeof(INIT_SEQ));
    memset(s_fb, 0x00, sizeof(s_fb));
    fb_flush();
    xSemaphoreGive(i2c_mutex);
}

void display_clear(void) {
    memset(s_fb, 0x00, sizeof(s_fb));
}

void display_show_status(const char *msg) {
    display_clear();
    fb_str(4, 28, msg ? msg : "", 1);
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    fb_flush();
    xSemaphoreGive(i2c_mutex);
}

void display_show_reading(const sample_t *s) {
    if (!s) return;
    display_clear();

    char buf[32];

    // ── Top bar (row 0): time | reading # | battery % ──────────────────────
    // Time HH:MM
    snprintf(buf, sizeof(buf), "%02d:%02d",
             s->datetime.tm_hour, s->datetime.tm_min);
    fb_str(0, 0, buf, 1);

    // Reading number — centred in top bar (only after warmup)
    if (!(s->status & STATUS_SENSOR_WARMUP) && !(s->status & STATUS_CALIBRATING)) {
        snprintf(buf, sizeof(buf), "#%lu", (unsigned long)s->seq);
        int seq_w = (int)strlen(buf) * 6;
        fb_str((OLED_W - seq_w) / 2, 0, buf, 1);
    }

    // Battery % — right-aligned; blink border when below 20%
    uint32_t batt_pct = 0;
    if (s->battery_mv >= 4200) {
        batt_pct = 100;
    } else if (s->battery_mv > 3000) {
        batt_pct = (s->battery_mv - 3000) * 100 / (4200 - 3000);
    }
    snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)batt_pct);
    // Inset 2 px from right and 1 px from top so the blink border has room.
    int batt_x = OLED_W - (int)strlen(buf) * 6 - 2;
    fb_str(batt_x, 2, buf, 1);
    if (batt_pct < 20 && s_blink_on) {
        fb_rect(batt_x - 2, 0, OLED_W - 1, 10);
    }

    // ── Main area ────────────────────────────────────────────────────────────
    if (s->status & STATUS_SENSOR_WARMUP) {
        fb_str((OLED_W - 72) / 2, 18, "WARMUP", 2);
    } else if (s->status & STATUS_CALIBRATING) {
        // "CAL" large (scale=2), step counter "N/10" below, label below that
        fb_str((OLED_W - 36) / 2, 14, "CAL", 2);
        char cal_buf[8];
        snprintf(cal_buf, sizeof(cal_buf), "%lu/%d", (unsigned long)s->seq, SENSOR_CAL_SAMPLES);
        int cal_w = (int)strlen(cal_buf) * 12;  // 6px × scale=2
        fb_str((OLED_W - cal_w) / 2, 30, cal_buf, 2);
        fb_str((OLED_W - 42) / 2, 47, "ZEROING", 1);
    } else {
        // PPM — large (scale=3, 21 px tall), centred, starts at row 14
        snprintf(buf, sizeof(buf), "%.1f", s->ppb);
        int text_w = (int)strlen(buf) * 18;  // 6 * scale=3
        fb_str((OLED_W - text_w) / 2, 14, buf, 3);
        fb_str(54, 38, "ppm", 1);
    }

    // Temperature — centred, row 52
    snprintf(buf, sizeof(buf), "%.1fC", s->temperature);
    fb_str((OLED_W - (int)strlen(buf) * 6) / 2, 52, buf, 1);

    // SD error — bottom-left, row 57
    if (s->status & STATUS_SD_ERROR) {
        fb_str(2, 57, "SD ERR", 1);
    }
    // Charging indicator — bottom-right, row 57
    if (s->status & STATUS_CHARGING) {
        fb_str(OLED_W - 20, 57, "CHG", 1);
    }

    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    fb_flush();
    xSemaphoreGive(i2c_mutex);
}

void display_backlight_on(void)  { /* no backlight GPIO on SSD1306 module */ }
void display_backlight_off(void) { }

void display_sleep(void) {
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    const uint8_t cmd = 0xAE;
    oled_cmd_seq(&cmd, 1);  // display off
    xSemaphoreGive(i2c_mutex);
}

void display_wake(void) {
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    const uint8_t cmd = 0xAF;
    oled_cmd_seq(&cmd, 1);  // display on
    xSemaphoreGive(i2c_mutex);
}

void display_task(void *pvParameter) {
    (void)pvParameter;
    sample_t latest = {};
    bool has_reading = false;
    for (;;) {
        bool got = (q_display && xQueueReceive(q_display, &latest, pdMS_TO_TICKS(APP_DISPLAY_REFRESH_MS)) == pdTRUE);
        if (got) has_reading = true;

        if (has_reading) {
            uint32_t batt_pct = (latest.battery_mv > 3000)
                ? (latest.battery_mv - 3000) * 100 / (4200 - 3000) : 0;
            bool low_batt = (batt_pct < 20);
            if (got || low_batt) {
                // Re-render on new sample, or every 1 s when low to drive blink.
                s_blink_on = !s_blink_on;
                display_show_reading(&latest);
            }
        } else {
            display_show_status("Waiting...");
        }
    }
}
