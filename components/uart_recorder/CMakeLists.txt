idf_component_register(
    SRCS "uart_recorder.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart sd_storage ppg_config freertos esp_timer
)
