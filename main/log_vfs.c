/*
 * Doom printf() output is redirected to /dev/doomlog, which emits framed
 * LOG messages. ESP-IDF's own ESP_LOG output remains on UART0.
 */

#include "log_vfs.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"

#include "serial_out.h"

static const char *TAG = "log_vfs";

static int log_open_p(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)path;
    (void)flags;
    (void)mode;
    return 0;
}

static int log_close_p(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}

static ssize_t log_write_p(void *ctx, int fd, const void *data, size_t size)
{
    (void)ctx;
    (void)fd;
    serial_out_log((const char *)data, size);
    return (ssize_t)size;
}

esp_err_t log_vfs_init(void)
{
    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = log_open_p,
        .close_p = log_close_p,
        .write_p = log_write_p,
    };

    esp_err_t err = esp_vfs_register("/dev/doomlog", &vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register failed: %s", esp_err_to_name(err));
        return err;
    }

    if (freopen("/dev/doomlog", "w", stdout) == NULL ||
        freopen("/dev/doomlog", "w", stderr) == NULL) {
        ESP_LOGE(TAG, "failed to redirect stdout/stderr");
        return ESP_FAIL;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "doom stdout/stderr -> framed LOG frames");
    return ESP_OK;
}
