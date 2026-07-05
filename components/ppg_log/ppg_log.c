/**
 * @file ppg_log.c
 * @brief 异步日志系统实现
 *
 * 架构:
 *   各任务 → ppg_log() → RAM 环形缓冲区 (8KB, mutex 保护)
 *                                │
 *                         半满 / 定时30s / 关机前 / BLE请求
 *                                ▼
 *                          Logger 任务 → 批量 fwrite → TF 卡 /log/
 *
 * 日志格式:
 *   [1750008601] [2026-06-15 22:30:01.234] [E] [ppg_algo] ppg_process:162 SpO2 quality low
 *
 * UART 调试:
 *   PPG_LOG_UART_ENABLE=1: 同步输出到 UART0 (GPIO20/21, CH343 USB转串口)
 *   PPG_LOG_UART_ENABLE=0: 编译后完全移除, 零开销
 */

#include "ppg_log.h"
#include "ppg_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

/* ========== 环形缓冲区 ========== */

#define RING_BUF_SIZE   LOG_RING_BUFFER_SIZE
#define FLUSH_THRESHOLD LOG_FLUSH_THRESHOLD
#define LINE_MAX_LEN    128

typedef struct {
    char data[RING_BUF_SIZE];
    volatile size_t head;
    volatile size_t tail;
    size_t count;
} ring_buf_t;

static ring_buf_t s_ring_buf;
static SemaphoreHandle_t s_mutex = NULL;
static TimerHandle_t s_flush_timer = NULL;

/* 日志等级 */
static ppg_log_level_t s_current_level = PPG_LOG_INFO;

/* Logger 任务 */
static TaskHandle_t s_logger_task_handle = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;

/* ========== 环形缓冲操作 ========== */

static size_t ring_buf_free_space(void)
{
    return RING_BUF_SIZE - s_ring_buf.count;
}

static size_t ring_buf_write(const char *data, size_t len)
{
    size_t free = ring_buf_free_space();
    if (len > free) len = free;

    for (size_t i = 0; i < len; i++) {
        s_ring_buf.data[s_ring_buf.head] = data[i];
        s_ring_buf.head = (s_ring_buf.head + 1) % RING_BUF_SIZE;
    }
    s_ring_buf.count += len;
    return len;
}

static size_t ring_buf_read(char *buf, size_t max_len)
{
    size_t to_read = s_ring_buf.count;
    if (to_read > max_len) to_read = max_len;

    for (size_t i = 0; i < to_read; i++) {
        buf[i] = s_ring_buf.data[s_ring_buf.tail];
        s_ring_buf.tail = (s_ring_buf.tail + 1) % RING_BUF_SIZE;
    }
    s_ring_buf.count -= to_read;
    return to_read;
}

/* ========== UART 输出 ========== */

#if PPG_LOG_UART_ENABLE
static void uart_output(const char *str, size_t len)
{
    fwrite(str, 1, len, stdout);
    fflush(stdout);
}
#endif

/* ========== Logger 任务 ========== */

static void logger_task(void *arg)
{
    char buf[512];

    while (1) {
        /* Wait for flush signal or threshold */
        bool should_flush = false;
        if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS)) == pdTRUE) {
            should_flush = true;
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            should_flush = (s_ring_buf.count >= FLUSH_THRESHOLD);
            xSemaphoreGive(s_mutex);
        }

        if (should_flush) {
            /* Read buffered data */
            size_t total = 0;
            while (1) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                size_t avail = s_ring_buf.count;
                size_t read = (avail > 0) ? ring_buf_read(buf + total, sizeof(buf) - total - 1) : 0;
                xSemaphoreGive(s_mutex);

                if (read == 0) break;
                total += read;

                /* 写入 TF 卡 (如果已挂载) */
                if (total > 0) {
                    /* 固定日志文件名，追加写入，按大小轮转 */
                    char path[64];
                    static int s_log_file_idx = 0;
                    snprintf(path, sizeof(path), "%s/log/ppg_%d.log", SD_MOUNT_POINT, s_log_file_idx);

                    /* 检查文件大小，超过上限则轮转 */
                    struct stat st;
                    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_FILE_SIZE) {
                        s_log_file_idx = (s_log_file_idx + 1) % LOG_MAX_FILES;
                        snprintf(path, sizeof(path), "%s/log/ppg_%d.log", SD_MOUNT_POINT, s_log_file_idx);
                        /* 轮转时清空文件 */
                        FILE *f_trunc = fopen(path, "w");
                        if (f_trunc) fclose(f_trunc);
                    }

                    FILE *f = fopen(path, "a");
                    if (f) {
                        fwrite(buf, 1, total, f);
                        fclose(f);
                    }
                    total = 0;
                }
            }
        }
    }
}

/* ========== 定时刷写回调 ========== */

static void flush_timer_callback(TimerHandle_t timer)
{
    if (s_logger_task_handle) {
        xSemaphoreGive(s_flush_sem);
    }
}

/* ========== esp_log 拦截 ========== */

static int esp_log_vprintf(const char *fmt, va_list args)
{
    /* Stack-local buffer to avoid static buffer race condition */
    char log_buf[LINE_MAX_LEN];
    int len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    if (len > 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        ring_buf_write(log_buf, len);
        xSemaphoreGive(s_mutex);

        /* UART output (outside mutex to avoid holding lock during I/O) */
        #if PPG_LOG_UART_ENABLE
        uart_output(log_buf, len);
        #endif
    }
    return len;
}

/* ========== 公共 API ========== */

esp_err_t ppg_log_init(void)
{
    if (s_mutex) return ESP_OK;  /* 已初始化 */

    /* 创建互斥锁 */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        puts("ppg_log: mutex failed");
        return ESP_FAIL;
    }

    /* 创建刷写信号量 */
    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) {
        puts("ppg_log: semaphore failed");
        return ESP_FAIL;
    }

    /* 初始化环形缓冲 */
    memset(&s_ring_buf, 0, sizeof(s_ring_buf));

    /* 创建定时刷写定时器 */
    s_flush_timer = xTimerCreate("log_flush",
                                  pdMS_TO_TICKS(LOG_FLUSH_INTERVAL_MS),
                                  pdTRUE, NULL, flush_timer_callback);
    if (s_flush_timer) {
        xTimerStart(s_flush_timer, 0);
    }

    /* 创建 Logger 任务 */
    xTaskCreate(logger_task, "logger", 4096, NULL, 1, &s_logger_task_handle);

    /* UART 已由 sdkconfig 配置（GPIO20/21, 1M baud），无需重新初始化 */

    /* 注意：不在这里设置自定义 log handler，避免初始化阶段栈溢出 */
    /* 调用 ppg_log_enable_redirect() 启用重定向 */

    return ESP_OK;
}

void ppg_log_enable_redirect(void)
{
    esp_log_set_vprintf(esp_log_vprintf);
}

void ppg_log_write(ppg_log_level_t level, const char *tag,
                   const char *func, int line, const char *fmt, ...)
{
    /* 等级过滤 */
    if (level > s_current_level) return;

    /* 格式化时间戳 */
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        snprintf(timestamp, sizeof(timestamp), "[%lld] [%04d-%02d-%02d %02d:%02d:%02d.%03ld]",
                 (long long)now, tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
                 (long)(tv.tv_usec / 1000));
    } else {
        snprintf(timestamp, sizeof(timestamp), "[0] [1970-01-01 00:00:00.000]");
    }

    /* 等级字符 */
    const char *level_str[] = {"", "E", "W", "I", "D", "V"};
    const char *lvl = (level <= PPG_LOG_VERBOSE) ? level_str[level] : "?";

    /* 格式化消息 */
    char line_buf[LINE_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    int header_len = snprintf(line_buf, sizeof(line_buf), "%s [%s] [%s] %s:%d ",
                              timestamp, lvl, tag, func, line);
    int msg_len = vsnprintf(line_buf + header_len, sizeof(line_buf) - header_len, fmt, args);
    va_end(args);

    if (msg_len < 0) return;
    int total_len = header_len + msg_len;
    if (total_len >= sizeof(line_buf) - 2) total_len = sizeof(line_buf) - 2;
    line_buf[total_len++] = '\n';
    line_buf[total_len] = '\0';

    /* 写入环形缓冲 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ring_buf_write(line_buf, total_len);
    xSemaphoreGive(s_mutex);

    /* UART 输出 */
    #if PPG_LOG_UART_ENABLE
    uart_output(line_buf, total_len);
    #endif
}

esp_err_t ppg_log_flush(void)
{
    if (s_logger_task_handle) {
        xSemaphoreGive(s_flush_sem);
        vTaskDelay(pdMS_TO_TICKS(100));  /* 等待 Logger 刷写完成 */
    }
    return ESP_OK;
}

void ppg_log_set_level(uint8_t level)
{
    if (level > PPG_LOG_VERBOSE) level = PPG_LOG_VERBOSE;
    s_current_level = (ppg_log_level_t)level;
}

uint8_t ppg_log_get_level(void)
{
    return (uint8_t)s_current_level;
}

size_t ppg_log_get_buffer_count(void)
{
    return s_ring_buf.count;
}

size_t ppg_log_export_summary(char *buf, size_t len)
{
    /* 导出最近的日志摘要 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = s_ring_buf.count;
    if (count > len - 1) count = len - 1;
    ring_buf_read(buf, count);
    buf[count] = '\0';
    xSemaphoreGive(s_mutex);
    return count;
}
