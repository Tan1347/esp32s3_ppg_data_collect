/**
 * @file sd_storage.h
 * @brief TF card storage management
 *
 * SPI mode, FAT32 filesystem
 * Double buffer (32KB primary + 8KB backup)
 * Binary struct format with checksum
 * Directory: /raw/ /csv/ /log/
 */

#pragma once

#include "esp_err.h"
#include "max30102.h"
#include "ppg_algo.h"
#include <stdint.h>
#include <stdbool.h>

/* Binary data structures (packed, no padding) */

/**
 * @brief PPG raw data record (13 bytes)
 *
 * Stored in /raw/ directory as binary file
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;     /* Unix timestamp (seconds) */
    uint32_t red;           /* Red light raw value */
    uint32_t ir;            /* IR light raw value */
    uint8_t  checksum;      /* XOR of all previous bytes */
} ppg_raw_record_t;

/**
 * @brief PPG result record (14 bytes)
 *
 * Stored in /csv/ directory as binary file
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp;     /* Unix timestamp (seconds) */
    int32_t  heart_rate;    /* Heart rate (bpm), -999 = invalid */
    int32_t  spo2;          /* SpO2 (%), -999 = invalid */
    uint8_t  hr_valid;      /* HR valid flag (1=valid) */
    uint8_t  spo2_valid;    /* SpO2 valid flag (1=valid) */
    uint8_t  checksum;      /* XOR of all previous bytes */
} ppg_result_record_t;

/* Checksum calculation */
static inline uint8_t calc_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len - 1; i++) {  /* Exclude checksum byte */
        sum ^= p[i];
    }
    return sum;
}

/* Verify checksum */
static inline bool verify_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    return p[len - 1] == calc_checksum(data, len);
}

/* Compatible type definitions */
typedef max30102_raw_t max30102_sample_t;
typedef ppg_algo_result_t ppg_result_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TF 卡存储
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_init(void);

/**
 * @brief 挂载 TF 卡
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_mount(void);

/**
 * @brief 卸载 TF 卡
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_unmount(void);

/**
 * @brief 检测 TF 卡是否已插入
 * @return true 已插入
 */
bool sd_storage_card_inserted(void);

/**
 * @brief 检测 TF 卡是否已挂载
 * @return true 已挂载
 */
bool sd_storage_is_mounted(void);

/**
 * @brief 写入原始 PPG 数据
 * @param sample 原始采样
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_write_raw(const max30102_sample_t *sample);

/**
 * @brief 写入算法结果到 CSV
 * @param result 算法结果
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_write_csv(const ppg_result_t *result);

/**
 * @brief 刷写所有缓冲数据到 TF 卡
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_flush(void);

/**
 * @brief 获取 TF 卡文件列表 JSON
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t sd_storage_get_file_list(char *buf, size_t len);

/**
 * @brief 获取 TF 卡剩余空间 (MB)
 * @return 剩余空间 MB
 */
uint32_t sd_storage_get_free_space_mb(void);

/**
 * @brief 获取 TF 卡总空间 (MB)
 * @return 总空间 MB
 */
uint32_t sd_storage_get_total_space_mb(void);

/**
 * @brief Release write buffers to free memory (e.g., before WiFi init)
 * Flushes pending data first, then frees primary/backup/CSV buffers.
 * SD card remains mounted for file reads.
 */
void sd_storage_release_buffers(void);

#ifdef __cplusplus
}
#endif
