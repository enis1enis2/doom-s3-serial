#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

void serial_io_init(void);

/** Returns number of bytes actually read (may be 0 on timeout). */
int serial_io_read(uint8_t *buf, size_t len, TickType_t ticks_to_wait);

/** Returns number of bytes actually written (may be < len). */
int serial_io_write(const uint8_t *buf, size_t len, TickType_t ticks_to_wait);
