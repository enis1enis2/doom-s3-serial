#pragma once

#include <stdint.h>

/** Enqueue a key event from the host. Safe to call from the serial input task. */
void dg_queue_key(int pressed, unsigned char key);

/** Initialize the platform-side key queue and frame pacing state. */
void dg_platform_init(void);
