/**
 * @file battery.h
 * @brief 电池电压检测与电量估算
 *
 * 硬件: 100K+100K 分压, ADC 量程 0-2.5V, 并联 100nF 滤波电容
 * 采样: 64 次均值, 结果 ×100 返回 (如 420 = 4.20V)
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池 ADC 检测
 * @return ESP_OK 成功
 */
esp_err_t battery_init(void);

/**
 * @brief 读取电池电压
 * @return 电压值 ×100 (如 420 = 4.20V)
 */
uint32_t battery_get_voltage(void);

/**
 * @brief 电压转电量百分比
 * @param voltage_x100 电压 ×100
 * @return 电量 0-100
 */
uint8_t battery_voltage_to_soc(uint32_t voltage_x100);

/**
 * @brief 检测是否在充电
 * @return true 正在充电
 */
bool battery_is_charging(void);

/**
 * @brief 获取电池状态描述字符串
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void battery_get_status_str(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
