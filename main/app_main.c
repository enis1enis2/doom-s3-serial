#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dg_platform.h"
#include "doomgeneric.h"
#include "log_vfs.h"
#include "serial_in.h"
#include "serial_io.h"
#include "serial_out.h"
#include "wad_vfs.h"

static const char *TAG = "app_main";

static void serial_input_task(void *arg)
{
    (void)arg;
    while (1) {
        serial_in_poll();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void fail_missing_psram(bool present, size_t size)
{
    const char *msg = present
        ? "ERROR: PSRAM is too small for DOOM. Need at least 2 MiB.\n"
        : "ERROR: No PSRAM detected. This build needs a PSRAM-equipped ESP32-S3 "
          "(for example N8R8 or N8R2). Check the module marking.\n";

    ESP_LOGE(TAG, "%s", msg);
    serial_out_log(msg, strlen(msg));
    abort();
}

void app_main(void)
{
    serial_io_init();
    serial_out_init();
    serial_out_hello();

    ESP_ERROR_CHECK(log_vfs_init());

    const bool psram_ok = esp_psram_is_initialized();
    const size_t psram_size = psram_ok ? esp_psram_get_size() : 0;
    ESP_LOGI(TAG, "PSRAM: %s (%u bytes)", psram_ok ? "present" : "not found",
             (unsigned)psram_size);

    char psram_msg[96];
    snprintf(psram_msg, sizeof(psram_msg), "PSRAM: %s (%u bytes)\n",
             psram_ok ? "present" : "not found", (unsigned)psram_size);
    serial_out_log(psram_msg, strlen(psram_msg));

    if (!psram_ok || psram_size < 2u * 1024u * 1024u) {
        fail_missing_psram(psram_ok, psram_size);
    }

    ESP_ERROR_CHECK(wad_vfs_init());

    /* Give DOOM the largest zone that fits the installed PSRAM. */
    int zone_mb = 6;
    if (psram_size < 6u * 1024u * 1024u) {
        zone_mb = 2;
    }
    if (psram_size < 4u * 1024u * 1024u) {
        zone_mb = 1;
    }
    static char zone_arg[8];
    snprintf(zone_arg, sizeof(zone_arg), "%d", zone_mb);
    ESP_LOGI(TAG, "DOOM zone memory: %d MiB", zone_mb);

    char zone_msg[64];
    snprintf(zone_msg, sizeof(zone_msg), "DOOM zone memory: %d MiB\n", zone_mb);
    serial_out_log(zone_msg, strlen(zone_msg));

    xTaskCreate(serial_input_task, "serial_in", 4096, NULL, 5, NULL);

    char *argv[] = {
        "doom",
        "-iwad", "/doom/doom1.wad",
        "-mb", zone_arg,
        NULL,
    };
    doomgeneric_Create((int)(sizeof(argv) / sizeof(argv[0]) - 1), argv);

    while (1) {
        doomgeneric_Tick();
    }
}
