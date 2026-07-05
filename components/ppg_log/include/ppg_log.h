/**
 * @file ppg_log.h
 * @brief 异步日志系统
 *
 * RAM 环形缓冲 (8KB, mutex 保护)
 * 半满/定时30s/关机前/BLE请求 刷写到 TF 卡
 * 日志格式: [unix_ts] [可读时间] [等级] [tag] 函数名:行号 消息
 * 单文件 ≤10MB, 保留最近 5 个文件
 * UART 调试宏开关 (PPG_LOG_UART_ENABLE)
 * esp_log 拦截集成
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 日志等级 */
typedef enum {
    PPG_LOG_NONE = 0,
    PPG_LOG_ERROR,
    PPG_LOG_WARN,
    PPG_LOG_INFO,
    PPG_LOG_DEBUG,
    PPG_LOG_VERBOSE,
} ppg_log_level_t;

/**
 * @brief 初始化日志系统
 * @return ESP_OK 成功
 */
esp_err_t ppg_log_init(void);

/**
 * @brief 启用 esp_log 重定向到 ppg_log
 *
 * 必须在所有初始化完成后调用，避免初始化阶段栈溢出
 */
void ppg_log_enable_redirect(void);

/**
 * @brief 写入日志
 * @param level 日志等级
 * @param tag 标签
 * @param func 函数名
 * @param line 行号
 * @param fmt 格式字符串
 * @param ... 参数
 */
void ppg_log_write(ppg_log_level_t level, const char *tag,
                   const char *func, int line, const char *fmt, ...);

/**
 * @brief 刷写日志缓冲到 TF 卡
 * @return ESP_OK 成功
 */
esp_err_t ppg_log_flush(void);

/**
 * @brief 设置运行时日志等级
 * @param level 日志等级
 */
void ppg_log_set_level(uint8_t level);

/**
 * @brief 获取当前日志等级
 * @return 日志等级
 */
uint8_t ppg_log_get_level(void);

/**
 * @brief 获取缓冲区中的日志条数
 * @return 日志条数
 */
size_t ppg_log_get_buffer_count(void);

/**
 * @brief 导出最近日志摘要
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return 写入长度
 */
size_t ppg_log_export_summary(char *buf, size_t len);

/* 便捷宏 */
#define PPG_LOGE(tag, func, line, fmt, ...) \
    ppg_log_write(PPG_LOG_ERROR, tag, func, line, fmt, ##__VA_ARGS__)
#define PPG_LOGW(tag, func, line, fmt, ...) \
    ppg_log_write(PPG_LOG_WARN, tag, func, line, fmt, ##__VA_ARGS__)
#define PPG_LOGI(tag, func, line, fmt, ...) \
    ppg_log_write(PPG_LOG_INFO, tag, func, line, fmt, ##__VA_ARGS__)
#define PPG_LOGD(tag, func, line, fmt, ...) \
    ppg_log_write(PPG_LOG_DEBUG, tag, func, line, fmt, ##__VA_ARGS__)
#define PPG_LOGV(tag, func, line, fmt, ...) \
    ppg_log_write(PPG_LOG_VERBOSE, tag, func, line, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
