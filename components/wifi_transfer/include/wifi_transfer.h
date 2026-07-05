/**
 * @file wifi_transfer.h
 * @brief WiFi HTTP 传输服务
 *
 * Station 模式连接路由器, 局域网内通过设备 IP 访问
 * 端点: /api/files, /api/download, /api/status, /api/ota, /api/logs, /api/shutdown
 * 120 秒无活动自动关闭
 */

#pragma once

#include "esp_err.h"
#include "http_callbacks.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 WiFi + HTTP Server
 * @param callbacks 回调函数表（用于解耦兄弟组件）
 * @return ESP_OK 成功
 */
esp_err_t wifi_transfer_start(const http_callbacks_t *callbacks);

/**
 * @brief 停止 WiFi + HTTP Server
 * @return ESP_OK 成功
 */
esp_err_t wifi_transfer_stop(void);

/**
 * @brief 设置超时时间
 * @param seconds 超时秒数
 */
void wifi_transfer_set_timeout(uint32_t seconds);

/**
 * @brief 启动 OTA 模式
 * @return ESP_OK 成功
 */
esp_err_t wifi_transfer_start_ota(void);

/**
 * @brief 检查 HTTP Server 是否运行中
 * @return true 运行中
 */
bool wifi_transfer_is_running(void);

#ifdef __cplusplus
}
#endif
