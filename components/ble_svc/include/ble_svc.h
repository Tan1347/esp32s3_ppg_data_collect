/**
 * @file ble_svc.h
 * @brief BLE GATT 服务
 *
 * NimBLE 主机栈 (比 Bluedroid 省 15KB RAM)
 * 服务 UUID: 0xFFF0
 * 特征值: Status(0xFFF1), Live Data(0xFFF2), Command(0xFFF3), File List(0xFFF4)
 * Bonding + NVS 持久化 LTK
 * 定向广播自动重连
 */

#pragma once

#include "esp_err.h"
#include "ppg_algo.h"
#include "ble_callbacks.h"
#include <stdint.h>

/* 兼容旧类型定义 */
typedef ppg_algo_result_t ppg_result_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 BLE 服务
 * @param callbacks 回调函数表（用于解耦兄弟组件）
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_init(const ble_callbacks_t *callbacks);

/**
 * @brief 启动 BLE 广播
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_start_advertising(void);

/**
 * @brief 停止 BLE 广播
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_stop_advertising(void);

/**
 * @brief 通知实时 PPG 数据
 * @param result 算法结果
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_notify_live_data(const ppg_result_t *result);

/**
 * @brief 更新设备状态
 * @param batt_pct 电量百分比
 * @param battery_voltage 电压 ×100
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_update_status(uint8_t batt_pct, uint32_t battery_voltage);

/**
 * @brief 检查 BLE 是否已连接
 * @return true 已连接
 */
bool ble_svc_is_connected(void);

/**
 * @brief 获取已连接设备地址
 * @param addr 输出地址缓冲区 (6 bytes)
 * @return ESP_OK 成功
 */
esp_err_t ble_svc_get_peer_addr(uint8_t *addr);

/**
 * @brief Check if NimBLE host has synced with controller
 * @return true if synced, false otherwise
 */
bool ble_svc_is_nimble_synced(void);

#ifdef __cplusplus
}
#endif
