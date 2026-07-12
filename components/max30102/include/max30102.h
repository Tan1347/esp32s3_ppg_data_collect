/**
 * @file max30102.h
 * @brief MAX30102 PPG 传感器驱动（ESP32-C3 I2C 硬件驱动）
 *
 * 参考 STM32 实现，改用 ESP-IDF I2C 硬件驱动
 * 纯驱动层，不包含算法逻辑
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* I2C 总线句柄前向声明 */
struct i2c_master_bus_t;
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 寄存器地址 ==================== */
#define MAX30102_REG_INTR_STATUS_1   0x00
#define MAX30102_REG_INTR_STATUS_2   0x01
#define MAX30102_REG_INTR_ENABLE_1   0x02
#define MAX30102_REG_INTR_ENABLE_2   0x03
#define MAX30102_REG_FIFO_WR_PTR    0x04
#define MAX30102_REG_OVF_COUNTER    0x05
#define MAX30102_REG_FIFO_RD_PTR    0x06
#define MAX30102_REG_FIFO_DATA      0x07
#define MAX30102_REG_FIFO_CONFIG    0x08
#define MAX30102_REG_MODE_CONFIG    0x09
#define MAX30102_REG_SPO2_CONFIG    0x0A
#define MAX30102_REG_LED1_PA        0x0C  /* Red LED 电流 */
#define MAX30102_REG_LED2_PA        0x0D  /* IR LED 电流 */
#define MAX30102_REG_PILOT_PA       0x10
#define MAX30102_REG_MULTI_LED_CTRL1 0x11
#define MAX30102_REG_MULTI_LED_CTRL2 0x12
#define MAX30102_REG_TEMP_INTR      0x1F
#define MAX30102_REG_TEMP_FRAC      0x20
#define MAX30102_REG_TEMP_CONFIG    0x21
#define MAX30102_REG_PROX_INT_THRESH 0x30
#define MAX30102_REG_REV_ID         0xFE
#define MAX30102_REG_PART_ID        0xFF

/* ==================== I2C 地址 ==================== */
#define MAX30102_I2C_ADDR           0x57   /* 7-bit 地址 */
#define MAX30102_I2C_WRITE_ADDR     0xAE   /* 8-bit 写地址 */
#define MAX30102_I2C_READ_ADDR      0xAF   /* 8-bit 读地址 */

/* ==================== 采样配置 ==================== */
#define MAX30102_SAMPLE_RATE        100    /* 100Hz */
#define MAX30102_BUFFER_SIZE        (MAX30102_SAMPLE_RATE * 5)  /* 5 秒缓冲 */

/* ==================== 原始采样数据 ==================== */
typedef struct {
    uint32_t red;           /**< 红光 ADC 值 (18-bit) */
    uint32_t ir;            /**< 红外光 ADC 值 (18-bit) */
} max30102_raw_t;

/* ==================== 传感器配置 ==================== */
typedef struct {
    uint8_t led_current_red;   /**< 红光电流 (0-255, ~0.2mA/step) */
    uint8_t led_current_ir;    /**< 红外光电流 (0-255) */
    uint8_t sample_rate;       /**< 采样率: 50/100/200/400/800/1000 */
    uint8_t pulse_width;       /**< 脉宽: 0=69us, 1=118us, 2=215us, 3=411us */
    uint8_t sample_avg;        /**< 采样平均: 1/2/4/8/16/32 */
} max30102_config_t;

/* ==================== 驱动 API ==================== */

/**
 * @brief 初始化 MAX30102 传感器
 * @return ESP_OK 成功
 */
esp_err_t max30102_init(void);

/**
 * @brief 配置 MAX30102 参数
 * @param config 配置参数
 * @return ESP_OK 成功
 */
esp_err_t max30102_configure(const max30102_config_t *config);

/**
 * @brief 启动连续采样
 * @return ESP_OK 成功
 */
esp_err_t max30102_start(void);

/**
 * @brief 停止采样
 * @return ESP_OK 成功
 */
esp_err_t max30102_stop(void);

/**
 * @brief 读取一个采样点
 * @param raw 输出原始数据
 * @return ESP_OK 成功, ESP_ERR_TIMEOUT 无新数据
 */
esp_err_t max30102_read_sample(max30102_raw_t *raw);

/**
 * @brief 批量读取 FIFO 中的采样数据
 * @param buf 输出缓冲区
 * @param max_count 最大读取数量
 * @return 实际读取数量
 */
uint8_t max30102_read_fifo(max30102_raw_t *buf, uint8_t max_count);

/**
 * @brief 获取 FIFO 中可用采样数
 * @return 可用采样数
 */
uint8_t max30102_get_fifo_count(void);

/**
 * @brief 等待 FIFO 数据就绪 (中断驱动)
 * @param timeout_ms 超时时间 (ms)
 * @return ESP_OK 数据就绪, ESP_ERR_TIMEOUT 超时
 */
esp_err_t max30102_wait_data(uint32_t timeout_ms);

/**
 * @brief 批量读取 FIFO 数据 (单次 I2C 传输, 高效)
 * @param buf 输出缓冲区
 * @param max_count 缓冲区最大容量
 * @return 实际读取的采样数
 */
uint8_t max30102_read_fifo_batch(max30102_raw_t *buf, uint8_t max_count);

/**
 * @brief 设置 LED 电流
 * @param red 红光电流 (0-255)
 * @param ir 红外光电流 (0-255)
 * @return ESP_OK 成功
 */
esp_err_t max30102_set_led_current(uint8_t red, uint8_t ir);

/**
 * @brief 读取温度
 * @param temp_out 输出温度 (°C, 放大 100 倍, 如 3650 = 36.50°C)
 * @return ESP_OK 成功
 */
esp_err_t max30102_read_temperature(int16_t *temp_out);

/**
 * @brief 软复位传感器
 * @return ESP_OK 成功
 */
esp_err_t max30102_reset(void);

/**
 * @brief 检测传感器是否在线
 * @return true 在线
 */
bool max30102_is_present(void);

/**
 * @brief 读取中断状态
 * @param status1 输出中断状态 1
 * @param status2 输出中断状态 2
 * @return ESP_OK 成功
 */
esp_err_t max30102_read_interrupt_status(uint8_t *status1, uint8_t *status2);

/**
 * @brief 获取 GPIO 中断计数（不清零）
 * @return 累计中断次数
 */
uint32_t max30102_get_int_count(void);

/**
 * @brief 读取并清零 GPIO 中断计数
 * @return 本次读取的中断次数
 */
uint32_t max30102_reset_int_count(void);

/**
 * @brief 获取 I2C 主机总线句柄（供其他 I2C 设备共享使用）
 * @return I2C 主机总线句柄，未初始化时返回 NULL
 */
i2c_master_bus_handle_t max30102_get_i2c_bus(void);

/* ==================== 底层寄存器读写（供调试用） ==================== */

esp_err_t max30102_write_reg(uint8_t reg, uint8_t value);
esp_err_t max30102_read_reg(uint8_t reg, uint8_t *value);
esp_err_t max30102_read_regs(uint8_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
