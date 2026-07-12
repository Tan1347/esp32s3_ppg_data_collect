idf_component_register(
    SRCS "ppg_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_uart sd_storage ppg_config
)
