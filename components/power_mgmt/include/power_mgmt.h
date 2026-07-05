/**
 * @file power_mgmt.h
 * @brief 电源管理
 *
 * DFS 动态调频: 10MHz-160MHz 自动切换
 * Deep-sleep: GPIO9 (BOOT 按钮) 唤醒 (~5µA)
 * Light-sleep: 等待 BLE 连接 (~100µA)
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电源管理 (含 DFS)
 * @return ESP_OK 成功
 */
esp_err_t power_mgmt_init(void);

/**
 * @brief 进入 Deep-sleep
 */
void power_mgmt_enter_deep_sleep(void);

/**
 * @brief 进入 Light-sleep
 */
void power_mgmt_enter_light_sleep(void);

/**
 * @brief 设置 CPU 频率
 * @param freq_mhz 频率 (MHz)
 * @return ESP_OK 成功
 */
esp_err_t power_mgmt_set_freq(uint32_t freq_mhz);

/**
 * @brief 启用/禁用 DFS 自动调频
 * @param enable true 启用
 * @return ESP_OK 成功
 */
esp_err_t power_mgmt_set_dfs(bool enable);

#ifdef __cplusplus
}
#endif
