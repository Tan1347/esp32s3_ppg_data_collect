/**
 * @file max30102.c
 * @brief MAX30102 PPG 传感器驱动实现
 *
 * 使用 ESP-IDF v6.0 新版 I2C 驱动 (driver/i2c_master.h)
 * I2C 地址: 0x57 (7-bit)
 * 通信速率: 400kHz Fast-mode
 * 纯驱动层，不包含算法逻辑
 */

#include "max30102.h"
#include "ppg_config.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "max30102";

/* 预期 Part ID */
#define MAX30102_PART_ID_VALUE      0x15

/* I2C 超时 */
#define I2C_TIMEOUT_MS              100

/* Event group bits */
#define EVT_DATA_READY              BIT0

/* FIFO almost-full threshold: interrupt when FIFO has (32-threshold) samples */
#define MAX30102_FIFO_THRESHOLD     7   /* 25 samples per batch (250ms at 100Hz) */

/* 驱动状态 */
static bool s_initialized = false;
static bool s_measuring = false;
static volatile bool s_polling_mode = false;

/* I2C 主机句柄 */
static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;

/* Event group for interrupt-driven reading */
static EventGroupHandle_t s_event_group = NULL;

/* GPIO 中断回调 — set event bit for task notification */
static void IRAM_ATTR max30102_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_event_group) {
        xEventGroupSetBitsFromISR(s_event_group, EVT_DATA_READY,
                                   &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ========== 底层 I2C 读写 ========== */

esp_err_t max30102_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t max30102_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t max30102_read_reg(uint8_t reg, uint8_t *value)
{
    return max30102_read_regs(reg, value, 1);
}

/* ========== I2C 总线初始化 ========== */

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = PPG_I2C_PORT,
        .sda_io_num = PPG_I2C_SDA_PIN,
        .scl_io_num = PPG_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus_handle), TAG, "I2C bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX30102_I2C_ADDR,
        .scl_speed_hz = PPG_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle),
                        TAG, "I2C add device failed");

    return ESP_OK;
}

/* ========== 公共 API ========== */

esp_err_t max30102_init(void)
{
    if (s_initialized) return ESP_OK;

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "I2C bus init failed");

    /* Verify sensor presence */
    if (!max30102_is_present()) {
        ESP_LOGE(TAG, "MAX30102 not found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Configure GPIO interrupt (MAX30102 INT, falling edge) */
    gpio_config_t int_conf = {
        .pin_bit_mask = (1ULL << MAX30102_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_conf), TAG, "GPIO config failed");
    gpio_install_isr_service(0);
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(MAX30102_INT_PIN, max30102_isr_handler, NULL),
                        TAG, "GPIO ISR add failed");
    ESP_LOGI(TAG, "GPIO%d interrupt configured", MAX30102_INT_PIN);

    /* Default configuration */
    max30102_config_t default_cfg = {
        .led_current_red = 0x24,    /* ~7mA */
        .led_current_ir = 0x24,     /* ~7mA */
        .sample_rate = 100,         /* 100Hz */
        .pulse_width = 3,           /* 411us, 18-bit */
        .sample_avg = 1,            /* No averaging */
    };
    ESP_RETURN_ON_ERROR(max30102_configure(&default_cfg), TAG, "Configure failed");

    /* Create event group for interrupt-driven reading */
    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
    }

    s_initialized = true;
    puts("[MAX30102] Init done");
    return ESP_OK;
}

esp_err_t max30102_configure(const max30102_config_t *config)
{
    /* Interrupt config: A_FULL + PPG_RDY */
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_INTR_ENABLE_1, 0xC0),
                        TAG, "Set interrupt enable 1 failed");
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_INTR_ENABLE_2, 0x00),
                        TAG, "Set interrupt enable 2 failed");

    /* Clear interrupt status */
    uint8_t dummy;
    max30102_read_reg(MAX30102_REG_INTR_STATUS_1, &dummy);
    max30102_read_reg(MAX30102_REG_INTR_STATUS_2, &dummy);

    /* FIFO config: sample_avg, rollover=0, almost_full=threshold */
    uint8_t fifo_cfg = ((config->sample_avg & 0x07) << 5) | (MAX30102_FIFO_THRESHOLD & 0x0F);
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_FIFO_CONFIG, fifo_cfg),
                        TAG, "Set FIFO config failed");

    /* Mode: SpO2 */
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_MODE_CONFIG, 0x03),
                        TAG, "Set SpO2 mode failed");

    /* SpO2 config: ADC range, sample rate, pulse width */
    uint8_t spo2_sr;
    uint16_t sr = config->sample_rate;
    if (sr <= 50)       spo2_sr = 0x00;
    else if (sr <= 100) spo2_sr = 0x01;
    else if (sr <= 200) spo2_sr = 0x02;
    else if (sr <= 400) spo2_sr = 0x03;
    else if (sr <= 800) spo2_sr = 0x04;
    else                spo2_sr = 0x05;

    uint8_t spo2_cfg = (0x01 << 5) | (spo2_sr << 2) | 0x03;  /* 4096nA, rate, 411us */
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_SPO2_CONFIG, spo2_cfg),
                        TAG, "Set SpO2 config failed");

    /* LED current */
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_LED1_PA, config->led_current_red),
                        TAG, "Set RED LED current failed");
    ESP_RETURN_ON_ERROR(max30102_write_reg(MAX30102_REG_LED2_PA, config->led_current_ir),
                        TAG, "Set IR LED current failed");

    ESP_LOGI(TAG, "Config: %dHz, avg=%d, IR=0x%02X R=0x%02X",
             config->sample_rate, config->sample_avg,
             config->led_current_ir, config->led_current_red);
    return ESP_OK;
}

esp_err_t max30102_start(void)
{
    /* Restore LED currents before entering SpO2 mode */
    esp_err_t ret = max30102_set_led_current(0x24, 0x24);
    if (ret != ESP_OK) {
        printf("[MAX30102] Restore LED currents failed: 0x%x\n", ret);
        return ret;
    }

    /* SpO2 mode */
    ret = max30102_write_reg(MAX30102_REG_MODE_CONFIG, 0x03);
    if (ret == ESP_OK) {
        s_measuring = true;
        puts("[MAX30102] Started (SpO2 mode)");
    }
    return ret;
}

esp_err_t max30102_stop(void)
{
    /* Turn off LED currents first (hardware-level LED shutdown) */
    esp_err_t ret = max30102_set_led_current(0, 0);
    if (ret != ESP_OK) {
        printf("[MAX30102] Zero LED currents failed: 0x%x\n", ret);
    }

    /* Enter shutdown mode */
    ret = max30102_write_reg(MAX30102_REG_MODE_CONFIG, 0x80);
    if (ret == ESP_OK) {
        s_measuring = false;
        puts("[MAX30102] Shutdown OK");
    } else {
        printf("[MAX30102] Shutdown FAILED: 0x%x\n", ret);
    }
    return ret;
}

esp_err_t max30102_read_sample(max30102_raw_t *raw)
{
    if (!s_measuring) return ESP_ERR_INVALID_STATE;

    /* 读取 FIFO 数据 (6 bytes: RED[3] + IR[3]) */
    uint8_t fifo_data[6];
    esp_err_t ret = max30102_read_regs(MAX30102_REG_FIFO_DATA, fifo_data, 6);
    if (ret != ESP_OK) return ret;

    /* 解析 18-bit 数据 */
    raw->red = ((uint32_t)fifo_data[0] << 16) |
               ((uint32_t)fifo_data[1] << 8) |
               ((uint32_t)fifo_data[2]);
    raw->ir = ((uint32_t)fifo_data[3] << 16) |
              ((uint32_t)fifo_data[4] << 8) |
              ((uint32_t)fifo_data[5]);

    /* 掩码高 6 位 (18-bit 有效) */
    raw->red &= 0x03FFFF;
    raw->ir &= 0x03FFFF;

    return ESP_OK;
}

uint8_t max30102_read_fifo(max30102_raw_t *buf, uint8_t max_count)
{
    uint8_t count = max30102_get_fifo_count();
    if (count > max_count) count = max_count;
    if (count == 0) return 0;

    /* 清空中断状态 */
    uint8_t dummy;
    max30102_read_reg(MAX30102_REG_INTR_STATUS_1, &dummy);
    max30102_read_reg(MAX30102_REG_INTR_STATUS_2, &dummy);

    uint8_t read_count = 0;
    for (int i = 0; i < count; i++) {
        if (max30102_read_sample(&buf[i]) == ESP_OK) {
            read_count++;
        }
    }

    return read_count;
}

uint8_t max30102_get_fifo_count(void)
{
    uint8_t wr_ptr, rd_ptr;
    if (max30102_read_reg(MAX30102_REG_FIFO_WR_PTR, &wr_ptr) != ESP_OK) return 0;
    if (max30102_read_reg(MAX30102_REG_FIFO_RD_PTR, &rd_ptr) != ESP_OK) return 0;
    return (wr_ptr - rd_ptr) & 0x1F;
}

esp_err_t max30102_wait_data(uint32_t timeout_ms)
{
    if (!s_event_group) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_event_group, EVT_DATA_READY,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & EVT_DATA_READY) ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint8_t max30102_read_fifo_batch(max30102_raw_t *buf, uint8_t max_count)
{
    /* Always clear interrupt status first — even if FIFO is empty.
     * Otherwise INT pin stays LOW permanently, blocking new falling edges. */
    uint8_t dummy;
    max30102_read_reg(MAX30102_REG_INTR_STATUS_1, &dummy);
    max30102_read_reg(MAX30102_REG_INTR_STATUS_2, &dummy);

    uint8_t count = max30102_get_fifo_count();
    if (count == 0) return 0;
    if (count > max_count) count = max_count;

    /* Batch read: 6 bytes per sample (RED[3] + IR[3]) */
    uint8_t raw_buf[6 * 32];  /* Max 32 samples */
    size_t bytes = count * 6;
    esp_err_t ret = max30102_read_regs(MAX30102_REG_FIFO_DATA, raw_buf, bytes);
    if (ret != ESP_OK) return 0;

    /* Parse 18-bit samples */
    for (int i = 0; i < count; i++) {
        int off = i * 6;
        buf[i].red = ((uint32_t)raw_buf[off] << 16) |
                     ((uint32_t)raw_buf[off + 1] << 8) |
                     ((uint32_t)raw_buf[off + 2]);
        buf[i].ir = ((uint32_t)raw_buf[off + 3] << 16) |
                    ((uint32_t)raw_buf[off + 4] << 8) |
                    ((uint32_t)raw_buf[off + 5]);
        buf[i].red &= 0x03FFFF;
        buf[i].ir &= 0x03FFFF;
    }

    return count;
}

esp_err_t max30102_set_led_current(uint8_t red, uint8_t ir)
{
    esp_err_t ret = max30102_write_reg(MAX30102_REG_LED1_PA, red);
    if (ret != ESP_OK) return ret;
    return max30102_write_reg(MAX30102_REG_LED2_PA, ir);
}

esp_err_t max30102_read_temperature(int16_t *temp_out)
{
    /* 启动温度测量 */
    esp_err_t ret = max30102_write_reg(MAX30102_REG_TEMP_CONFIG, 0x01);
    if (ret != ESP_OK) return ret;

    /* 等待测量完成 */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t temp_int, temp_frac;
    ret = max30102_read_reg(MAX30102_REG_TEMP_INTR, &temp_int);
    if (ret != ESP_OK) return ret;
    ret = max30102_read_reg(MAX30102_REG_TEMP_FRAC, &temp_frac);
    if (ret != ESP_OK) return ret;

    /* 温度 = 整数 + 小数/16，放大 100 倍 */
    int8_t sign = (temp_int & 0x80) ? -1 : 1;
    *temp_out = sign * ((abs((int8_t)temp_int)) * 100 + (temp_frac * 100 / 16));

    return ESP_OK;
}

esp_err_t max30102_reset(void)
{
    return max30102_write_reg(MAX30102_REG_MODE_CONFIG, 0x40);
}

bool max30102_is_present(void)
{
    uint8_t part_id;
    esp_err_t ret = max30102_read_reg(MAX30102_REG_PART_ID, &part_id);
    if (ret != ESP_OK) return false;
    ESP_LOGI(TAG, "MAX30102 Part ID: 0x%02X", part_id);
    return (part_id == MAX30102_PART_ID_VALUE);
}

esp_err_t max30102_read_interrupt_status(uint8_t *status1, uint8_t *status2)
{
    esp_err_t ret = max30102_read_reg(MAX30102_REG_INTR_STATUS_1, status1);
    if (ret != ESP_OK) return ret;
    return max30102_read_reg(MAX30102_REG_INTR_STATUS_2, status2);
}

uint32_t max30102_get_int_count(void)
{
    if (!s_event_group) return 0;
    return (xEventGroupGetBits(s_event_group) & EVT_DATA_READY) ? 1 : 0;
}

uint32_t max30102_reset_int_count(void)
{
    if (!s_event_group) return 0;
    EventBits_t bits = xEventGroupClearBits(s_event_group, EVT_DATA_READY);
    return (bits & EVT_DATA_READY) ? 1 : 0;
}

/* ========== Read mode switching ========== */

void max30102_set_polling_mode(bool enable)
{
    s_polling_mode = enable;

    /* Disable/enable MAX30102 interrupt output (INTR_ENABLE_1).
     * GPIO ISR stays installed but won't fire when interrupts are disabled. */
    if (enable) {
        max30102_write_reg(MAX30102_REG_INTR_ENABLE_1, 0x00);
        /* Clear any stale event bit */
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, EVT_DATA_READY);
        }
        puts("[MAX30102] Polling mode ON");
    } else {
        max30102_write_reg(MAX30102_REG_INTR_ENABLE_1, 0xC0);
        /* Clear any stale event bit */
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, EVT_DATA_READY);
        }
        puts("[MAX30102] Interrupt mode ON");
    }
}

bool max30102_is_polling_mode(void)
{
    return s_polling_mode;
}

i2c_master_bus_handle_t max30102_get_i2c_bus(void)
{
    return s_bus_handle;
}
