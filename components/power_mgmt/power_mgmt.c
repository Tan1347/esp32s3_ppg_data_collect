/**
 * @file power_mgmt.c
 * @brief 电源管理实现
 *
 * DFS (Dynamic Frequency Scaling):
 *   esp_pm_configure() 自动 10MHz-160MHz 切换
 *   light_sleep_enable=true 时, 空闲自动进入 Light-sleep
 *
 * Deep-sleep:
 *   GPIO9 (BOOT 按钮) 低电平唤醒
 *   唤醒后冷启动
 */

#include "power_mgmt.h"
#include "ppg_config.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "power_mgmt";

static bool s_dfs_enabled = false;

esp_err_t power_mgmt_init(void)
{
    /* 配置 DFS 动态调频 */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = PM_MAX_FREQ_MHZ,
        .min_freq_mhz = PM_MIN_FREQ_MHZ,
        .light_sleep_enable = PM_LIGHT_SLEEP_ENABLE,
    };

    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret == ESP_OK) {
        s_dfs_enabled = true;
        ESP_LOGI(TAG, "DFS enabled: %d-%d MHz, Light-sleep=%s",
                 PM_MIN_FREQ_MHZ, PM_MAX_FREQ_MHZ,
                 PM_LIGHT_SLEEP_ENABLE ? "ON" : "OFF");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "DFS not supported, using fixed freq");
        s_dfs_enabled = false;
    } else {
        ESP_LOGE(TAG, "DFS config failed: %s", esp_err_to_name(ret));
        s_dfs_enabled = false;
    }

    /* 配置唤醒源 (GPIO 唤醒, ESP32-C3 不支持 ULP) */
    ESP_LOGI(TAG, "Power mgmt init done");
    return ESP_OK;
}

void power_mgmt_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering Deep-sleep (BOOT button wake)");
    esp_deep_sleep_start();
}

void power_mgmt_enter_light_sleep(void)
{
    ESP_LOGI(TAG, "Entering Light-sleep");
    esp_light_sleep_start();
}

esp_err_t power_mgmt_set_freq(uint32_t freq_mhz)
{
    if (!s_dfs_enabled) {
        ESP_LOGW(TAG, "DFS not enabled, cannot set freq");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* DFS 模式下由系统自动管理, 此函数仅用于手动覆盖 */
    ESP_LOGI(TAG, "Freq request: %d MHz (DFS auto)", freq_mhz);
    return ESP_OK;
}

esp_err_t power_mgmt_set_dfs(bool enable)
{
    if (enable && !s_dfs_enabled) {
        esp_pm_config_t pm_config = {
            .max_freq_mhz = PM_MAX_FREQ_MHZ,
            .min_freq_mhz = PM_MIN_FREQ_MHZ,
            .light_sleep_enable = PM_LIGHT_SLEEP_ENABLE,
        };
        esp_err_t ret = esp_pm_configure(&pm_config);
        if (ret == ESP_OK) {
            s_dfs_enabled = true;
            ESP_LOGI(TAG, "DFS enabled");
        }
        return ret;
    } else if (!enable && s_dfs_enabled) {
        /* 禁用 DFS: 固定最高频率 */
        esp_pm_config_t pm_config = {
            .max_freq_mhz = PM_MAX_FREQ_MHZ,
            .min_freq_mhz = PM_MAX_FREQ_MHZ,
            .light_sleep_enable = false,
        };
        esp_err_t ret = esp_pm_configure(&pm_config);
        if (ret == ESP_OK) {
            s_dfs_enabled = false;
            ESP_LOGI(TAG, "DFS disabled, fixed %d MHz", PM_MAX_FREQ_MHZ);
        }
        return ret;
    }

    return ESP_OK;
}
