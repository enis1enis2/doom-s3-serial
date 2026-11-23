#pragma once

#include "esp_err.h"

/** Redirect DOOM's stdout/stderr into framed LOG messages on the serial link. */
esp_err_t log_vfs_init(void);
