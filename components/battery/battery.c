/**
 * @file battery.c
 * @brief 电池电压检测实现（带 ADC 校准）
 *
 * 分压: 100K + 100K, 分压比 = 2
 * ADC: GPIO8, ADC_CHANNEL_7, attenuation 12dB (150-2450mV linear range)
 * 并联 100nF X7R 陶瓷电容滤波
 * 64 次均值采样, 使用 eFuse 校准值补偿 Vref 漂移
 * 结果 ×100 返回
 */

#include "battery.h"
#include "ppg_config.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

/* ADC 句柄 */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_initialized = false;
static bool s_calibrated = false;

/* 电量查表曲线 (高压锂电) */
static const struct {
    uint32_t voltage;   /* ×100 */
    uint8_t  soc;
} s_soc_table[] = {
    {420, 100},
    {410, 90},
    {400, 80},
    {390, 65},
    {380, 50},
    {370, 35},
    {360, 20},
    {350, 10},
    {340, 0},
};

/**
 * @brief 初始化 ADC 校准（读取 eFuse 中的 Vref 校准值）
 */
static void init_calibration(void)
{
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };

    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (ret == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibrated (eFuse Vref)");
    } else {
        ESP_LOGW(TAG, "ADC calib failed, using default: %s", esp_err_to_name(ret));
        s_calibrated = false;
    }
}

esp_err_t battery_init(void)
{
    if (s_initialized) return ESP_OK;

    /* 初始化 ADC 单元 */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置 ADC 通道 */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 初始化校准 */
    init_calibration();

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC init done (GPIO4, calib=%s)", s_calibrated ? "yes" : "no");
    return ESP_OK;
}

uint32_t battery_get_voltage(void)
{
    static uint32_t s_last_good = 0;

    if (!s_initialized) return s_last_good;

    int raw_sum = 0;
    int valid_count = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int raw;
        esp_err_t ret = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
        if (ret == ESP_OK && raw > 100) {
            raw_sum += raw;
            valid_count++;
        }
    }

    if (valid_count == 0) return s_last_good;

    int raw_avg = raw_sum / valid_count;

    /* 使用校准 API 将 raw 值转换为 mV */
    int adc_mv = 0;
    if (s_calibrated) {
        adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &adc_mv);
    } else {
        /* 未校准时的回退公式（精度较低） */
        adc_mv = raw_avg * 2500 / 4095;
    }

    /* 乘以分压比 2 = 实际电池电压 (mV) */
    /* 返回 ×100, 即 adc_mv * 2 / 10 */
    uint32_t voltage_x100 = (uint32_t)adc_mv * 2 / 10;

    if (voltage_x100 > 0) {
        s_last_good = voltage_x100;
    }
    return voltage_x100;
}

uint8_t battery_voltage_to_soc(uint32_t voltage_x100)
{
    /* 查表 + 线性插值 */
    int table_size = sizeof(s_soc_table) / sizeof(s_soc_table[0]);

    if (voltage_x100 >= s_soc_table[0].voltage) {
        return 100;
    }

    if (voltage_x100 <= s_soc_table[table_size - 1].voltage) {
        return 0;
    }

    for (int i = 0; i < table_size - 1; i++) {
        if (voltage_x100 >= s_soc_table[i + 1].voltage &&
            voltage_x100 <= s_soc_table[i].voltage) {
            uint32_t v_range = s_soc_table[i].voltage - s_soc_table[i + 1].voltage;
            uint32_t v_offset = s_soc_table[i].voltage - voltage_x100;
            uint8_t soc_range = s_soc_table[i].soc - s_soc_table[i + 1].soc;
            return s_soc_table[i].soc - (uint8_t)(v_offset * soc_range / v_range);
        }
    }

    return 0;
}

bool battery_is_charging(void)
{
    return false;
}

void battery_get_status_str(char *buf, size_t len)
{
    uint32_t voltage = battery_get_voltage();
    uint8_t batt_pct = battery_voltage_to_soc(voltage);
    snprintf(buf, len, "V:%lu.%02luV Batt:%u%%",
             (unsigned long)(voltage / 100), (unsigned long)(voltage % 100), batt_pct);
}
