idf_component_register(
    SRCS "power_mgmt.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_pm esp_system esp_driver_gpio ppg_config
)
