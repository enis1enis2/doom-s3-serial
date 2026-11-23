#pragma once

#include "esp_err.h"

/**
 * Register /doom/doom1.wad as a read-only file backed by the `doom_wad`
 * flash partition. The WAD itself is flashed separately from the app.
 */
esp_err_t wad_vfs_init(void);
