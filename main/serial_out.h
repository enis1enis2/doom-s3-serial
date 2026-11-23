#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FRAME_ANSI = 1,
    FRAME_RAW = 2,
    FRAME_LOG = 3,
    FRAME_HELLO = 4,
    FRAME_PALETTE = 5,
} serial_frame_type_t;

typedef enum {
    DOOM_VIDEO_ANSI = 0,
    DOOM_VIDEO_RAW = 1,
} doom_video_mode_t;

void serial_out_init(void);
void serial_out_frame(serial_frame_type_t type, const uint8_t *payload, size_t len);
void serial_out_hello(void);
void serial_out_log(const char *data, size_t len);
void serial_out_palette(const uint8_t *rgb768);
void serial_out_ansi(const uint8_t *fb, int src_w, int src_h, const uint8_t *rgb768);
void serial_out_raw(const uint8_t *fb, int src_w, int src_h);
doom_video_mode_t serial_out_mode(void);
void serial_out_set_mode(doom_video_mode_t mode);
