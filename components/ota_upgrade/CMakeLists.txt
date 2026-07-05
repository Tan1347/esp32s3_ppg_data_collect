idf_component_register(
    SRCS "ota_upgrade.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update ppg_config ppg_log
)
