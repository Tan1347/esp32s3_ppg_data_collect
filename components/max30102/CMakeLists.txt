idf_component_register(
    SRCS "max30102.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver ppg_config esp_timer esp_driver_gpio esp_driver_i2c
)
