/**
 * @file main.c
 * @brief PPG signal acquisition tool - ESP32-S3 main entry
 *
 * Architecture:
 * - Resident tasks: button1_task, sys_led_task (always running)
 * - Collection tasks: ppg_task, power_task (created on demand)
 * - Main loop: lightweight state check, non-blocking
 *
 * State machine:
 *   DEEP_SLEEP -> GPIO12 wake -> STANDALONE -> BLE/WiFi -> DEEP_SLEEP
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"

#include "ppg_config.h"
#include "power_mgmt.h"
#include "max30102.h"
#include "battery.h"
#include "ppg_algo.h"
#include "sd_storage.h"
#include "ble_svc.h"
#include "wifi_prov.h"
#include "wifi_transfer.h"
#include "ppg_log.h"
#include "ota_upgrade.h"
#include "uart_recorder.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

/* System state (protected by spinlock for thread safety) */
static system_state_t s_system_state = STATE_STANDALONE;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static const char *s_sleep_reason = "";  /* Deep-sleep reason for logging */

/* Event group */
static EventGroupHandle_t s_system_event_group;
#define EVT_MEASURING_DONE   BIT0
#define EVT_WIFI_DONE        BIT1
#define EVT_BLE_CONNECTED    BIT2
#define EVT_SHUTDOWN         BIT4

/* BLE/WiFi initialization status */
static bool s_ble_initialized = false;
static bool s_wifi_initialized = false;

/* PPG collection status (for LED task) */
static volatile bool s_ppg_collecting = false;

/* Bad signal flag: set by ppg_task when data invalid > BAD_SIGNAL_TIMEOUT_SEC */
static volatile bool s_bad_signal = false;

/* Collection task handles */
static TaskHandle_t s_ppg_task_handle = NULL;
static TaskHandle_t s_power_task_handle = NULL;
static TaskHandle_t s_button1_task_handle = NULL;

/* PPG algorithm context (in BSS to avoid stack overflow) */
static ppg_algo_ctx_t s_algo_ctx;
static ppg_algo_result_t s_algo_result;

/* Forward declarations */
static void ppg_task(void *arg);
static void power_task(void *arg);
static void button1_task(void *arg);
static void sys_led_task(void *arg);
static void request_deep_sleep(const char *reason);

/* LED control flag (true = LED task runs, false = LED forced off) */
static volatile bool s_led_active = true;

/* ========== Lazy initialization ========== */

/* Adapter: ble_uart_config_t -> uart_recorder_config_t (identical layout) */
static esp_err_t ble_uart_record_start(const ble_uart_config_t *cfg)
{
    const uart_recorder_config_t *rec = (const uart_recorder_config_t *)cfg;
    return uart_recorder_start(rec);
}

static void ble_uart_record_stop(void)
{
    uart_recorder_stop();
}

static const ble_callbacks_t s_ble_cbs = {
    .set_state = system_set_state,
    .get_voltage = battery_get_voltage,
    .voltage_to_soc = battery_voltage_to_soc,
    .wifi_add = wifi_prov_add,
    .wifi_clear_all = wifi_prov_clear_all,
    .wifi_delete = wifi_prov_delete,
    .wifi_get_list_json = wifi_prov_get_list_json,
    .wifi_get_detail_json = wifi_prov_get_detail_json,
    .wifi_is_connected = wifi_prov_is_connected,
    .wifi_get_ip = wifi_prov_get_ip,
    .sd_get_file_list = sd_storage_get_file_list,
    .sd_get_free_mb = sd_storage_get_free_space_mb,
    .sd_get_total_mb = sd_storage_get_total_space_mb,
    .log_set_level = ppg_log_set_level,
    .log_get_level = ppg_log_get_level,
    .log_get_buffer_count = ppg_log_get_buffer_count,
    .uart_record_start = ble_uart_record_start,
    .uart_record_stop = ble_uart_record_stop,
};

static const http_callbacks_t s_http_cbs = {
    .set_state = system_set_state,
    .get_voltage = battery_get_voltage,
    .voltage_to_soc = battery_voltage_to_soc,
    .wifi_auto_connect = wifi_prov_auto_connect,
    .wifi_is_connected = wifi_prov_is_connected,
    .wifi_get_ip = wifi_prov_get_ip,
    .wifi_disconnect = wifi_prov_disconnect,
    .sd_get_file_list = sd_storage_get_file_list,
    .sd_get_free_mb = sd_storage_get_free_space_mb,
    .ota_get_version = ota_upgrade_get_current_version,
    .ota_get_build_time = ota_upgrade_get_build_time,
    .ota_upgrade_from_http = ota_upgrade_from_http,
};

static esp_err_t ensure_ble_init(void)
{
    if (s_ble_initialized) return ESP_OK;
    puts("BLE init (lazy)...");
    esp_err_t ret = ble_svc_init(&s_ble_cbs);
    if (ret == ESP_OK) {
        s_ble_initialized = true;
        puts("BLE init done");
    }
    return ret;
}

static esp_err_t ensure_wifi_init(void)
{
    if (s_wifi_initialized) return ESP_OK;
    puts("WiFi init (lazy)...");
    esp_err_t ret = wifi_prov_init();
    if (ret == ESP_OK) {
        s_wifi_initialized = true;
        uint8_t mac[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            printf("WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    return ret;
}

/* ========== GPIO wake-up check ========== */

static bool is_gpio_wakeup(void)
{
    uint32_t causes = esp_sleep_get_wakeup_causes();
    return (causes & (1 << ESP_SLEEP_WAKEUP_GPIO));
}

/* ========== LED GPIO init ========== */

static void led_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SYS_LED_PIN) | (1ULL << PPG_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(SYS_LED_PIN, 0);
    gpio_set_level(PPG_LED_PIN, 0);
}

/* ========== Collection task management ========== */

static void start_collection_tasks(void)
{
    if (s_ppg_task_handle == NULL) {
        xTaskCreate(ppg_task, "ppg", STACK_PPG_TASK, NULL, 6, &s_ppg_task_handle);
    }
    if (s_power_task_handle == NULL) {
        xTaskCreate(power_task, "power", STACK_POWER, NULL, 1, &s_power_task_handle);
    }
}

static void stop_collection_tasks(void)
{
    /* Wait for tasks to exit (they check s_system_state and self-delete) */
    TaskHandle_t handles[] = { s_ppg_task_handle, s_power_task_handle };
    int num_handles = sizeof(handles) / sizeof(handles[0]);
    for (int i = 0; i < num_handles; i++) {
        if (handles[i] != NULL) {
            for (int retry = 0; retry < 50 && eTaskGetState(handles[i]) != eDeleted; retry++) {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_task_wdt_reset();
            }
        }
    }
    s_ppg_task_handle = NULL;
    s_power_task_handle = NULL;
}

/* ========== System initialization ========== */

#define INIT_CHECK(func, name) do { \
    esp_err_t _ret = (func); \
    if (_ret != ESP_OK) { printf("[INIT] %s failed\n", name); } \
} while(0)

static esp_err_t system_init(void)
{
    puts("[INIT] System init...");

    puts("[1/7] NVS init...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        puts("NVS init failed, erase retry");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) puts("NVS init failed");

    puts("[2/7] Netif init...");
    esp_netif_init();
    esp_event_loop_create_default();

    INIT_CHECK(ppg_log_init(), "Log system");
    INIT_CHECK(power_mgmt_init(), "Power mgmt");
    INIT_CHECK(battery_init(), "Battery");
    INIT_CHECK(sd_storage_init(), "SD storage");

    /* Try mount TF card at boot and print usage info */
    if (sd_storage_mount() == ESP_OK) {
        uint32_t total_mb = sd_storage_get_total_space_mb();
        uint32_t free_mb = sd_storage_get_free_space_mb();
        uint32_t used_mb = total_mb - free_mb;
        unsigned long pct = total_mb > 0 ? (used_mb * 100 / total_mb) : 0;
        printf("[INIT] TF card mounted: %luMB used / %luMB total (%lu%% used)\n",
               (unsigned long)used_mb, (unsigned long)total_mb, pct);
    } else {
        puts("[INIT] TF card not available");
    }

    INIT_CHECK(max30102_init(), "MAX30102");
    INIT_CHECK(ota_upgrade_init(NULL), "OTA");
    INIT_CHECK(uart_recorder_init(), "UART recorder");

    /* DFS enabled, but auto light-sleep disabled (tick conflict on S3) */
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 10,
        .light_sleep_enable = PM_LIGHT_SLEEP_ENABLE,  /* false */
    };
    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        puts("[INIT] DFS enabled (10-240MHz), manual light-sleep");
    } else {
        printf("[INIT] DFS failed: %s\n", esp_err_to_name(pm_ret));
    }

    if (ota_upgrade_pending_confirm()) {
        puts("[OTA] New firmware detected, starting 60s stability check...");
        /* Wait 60s to confirm firmware is stable.
         * If the device crashes during this period, bootloader will auto-rollback. */
        for (int i = 60; i > 0; i--) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
            if (i % 10 == 0) {
                printf("[OTA] Stability check: %ds remaining...\n", i);
            }
        }
        ota_upgrade_confirm();
        puts("[OTA] Firmware confirmed stable, rollback cleared");
    }

    puts("[INIT] System init done");

    /* Shutdown MAX30102 to save power until collection starts */
    max30102_stop();
    puts("[INIT] MAX30102 shutdown (power save)");

    return ESP_OK;
}

/* ========== Resident tasks ========== */

/**
 * @brief System status LED task (GPIO10)
 * Blink pattern: ON for s_led_on_ms, OFF for s_led_off_ms
 */
static volatile int s_led_on_ms = 500;
static volatile int s_led_off_ms = 500;

static void sys_led_task(void *arg)
{
    bool wdt_ok = (esp_task_wdt_add(NULL) == ESP_OK);

    while (1) {
        if (s_led_active) {
            int on_ms = s_led_on_ms;
            int off_ms = s_led_off_ms;
            gpio_set_level(SYS_LED_PIN, 1);
            for (int rem = on_ms; rem > 0; rem -= 1000) {
                vTaskDelay(pdMS_TO_TICKS(rem > 1000 ? 1000 : rem));
                if (wdt_ok) esp_task_wdt_reset();
            }
            gpio_set_level(SYS_LED_PIN, 0);
            for (int rem = off_ms; rem > 0; rem -= 1000) {
                vTaskDelay(pdMS_TO_TICKS(rem > 1000 ? 1000 : rem));
                if (wdt_ok) esp_task_wdt_reset();
            }
        } else {
            gpio_set_level(SYS_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (wdt_ok) esp_task_wdt_reset();
        }
    }
}

/**
 * @brief PPG collection status LED task
 * Uses PPG_LED (GPIO11) to indicate collection status
 */
static void ppg_led_task(void *arg)
{
    while (1) {
        if (!s_ppg_collecting) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t count = max30102_reset_int_count();
        int blink_ms;

        if (count == 0) {
            blink_ms = 0;
        } else if (count < PPG_LED_RATE_OFF) {
            blink_ms = PPG_LED_BLINK_SLOW_MS;
        } else if (count < PPG_LED_RATE_SLOW) {
            blink_ms = PPG_LED_BLINK_MED_MS;
        } else if (count < PPG_LED_RATE_FAST) {
            blink_ms = PPG_LED_BLINK_FAST_MS;
        } else {
            blink_ms = PPG_LED_BLINK_MAX_MS;
        }

        if (blink_ms > 0) {
            gpio_set_level(PPG_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(blink_ms));
            gpio_set_level(PPG_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(blink_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/**
 * @brief BUTTON1 monitor task
 * Single click: toggle mode, Double click: WiFi, Long press: BLE
 */
static void button1_task(void *arg)
{
    puts("[BUTTON1] Monitor task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Check if button pressed */
        if (gpio_get_level(BUTTON1_GPIO) != 0) {
            continue;
        }

        /* Button pressed, start timing */
        puts("[BUTTON1] Pressed, detecting...");
        int press_ms = 0;
        bool long_pressed = false;
        while (gpio_get_level(BUTTON1_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            press_ms += 100;

            /* Long press 3s: BLE pairing */
            if (press_ms >= TIMEOUT_BUTTON_LONG_PRESS) {
                printf("[BUTTON1] Long press (%dms) -> BLE pairing\n", press_ms);
                while (gpio_get_level(BUTTON1_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                system_set_state(STATE_BLE_PAIRING);
                long_pressed = true;
                break;
            }
        }

        if (long_pressed) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Short press released, wait for double click */
        printf("[BUTTON1] Short press (%dms), wait double click...\n", press_ms);
        int wait_ms = 0;
        bool double_click = false;
        while (wait_ms < 500) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
            if (gpio_get_level(BUTTON1_GPIO) == 0) {
                double_click = true;
                while (gpio_get_level(BUTTON1_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }
        }

        if (double_click) {
            puts("[BUTTON1] Double click -> WiFi mode");
            system_set_state(STATE_WIFI_STA);
        } else {
            /* Single click: toggle mode */
            puts("[BUTTON1] Single click -> toggle mode");
            if (system_get_state() == STATE_STANDALONE) {
                system_set_state(STATE_MEASURING);
                puts("Mode: STANDALONE -> MEASURING");
            } else if (system_get_state() == STATE_MEASURING) {
                system_set_state(STATE_STANDALONE);
                puts("Mode: MEASURING -> STANDALONE");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ========== Collection tasks ========== */

/**
 * @brief PPG data collection task
 */
static void ppg_task(void *arg)
{
    puts("[PPG] Task started");

    /* High frequency for PPG data processing */
    power_mgmt_set_freq(PM_MAX_FREQ_MHZ);

    ppg_algo_init(&s_algo_ctx);
    max30102_start();
    s_ppg_collecting = true;

    int no_data_sec = 0;
    int invalid_sec = 0;  /* Consecutive seconds with invalid result */
    int64_t last_data_time = esp_timer_get_time();
    int total_samples = 0;  /* For battery check */

    /* Batch buffer for interrupt-driven reading */
    max30102_raw_t batch_buf[32];

    while (s_system_state == STATE_MEASURING || s_system_state == STATE_STANDALONE) {
        /* Wait for MAX30102 interrupt (FIFO data ready) */
        esp_err_t ret = max30102_wait_data(1000);
        if (ret != ESP_OK) {
            int64_t now = esp_timer_get_time();
            no_data_sec = (int)((now - last_data_time) / 1000000);
            if (no_data_sec >= 60) {
                puts("[PPG] No data for 1min, stop");
                break;
            }
            continue;
        }

        /* Batch read all available samples */
        uint8_t count = max30102_read_fifo_batch(batch_buf, 32);
        if (count == 0) continue;

        last_data_time = esp_timer_get_time();
        no_data_sec = 0;

        /* Process each sample */
        for (int i = 0; i < count; i++) {
            sd_storage_write_raw((const sd_raw_record_t *)&batch_buf[i]);
            ppg_algo_add_sample(&s_algo_ctx, batch_buf[i].red, batch_buf[i].ir);
            total_samples++;
        }

#if BATTERY_CHECK_ENABLE
        if (total_samples >= 100) {
            total_samples = 0;
            uint8_t batt_pct = battery_voltage_to_soc(battery_get_voltage());
            if (batt_pct < BATTERY_PPG_MIN_SOC) {
                printf("[PPG] Battery low (%d%%), stop\n", batt_pct);
                break;
            }
        }
#endif

        /* Process algorithm (every ~5s when buffer full) */
        if (ppg_algo_process(&s_algo_ctx, &s_algo_result)) {
            sd_storage_write_csv(&s_algo_result);
            ble_svc_notify_live_data(&s_algo_result);

#if PPG_DEBUG_ENABLE
            printf("[PPG] HR=%ld(%c) SpO2=%ld(%c) Q=%d PK=%d IR_DC=%lu IR_Amp=%lu RED_DC=%lu R=%ld\n",
                   (long)s_algo_result.heart_rate, s_algo_result.hr_valid ? 'V' : 'I',
                   (long)s_algo_result.spo2, s_algo_result.spo2_valid ? 'V' : 'I',
                   s_algo_result.quality,
                   s_algo_result.peak_count,
                   (unsigned long)s_algo_result.ir_dc_mean,
                   (unsigned long)s_algo_result.ir_amplitude,
                   (unsigned long)s_algo_result.red_dc_mean,
                   (long)s_algo_result.spo2_ratio);
#endif

            /* Check data validity (perfusion ratio) */
            bool data_valid = s_algo_result.hr_valid || s_algo_result.spo2_valid;
#if PPG_DEBUG_ENABLE
            /* Also reject if IR amplitude too low (no finger) */
            if (s_algo_result.ir_amplitude < 500) data_valid = false;
#endif
            if (!data_valid) {
                invalid_sec += 5;  /* Algorithm processes every 5s */
                printf("[PPG] Invalid data, %ds/%ds\n", invalid_sec, BAD_SIGNAL_TIMEOUT_SEC);
                if (invalid_sec >= BAD_SIGNAL_TIMEOUT_SEC) {
                    puts("[PPG] Bad signal for 10s, stopping");
                    s_bad_signal = true;
                    break;
                }
            } else {
                invalid_sec = 0;  /* Reset on valid data */
            }
        }
    }

    max30102_stop();
    s_ppg_collecting = false;

    /* Low frequency after PPG collection ends */
    power_mgmt_set_freq(PM_MIN_FREQ_MHZ);

    puts("[PPG] Task ended");
    s_ppg_task_handle = NULL;
    xEventGroupSetBits(s_system_event_group, EVT_MEASURING_DONE);
    vTaskDelete(NULL);
}

/**
 * @brief Power monitoring task
 */
static void power_task(void *arg)
{
#if BATTERY_CHECK_ENABLE
    int low_voltage_count = 0;
#endif

    while (s_system_state == STATE_MEASURING || s_system_state == STATE_STANDALONE) {
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_heap = esp_get_minimum_free_heap_size();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t voltage = battery_get_voltage();
        uint8_t batt_pct = battery_voltage_to_soc(voltage);
        printf("free_heap %luKB (min %luKB), free_psram %luKB, batt %lu.%02luV %d%%\n",
               (unsigned long)(free_heap / 1024),
               (unsigned long)(min_heap / 1024),
               (unsigned long)(free_psram / 1024),
               (unsigned long)(voltage / 100), (unsigned long)(voltage % 100),
               batt_pct);

        ble_svc_update_status(batt_pct, voltage);

        /* Low voltage protection: 3 consecutive readings < 3.3V */
#if BATTERY_CHECK_ENABLE
        if (voltage < 330) {
            low_voltage_count++;
            printf("[POWER] Battery low (%lu.%02luV), count=%d/3\n",
                   (unsigned long)(voltage / 100), (unsigned long)(voltage % 100), low_voltage_count);
            if (low_voltage_count >= 3) {
                request_deep_sleep("battery low");
                xEventGroupSetBits(s_system_event_group, EVT_SHUTDOWN);
                break;
            }
        } else {
            low_voltage_count = 0;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    s_power_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ========== BLE/WiFi modes ========== */

/**
 * @brief Poll until condition is true or timeout
 * @return true if condition met, false if timeout
 */
static bool poll_until(bool (*cond)(void), int timeout_sec, const char *label)
{
    for (int i = 0; i < timeout_sec; i++) {
        if (cond()) return true;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i % 10 == 0 && i > 0) {
            printf("[POLL] %s: %d/%ds\n", label, i, timeout_sec);
        }
    }
    printf("[POLL] %s timeout (%ds)\n", label, timeout_sec);
    return false;
}

static void enter_ble_pairing(void)
{
    if (ensure_ble_init() != ESP_OK) {
        request_deep_sleep("BLE init failed");
        return;
    }

    bool is_wakeup = is_gpio_wakeup();
    int timeout_sec = is_wakeup ? (TIMEOUT_BLE_PAIR_WAKEUP / 1000) : (TIMEOUT_BLE_PAIR_COLDBOOT / 1000);

    puts("BLE advertising started");
    ble_svc_start_advertising();

    bool connected = poll_until(ble_svc_is_connected, timeout_sec, "BLE connect");

    if (connected) {
        puts("BLE connected");
        system_set_state(STATE_BLE_CONNECTED);
    } else {
        request_deep_sleep("BLE timeout");
    }
}

static void enter_wifi_mode(void)
{
    /* Release SD write buffers to free memory for WiFi (~24KB) */
    printf("[WIFI] Before release: free_heap=%luKB mounted=%d\n",
           (unsigned long)(esp_get_free_heap_size() / 1024),
           sd_storage_is_mounted());
    sd_storage_release_buffers();
    printf("[WIFI] After release: free_heap=%luKB\n",
           (unsigned long)(esp_get_free_heap_size() / 1024));

    if (ensure_wifi_init() != ESP_OK) {
        request_deep_sleep("WiFi init failed");
        return;
    }

#if BATTERY_CHECK_ENABLE
    uint8_t batt_pct = battery_voltage_to_soc(battery_get_voltage());
    if (batt_pct < BATTERY_WIFI_MIN_SOC) {
        printf("Battery low (%d%%), WiFi disabled\n", batt_pct);
        request_deep_sleep("battery low for WiFi");
        return;
    }
#endif

    esp_err_t ret = wifi_prov_auto_connect();
    if (ret != ESP_OK) {
        request_deep_sleep("WiFi start failed");
        return;
    }

    /* Wait for connection */
    bool connected = poll_until(wifi_prov_is_connected, (TIMEOUT_WIFI_CONNECT / 1000), "WiFi connect");

    if (!connected) {
        request_deep_sleep("WiFi connect timeout");
        return;
    }

    /* Connected */
    char ip[16];
    wifi_prov_get_ip(ip, sizeof(ip));
    printf("========================================\n");
    printf("  WiFi Connected!\n");
    printf("  IP Address: %s\n", ip);
    printf("========================================\n");

    /* Get network time */
    esp_http_client_config_t http_cfg = {
        .url = "http://ip.ddnspod.com/timestamp",
        .timeout_ms = TIMEOUT_WDT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            char buf[32] = {0};
            int read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
            if (read_len > 0) {
                uint64_t ts_ms = strtoull(buf, NULL, 10);
                uint32_t timestamp = (uint32_t)(ts_ms / 1000);
                if (timestamp > MIN_VALID_TIMESTAMP) {
                    printf("Timestamp: %lu\n", (unsigned long)timestamp);
                    struct timeval tv = { .tv_sec = timestamp, .tv_usec = 0 };
                    settimeofday(&tv, NULL);
                    puts("System time updated");
                }
            }
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
    }

    /* Start HTTP server */
    wifi_transfer_start(&s_http_cbs);

    /* Maintain connection for 1 minute (auto-close if no activity) */
    puts("WiFi active, auto-close in 60s if no activity");
    int maintain_sec = 0;
    while (maintain_sec < 60) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
        maintain_sec++;

        if (maintain_sec % 10 == 0) {
            printf("WiFi running... %d/60s\n", maintain_sec);
        }

        if (!wifi_prov_is_connected()) {
            puts("WiFi disconnected");
            break;
        }
    }

    /* Close WiFi */
    request_deep_sleep("WiFi idle timeout");
}

/* ========== Deep sleep ========== */

/**
 * @brief Request deep-sleep with given reason
 * Sets state and logs reason. Actual sleep happens in enter_deep_sleep().
 */
static void request_deep_sleep(const char *reason)
{
    printf("[SLEEP] Requesting deep-sleep: %s\n", reason);
    s_sleep_reason = reason;
    system_set_state(STATE_DEEP_SLEEP);
}

/**
 * @brief Unified deep-sleep entry with full resource cleanup
 * Called from main_loop when s_system_state == STATE_DEEP_SLEEP
 */
static void enter_deep_sleep(void)
{
    printf("Entering Deep-sleep: %s (press BOOT to cancel in 3s)\n", s_sleep_reason);

    /* Low frequency before sleep */
    power_mgmt_set_freq(PM_MIN_FREQ_MHZ);

    /* Stop BLE */
    if (s_ble_initialized) {
        ble_svc_stop_advertising();
        puts("[SLEEP] BLE stopped");
    }

    /* Stop WiFi */
    if (s_wifi_initialized) {
        wifi_transfer_stop();
        puts("[SLEEP] WiFi stopped");
    }

    /* Stop sensors */
    max30102_stop();

    /* Flush logs */
    ppg_log_flush();

    /* Flush and unmount SD card */
    if (sd_storage_is_mounted()) {
        puts("[SLEEP] Flushing TF card...");
        sd_storage_flush();
        sd_storage_unmount();
        puts("[SLEEP] TF card saved");
    }

    /* Release SPI bus to reduce leakage current */
    spi_bus_free(SD_SPI_HOST);
    gpio_set_direction(SD_SPI_CLK_PIN, GPIO_MODE_DISABLE);
    gpio_set_direction(SD_SPI_MOSI_PIN, GPIO_MODE_DISABLE);
    gpio_set_direction(SD_SPI_MISO_PIN, GPIO_MODE_DISABLE);
    gpio_set_direction(SD_SPI_CS_PIN, GPIO_MODE_DISABLE);

    /* 3s cancel window (press BOOT to abort) */
    for (int i = 3; i > 0; i--) {
        printf("  %d...\n", i);
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
        if (gpio_get_level(MAX30102_INT_PIN) == 0) {
            puts("MAX30102 interrupt detected, cancel deep-sleep");
            system_set_state(STATE_STANDALONE);
            return;
        }
    }

    /* Configure wake sources */
    gpio_deep_sleep_hold_dis();
    gpio_set_direction(MAX30102_INT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MAX30102_INT_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(BUTTON1_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON1_GPIO, GPIO_PULLUP_ONLY);

    esp_sleep_enable_ext1_wakeup_io(
        (1ULL << MAX30102_INT_PIN) | (1ULL << BUTTON1_GPIO),
        ESP_EXT1_WAKEUP_ANY_LOW);
    printf("[SLEEP] Wake: GPIO%d (MAX30102) + GPIO%d (BUTTON1)\n",
           MAX30102_INT_PIN, BUTTON1_GPIO);

    power_mgmt_enter_deep_sleep();
}

/**
 * @brief Standalone collection mode (no BLE/WiFi)
 *
 * Flow: stop BLE → start collection → stay awake 30s → light-sleep loop
 *        → deep-sleep after 5min without MAX30102 interrupt
 */
static void handle_standalone_state(void)
{
    puts("Standalone mode (WiFi/BLE off)");
    if (s_ble_initialized) {
        ble_svc_stop_advertising();
        puts("BLE advertising stopped");
    }
    start_collection_tasks();

    /* LED: 1s ON, 9s OFF (10s total cycle) for low power */
    s_led_on_ms = 1000;
    s_led_off_ms = 9000;

    /* Stay awake 30s, then manual light-sleep */
    if (s_bad_signal) {
        puts("Bad signal detected, skip 30s countdown");
    } else {
        puts("Staying awake for 30s...");
        for (int i = TIMEOUT_STANDBY_AWAKE / 1000; i > 0; i--) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
            if (i % 10 == 0) {
                printf("  %ds remaining...\n", i);
            }
            if (s_system_state != STATE_STANDALONE) goto restore;
        }
    }
    puts("Entering manual Light-sleep...");

    /* Stop all collection tasks — they access SD card which is unsafe during light-sleep */
    stop_collection_tasks();

    /* Flush SD buffers before sleep — otherwise unflushed data is lost */
    esp_task_wdt_reset();
    sd_storage_flush();
    puts("SD buffers flushed");

    /* Stop button polling for low power (keep alive during 30s for mode switch) */
    esp_task_wdt_reset();
    if (s_button1_task_handle != NULL) {
        vTaskDelete(s_button1_task_handle);
        s_button1_task_handle = NULL;
        puts("Button1 polling stopped");
    }

    /* Turn off LEDs and disable LED task */
    esp_task_wdt_reset();
    s_led_active = false;
    gpio_set_level(SYS_LED_PIN, 0);
    gpio_set_level(PPG_LED_PIN, 0);

    /* Shutdown MAX30102 to save power during light-sleep */
    esp_task_wdt_reset();
    max30102_stop();
    puts("MAX30102 shutdown, LEDs off for light-sleep");

    /* Manual light-sleep loop: 1s timer wake, check button1 */
    int64_t last_interrupt_time = esp_timer_get_time();
    int deep_sleep_ms = s_bad_signal ? (BAD_SIGNAL_LIGHT_SLEEP_SEC * 1000) : TIMEOUT_DEEP_SLEEP_NO_INT;
    if (s_bad_signal) {
        printf("Bad signal: deep-sleep in %ds\n", BAD_SIGNAL_LIGHT_SLEEP_SEC);
    }

    /* Remove main task from WDT — IDLE task feeds WDT during light-sleep */
    esp_task_wdt_delete(NULL);

    while (s_system_state == STATE_STANDALONE) {

        /* Check for button1 wake (GPIO18 low) */
        if (gpio_get_level(BUTTON1_GPIO) == 0) {
            puts("Button1 pressed -> toggle mode");
            /* Restore everything and let main_loop handle the state change */
            s_led_active = true;
            gpio_set_level(SYS_LED_PIN, 1);
            power_mgmt_set_freq(PM_MAX_FREQ_MHZ);
            /* Toggle: STANDALONE -> MEASURING */
            system_set_state(STATE_MEASURING);
            vTaskDelay(pdMS_TO_TICKS(500));  /* debounce */
            break;  /* Exit light-sleep loop, main_loop handles state */
        }

        /* Check for MAX30102 interrupt (if it was re-activated) */
        if (max30102_get_int_count() > 0) {
            last_interrupt_time = esp_timer_get_time();
            max30102_reset_int_count();
        }

        /* Timeout: no activity -> deep-sleep */
        int64_t no_interrupt_sec = (esp_timer_get_time() - last_interrupt_time) / 1000000;
        if (no_interrupt_sec * 1000 >= deep_sleep_ms) {
            request_deep_sleep("no activity for 5min");
            break;
        }

        /* Feed WDT before sleep, then sleep 1s */
        esp_sleep_enable_timer_wakeup(1000000);  /* 1 second */
        esp_light_sleep_start();
    }

restore:
    /* Re-add main task to WDT */
    esp_task_wdt_add(NULL);

    /* Restore LED rate and button task */
    s_led_on_ms = 500;
    s_led_off_ms = 500;
    s_bad_signal = false;

    /* Recreate button task if it was deleted */
    if (s_button1_task_handle == NULL) {
        xTaskCreate(button1_task, "button1", STACK_BUTTON1, NULL, 2, &s_button1_task_handle);
        puts("Button1 polling restarted");
    }

    power_mgmt_set_freq(PM_MAX_FREQ_MHZ);
    puts("Standalone done");
}

/* ========== Main loop ========== */

static void main_loop(void)
{
    esp_task_wdt_add(NULL);

    system_state_t prev_state = STATE_STANDALONE;

    while (1) {
        esp_task_wdt_reset();

        /* State changed */
        if (s_system_state != prev_state) {
            printf("State: %d -> %d\n", prev_state, s_system_state);
            prev_state = s_system_state;
        }

        switch (s_system_state) {
        case STATE_DEEP_SLEEP:
            enter_deep_sleep();
            break;

        case STATE_MEASURING:
            puts("Start PPG measurement");
            start_collection_tasks();
            /* Wait for measurement to end, feed WDT periodically */
            while (s_system_state == STATE_MEASURING) {
                esp_task_wdt_reset();
                EventBits_t bits = xEventGroupWaitBits(s_system_event_group, EVT_MEASURING_DONE,
                                                       pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
                if (bits & EVT_MEASURING_DONE) break;
            }
            stop_collection_tasks();
            break;

        case STATE_BLE_CONNECTED:
            puts("BLE connected, wait cmd");
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        case STATE_BLE_PAIRING:
            puts("BLE pairing mode");
            enter_ble_pairing();
            break;

        case STATE_WIFI_STA:
            puts("WiFi mode");
            enter_wifi_mode();
            break;

        case STATE_OTA:
            puts("OTA mode");
            wifi_transfer_start_ota();
            break;

        case STATE_STANDALONE:
            handle_standalone_state();
            break;

        default:
            request_deep_sleep("unknown state");
            break;
        }
    }
}

/* ========== Public API ========== */

void system_set_state(system_state_t new_state)
{
    portENTER_CRITICAL(&s_state_mux);
    system_state_t old = s_system_state;
    s_system_state = new_state;
    portEXIT_CRITICAL(&s_state_mux);
    printf("State change: %d -> %d\n", old, new_state);
}

system_state_t system_get_state(void)
{
    portENTER_CRITICAL(&s_state_mux);
    system_state_t state = s_system_state;
    portEXIT_CRITICAL(&s_state_mux);
    return state;
}

/* ========== App entry ========== */

void app_main(void)
{
    /* Startup banner */
    puts("");
    puts("========================================");
    fputs("  PPG Monitor v", stdout);
    puts(PPG_FW_VERSION);
    puts("  ESP32-S3 | UART0 @ 1M baud");
    fputs("  Build: ", stdout);
    puts(PPG_FW_BUILD_TS);
    puts("========================================");
    puts("");

    puts("System started");

    /* Create event group */
    s_system_event_group = xEventGroupCreate();
    assert(s_system_event_group);

    /* Init LED GPIO */
    led_gpio_init();

    /* Init Task WDT */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = TIMEOUT_WDT,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t wdt_ret = esp_task_wdt_init(&wdt_cfg);
    if (wdt_ret == ESP_ERR_INVALID_STATE) {
        puts("Task WDT already init (IDF default 5s), skip");
    } else if (wdt_ret == ESP_OK) {
        puts("Task WDT enabled (10s)");
    } else {
        puts("Task WDT init failed");
    }

    /* System init */
    puts("Calling system_init()...");
    system_init();
    puts("system_init() done");

    /* Create resident tasks */
    xTaskCreate(sys_led_task, "sys_led", STACK_SYS_LED, NULL, 1, NULL);
    xTaskCreate(ppg_led_task, "ppg_led", STACK_PPG_LED, NULL, 1, NULL);

    /* Init BUTTON1 GPIO */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON1_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* Init BOOT GPIO (GPIO0) - enable internal pull-up to prevent
     * floating pin from triggering download mode on reset */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);

    /* Create BUTTON1 monitor task */
    xTaskCreate(button1_task, "button1", STACK_BUTTON1, NULL, 2, &s_button1_task_handle);

    puts("Resident tasks started: sys_led, ppg_led, button1");

    /* Check wake-up reason */
    bool gpio_wakeup = is_gpio_wakeup();

    /* Check BUTTON1 state */
    int level = gpio_get_level(BUTTON1_GPIO);
    printf("[BUTTON1] GPIO%d level=%d (0=pressed)\n", BUTTON1_GPIO, level);

    if (level == 0) {
        /* Button pressed at boot, enter BLE pairing */
        puts("BUTTON1 pressed at boot -> BLE pairing");
        system_set_state(STATE_BLE_PAIRING);
    } else if (gpio_wakeup) {
        puts("GPIO wake -> standalone");
        system_set_state(STATE_STANDALONE);
    } else {
        /* Cold boot: RF off by default, standalone collection only */
        puts("Cold boot -> standalone (RF off)");
        system_set_state(STATE_STANDALONE);
    }

    /* Enter main loop */
    main_loop();
}
