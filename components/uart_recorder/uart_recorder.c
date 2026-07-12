/**
 * @file uart_recorder.c
 * @brief UART data recorder with double-buffered DMA
 *
 * Architecture:
 *   UART2 RX (GPIO7) -> DMA -> buffer[A/B] (4KB each)
 *   When buffer full -> notify writer task -> switch buffer
 *   Writer task: open file, write buffer, close
 *
 * File management:
 *   /sdcard/uart2/YYYYMMDD_HHMMSS.log
 *   Single file max 10MB, then create new file
 */

#include "uart_recorder.h"
#include "ppg_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "uart_rec";

/* Double buffer configuration - S3 has 8MB PSRAM, use larger buffers */
/* At 115200 baud: ~11KB/s, 16KB buffer fills in ~1.4s */
/* At 5Mbps: ~625KB/s, 16KB buffer fills in ~25ms */
#define BUF_SIZE        (16 * 1024)     /* 16KB per buffer (2 x 16KB = 32KB total) */
#define UART_RX_BUF_SIZE (16 * 1024)    /* 16KB UART driver RX buffer for DMA */
#define MAX_FILE_SIZE   (10 * 1024 * 1024)  /* 10MB per file */
#define READ_INTERVAL_MS 10             /* 10ms read interval for high-speed capture */
#define MOUNT_POINT     "/sdcard"
#define UART_REC_DIR    "/uart2"

/* Buffer state */
typedef struct {
    uint8_t data[BUF_SIZE];
    size_t  len;
    volatile bool ready;  /* true = filled, needs write to file */
} uart_buf_t;

static uart_buf_t s_bufs[2];           /* Double buffers */
static volatile int s_active_buf = 0;  /* Current write buffer index */
static SemaphoreHandle_t s_buf_mutex = NULL;

/* File state */
static FILE *s_log_file = NULL;
static uint32_t s_file_size = 0;
static bool s_active = false;
static uint32_t s_baud_rate = 0;

/* Tasks */
static TaskHandle_t s_uart_read_task = NULL;
static TaskHandle_t s_file_write_task = NULL;

/* Notification for buffer ready */
static SemaphoreHandle_t s_buf_ready_sem = NULL;

/* ========== File management ========== */

static void create_log_file(void)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    char path[128];
    if (tm_info && tm_info->tm_year > 100) {
        snprintf(path, sizeof(path), "%s/%s/%04d%02d%02d_%02d%02d%02d.log",
                 MOUNT_POINT, UART_REC_DIR,
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    } else {
        snprintf(path, sizeof(path), "%s/%s/unknown.log", MOUNT_POINT, UART_REC_DIR);
    }

    /* Ensure directory exists */
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", MOUNT_POINT, UART_REC_DIR);
    mkdir(dir_path, 0777);

    s_log_file = fopen(path, "w");
    s_file_size = 0;

    if (s_log_file) {
        ESP_LOGI(TAG, "Created log file: %s", path);
    } else {
        ESP_LOGE(TAG, "Failed to create log file: %s", path);
    }
}

static void rotate_file(void)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    create_log_file();
}

/* ========== UART read task ========== */

static void uart_read_task(void *arg)
{
    ESP_LOGI(TAG, "UART2 read task started");

    while (s_active) {
        /* Read from UART2 into active buffer - short timeout for high-speed capture */
        uart_buf_t *buf = &s_bufs[s_active_buf];
        int len = uart_read_bytes(UART_NUM_2, buf->data + buf->len,
                                   BUF_SIZE - buf->len, pdMS_TO_TICKS(READ_INTERVAL_MS));

        if (len > 0) {
            buf->len += len;

            /* Buffer full? Switch and notify */
            if (buf->len >= BUF_SIZE) {
                buf->ready = true;
                xSemaphoreGive(s_buf_ready_sem);

                /* Switch to other buffer (protected to avoid race with write task) */
                xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
                s_active_buf = 1 - s_active_buf;
                s_bufs[s_active_buf].len = 0;
                s_bufs[s_active_buf].ready = false;
                xSemaphoreGive(s_buf_mutex);
            }
        }
    }

    /* Flush remaining data */
    uart_buf_t *buf = &s_bufs[s_active_buf];
    if (buf->len > 0) {
        buf->ready = true;
        xSemaphoreGive(s_buf_ready_sem);
    }

    ESP_LOGI(TAG, "UART2 read task stopped");
    s_uart_read_task = NULL;
    vTaskDelete(NULL);
}

/* ========== File write task ========== */

static void file_write_task(void *arg)
{
    ESP_LOGI(TAG, "File write task started");

    while (s_active || uxSemaphoreGetCount(s_buf_ready_sem) > 0) {
        /* Wait for buffer ready signal */
        if (xSemaphoreTake(s_buf_ready_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        /* Find ready buffer and write to file under mutex */
        for (int i = 0; i < 2; i++) {
            xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
            if (s_bufs[i].ready && s_bufs[i].len > 0) {
                /* Snapshot buffer info under lock, then release before file I/O */
                size_t write_len = s_bufs[i].len;
                /* Ensure file is open */
                if (!s_log_file) {
                    create_log_file();
                }

                /* Check file size limit */
                if (s_file_size + write_len > MAX_FILE_SIZE) {
                    rotate_file();
                }

                /* Write to file */
                if (s_log_file) {
                    size_t written = fwrite(s_bufs[i].data, 1, write_len, s_log_file);
                    if (written > 0) {
                        s_file_size += written;
                        fflush(s_log_file);
                    }
                }

                /* Mark buffer as consumed */
                s_bufs[i].ready = false;
                s_bufs[i].len = 0;
            }
            xSemaphoreGive(s_buf_mutex);
        }
    }

    ESP_LOGI(TAG, "File write task stopped");
    s_file_write_task = NULL;
    vTaskDelete(NULL);
}

/* ========== Public API ========== */

esp_err_t uart_recorder_init(void)
{
    s_buf_mutex = xSemaphoreCreateMutex();
    s_buf_ready_sem = xSemaphoreCreateCounting(4, 0);

    memset(s_bufs, 0, sizeof(s_bufs));
    s_active_buf = 0;
    s_active = false;

    ESP_LOGI(TAG, "UART2 recorder init done (buf=%dKB x2)", BUF_SIZE / 1024);
    return ESP_OK;
}

esp_err_t uart_recorder_start(const uart_recorder_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_active) {
        ESP_LOGW(TAG, "Already recording, stop first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Map config to ESP-IDF UART types */
    uart_word_length_t data_bits;
    switch (config->data_bits) {
        case 5: data_bits = UART_DATA_5_BITS; break;
        case 6: data_bits = UART_DATA_6_BITS; break;
        case 7: data_bits = UART_DATA_7_BITS; break;
        default: data_bits = UART_DATA_8_BITS; break;
    }

    uart_parity_t parity;
    switch (config->parity) {
        case 1: parity = UART_PARITY_EVEN; break;
        case 2: parity = UART_PARITY_ODD; break;
        default: parity = UART_PARITY_DISABLE; break;
    }

    uart_stop_bits_t stop_bits;
    stop_bits = (config->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    /* Configure UART2 with GPIO6-TX / GPIO7-RX */
    uart_config_t uart_cfg = {
        .baud_rate = config->baud_rate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(UART_NUM_2, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART2 config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(UART_NUM_2, UART2_TX_PIN, UART2_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART2 set pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install UART driver with large RX buffer for high-speed capture */
    ret = uart_driver_install(UART_NUM_2, UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART2 install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Reset buffers */
    memset(s_bufs, 0, sizeof(s_bufs));
    s_active_buf = 0;
    s_bufs[0].len = 0;
    s_bufs[1].len = 0;
    s_file_size = 0;

    s_active = true;
    s_baud_rate = config->baud_rate;

    /* Start tasks */
    xTaskCreate(uart_read_task, "uart_rd", STACK_PPG_TASK, NULL, 4, &s_uart_read_task);
    xTaskCreate(file_write_task, "uart_wr", STACK_DHT11, NULL, 2, &s_file_write_task);

    ESP_LOGI(TAG, "UART2 recording started: %lu baud %d%c%d",
             (unsigned long)config->baud_rate,
             config->data_bits,
             config->parity == 0 ? 'N' : (config->parity == 1 ? 'E' : 'O'),
             config->stop_bits);
    return ESP_OK;
}

esp_err_t uart_recorder_stop(void)
{
    if (!s_active) return ESP_OK;

    s_active = false;

    /* Wait for tasks to finish */
    if (s_buf_ready_sem) {
        xSemaphoreGive(s_buf_ready_sem);  /* Unblock writer */
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    /* Close file */
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }

    /* Uninstall UART driver */
    uart_driver_delete(UART_NUM_2);

    ESP_LOGI(TAG, "UART2 recording stopped");
    return ESP_OK;
}

bool uart_recorder_is_active(void)
{
    return s_active;
}
