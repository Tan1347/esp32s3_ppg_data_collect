/**
 * @file dht11.h
 * @brief DHT11 温湿度传感器驱动
 *
 * 单总线协议，40bit 数据：
 *   [湿度整数 8bit][湿度小数 8bit][温度整数 8bit][温度小数 8bit][校验 8bit]
 *   校验 = 前四字节之和的低 8 位
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** DHT11 读取结果 */
typedef struct {
    int  humidity;       /**< 湿度 (%RH)，整数 */
    int  temperature;    /**< 温度 (°C)，整数 */
    bool checksum_ok;    /**< 校验是否通过 */
} dht11_data_t;

/**
 * @brief 初始化 DHT11 GPIO
 * @return ESP_OK 成功
 */
esp_err_t dht11_init(void);

/**
 * @brief 读取 DHT11 温湿度数据
 *
 * 每次读取间隔建议 >= 2 秒，否则传感器可能返回旧数据。
 *
 * @param[out] data 读取结果
 * @return true 读取成功且校验通过，false 失败
 */
bool dht11_read(dht11_data_t *data);

#ifdef __cplusplus
}
#endif
