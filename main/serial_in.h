#pragma once

/** Read and parse any pending host frames (keys + mode changes). Non-blocking. */
void serial_in_poll(void);
