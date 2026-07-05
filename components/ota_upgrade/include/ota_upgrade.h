/**
 * @file ota_upgrade.h
 * @brief OTA 固件升级模块
 *
 * 安全校验：
 *   - 固件头魔数校验
 *   - SHA-256 摘要验证
 *   - 版本号递增检查（防降级）
 *   - 原子操作（esp_ota_begin/write/end）
 *   - 失败自动回滚到旧分区
 *
 * 支持 Web OTA 和 App OTA 两种方式
 */

#pragma once

#include "esp_err.h"
#include "esp_ota_ops.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OTA 状态 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_STARTED,
    OTA_STATE_WRITING,
    OTA_STATE_VERIFYING,
    OTA_STATE_DONE,
    OTA_STATE_FAILED,
} ota_state_t;

/** OTA 进度回调 */
typedef void (*ota_progress_cb_t)(int received, int total, ota_state_t state);

/** OTA 配置 */
typedef struct {
    size_t buffer_size;         /**< 接收缓冲区大小 */
    bool   check_version;       /**< 是否检查版本递增 */
    bool   check_sha256;        /**< 是否校验 SHA-256 */
    bool   auto_rollback;       /**< 失败是否自动回滚 */
} ota_config_t;

/**
 * @brief 初始化 OTA 模块
 * @param config 配置参数，NULL 使用默认配置
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_init(const ota_config_t *config);

/**
 * @brief 开始 OTA 升级
 * @param expected_size 预期固件大小，0 表示未知
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_begin(size_t expected_size);

/**
 * @brief 写入固件数据
 * @param data 数据指针
 * @param len 数据长度
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_write(const void *data, size_t len);

/**
 * @brief 完成 OTA 升级（校验 + 切换分区）
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_end(void);

/**
 * @brief 中止 OTA 升级
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_abort(void);

/**
 * @brief 获取当前 OTA 状态
 * @return OTA 状态
 */
ota_state_t ota_upgrade_get_state(void);

/**
 * @brief 获取已接收字节数
 * @return 已接收字节数
 */
size_t ota_upgrade_get_received(void);

/**
 * @brief 获取固件总大小
 * @return 固件总大小，0 表示未知
 */
size_t ota_upgrade_get_total(void);

/**
 * @brief 设置进度回调
 * @param callback 回调函数
 */
void ota_upgrade_set_progress_cb(ota_progress_cb_t callback);

/**
 * @brief 获取当前运行固件的版本号
 * @return 版本字符串
 */
const char *ota_upgrade_get_current_version(void);

/**
 * @brief 获取当前固件的编译时间
 * @return 编译时间字符串
 */
const char *ota_upgrade_get_build_time(void);

/**
 * @brief 检查是否有待确认的 OTA（重启后需要确认）
 * @return true 有待确认
 */
bool ota_upgrade_pending_confirm(void);

/**
 * @brief 确认 OTA 升级成功（取消回滚标记）
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_confirm(void);

/**
 * @brief 回滚到上一个固件
 * @return ESP_OK 成功
 */
esp_err_t ota_upgrade_rollback(void);

/**
 * @brief 从 HTTP 请求处理 OTA（Web OTA 入口）
 * @param content_len 固件大小
 * @param read_func 数据读取回调
 * @param read_ctx 读取上下文
 * @return ESP_OK 成功
 */
typedef int (*ota_read_func_t)(void *ctx, void *buf, size_t len);
esp_err_t ota_upgrade_from_http(size_t content_len, ota_read_func_t read_func, void *read_ctx);

#ifdef __cplusplus
}
#endif
