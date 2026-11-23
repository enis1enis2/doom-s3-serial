#include "serial_in.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "dg_platform.h"
#include "serial_io.h"
#include "serial_out.h"

static const char *TAG = "serial_in";

#define HOST_SYNC0 0x5A
#define HOST_SYNC1 0xA5
#define HOST_KEY 0x10
#define HOST_MODE 0x11

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

typedef enum {
    ST_SYNC0,
    ST_SYNC1,
    ST_TYPE,
    ST_LEN_LO,
    ST_LEN_HI,
    ST_PAYLOAD,
    ST_CRC_LO,
    ST_CRC_HI,
} parse_state_t;

void serial_in_poll(void)
{
    static parse_state_t state = ST_SYNC0;
    static uint8_t frame_type;
    static uint16_t payload_len;
    static uint16_t payload_pos;
    static uint8_t payload[64];
    static uint16_t crc;

    uint8_t buf[128];
    int n = serial_io_read(buf, sizeof(buf), 0);
    if (n <= 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {
        uint8_t b = buf[i];
        switch (state) {
        case ST_SYNC0:
            if (b == HOST_SYNC0) {
                state = ST_SYNC1;
            }
            break;
        case ST_SYNC1:
            if (b == HOST_SYNC1) {
                state = ST_TYPE;
            } else {
                state = (b == HOST_SYNC0) ? ST_SYNC1 : ST_SYNC0;
            }
            break;
        case ST_TYPE:
            frame_type = b;
            crc = crc16_ccitt(&b, 1, 0xffff);
            state = ST_LEN_LO;
            break;
        case ST_LEN_LO:
            payload_len = b;
            crc = crc16_ccitt(&b, 1, crc);
            state = ST_LEN_HI;
            break;
        case ST_LEN_HI:
            payload_len |= (uint16_t)b << 8;
            crc = crc16_ccitt(&b, 1, crc);
            payload_pos = 0;
            state = (payload_len > 0 && payload_len <= sizeof(payload))
                        ? ST_PAYLOAD
                        : (payload_len == 0 ? ST_CRC_LO : ST_SYNC0);
            break;
        case ST_PAYLOAD:
            payload[payload_pos++] = b;
            crc = crc16_ccitt(&b, 1, crc);
            if (payload_pos == payload_len) {
                state = ST_CRC_LO;
            }
            break;
        case ST_CRC_LO:
            if ((uint8_t)(crc & 0xff) != b) {
                ESP_LOGW(TAG, "bad CRC (lo)");
                state = ST_SYNC0;
            } else {
                state = ST_CRC_HI;
            }
            break;
        case ST_CRC_HI:
            if ((uint8_t)(crc >> 8) != b) {
                ESP_LOGW(TAG, "bad CRC (hi)");
            } else if (frame_type == HOST_KEY && payload_len >= 2) {
                dg_queue_key(payload[0] != 0, payload[1]);
            } else if (frame_type == HOST_MODE && payload_len >= 1) {
                serial_out_set_mode(payload[0] ? DOOM_VIDEO_RAW : DOOM_VIDEO_ANSI);
                ESP_LOGI(TAG, "video mode -> %s", payload[0] ? "raw" : "ansi");
            }
            state = ST_SYNC0;
            break;
        }
    }
}
