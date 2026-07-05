idf_component_register(
    SRCS "ppg_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver sd_storage ppg_config
)
