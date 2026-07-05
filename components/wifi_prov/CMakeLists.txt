idf_component_register(
    SRCS "wifi_prov.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif ppg_config esp_timer
)
