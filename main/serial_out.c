/*
 * Frame encoder for the DOOM serial port.
 *
 * Host frames (ESP -> PC):  A5 5A <type> <len:u16le> <payload> <crc16:u16le>
 * Key frames (PC -> ESP):   5A A5 <type> <len:u16le> <payload> <crc16:u16le>
 */

#include "serial_out.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "serial_io.h"

static const char *TAG = "serial_out";

#define FRAME_SYNC0 0xA5
#define FRAME_SYNC1 0x5A

static doom_video_mode_t s_mode = DOOM_VIDEO_ANSI;
static uint8_t s_log_buf[256];
static size_t s_log_len;
static SemaphoreHandle_t s_tx_lock;

/* ANSI encoder state */
static uint16_t *s_cells;      /* low byte = fg xterm index, high byte = bg */
static uint8_t *s_have_prev;
static uint8_t s_xterm_map[256];
static uint8_t *s_ansi_buf;
static size_t s_ansi_buf_size;

/* Raw encoder state */
static uint8_t *s_raw_buf;

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 512) {
            chunk = 512;
        }
        int n = serial_io_write(data + off, chunk, portMAX_DELAY);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void *alloc_psram(size_t size, bool zero)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL && zero) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void serial_out_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    assert(s_tx_lock != NULL);

    const int cols = CONFIG_DOOM_ANSI_COLS;
    const int rows = CONFIG_DOOM_ANSI_ROWS;
    s_cells = alloc_psram((size_t)cols * rows * 2 * sizeof(uint16_t), true);
    s_have_prev = alloc_psram((size_t)cols * rows, true);
    s_ansi_buf_size = (size_t)cols * rows * 64 + 1024;
    s_ansi_buf = alloc_psram(s_ansi_buf_size, false);

    const int rw = CONFIG_DOOM_RAW_WIDTH;
    const int rh = CONFIG_DOOM_RAW_HEIGHT;
    s_raw_buf = alloc_psram((size_t)rw * rh * 2 + 16, false);

    if (!s_cells || !s_have_prev || !s_ansi_buf || !s_raw_buf) {
        ESP_LOGE(TAG, "failed to allocate video encoder buffers from PSRAM");
        abort();
    }
    memset(s_xterm_map, 0, sizeof(s_xterm_map));
}

doom_video_mode_t serial_out_mode(void)
{
    return s_mode;
}

void serial_out_set_mode(doom_video_mode_t mode)
{
    s_mode = mode;
    memset(s_have_prev, 0, (size_t)CONFIG_DOOM_ANSI_COLS * CONFIG_DOOM_ANSI_ROWS);
}

void serial_out_frame(serial_frame_type_t type, const uint8_t *payload, size_t len)
{
    if (len > 0xffff) {
        ESP_LOGW(TAG, "frame too large (%u)", (unsigned)len);
        return;
    }

    uint8_t header[5];
    header[0] = FRAME_SYNC0;
    header[1] = FRAME_SYNC1;
    header[2] = (uint8_t)type;
    header[3] = (uint8_t)(len & 0xff);
    header[4] = (uint8_t)(len >> 8);

    uint16_t crc = crc16_ccitt(&header[2], 3, 0xffff);
    crc = crc16_ccitt(payload, len, crc);
    uint8_t trailer[2] = {
        (uint8_t)(crc & 0xff),
        (uint8_t)(crc >> 8),
    };

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    write_all(header, sizeof(header));
    if (len) {
        write_all(payload, len);
    }
    write_all(trailer, sizeof(trailer));
    xSemaphoreGive(s_tx_lock);
}

void serial_out_hello(void)
{
    static const uint8_t hello[] = {
        'D', 'O', 'O', 'M', 'S', '3',
        CONFIG_DOOM_ANSI_COLS & 0xff, CONFIG_DOOM_ANSI_COLS >> 8,
        CONFIG_DOOM_ANSI_ROWS & 0xff, CONFIG_DOOM_ANSI_ROWS >> 8,
        CONFIG_DOOM_RAW_WIDTH & 0xff, CONFIG_DOOM_RAW_WIDTH >> 8,
        CONFIG_DOOM_RAW_HEIGHT & 0xff, CONFIG_DOOM_RAW_HEIGHT >> 8,
    };
    serial_out_frame(FRAME_HELLO, hello, sizeof(hello));
}

void serial_out_log(const char *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        s_log_buf[s_log_len++] = (uint8_t)data[i];
        if (s_log_len == sizeof(s_log_buf) || data[i] == '\n') {
            serial_out_frame(FRAME_LOG, s_log_buf, s_log_len);
            s_log_len = 0;
        }
    }
}

/* xterm-256 colour cube + grey ramp */
static void xterm_rgb(int idx, int *r, int *g, int *b)
{
    if (idx < 16) {
        static const int base[16][3] = {
            {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
            {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
            {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
            {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
        };
        *r = base[idx][0];
        *g = base[idx][1];
        *b = base[idx][2];
    } else if (idx < 232) {
        int n = idx - 16;
        int levels[6] = {0, 95, 135, 175, 215, 255};
        *r = levels[n / 36];
        *g = levels[(n / 6) % 6];
        *b = levels[n % 6];
    } else {
        int v = 8 + (idx - 232) * 10;
        *r = *g = *b = v;
    }
}

static uint8_t nearest_xterm(int r, int g, int b)
{
    int best = 0;
    long best_dist = 0x7fffffffL;
    for (int i = 0; i < 256; ++i) {
        int tr, tg, tb;
        xterm_rgb(i, &tr, &tg, &tb);
        /* Weighted distance: green matters most to the human eye. */
        long dr = r - tr, dg = g - tg, db = b - tb;
        long dist = dr * dr * 2 + dg * dg * 3 + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
        if (dist == 0) {
            break;
        }
    }
    return (uint8_t)best;
}

static void build_palette_map(const uint8_t *rgb768)
{
    for (int i = 0; i < 256; ++i) {
        s_xterm_map[i] = nearest_xterm(rgb768[i * 3], rgb768[i * 3 + 1],
                                       rgb768[i * 3 + 2]);
    }
}

void serial_out_palette(const uint8_t *rgb768)
{
    build_palette_map(rgb768);
    serial_out_frame(FRAME_PALETTE, rgb768, 768);
    memset(s_have_prev, 0, (size_t)CONFIG_DOOM_ANSI_COLS * CONFIG_DOOM_ANSI_ROWS);
}

static size_t append_str(uint8_t *out, size_t pos, const char *s)
{
    while (*s) {
        out[pos++] = (uint8_t)*s++;
    }
    return pos;
}

static size_t append_uint(uint8_t *out, size_t pos, unsigned v)
{
    uint8_t tmp[12];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v) {
        tmp[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    }
    while (n) {
        out[pos++] = tmp[--n];
    }
    return pos;
}

void serial_out_ansi(const uint8_t *fb, int src_w, int src_h, const uint8_t *rgb768)
{
    (void)rgb768;
    const int cols = CONFIG_DOOM_ANSI_COLS;
    const int rows = CONFIG_DOOM_ANSI_ROWS;
    const size_t total = (size_t)cols * rows;

    /* Sample the 320x200 indexed framebuffer into half-block cells. */
    for (int cy = 0; cy < rows; ++cy) {
        int y0 = cy * src_h / rows;
        int y1 = (cy + 1) * src_h / rows - 1;
        if (y1 >= src_h) {
            y1 = src_h - 1;
        }
        for (int cx = 0; cx < cols; ++cx) {
            int x = cx * src_w / cols;
            uint8_t top = fb[y0 * src_w + x];
            uint8_t bot = fb[y1 * src_w + x];
            s_cells[cy * cols + cx] = (uint16_t)s_xterm_map[top] |
                                      ((uint16_t)s_xterm_map[bot] << 8);
        }
    }

    uint8_t *out = s_ansi_buf;
    size_t pos = 0;

    /* Emit only changed cells, grouping consecutive runs in the same row. */
    for (int cy = 0; cy < rows; ++cy) {
        int cx = 0;
        while (cx < cols) {
            size_t idx = (size_t)cy * cols + cx;
            uint16_t cell = s_cells[idx];
            /* s_have_prev is used as a "previous value valid" byte array below;
             * actual previous values live in the second half of s_cells. */
            uint16_t prev = s_cells[total + idx];
            uint8_t valid = s_have_prev[idx];
            if (valid && cell == prev) {
                ++cx;
                continue;
            }

            /* Position the cursor at the start of the changed run. */
            pos = append_str(out, pos, "\x1b[");
            pos = append_uint(out, pos, (unsigned)(cy + 1));
            pos = append_str(out, pos, ";");
            pos = append_uint(out, pos, (unsigned)(cx + 1));
            pos = append_str(out, pos, "H");

            int run_fg = -1, run_bg = -1;
            while (cx < cols) {
                idx = (size_t)cy * cols + cx;
                cell = s_cells[idx];
                prev = s_cells[total + idx];
                valid = s_have_prev[idx];
                if (valid && cell == prev) {
                    break;
                }
                int fg = cell & 0xff;
                int bg = (cell >> 8) & 0xff;
                if (fg != run_fg || bg != run_bg) {
                    pos = append_str(out, pos, "\x1b[38;5;");
                    pos = append_uint(out, pos, (unsigned)fg);
                    pos = append_str(out, pos, "m\x1b[48;5;");
                    pos = append_uint(out, pos, (unsigned)bg);
                    pos = append_str(out, pos, "m");
                    run_fg = fg;
                    run_bg = bg;
                }
                /* U+2580 UPPER HALF BLOCK */
                out[pos++] = 0xE2;
                out[pos++] = 0x96;
                out[pos++] = 0x80;
                ++cx;
            }
        }
    }

    /* Save current cells as previous, and remember that we have a previous frame. */
    memcpy(s_cells + total, s_cells, total * sizeof(uint16_t));
    memset(s_have_prev, 1, total);

    if (pos > 0 && pos <= s_ansi_buf_size) {
        /* A full-screen ANSI update can exceed one 64 KiB frame payload.
         * Split it; every chunk contains absolute cursor moves, so the host
         * can simply write the payloads in order. */
        size_t off = 0;
        while (off < pos) {
            size_t chunk = pos - off;
            if (chunk > 60000) {
                chunk = 60000;
            }
            serial_out_frame(FRAME_ANSI, out + off, chunk);
            off += chunk;
        }
    } else if (pos > s_ansi_buf_size) {
        ESP_LOGW(TAG, "ANSI buffer overflow (%u > %u)",
                 (unsigned)pos, (unsigned)s_ansi_buf_size);
    }
}

void serial_out_raw(const uint8_t *fb, int src_w, int src_h)
{
    const int w = CONFIG_DOOM_RAW_WIDTH;
    const int h = CONFIG_DOOM_RAW_HEIGHT;
    uint8_t *out = s_raw_buf;
    size_t pos = 0;

    out[pos++] = (uint8_t)(w & 0xff);
    out[pos++] = (uint8_t)(w >> 8);
    out[pos++] = (uint8_t)(h & 0xff);
    out[pos++] = (uint8_t)(h >> 8);

    for (int y = 0; y < h; ++y) {
        int sy = y * src_h / h;
        int run = 1;
        uint8_t val = fb[sy * src_w];
        for (int x = 1; x < w; ++x) {
            int sx = x * src_w / w;
            uint8_t v = fb[sy * src_w + sx];
            if (v == val && run < 255) {
                ++run;
            } else {
                out[pos++] = (uint8_t)run;
                out[pos++] = val;
                run = 1;
                val = v;
            }
        }
        out[pos++] = (uint8_t)run;
        out[pos++] = val;
    }

    serial_out_frame(FRAME_RAW, out, pos);
}
