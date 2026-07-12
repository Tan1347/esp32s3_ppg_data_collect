/**
 * @file http_callbacks.h
 * @brief HTTP server callback interface for decoupling
 *
 * wifi_transfer uses these callbacks instead of directly calling sibling components.
 * main.c registers the callbacks at initialization.
 */

#pragma once

#include "esp_err.h"
#include "ppg_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* System state */
    void (*set_state)(system_state_t new_state);

    /* Battery */
    uint32_t (*get_voltage)(void);
    uint8_t (*voltage_to_soc)(uint32_t voltage);

    /* WiFi provisioning */
    esp_err_t (*wifi_auto_connect)(void);
    bool (*wifi_is_connected)(void);
    esp_err_t (*wifi_get_ip)(char *buf, size_t len);
    esp_err_t (*wifi_disconnect)(void);

    /* SD card storage */
    esp_err_t (*sd_get_file_list)(char *buf, size_t len);
    uint32_t (*sd_get_free_mb)(void);

    /* OTA upgrade */
    const char *(*ota_get_version)(void);
    const char *(*ota_get_build_time)(void);
    esp_err_t (*ota_upgrade_from_http)(size_t content_len, int (*read_func)(void *ctx, void *buf, size_t len), void *read_ctx);
} http_callbacks_t;

#ifdef __cplusplus
}
#endif
