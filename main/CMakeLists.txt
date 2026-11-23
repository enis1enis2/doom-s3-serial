idf_component_register(
    SRCS
        "app_main.c"
        "dg_platform.c"
        "serial_io.c"
        "serial_out.c"
        "serial_in.c"
        "wad_vfs.c"
        "log_vfs.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        doom_engine
        driver
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_partition
        esp_psram
        vfs
        esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CMAP256=1
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
)
