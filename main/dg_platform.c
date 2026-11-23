/*
 * doomgeneric platform callbacks for ESP32-S3.
 *
 * The engine calls DG_Init once, then DG_DrawFrame after every rendered frame.
 * Keys arrive asynchronously from the serial input task via dg_queue_key().
 */

#include "dg_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "doomgeneric.h"
#include "i_video.h"

#include "serial_out.h"

static const char *TAG = "dg_platform";

#define KEY_QUEUE_LEN 64

typedef struct {
    uint8_t pressed;
    uint8_t key;
} key_event_t;

static key_event_t s_keys[KEY_QUEUE_LEN];
static volatile size_t s_key_head;
static volatile size_t s_key_tail;
static volatile size_t s_key_count;
static SemaphoreHandle_t s_key_lock;

static int64_t s_next_frame_us;

void dg_platform_init(void)
{
    if (s_key_lock == NULL) {
        s_key_lock = xSemaphoreCreateMutex();
        assert(s_key_lock != NULL);
    }
    s_next_frame_us = esp_timer_get_time();
}

void dg_queue_key(int pressed, unsigned char key)
{
    if (s_key_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_key_lock, portMAX_DELAY);
    if (s_key_count >= KEY_QUEUE_LEN) {
        xSemaphoreGive(s_key_lock);
        ESP_LOGW(TAG, "key queue full; dropping key 0x%02x", key);
        return;
    }
    s_keys[s_key_tail].pressed = pressed ? 1 : 0;
    s_keys[s_key_tail].key = key;
    s_key_tail = (s_key_tail + 1) % KEY_QUEUE_LEN;
    ++s_key_count;
    xSemaphoreGive(s_key_lock);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (s_key_lock == NULL || pressed == NULL || key == NULL) {
        return 0;
    }

    xSemaphoreTake(s_key_lock, portMAX_DELAY);
    if (s_key_count == 0) {
        xSemaphoreGive(s_key_lock);
        return 0;
    }
    *pressed = s_keys[s_key_head].pressed;
    *key = s_keys[s_key_head].key;
    s_key_head = (s_key_head + 1) % KEY_QUEUE_LEN;
    --s_key_count;
    xSemaphoreGive(s_key_lock);
    return 1;
}

void DG_Init(void)
{
    dg_platform_init();
    ESP_LOGI(TAG, "doomgeneric platform ready (%dx%d)",
             DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

static void send_palette_if_changed(void)
{
    if (!palette_changed) {
        return;
    }

    uint8_t rgb[768];
    for (int i = 0; i < 256; ++i) {
        rgb[i * 3 + 0] = (uint8_t)colors[i].r;
        rgb[i * 3 + 1] = (uint8_t)colors[i].g;
        rgb[i * 3 + 2] = (uint8_t)colors[i].b;
    }
    serial_out_palette(rgb);
    palette_changed = false;
}

void DG_DrawFrame(void)
{
    /* Cap the serial video stream so slow terminals do not break the game. */
    int64_t now = esp_timer_get_time();
    if (now < s_next_frame_us) {
        int64_t wait_us = s_next_frame_us - now;
        if (wait_us > 1000000) {
            wait_us = 1000000;
        }
        vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
    }
    const int64_t frame_us = 1000000 / CONFIG_DOOM_TARGET_FPS;
    s_next_frame_us = esp_timer_get_time() + frame_us;

    send_palette_if_changed();

    const uint8_t *fb = (const uint8_t *)DG_ScreenBuffer;
    if (fb == NULL) {
        return;
    }

    if (serial_out_mode() == DOOM_VIDEO_RAW) {
        serial_out_raw(fb, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    } else {
        serial_out_ansi(fb, DOOMGENERIC_RESX, DOOMGENERIC_RESY, NULL);
    }
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void DG_SetWindowTitle(const char *title)
{
    if (title == NULL) {
        return;
    }
    char msg[256];
    int n = snprintf(msg, sizeof(msg), "[doom] window title: %s\n", title);
    if (n > 0) {
        serial_out_log(msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg));
    }
}
