idf_component_register(
    SRCS "wifi_transfer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif ota_upgrade app_update ppg_config fatfs heap esp_timer
)
