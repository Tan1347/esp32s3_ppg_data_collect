/**
 * @file ble_callbacks.h
 * @brief BLE service callback interface for decoupling
 *
 * ble_svc uses these callbacks instead of directly calling sibling components.
 * main.c registers the callbacks at initialization.
 */

#pragma once

#include "esp_err.h"
#include "ppg_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** UART recorder config (mirrors uart_recorder_config_t to avoid header dependency) */
typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
} ble_uart_config_t;

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
    esp_err_t (*wifi_add)(const char *ssid, const char *password, uint8_t priority);
    esp_err_t (*wifi_clear_all)(void);
    esp_err_t (*wifi_delete)(uint8_t index);
    esp_err_t (*wifi_get_list_json)(char *buf, size_t len);
    esp_err_t (*wifi_get_detail_json)(uint8_t index, char *buf, size_t len);
    bool (*wifi_is_connected)(void);
    esp_err_t (*wifi_get_ip)(char *buf, size_t len);

    /* SD card storage */
    esp_err_t (*sd_get_file_list)(char *buf, size_t len);
    uint32_t (*sd_get_free_mb)(void);
    uint32_t (*sd_get_total_mb)(void);

    /* Log system */
    void (*log_set_level)(uint8_t level);
    uint8_t (*log_get_level)(void);
    size_t (*log_get_buffer_count)(void);

    /* UART recorder */
    esp_err_t (*uart_record_start)(const ble_uart_config_t *config);
    void (*uart_record_stop)(void);
} ble_callbacks_t;

#ifdef __cplusplus
}
#endif
