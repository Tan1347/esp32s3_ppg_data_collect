idf_component_register(
    SRCS "sd_storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver fatfs max30102 ppg_algo ppg_config sdmmc esp_driver_sdspi compress freertos
)
