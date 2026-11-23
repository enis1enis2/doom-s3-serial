#include "serial_io.h"

#include "esp_err.h"
#include "esp_log.h"

#if CONFIG_DOOM_TRANSPORT_USB_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif

static const char *TAG = "serial_io";

#if CONFIG_DOOM_TRANSPORT_USB_JTAG

void serial_io_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 2048;
    cfg.rx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    ESP_LOGI(TAG, "video/keyboard on USB-Serial-JTAG");
}

int serial_io_read(uint8_t *buf, size_t len, TickType_t ticks)
{
    return usb_serial_jtag_read_bytes(buf, (uint32_t)len, ticks);
}

int serial_io_write(const uint8_t *buf, size_t len, TickType_t ticks)
{
    return usb_serial_jtag_write_bytes(buf, len, ticks);
}

#else /* UART1 */

#define DOOM_UART UART_NUM_1

void serial_io_init(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_DOOM_UART1_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(DOOM_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DOOM_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DOOM_UART, CONFIG_DOOM_UART1_TX_PIN,
                                 CONFIG_DOOM_UART1_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "video/keyboard on UART1 @ %d baud", CONFIG_DOOM_UART1_BAUD);
}

int serial_io_read(uint8_t *buf, size_t len, TickType_t ticks)
{
    return uart_read_bytes(DOOM_UART, buf, (uint32_t)len, ticks);
}

int serial_io_write(const uint8_t *buf, size_t len, TickType_t ticks)
{
    return uart_write_bytes(DOOM_UART, (const char *)buf, len);
}

#endif
