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

/* Never split an ANSI escape sequence or a UTF-8 block character. */
#define ANSI_CHUNK_LIMIT 60000
#define ANSI_CELL_MAX 64
#define CURSOR_PREFIX_MAX 16

static doom_video_mode_t s_mode = DOOM_VIDEO_ANSI;
static uint8_t s_log_buf[256];
static size_t s_log_len;
static SemaphoreHandle_t s_tx_lock;

/* ANSI encoder state */
static uint16_t *s_cells;      /* [0..total) current, [total..2*total) previous */
static uint8_t *s_have_prev;
static uint8_t s_xterm_map[256];
static uint8_t s_palette_rgb[256][3];
static uint8_t *s_palette_blend; /* [a * 256 + b] nearest palette to average(a,b) */
static uint8_t *s_ansi_buf;
static size_t s_ansi_buf_size;
static uint8_t *s_cursor_buf;  /* CURSOR_PREFIX_MAX bytes per cell */
static uint8_t *s_cursor_len;
static int *s_cell_x0;
static int *s_cell_x1;
static int *s_cell_y0;
static int *s_cell_y1;
static int *s_cell_split;
static int s_emit_fg = -1;
static int s_emit_bg = -1;

/* Raw encoder state */
static uint8_t *s_raw_buf;
static int *s_raw_x0;
static int *s_raw_x1;
static int *s_raw_y0;
static int *s_raw_y1;

/* Source dimensions used to build the sampling maps. */
static int s_map_src_w;
static int s_map_src_h;

/* CRC lookup table: same CCITT-16 as before, just much faster. */
static uint16_t s_crc_table[256];
static bool s_crc_ready;

static void crc_table_init(void)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
        s_crc_table[i] = crc;
    }
    s_crc_ready = true;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc)
{
    if (!s_crc_ready) {
        crc_table_init();
    }
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)((crc << 8) ^
                         s_crc_table[((crc >> 8) ^ data[i]) & 0xff]);
    }
    return crc;
}

static int write_all(const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 2048) {
            chunk = 2048;
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
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        int n = idx - 16;
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
        long dr = r - tr, dg = g - tg, db = b - tb;
        /* Weighted distance: green matters most to the human eye. */
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

static int nearest_level(int v)
{
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    int best = 0;
    int best_dist = 0x7fffffff;
    for (int i = 0; i < 6; ++i) {
        int d = v > levels[i] ? v - levels[i] : levels[i] - v;
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

/* Fast inverse mapping for averaged cell colours. */
static uint8_t xterm_from_rgb(int r, int g, int b)
{
    int max = r > g ? r : g;
    if (b > max) {
        max = b;
    }
    int min = r < g ? r : g;
    if (b < min) {
        min = b;
    }

    if (max <= 8) {
        return 0;
    }
    if (max - min <= 12 && max >= 8 && max <= 238) {
        int v = (r + g + b) / 3;
        int idx = (v - 8 + 5) / 10;
        if (idx < 0) {
            idx = 0;
        } else if (idx > 23) {
            idx = 23;
        }
        return (uint8_t)(232 + idx);
    }
    if (min >= 240) {
        return 231;
    }
    return (uint8_t)(16 + 36 * nearest_level(r) +
                     6 * nearest_level(g) + nearest_level(b));
}

static uint8_t nearest_palette(int r, int g, int b)
{
    int best = 0;
    long best_dist = 0x7fffffffL;
    for (int i = 0; i < 256; ++i) {
        long dr = r - s_palette_rgb[i][0];
        long dg = g - s_palette_rgb[i][1];
        long db = b - s_palette_rgb[i][2];
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
        int r = rgb768[i * 3 + 0];
        int g = rgb768[i * 3 + 1];
        int b = rgb768[i * 3 + 2];
        s_palette_rgb[i][0] = (uint8_t)r;
        s_palette_rgb[i][1] = (uint8_t)g;
        s_palette_rgb[i][2] = (uint8_t)b;
        s_xterm_map[i] = nearest_xterm(r, g, b);
    }

    /*
     * For RAW output one output pixel covers several source pixels.  This
     * table gives the closest palette colour to the average of any pair, so
     * downscaling blends colours instead of picking a single source pixel.
     */
    for (int a = 0; a < 256; ++a) {
        for (int b = a; b < 256; ++b) {
            uint8_t idx;
            if (a == b) {
                idx = (uint8_t)a;
            } else {
                int r = (s_palette_rgb[a][0] + s_palette_rgb[b][0] + 1) >> 1;
                int g = (s_palette_rgb[a][1] + s_palette_rgb[b][1] + 1) >> 1;
                int bl = (s_palette_rgb[a][2] + s_palette_rgb[b][2] + 1) >> 1;
                idx = nearest_palette(r, g, bl);
            }
            s_palette_blend[(unsigned)a * 256 + b] = idx;
            s_palette_blend[(unsigned)b * 256 + a] = idx;
        }
    }
}

static void build_output_maps(int src_w, int src_h)
{
    if (src_w <= 0 || src_h <= 0) {
        return;
    }
    if (s_map_src_w == src_w && s_map_src_h == src_h) {
        return;
    }

    const int cols = CONFIG_DOOM_ANSI_COLS;
    const int rows = CONFIG_DOOM_ANSI_ROWS;
    const int rw = CONFIG_DOOM_RAW_WIDTH;
    const int rh = CONFIG_DOOM_RAW_HEIGHT;

    /* Half-block cells: [x0, x1) horizontally, [y0, split)/[split, y1) vertically. */
    for (int cx = 0; cx <= cols; ++cx) {
        int x0 = cx * src_w / cols;
        int x1 = (cx + 1) * src_w / cols;
        if (x1 <= x0) {
            x1 = x0 + 1;
        }
        if (x1 > src_w) {
            x1 = src_w;
        }
        s_cell_x0[cx] = x0;
        s_cell_x1[cx] = x1;
    }
    for (int cy = 0; cy <= rows; ++cy) {
        int y0 = cy * src_h / rows;
        int y1 = (cy + 1) * src_h / rows;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > src_h) {
            y1 = src_h;
        }
        int split = y0 + (y1 - y0) / 2;
        if (split <= y0) {
            split = y0 + 1;
        }
        if (split >= y1) {
            split = y1 - 1;
        }
        s_cell_y0[cy] = y0;
        s_cell_y1[cy] = y1;
        s_cell_split[cy] = split;
    }

    /* RAW output pixel regions. */
    for (int x = 0; x < rw; ++x) {
        int x0 = x * src_w / rw;
        int x1 = (x + 1) * src_w / rw;
        if (x1 <= x0) {
            x1 = x0 + 1;
        }
        if (x1 > src_w) {
            x1 = src_w;
        }
        s_raw_x0[x] = x0;
        s_raw_x1[x] = x1;
    }
    for (int y = 0; y < rh; ++y) {
        int y0 = y * src_h / rh;
        int y1 = (y + 1) * src_h / rh;
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > src_h) {
            y1 = src_h;
        }
        s_raw_y0[y] = y0;
        s_raw_y1[y] = y1;
    }

    s_map_src_w = src_w;
    s_map_src_h = src_h;

    const size_t total = (size_t)cols * rows;
    memset(s_have_prev, 0, total);
    s_emit_fg = -1;
    s_emit_bg = -1;
}

void serial_out_init(void)
{
    crc_table_init();

    s_tx_lock = xSemaphoreCreateMutex();
    assert(s_tx_lock != NULL);

    const int cols = CONFIG_DOOM_ANSI_COLS;
    const int rows = CONFIG_DOOM_ANSI_ROWS;
    const size_t total = (size_t)cols * rows;

    s_cells = alloc_psram(total * 2 * sizeof(uint16_t), true);
    s_have_prev = alloc_psram(total, true);
    s_ansi_buf_size = total * ANSI_CELL_MAX + 1024;
    s_ansi_buf = alloc_psram(s_ansi_buf_size, false);
    s_cursor_buf = alloc_psram(total * CURSOR_PREFIX_MAX, false);
    s_cursor_len = alloc_psram(total, false);

    s_cell_x0 = alloc_psram((size_t)(cols + 1) * sizeof(int), false);
    s_cell_x1 = alloc_psram((size_t)(cols + 1) * sizeof(int), false);
    s_cell_y0 = alloc_psram((size_t)(rows + 1) * sizeof(int), false);
    s_cell_y1 = alloc_psram((size_t)(rows + 1) * sizeof(int), false);
    s_cell_split = alloc_psram((size_t)(rows + 1) * sizeof(int), false);

    s_palette_blend = alloc_psram(256u * 256u, true);

    const int rw = CONFIG_DOOM_RAW_WIDTH;
    const int rh = CONFIG_DOOM_RAW_HEIGHT;
    s_raw_buf = alloc_psram((size_t)rw * rh * 2 + 16, false);
    s_raw_x0 = alloc_psram((size_t)rw * sizeof(int), false);
    s_raw_x1 = alloc_psram((size_t)rw * sizeof(int), false);
    s_raw_y0 = alloc_psram((size_t)rh * sizeof(int), false);
    s_raw_y1 = alloc_psram((size_t)rh * sizeof(int), false);

    if (!s_cells || !s_have_prev || !s_ansi_buf || !s_cursor_buf ||
        !s_cursor_len || !s_cell_x0 || !s_cell_x1 || !s_cell_y0 ||
        !s_cell_y1 || !s_cell_split || !s_palette_blend || !s_raw_buf ||
        !s_raw_x0 || !s_raw_x1 || !s_raw_y0 || !s_raw_y1) {
        ESP_LOGE(TAG, "failed to allocate video encoder buffers from PSRAM");
        abort();
    }

    memset(s_xterm_map, 0, sizeof(s_xterm_map));
    memset(s_palette_rgb, 0, sizeof(s_palette_rgb));

    /* Cursor prefixes are independent of the framebuffer size. */
    for (size_t i = 0; i < total; ++i) {
        size_t off = i * CURSOR_PREFIX_MAX;
        size_t p = 0;
        uint8_t *prefix = s_cursor_buf + off;
        prefix[p++] = 0x1b;
        prefix[p++] = '[';
        p = append_uint(prefix, p, (unsigned)(i / cols + 1));
        prefix[p++] = ';';
        p = append_uint(prefix, p, (unsigned)(i % cols + 1));
        prefix[p++] = 'H';
        s_cursor_len[i] = (uint8_t)p;
    }
}

doom_video_mode_t serial_out_mode(void)
{
    return s_mode;
}

void serial_out_set_mode(doom_video_mode_t mode)
{
    s_mode = mode;
    const size_t total = (size_t)CONFIG_DOOM_ANSI_COLS * CONFIG_DOOM_ANSI_ROWS;
    memset(s_have_prev, 0, total);
    s_emit_fg = -1;
    s_emit_bg = -1;
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

void serial_out_palette(const uint8_t *rgb768)
{
    build_palette_map(rgb768);
    serial_out_frame(FRAME_PALETTE, rgb768, 768);
    const size_t total = (size_t)CONFIG_DOOM_ANSI_COLS * CONFIG_DOOM_ANSI_ROWS;
    memset(s_have_prev, 0, total);
    s_emit_fg = -1;
    s_emit_bg = -1;
}

static void append_cell_sample(int *sum, int *count, uint8_t palette_idx)
{
    const uint8_t *rgb = s_palette_rgb[palette_idx];
    sum[0] += rgb[0];
    sum[1] += rgb[1];
    sum[2] += rgb[2];
    ++*count;
}

void serial_out_ansi(const uint8_t *fb, int src_w, int src_h, const uint8_t *rgb768)
{
    (void)rgb768;
    build_output_maps(src_w, src_h);
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    const int cols = CONFIG_DOOM_ANSI_COLS;
    const int rows = CONFIG_DOOM_ANSI_ROWS;
    const size_t total = (size_t)cols * rows;

    /*
     * Build one half-block cell per character.  Average every source pixel in
     * the cell's top/bottom half instead of point-sampling; this removes most
     * of the stair-stepping on walls and sprites.
     */
    for (int cy = 0; cy < rows; ++cy) {
        const int y0 = s_cell_y0[cy];
        const int y1 = s_cell_y1[cy];
        const int split = s_cell_split[cy];
        for (int cx = 0; cx < cols; ++cx) {
            const int x0 = s_cell_x0[cx];
            const int x1 = s_cell_x1[cx];
            int top_sum[3] = {0, 0, 0};
            int bot_sum[3] = {0, 0, 0};
            int top_n = 0, bot_n = 0;
            int first = -1;
            bool uniform = true;

            for (int y = y0; y < y1; ++y) {
                const uint8_t *row = fb + (size_t)y * (size_t)src_w;
                bool top = y < split;
                for (int x = x0; x < x1; ++x) {
                    uint8_t pal = row[x];
                    if (first < 0) {
                        first = pal;
                    } else if ((int)pal != first) {
                        uniform = false;
                    }
                    if (top) {
                        append_cell_sample(top_sum, &top_n, pal);
                    } else {
                        append_cell_sample(bot_sum, &bot_n, pal);
                    }
                }
            }

            uint8_t top_idx, bot_idx;
            if (uniform && first >= 0) {
                /* Exact palette colour: no averaging error. */
                top_idx = bot_idx = s_xterm_map[first];
            } else {
                top_idx = top_n ? xterm_from_rgb(top_sum[0] / top_n,
                                                 top_sum[1] / top_n,
                                                 top_sum[2] / top_n) : 0;
                bot_idx = bot_n ? xterm_from_rgb(bot_sum[0] / bot_n,
                                                 bot_sum[1] / bot_n,
                                                 bot_sum[2] / bot_n) : 0;
            }
            size_t idx = (size_t)cy * cols + cx;
            s_cells[idx] = (uint16_t)top_idx | ((uint16_t)bot_idx << 8);
        }
    }

    uint8_t *out = s_ansi_buf;
    size_t pos = 0;
    s_emit_fg = -1;
    s_emit_bg = -1;

    /* Emit only changed cells, grouping consecutive runs in the same row. */
    for (int cy = 0; cy < rows; ++cy) {
        int cx = 0;
        while (cx < cols) {
            size_t idx = (size_t)cy * cols + cx;
            uint16_t cell = s_cells[idx];
            uint16_t prev = s_cells[total + idx];
            if (s_have_prev[idx] && cell == prev) {
                ++cx;
                continue;
            }

            bool need_cursor = true;
            while (cx < cols) {
                idx = (size_t)cy * cols + cx;
                cell = s_cells[idx];
                prev = s_cells[total + idx];
                if (s_have_prev[idx] && cell == prev) {
                    break;
                }

                if (pos + ANSI_CELL_MAX > ANSI_CHUNK_LIMIT) {
                    serial_out_frame(FRAME_ANSI, out, pos);
                    pos = 0;
                    need_cursor = true;
                }

                if (need_cursor) {
                    memcpy(out + pos, s_cursor_buf + idx * CURSOR_PREFIX_MAX,
                           s_cursor_len[idx]);
                    pos += s_cursor_len[idx];
                    need_cursor = false;
                }

                int fg = cell & 0xff;
                int bg = (cell >> 8) & 0xff;
                if (fg != s_emit_fg) {
                    pos = append_str(out, pos, "\x1b[38;5;");
                    pos = append_uint(out, pos, (unsigned)fg);
                    out[pos++] = 'm';
                    s_emit_fg = fg;
                }
                if (bg != s_emit_bg) {
                    pos = append_str(out, pos, "\x1b[48;5;");
                    pos = append_uint(out, pos, (unsigned)bg);
                    out[pos++] = 'm';
                    s_emit_bg = bg;
                }
                /* U+2580 UPPER HALF BLOCK */
                out[pos++] = 0xE2;
                out[pos++] = 0x96;
                out[pos++] = 0x80;
                ++cx;
            }
        }
    }

    memcpy(s_cells + total, s_cells, total * sizeof(uint16_t));
    memset(s_have_prev, 1, total);

    if (pos > 0) {
        serial_out_frame(FRAME_ANSI, out, pos);
    }
}

static uint8_t blend_row(const uint8_t *row, int x0, int x1)
{
    uint8_t val = row[x0];
    for (int x = x0 + 1; x < x1; ++x) {
        val = s_palette_blend[((unsigned)val << 8) | row[x]];
    }
    return val;
}

static uint8_t raw_sample(const uint8_t *fb, int src_w, int ox, int oy)
{
    const int x0 = s_raw_x0[ox];
    const int x1 = s_raw_x1[ox];
    const int y0 = s_raw_y0[oy];
    const int y1 = s_raw_y1[oy];

    uint8_t val = blend_row(fb + (size_t)y0 * (size_t)src_w, x0, x1);
    for (int y = y0 + 1; y < y1; ++y) {
        uint8_t rowval = blend_row(fb + (size_t)y * (size_t)src_w, x0, x1);
        val = s_palette_blend[((unsigned)val << 8) | rowval];
    }
    return val;
}

void serial_out_raw(const uint8_t *fb, int src_w, int src_h)
{
    build_output_maps(src_w, src_h);
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    const int w = CONFIG_DOOM_RAW_WIDTH;
    const int h = CONFIG_DOOM_RAW_HEIGHT;
    uint8_t *out = s_raw_buf;
    size_t pos = 0;

    out[pos++] = (uint8_t)(w & 0xff);
    out[pos++] = (uint8_t)(w >> 8);
    out[pos++] = (uint8_t)(h & 0xff);
    out[pos++] = (uint8_t)(h >> 8);

    for (int y = 0; y < h; ++y) {
        int run = 1;
        uint8_t val = raw_sample(fb, src_w, 0, y);

        for (int x = 1; x < w; ++x) {
            uint8_t v = raw_sample(fb, src_w, x, y);
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
