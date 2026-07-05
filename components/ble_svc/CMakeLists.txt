idf_component_register(
    SRCS "ble_svc.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt nvs_flash ppg_algo ppg_config ppg_log max30102 uart_recorder
)
