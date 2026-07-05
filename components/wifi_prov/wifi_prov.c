/**
 * @file wifi_prov.c
 * @brief WiFi 配网管理实现
 *
 * NVS 存储结构:
 *   namespace: wifi_cred
 *   key: count (uint8), cred_0 ~ cred_4 (blob)
 *
 * 自动重连: 上电从 NVS 读取, 按优先级+信号强度逐个尝试
 */

#include "wifi_prov.h"
#include "ppg_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_prov";

#define NVS_NAMESPACE    "wifi_cred"
#define NVS_KEY_COUNT    "count"
#define NVS_KEY_PREFIX   "cred_"

/* 指数退避参数 */
#define RECONNECT_BASE_MS   1000    /* 初始 1 秒 */
#define RECONNECT_MAX_MS    30000   /* 最大 30 秒 */
#define RECONNECT_MAX_FAILS 10      /* 最大失败次数（防止溢出） */

static wifi_cred_t s_creds[WIFI_MAX_CREDENTIALS];
static uint8_t s_cred_count = 0;
static SemaphoreHandle_t s_creds_mutex = NULL;
static bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static char s_current_ip[16] = {0};
static char s_current_ssid[33] = {0};  /* Currently connected WiFi SSID */

/* 指数退避重连 */
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_reconnect_fail_count = 0;

/* ========== NVS 操作 ========== */

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No WiFi creds in NVS");
        s_cred_count = 0;
        return ESP_OK;
    }

    uint8_t count = 0;
    ret = nvs_get_u8(handle, NVS_KEY_COUNT, &count);
    if (ret != ESP_OK || count == 0) {
        s_cred_count = 0;
        nvs_close(handle);
        return ESP_OK;
    }

    if (count > WIFI_MAX_CREDENTIALS) count = WIFI_MAX_CREDENTIALS;

    for (int i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, i);
        size_t len = sizeof(wifi_cred_t);
        ret = nvs_get_blob(handle, key, &s_creds[i], &len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Read cred %d failed", i);
            break;
        }
    }

    s_cred_count = count;
    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded %d WiFi cred(s) from NVS", s_cred_count);
    for (int i = 0; i < s_cred_count; i++) {
        int pw_len = strlen(s_creds[i].password);
        ESP_LOGI(TAG, "  [%d] SSID=\"%.32s\" priority=%d fails=%d",
                 i, s_creds[i].ssid, s_creds[i].priority, s_creds[i].fail_count);
        if (pw_len > 0) {
            ESP_LOGI(TAG, "       Password: %d chars (****)", pw_len);
        } else {
            ESP_LOGI(TAG, "       Password: (open)");
        }
    }

    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open NVS failed: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_set_u8(handle, NVS_KEY_COUNT, s_cred_count);

    for (int i = 0; i < s_cred_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, i);
        nvs_set_blob(handle, key, &s_creds[i], sizeof(wifi_cred_t));
    }

    /* 清除多余的 key */
    for (int i = s_cred_count; i < WIFI_MAX_CREDENTIALS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, i);
        nvs_erase_key(handle, key);
    }

    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved %d WiFi creds to NVS", s_cred_count);
    return ESP_OK;
}

/* ========== 指数退避重连 ========== */

static uint32_t calc_backoff_ms(void)
{
    uint32_t delay_ms = RECONNECT_BASE_MS;
    for (uint32_t i = 0; i < s_reconnect_fail_count && delay_ms < RECONNECT_MAX_MS; i++) {
        delay_ms *= 2;
    }
    return (delay_ms > RECONNECT_MAX_MS) ? RECONNECT_MAX_MS : delay_ms;
}

static void reconnect_timer_cb(void *arg)
{
    if (s_connected) return;  /* 已连接，不需要重连 */

    uint32_t delay_ms = calc_backoff_ms();
    ESP_LOGI(TAG, "WiFi reconnect #%d, backoff %lums", s_reconnect_fail_count + 1, (unsigned long)delay_ms);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (s_reconnect_fail_count >= RECONNECT_MAX_FAILS) {
        s_reconnect_fail_count = RECONNECT_MAX_FAILS - 1;  /* 防止溢出 */
    }

    uint32_t delay_ms = calc_backoff_ms();

    /* 启动一次性定时器 */
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    }

    s_reconnect_fail_count++;
}

/* ========== WiFi 事件处理 ========== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_current_ssid[0] = '\0';  /* Clear connected SSID */
            ESP_LOGW(TAG, "WiFi disconnected, exp backoff");
            schedule_reconnect();
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_reconnect_fail_count = 0;  /* 连接成功，重置退避计数 */
        ESP_LOGI(TAG, "WiFi connected, IP: %s", s_current_ip);
    }
}

/* ========== 公共 API ========== */

esp_err_t wifi_prov_init(void)
{
    /* 创建 STA netif */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Create STA netif failed");
        return ESP_FAIL;
    }

    /* WiFi 初始化 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
        return ret;
    }

    /* 注册事件处理 */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip);

    /* 设置 Station 模式 */
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* WiFi TX power: indoor use, 7.75dBm is sufficient */
    esp_wifi_set_max_tx_power(31);  /* 31 * 0.25dBm = 7.75dBm */

    /* Disable fast connect to avoid hidden reconnection attempts */
    esp_wifi_clear_fast_connect();

    /* Enable WiFi power save mode */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    /* 创建凭据互斥锁 */
    s_creds_mutex = xSemaphoreCreateMutex();

    /* 从 NVS 加载凭据 */
    load_from_nvs();

    /* 创建重连定时器 */
    esp_timer_create_args_t timer_cfg = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    esp_timer_create(&timer_cfg, &s_reconnect_timer);

    ESP_LOGI(TAG, "WiFi prov init done");
    return ESP_OK;
}

esp_err_t wifi_prov_add(const char *ssid, const char *password, uint8_t priority)
{
    if (!ssid || !s_creds_mutex) {
        ESP_LOGE(TAG, "wifi_prov_add FAILED: ssid=%p mutex=%p", (void*)ssid, (void*)s_creds_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_creds_mutex, portMAX_DELAY);

    if (s_cred_count >= WIFI_MAX_CREDENTIALS) {
        xSemaphoreGive(s_creds_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    /* 检查是否已存在 */
    for (int i = 0; i < s_cred_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            strncpy(s_creds[i].password, password ? password : "", 64);
            s_creds[i].priority = priority;
            s_creds[i].fail_count = 0;
            ESP_LOGI(TAG, "Update existing WiFi: %s", ssid);
            xSemaphoreGive(s_creds_mutex);
            esp_err_t ret = save_to_nvs();
            ESP_LOGI(TAG, "Save to NVS result: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 新增 */
    wifi_cred_t *cred = &s_creds[s_cred_count];
    strncpy(cred->ssid, ssid, 32);
    cred->ssid[32] = '\0';
    strncpy(cred->password, password ? password : "", 64);
    cred->password[64] = '\0';
    cred->channel = 0;
    cred->rssi_last = 0;
    cred->priority = priority;
    cred->fail_count = 0;
    s_cred_count++;
    xSemaphoreGive(s_creds_mutex);

    ESP_LOGI(TAG, "Add WiFi #%d: %s (prio %d)", s_cred_count - 1, ssid, priority);
    esp_err_t ret = save_to_nvs();
    ESP_LOGI(TAG, "Save to NVS result: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t wifi_prov_delete(uint8_t index)
{
    if (!s_creds_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_creds_mutex, portMAX_DELAY);

    if (index >= s_cred_count) {
        xSemaphoreGive(s_creds_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Delete WiFi #%d: %s", index, s_creds[index].ssid);

    for (int i = index; i < s_cred_count - 1; i++) {
        memcpy(&s_creds[i], &s_creds[i + 1], sizeof(wifi_cred_t));
    }
    s_cred_count--;
    xSemaphoreGive(s_creds_mutex);

    return save_to_nvs();
}

esp_err_t wifi_prov_modify(uint8_t index, const char *ssid, const char *password)
{
    if (index >= s_cred_count || !ssid) return ESP_ERR_INVALID_ARG;

    strncpy(s_creds[index].ssid, ssid, 32);
    if (password) {
        strncpy(s_creds[index].password, password, 64);
    }
    s_creds[index].fail_count = 0;

    ESP_LOGI(TAG, "Modify WiFi #%d: %s", index, ssid);
    return save_to_nvs();
}

esp_err_t wifi_prov_clear_all(void)
{
    if (!s_creds_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_creds_mutex, portMAX_DELAY);
    s_cred_count = 0;
    memset(s_creds, 0, sizeof(s_creds));
    ESP_LOGI(TAG, "Clear all WiFi creds");
    xSemaphoreGive(s_creds_mutex);
    return save_to_nvs();
}

esp_err_t wifi_prov_get_list_json(char *buf, size_t len)
{
    int pos = snprintf(buf, len, "{\"count\":%d,\"connected\":%s,\"ip\":\"%s\",\"list\":[",
                       s_cred_count,
                       s_connected ? "true" : "false",
                       s_current_ip);
    for (int i = 0; i < s_cred_count; i++) {
        if (i > 0) pos += snprintf(buf + pos, len - pos, ",");
        pos += snprintf(buf + pos, len - pos,
                        "{\"idx\":%d,\"ssid\":\"%s\",\"has_pass\":%s,"
                        "\"priority\":%d,\"rssi\":%d,\"fails\":%d}",
                        i, s_creds[i].ssid,
                        strlen(s_creds[i].password) > 0 ? "true" : "false",
                        s_creds[i].priority,
                        s_creds[i].rssi_last,
                        s_creds[i].fail_count);
    }
    pos += snprintf(buf + pos, len - pos, "]}");
    return ESP_OK;
}

esp_err_t wifi_prov_get_detail_json(uint8_t index, char *buf, size_t len)
{
    if (index >= s_cred_count || !s_creds_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_creds_mutex, portMAX_DELAY);

    /* Check if this WiFi is currently connected */
    bool is_connected = s_connected && strcmp(s_creds[index].ssid, s_current_ssid) == 0;

    snprintf(buf, len,
             "{\"idx\":%d,\"ssid\":\"%s\",\"has_pass\":%s,"
             "\"priority\":%d,\"rssi\":%d,\"fails\":%d,"
             "\"connected\":%s,\"ip\":\"%s\"}",
             index, s_creds[index].ssid,
             strlen(s_creds[index].password) > 0 ? "true" : "false",
             s_creds[index].priority,
             s_creds[index].rssi_last,
             s_creds[index].fail_count,
             is_connected ? "true" : "false",
             is_connected ? s_current_ip : "");

    xSemaphoreGive(s_creds_mutex);

    return ESP_OK;
}

esp_err_t wifi_prov_get_status_json(char *buf, size_t len)
{
    snprintf(buf, len,
             "{\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"}",
             s_connected ? "true" : "false",
             s_current_ip,
             s_connected ? "connected" : "");
    return ESP_OK;
}

esp_err_t wifi_prov_set_priority(uint8_t index, uint8_t priority)
{
    if (index >= s_cred_count) return ESP_ERR_INVALID_ARG;
    s_creds[index].priority = priority;
    ESP_LOGI(TAG, "WiFi #%d priority set to %d", index, priority);
    return save_to_nvs();
}

esp_err_t wifi_prov_auto_connect(void)
{
    if (s_cred_count == 0) {
        ESP_LOGW(TAG, "No saved WiFi creds");
        return ESP_ERR_NOT_FOUND;
    }

    /* 按优先级排序 (简单冒泡) */
    for (int i = 0; i < s_cred_count - 1; i++) {
        for (int j = 0; j < s_cred_count - 1 - i; j++) {
            if (s_creds[j].priority > s_creds[j + 1].priority) {
                wifi_cred_t tmp = s_creds[j];
                s_creds[j] = s_creds[j + 1];
                s_creds[j + 1] = tmp;
            }
        }
    }

    /* 尝试连接（遍历所有凭据，按优先级排序后依次尝试） */
    for (int i = 0; i < s_cred_count; i++) {
        wifi_cred_t *cred = &s_creds[i];
        if (cred->fail_count >= 3) {
            ESP_LOGW(TAG, "Skip WiFi #%d (too many failures): %s", i, cred->ssid);
            continue;
        }

        ESP_LOGI(TAG, "Try connect WiFi #%d: SSID=[%s] pwd_len=%d prio=%d",
                 i, cred->ssid, (int)strlen(cred->password), cred->priority);

        /* Save SSID for connection status tracking */
        strncpy(s_current_ssid, cred->ssid, sizeof(s_current_ssid) - 1);
        s_current_ssid[sizeof(s_current_ssid) - 1] = '\0';

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid, cred->ssid, sizeof(wifi_cfg.sta.ssid));
        strncpy((char *)wifi_cfg.sta.password, cred->password, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.pmf_cfg.capable = true;
        wifi_cfg.sta.pmf_cfg.required = false;
        wifi_cfg.sta.threshold.rssi = -75;  /* Skip APs weaker than -75dBm */
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "No valid WiFi credentials to connect");
    return ESP_ERR_NOT_FOUND;
}

bool wifi_prov_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_prov_get_ip(char *buf, size_t len)
{
    if (!s_connected) {
        snprintf(buf, len, "0.0.0.0");
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(buf, s_current_ip, len);
    return ESP_OK;
}

esp_err_t wifi_prov_disconnect(void)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    s_connected = false;
    s_current_ip[0] = '\0';
    s_reconnect_fail_count = 0;
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi disconnected and stopped");
    return ESP_OK;
}
