/**
 * @file uart_recorder.h
 * @brief UART data recorder with double-buffered DMA
 *
 * Records data received on UART0 RX to files on TF card.
 * Uses double buffering: one buffer fills via DMA while the other is written to file.
 *
 * BLE command 0x50 format:
 *   Start: [0x50][0x01][BAUD_H][BAUD_2][BAUD_1][BAUD_L][DATA_BITS][PARITY][STOP_BITS]
 *   Stop:  [0x50][0x00]
 *
 * Data bits: 5=5, 6=6, 7=7, 8=8
 * Parity: 0=none, 1=even, 2=odd
 * Stop bits: 1=1, 2=2
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UART configuration for recording */
typedef struct {
    uint32_t baud_rate;     /* Baud rate (9600 ~ 5000000) */
    uint8_t  data_bits;     /* 5, 6, 7, or 8 */
    uint8_t  parity;        /* 0=none, 1=even, 2=odd */
    uint8_t  stop_bits;     /* 1 or 2 */
} uart_recorder_config_t;

/**
 * @brief Initialize UART recorder
 * @return ESP_OK success
 */
esp_err_t uart_recorder_init(void);

/**
 * @brief Start recording with specified config
 * @param config UART configuration
 * @return ESP_OK success
 */
esp_err_t uart_recorder_start(const uart_recorder_config_t *config);

/**
 * @brief Stop recording
 * @return ESP_OK success
 */
esp_err_t uart_recorder_stop(void);

/**
 * @brief Check if recording is active
 * @return true if recording
 */
bool uart_recorder_is_active(void);

#ifdef __cplusplus
}
#endif
