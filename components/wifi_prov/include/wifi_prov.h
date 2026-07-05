/**
 * @file wifi_prov.h
 * @brief WiFi 配网管理
 *
 * 通过 BLE 管理 WiFi 凭据 (增删改查)
 * NVS 持久化存储, 最多 5 条
 * Station 模式自动连接路由器
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi 凭据结构 */
typedef struct {
    char     ssid[33];
    char     password[65];
    uint8_t  channel;       /* 0=自动 */
    int8_t   rssi_last;     /* 上次信号强度 */
    uint8_t  priority;      /* 优先级 0=最高 */
    uint8_t  fail_count;    /* 连续失败计数 */
} wifi_cred_t;

/**
 * @brief 初始化 WiFi 配网模块
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_init(void);

/**
 * @brief 添加 WiFi 凭据
 * @param ssid SSID
 * @param password 密码
 * @param priority 优先级
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_add(const char *ssid, const char *password, uint8_t priority);

/**
 * @brief 删除指定索引的 WiFi
 * @param index 索引
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_delete(uint8_t index);

/**
 * @brief 修改 WiFi 凭据
 * @param index 索引
 * @param ssid 新 SSID
 * @param password 新密码
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_modify(uint8_t index, const char *ssid, const char *password);

/**
 * @brief 清除全部 WiFi 凭据
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_clear_all(void);

/**
 * @brief 获取 WiFi 列表 JSON (不含密码)
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_get_list_json(char *buf, size_t len);

/**
 * @brief Get WiFi details by index as JSON
 * @param index WiFi index
 * @param buf Output buffer
 * @param len Buffer size
 * @return ESP_OK on success
 */
esp_err_t wifi_prov_get_detail_json(uint8_t index, char *buf, size_t len);

/**
 * @brief 获取当前连接状态 JSON
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_get_status_json(char *buf, size_t len);

/**
 * @brief 调整优先级
 * @param index 索引
 * @param priority 新优先级
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_set_priority(uint8_t index, uint8_t priority);

/**
 * @brief 自动连接最优 WiFi
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_auto_connect(void);

/**
 * @brief 检查是否已连接
 * @return true 已连接
 */
bool wifi_prov_is_connected(void);

/**
 * @brief 获取当前 IP 地址字符串
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_get_ip(char *buf, size_t len);

/**
 * @brief 断开 WiFi 并停止无线电
 * @return ESP_OK 成功
 */
esp_err_t wifi_prov_disconnect(void);

#ifdef __cplusplus
}
#endif
