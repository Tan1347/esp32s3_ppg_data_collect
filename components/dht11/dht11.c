/**
 * @file dht11.c
 * @brief DHT11 温湿度传感器驱动
 *
 * 基于单总线协议，使用 esp_timer 高精度计时。
 * 改进点：
 *   - 使用 esp_timer_get_time() 替代忙等计数，时间判断更准确
 *   - 增加超时保护，避免死循环
 *   - 读取前丢弃首个不稳定数据
 */

#include <stdio.h>
#include "dht11.h"
#include "ppg_config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

static const char *TAG = "dht11";

/* 超时阈值（微秒） */
#define DHT_TIMEOUT_US        10000   /* 10ms 超时 */
#define DHT_START_LOW_US      18000   /* 主机拉低 >= 18ms */
#define DHT_START_HIGH_US     40      /* 主机拉高 20~40us */
#define DHT_BIT_THRESHOLD_US  40      /* 高电平 > 40us 为 1，否则为 0 */

esp_err_t dht11_init(void)
{
    /* 初始状态：输出高电平（空闲） */
    gpio_set_direction(DHT11_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_GPIO_PIN, 1);

    ESP_LOGI(TAG, "Init done, GPIO%d", DHT11_GPIO_PIN);
    return ESP_OK;
}

/**
 * @brief 等待 GPIO 达到指定电平，返回等待时间（微秒）
 *
 * @param level    目标电平 (0/1)
 * @param timeout  超时（微秒）
 * @return 等待的微秒数，-1 表示超时
 */
static int64_t wait_for_level(int level, int64_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO_PIN) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1; /* 超时 */
        }
    }
    return esp_timer_get_time() - start;
}

/**
 * @brief 读取单 bit 数据
 *
 * DHT11 协议：
 *   - 主机释放后，传感器先拉低 50us（等待阶段）
 *   - 然后拉高：高电平 26~28us = 0，高电平 70us = 1
 *
 * @param[out] bit_out 读取到的 bit 值
 * @return true 成功，false 超时
 */
static bool read_bit(int *bit_out)
{
    /* 等待低电平结束（传感器的 50us 准备阶段） */
    if (wait_for_level(1, DHT_TIMEOUT_US) < 0) {
        return false;
    }

    /* 测量高电平持续时间 */
    int64_t high_start = esp_timer_get_time();
    if (wait_for_level(0, DHT_TIMEOUT_US) < 0) {
        return false;
    }
    int64_t high_duration = esp_timer_get_time() - high_start;

    *bit_out = (high_duration > DHT_BIT_THRESHOLD_US) ? 1 : 0;
    return true;
}

bool dht11_read(dht11_data_t *data)
{
    if (!data) {
        return false;
    }

    uint8_t buffer[5] = {0};

    /* === 发送启动信号 === */
    gpio_set_direction(DHT11_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_GPIO_PIN, 0);
    ets_delay_us(DHT_START_LOW_US);   /* 拉低 >= 18ms */
    gpio_set_level(DHT11_GPIO_PIN, 1);
    ets_delay_us(DHT_START_HIGH_US);  /* 拉高 20~40us */

    /* 切换为输入模式，等待传感器响应 */
    gpio_set_direction(DHT11_GPIO_PIN, GPIO_MODE_INPUT);

    /* 传感器响应：先拉低 80us，再拉高 80us */
    if (wait_for_level(0, DHT_TIMEOUT_US) < 0) {
        ESP_LOGD(TAG, "Sensor no response (wait low timeout)");
        return false;
    }
    if (wait_for_level(1, DHT_TIMEOUT_US) < 0) {
        ESP_LOGD(TAG, "Sensor response abnormal (wait high timeout)");
        return false;
    }
    /* 跳过传感器的 80us 高电平 */
    if (wait_for_level(0, DHT_TIMEOUT_US) < 0) {
        ESP_LOGD(TAG, "Sensor response abnormal (data start timeout)");
        return false;
    }

    /* === 读取 40 bit 数据 === */
    for (int i = 0; i < 40; i++) {
        int bit = 0;
        if (!read_bit(&bit)) {
            ESP_LOGD(TAG, "Read bit %d timeout", i);
            return false;
        }
        buffer[i / 8] <<= 1;
        if (bit) {
            buffer[i / 8] |= 1;
        }
    }

    /* === 校验 === */
    uint8_t checksum = buffer[0] + buffer[1] + buffer[2] + buffer[3];
    data->humidity    = buffer[0];  /* 湿度整数 */
    data->temperature = buffer[2];  /* 温度整数 */
    data->checksum_ok = (checksum == buffer[4]);

    if (!data->checksum_ok) {
        ESP_LOGW(TAG, "Checksum failed: calc=0x%02X, recv=0x%02X", checksum, buffer[4]);
    }

    return data->checksum_ok;
}
