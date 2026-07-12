/**
 * @file ble_svc.c
 * @brief BLE GATT 服务实现
 *
 * NimBLE 主机栈, 自定义服务 0xFFF0
 * 特征值:
 *   0xFFF1: Status (Read/Notify) - 电量/内存/状态/固件版本
 *   0xFFF2: Live Data (Notify) - 实时 SpO2/HR/PI
 *   0xFFF3: Command (Write) - 控制命令（帧协议）
 *   0xFFF4: File List (Read) - TF 卡文件列表 JSON
 *
 * 帧协议 (Stop-and-Wait):
 *   [0xAA][CMD][LEN][DATA...][CHECKSUM]
 *   CHECKSUM = CMD + LEN + DATA 之和 & 0xFF
 *
 * 响应帧:
 *   [0xAA][CMD][0x01][STATUS][CHECKSUM]
 *   STATUS: 0=OK, 1=取消, 2=校验错, 3=未知命令
 */

#include "ble_svc.h"
#include "ppg_config.h"
#include "ppg_log.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "ble_svc";
static const ble_callbacks_t *s_cbs = NULL;

/* BLE debug macro - set to 1 to enable detailed BLE communication logging */
#define BLE_DEBUG_ENABLE        1

#if BLE_DEBUG_ENABLE
#define BLE_DEBUG_LOG(fmt, ...) printf("[BLE] " fmt "\n", ##__VA_ARGS__)
#define BLE_DEBUG_HEX(tag, data, len) \
    do { \
        printf("[BLE] %s (%d bytes):", tag, (int)(len)); \
        for (int _i = 0; _i < (len); _i++) printf(" %02X", (data)[_i]); \
        printf("\n"); \
    } while(0)
#else
#define BLE_DEBUG_LOG(fmt, ...)
#define BLE_DEBUG_HEX(tag, data, len)
#endif

/* ========== Frame protocol definition ========== */

#define FRAME_HEADER            0xAA
#define FRAME_MAX_DATA          200     /* Max data length per frame */
#define FRAME_MIN_LEN           4       /* Header + CMD + LEN + Checksum */

/* 帧状态码 */
#define FRAME_STATUS_OK         0
#define FRAME_STATUS_CANCEL     1
#define FRAME_STATUS_BAD_CRC    2
#define FRAME_STATUS_UNKNOWN    3

/* GATT 属性句柄 */
static uint16_t s_char_status_handle;
static uint16_t s_char_live_handle;
static uint16_t s_char_cmd_handle;
static uint16_t s_char_filelist_handle;

/* 连接句柄 */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_connected = false;
static bool s_status_subscribed = false;
static bool s_nimble_synced = false;

/* 状态数据 */
static uint8_t s_status_data[20];
static uint8_t s_live_data[10];
static char s_wifi_list_buf[256];  /* WiFi list JSON buffer */
static bool s_wifi_list_valid = false;  /* Whether WiFi list has been queried */

/* 命令队列（事件驱动解耦） */
#define CMD_QUEUE_DEPTH     8
typedef struct {
    uint8_t cmd;
    uint8_t data[FRAME_MAX_DATA];
    uint8_t len;
} ble_cmd_msg_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t  s_cmd_task_handle = NULL;

/* 前向声明 */
static int gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);
static void ble_cmd_task(void *arg);

/* ========== GATT 服务定义 ========== */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CHAR_STATUS_UUID),
                .access_cb = gatt_handler,
                .val_handle = &s_char_status_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CHAR_LIVE_UUID),
                .access_cb = gatt_handler,
                .val_handle = &s_char_live_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CHAR_CMD_UUID),
                .access_cb = gatt_handler,
                .val_handle = &s_char_cmd_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_CHAR_FILELIST_UUID),
                .access_cb = gatt_handler,
                .val_handle = &s_char_filelist_handle,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0}
        },
    },
    {0}
};

/* ========== 帧协议工具 ========== */

/**
 * @brief 计算校验和
 */
static uint8_t frame_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

/**
 * @brief Send response frame to phone
 */
static void ble_send_response(uint8_t cmd, uint8_t status)
{
    if (!s_connected) return;

    uint8_t resp[5];
    resp[0] = FRAME_HEADER;
    resp[1] = cmd;
    resp[2] = 0x01;     /* Data length = 1 */
    resp[3] = status;
    resp[4] = frame_checksum(&resp[1], 3);  /* CMD + LEN + STATUS */

    BLE_DEBUG_LOG("TX Response: cmd=0x%02X status=%d checksum=0x%02X", cmd, status, resp[4]);
    BLE_DEBUG_HEX("TX Raw", resp, 5);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(resp, sizeof(resp));
    if (!om) return;

    ble_gattc_notify_custom(s_conn_handle, s_char_cmd_handle, om);
}

/**
 * @brief Send data response frame to phone
 *
 * Frame: [0xAA][CMD][LEN][DATA...][CHECKSUM]
 */
static void ble_send_data_response(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (!s_connected) return;

    uint8_t resp[259];  /* 4 + max uint8_t len (255) */
    resp[0] = FRAME_HEADER;
    resp[1] = cmd;
    resp[2] = len;
    if (len > 0) memcpy(&resp[3], data, len);
    resp[3 + len] = frame_checksum(&resp[1], 2 + len);

    BLE_DEBUG_LOG("TX Data Response: cmd=0x%02X len=%d checksum=0x%02X", cmd, len, resp[3 + len]);

    size_t frame_len = 3 + len + 1;  /* Header + CMD + LEN + DATA + CHECKSUM */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(resp, frame_len);
    if (!om) return;

    ble_gattc_notify_custom(s_conn_handle, s_char_cmd_handle, om);
}

/**
 * @brief 解析并派发帧数据
 *
 * 帧格式: [0xAA][CMD][LEN][DATA...][CHECKSUM]
 * 返回: 0=成功, -1=格式错误, -2=校验错
 */
static int frame_parse_and_dispatch(const uint8_t *buf, size_t len)
{
    BLE_DEBUG_HEX("RX Raw", buf, len);

    if (len < FRAME_MIN_LEN) {
        ESP_LOGW(TAG, "Frame too short: %d bytes", len);
        BLE_DEBUG_LOG("ERROR: Frame too short (%d < %d)", (int)len, FRAME_MIN_LEN);
        return -1;
    }

    /* Check header */
    if (buf[0] != FRAME_HEADER) {
        ESP_LOGW(TAG, "Frame header error: 0x%02X", buf[0]);
        BLE_DEBUG_LOG("ERROR: Header mismatch (0x%02X != 0xAA)", buf[0]);
        return -1;
    }

    uint8_t cmd = buf[1];
    uint8_t data_len = buf[2];

    BLE_DEBUG_LOG("RX Frame: cmd=0x%02X data_len=%d total=%d", cmd, data_len, (int)len);

    /* Check length */
    if (len < (size_t)(FRAME_MIN_LEN + data_len)) {
        ESP_LOGW(TAG, "Frame len mismatch: declared=%d, actual=%d", data_len, len - 3);
        BLE_DEBUG_LOG("ERROR: Length mismatch (declared=%d, actual=%d)", data_len, (int)(len - 3));
        return -1;
    }

    /* Checksum verification */
    uint8_t expected_crc = frame_checksum(&buf[1], 2 + data_len);
    uint8_t actual_crc = buf[3 + data_len];
    BLE_DEBUG_LOG("Checksum: expected=0x%02X actual=0x%02X %s",
                  expected_crc, actual_crc,
                  (expected_crc == actual_crc) ? "OK" : "FAIL");

    if (expected_crc != actual_crc) {
        ESP_LOGW(TAG, "Checksum error: expect=0x%02X, got=0x%02X", expected_crc, actual_crc);
        ble_send_response(cmd, FRAME_STATUS_BAD_CRC);
        return -2;
    }

    /* Print data payload if any */
    if (data_len > 0) {
        BLE_DEBUG_HEX("RX Data", &buf[3], data_len);
    }

    /* Enqueue for dispatch */
    ble_cmd_msg_t msg;
    msg.cmd = cmd;
    msg.len = data_len;
    if (data_len > 0 && data_len <= FRAME_MAX_DATA) {
        memcpy(msg.data, &buf[3], data_len);
    }

    if (xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Cmd queue full, drop: 0x%02X", cmd);
        BLE_DEBUG_LOG("ERROR: Command queue full, dropping 0x%02X", cmd);
        return -1;
    }

    BLE_DEBUG_LOG("RX Enqueued: cmd=0x%02X", cmd);
    return 0;
}

/* ========== Command handlers (individual functions) ========== */

typedef void (*cmd_handler_t)(uint8_t cmd, const uint8_t *data, uint8_t len);

static void cmd_start_measure(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_start_measure", __LINE__, "Start measure");
    s_cbs->set_state(STATE_MEASURING);
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_stop_measure(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_stop_measure", __LINE__, "Stop measure");
    s_cbs->set_state(STATE_BLE_CONNECTED);
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_start_wifi(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_start_wifi", __LINE__, "Start WiFi");
    uint8_t batt_pct = s_cbs->voltage_to_soc(s_cbs->get_voltage());
    if (batt_pct < BATTERY_MIN_WIFI_PCT) {
        PPG_LOGW(TAG, "cmd_start_wifi", __LINE__, "Battery low (%d%%), WiFi disabled", batt_pct);
        ble_send_response(cmd, 0x04);
    } else {
        s_cbs->set_state(STATE_WIFI_STA);
        ble_send_response(cmd, FRAME_STATUS_OK);
    }
}

static void cmd_wifi_add(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len < 4) {
        ble_send_response(cmd, 0x02);
        return;
    }
    uint16_t ssid_len = ((uint16_t)data[0] << 8) | data[1];
    if (ssid_len > 32) ssid_len = 32;
    char ssid[33] = {0};
    memcpy(ssid, &data[2], ssid_len);
    char password[65] = {0};
    uint16_t pwd_offset = 2 + ssid_len;
    if (len > pwd_offset + 2) {
        uint16_t pass_len = ((uint16_t)data[pwd_offset] << 8) | data[pwd_offset + 1];
        if (pass_len > 64) pass_len = 64;
        memcpy(password, &data[pwd_offset + 2], pass_len);
    }
    printf("[BLE] WiFi Add: SSID=[%s] pwd_len=%d\n", ssid, (int)strlen(password));
    s_cbs->wifi_add(ssid, password, 0);
    ble_send_response(cmd, FRAME_STATUS_OK);
    PPG_LOGI(TAG, "cmd_wifi_add", __LINE__, "Auto-connecting WiFi...");
    s_cbs->set_state(STATE_WIFI_STA);
}

static void cmd_wifi_list(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_wifi_list", __LINE__, "Query WiFi list");

    /* Populate WiFi list buffer and send via Command characteristic */
    s_cbs->wifi_get_list_json(s_wifi_list_buf, sizeof(s_wifi_list_buf));
    s_wifi_list_valid = true;

    /* Send JSON data as notification on Command characteristic (0xFFF3) */
    if (s_connected) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_wifi_list_buf, strlen(s_wifi_list_buf));
        if (om) ble_gattc_notify_custom(s_conn_handle, s_char_cmd_handle, om);
    }
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_wifi_detail(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len < 1) {
        ble_send_response(cmd, 0x02);  /* Bad request */
        return;
    }
    uint8_t index = data[0];
    PPG_LOGI(TAG, "cmd_wifi_detail", __LINE__, "Query WiFi detail: %d", index);

    esp_err_t ret = s_cbs->wifi_get_detail_json(index, s_wifi_list_buf, sizeof(s_wifi_list_buf));
    if (ret == ESP_OK) {
        /* Send JSON data as notification on Command characteristic (0xFFF3) */
        if (s_connected) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(s_wifi_list_buf, strlen(s_wifi_list_buf));
            if (om) ble_gattc_notify_custom(s_conn_handle, s_char_cmd_handle, om);
        }
        ble_send_response(cmd, FRAME_STATUS_OK);
    } else {
        ble_send_response(cmd, 0x05);  /* Data invalid */
    }
}

static void cmd_wifi_clear(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_wifi_clear", __LINE__, "Clear all WiFi");
    s_cbs->wifi_clear_all();
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_wifi_delete(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len >= 1) {
        PPG_LOGI(TAG, "cmd_wifi_delete", __LINE__, "Delete WiFi index: %d", data[0]);
        s_cbs->wifi_delete(data[0]);
        ble_send_response(cmd, FRAME_STATUS_OK);
    }
}

static void cmd_ota_enter(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_ota_enter", __LINE__, "Enter OTA mode");
    s_cbs->set_state(STATE_OTA);
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_fw_version(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_fw_version", __LINE__, "FW version: %s", PPG_FW_VERSION);
    if (s_connected) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(PPG_FW_VERSION, strlen(PPG_FW_VERSION));
        if (om) ble_gattc_notify_custom(s_conn_handle, s_char_status_handle, om);
    }
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_query_status(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_query_status", __LINE__, "Query status");
    uint32_t voltage = s_cbs->get_voltage();
    uint8_t batt_pct = s_cbs->voltage_to_soc(voltage);
    ble_svc_update_status(batt_pct, voltage);

    printf("[BLE] cmd_query_status: batt=%d%% voltage=%lu.%02luV connected=%d\n",
           batt_pct, (unsigned long)(voltage / 100), (unsigned long)(voltage % 100), s_connected);
    printf("[BLE] Status data: ");
    for (int i = 0; i < 20; i++) printf("%02X ", s_status_data[i]);
    printf("\n");

    /* Send status notification so APP can receive it immediately */
    if (s_connected) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_status_data, sizeof(s_status_data));
        if (om) {
            ble_gattc_notify_custom(s_conn_handle, s_char_status_handle, om);
            printf("[BLE] Status notification sent (handle=%d)\n", s_char_status_handle);
        } else {
            printf("[BLE] Status notification FAILED (om alloc failed)\n");
        }
    } else {
        printf("[BLE] Status notification SKIPPED (not connected)\n");
    }

    /* Do NOT send response on Command characteristic - data already sent via notification */
}

static void cmd_query_sd_card(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_query_sd_card", __LINE__, "Query SD card");
    uint32_t free_mb = s_cbs->sd_get_free_mb();
    uint32_t total_mb = s_cbs->sd_get_total_mb();
    uint8_t resp[4] = {
        (free_mb >> 8) & 0xFF, free_mb & 0xFF,
        (total_mb >> 8) & 0xFF, total_mb & 0xFF
    };
    printf("[BLE] cmd_query_sd_card: free=%lumb total=%lumb connected=%d\n",
           (unsigned long)free_mb, (unsigned long)total_mb, s_connected);
    printf("[BLE] SD resp data: %02X %02X %02X %02X\n", resp[0], resp[1], resp[2], resp[3]);
    BLE_DEBUG_LOG("SD Card: free=%luMB total=%luMB", (unsigned long)free_mb, (unsigned long)total_mb);
    ble_send_data_response(cmd, resp, 4);
}

static void cmd_query_battery(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    uint32_t voltage = s_cbs->get_voltage();
    uint8_t batt_pct = s_cbs->voltage_to_soc(voltage);
    printf("[BLE] cmd_query_battery: batt=%d%% voltage=%lu.%02luV connected=%d\n",
           batt_pct, (unsigned long)(voltage / 100), (unsigned long)(voltage % 100), s_connected);
    uint8_t resp[1] = { batt_pct };
    printf("[BLE] Battery resp data: %02X\n", resp[0]);
    ble_send_data_response(cmd, resp, 1);
}

static void cmd_time_sync(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len < 4) {
        ble_send_response(cmd, 0x02);
        return;
    }
    uint32_t timestamp = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    PPG_LOGI(TAG, "cmd_time_sync", __LINE__, "Time sync: %lu", (unsigned long)timestamp);
    if (timestamp > MIN_VALID_TIMESTAMP) {
        struct timeval tv = { .tv_sec = timestamp, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ble_send_response(cmd, FRAME_STATUS_OK);
    } else {
        ble_send_response(cmd, 0x05);
    }
}

static void cmd_standalone(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_standalone", __LINE__, "Enter standalone");
    ble_send_response(cmd, FRAME_STATUS_OK);
    vTaskDelay(pdMS_TO_TICKS(TIMEOUT_BLE_CMD_DELAY));
    s_cbs->set_state(STATE_STANDALONE);
}

static void cmd_log_level(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len >= 1) {
        PPG_LOGI(TAG, "cmd_log_level", __LINE__, "Set log level: %d", data[0]);
        s_cbs->log_set_level(data[0]);
        ble_send_response(cmd, FRAME_STATUS_OK);
    }
}

static void cmd_log_status(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_log_status", __LINE__, "Query log status");
    ESP_LOGI(TAG, "Log status: level=%d, buffer=%d", s_cbs->log_get_level(), s_cbs->log_get_buffer_count());
    ble_send_response(cmd, FRAME_STATUS_OK);
}

static void cmd_file_download(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    (void)data; (void)len;
    PPG_LOGI(TAG, "cmd_file_download", __LINE__, "File download trigger");

    /* If already connected, send IP immediately */
    if (s_cbs->wifi_is_connected()) {
        char ip[16] = {0};
        s_cbs->wifi_get_ip(ip, sizeof(ip));
        uint8_t ip_len = (uint8_t)strlen(ip);
        uint8_t resp_data[1 + ip_len];
        resp_data[0] = ip_len;
        memcpy(&resp_data[1], ip, ip_len);
        ble_send_data_response(cmd, resp_data, 1 + ip_len);
        PPG_LOGI(TAG, "cmd_file_download", __LINE__, "WiFi ready, IP: %s", ip);
        return;
    }

    /* Not connected - trigger WiFi and respond async */
    s_cbs->set_state(STATE_WIFI_STA);
    ble_send_response(cmd, FRAME_STATUS_OK);
    PPG_LOGI(TAG, "cmd_file_download", __LINE__, "WiFi starting, will respond when connected");
}

static void cmd_uart_record(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len < 1) {
        ble_send_response(cmd, 0x02);
        return;
    }

    uint8_t enable = data[0];

    if (enable) {
        /* Parse: [enable][baud_h][baud_2][baud_1][baud_l][data_bits][parity][stop_bits] */
        ble_uart_config_t cfg = {
            .baud_rate = 115200,
            .data_bits = 8,
            .parity = 0,
            .stop_bits = 1,
        };

        if (len >= 5) {
            cfg.baud_rate = ((uint32_t)data[1] << 24) |
                            ((uint32_t)data[2] << 16) |
                            ((uint32_t)data[3] << 8) |
                            (uint32_t)data[4];
        }
        if (len >= 6) cfg.data_bits = data[5];
        if (len >= 7) cfg.parity = data[6];
        if (len >= 8) cfg.stop_bits = data[7];

        esp_err_t ret = s_cbs->uart_record_start(&cfg);
        if (ret == ESP_OK) {
            PPG_LOGI(TAG, "cmd_uart_record", __LINE__, "UART record: %lu baud %d%c%d",
                     (unsigned long)cfg.baud_rate, cfg.data_bits,
                     cfg.parity == 0 ? 'N' : (cfg.parity == 1 ? 'E' : 'O'),
                     cfg.stop_bits);
            ble_send_response(cmd, FRAME_STATUS_OK);
        } else {
            ble_send_response(cmd, 0x01);  /* Failed */
        }
    } else {
        s_cbs->uart_record_stop();
        PPG_LOGI(TAG, "cmd_uart_record", __LINE__, "UART record stopped");
        ble_send_response(cmd, FRAME_STATUS_OK);
    }
}

/* ========== Command dispatch table ========== */

static const struct {
    uint8_t cmd;
    cmd_handler_t handler;
} s_cmd_table[] = {
    { BLE_CMD_START_MEASURE, cmd_start_measure },
    { BLE_CMD_STOP_MEASURE,  cmd_stop_measure },
    { BLE_CMD_START_WIFI,    cmd_start_wifi },
    { BLE_CMD_WIFI_ADD,      cmd_wifi_add },
    { BLE_CMD_WIFI_LIST,     cmd_wifi_list },
    { BLE_CMD_WIFI_DETAIL,   cmd_wifi_detail },
    { BLE_CMD_WIFI_CLEAR,    cmd_wifi_clear },
    { BLE_CMD_WIFI_DELETE,   cmd_wifi_delete },
    { BLE_CMD_OTA_ENTER,     cmd_ota_enter },
    { BLE_CMD_FW_VERSION,    cmd_fw_version },
    { BLE_CMD_QUERY_STATUS,  cmd_query_status },
    { BLE_CMD_QUERY_SD_CARD, cmd_query_sd_card },
    { BLE_CMD_QUERY_BATTERY, cmd_query_battery },
    { BLE_CMD_TIME_SYNC,     cmd_time_sync },
    { BLE_CMD_STANDALONE,    cmd_standalone },
    { BLE_CMD_LOG_LEVEL,     cmd_log_level },
    { BLE_CMD_LOG_STATUS,    cmd_log_status },
    { BLE_CMD_FILE_DOWNLOAD, cmd_file_download },
    { BLE_CMD_UART_RECORD,  cmd_uart_record },
};

#define CMD_TABLE_SIZE (sizeof(s_cmd_table) / sizeof(s_cmd_table[0]))

/* ========== Command dispatch ========== */

static void handle_command(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    ESP_LOGI(TAG, "Handle cmd: 0x%02X, len=%d", cmd, len);
    BLE_DEBUG_LOG("Dispatch: cmd=0x%02X len=%d", cmd, len);

    for (int i = 0; i < CMD_TABLE_SIZE; i++) {
        if (s_cmd_table[i].cmd == cmd) {
            s_cmd_table[i].handler(cmd, data, len);
            return;
        }
    }
    ESP_LOGW(TAG, "Unknown cmd: 0x%02X", cmd);
    ble_send_response(cmd, FRAME_STATUS_UNKNOWN);
}

/**
 * @brief 命令处理任务（事件驱动，与 BLE 回调解耦）
 */
static void ble_cmd_task(void *arg)
{
    ble_cmd_msg_t msg;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdTRUE) {
            handle_command(msg.cmd, msg.data, msg.len);
        }
    }
}

/* ========== GATT 访问回调 ========== */

static int gatt_handler(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);

    switch (uuid) {
    case BLE_CHAR_STATUS_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            BLE_DEBUG_HEX("CHAR_STATUS Read", s_status_data, sizeof(s_status_data));
            BLE_DEBUG_LOG("Status: batt_pct=%d voltage=%d%d connected=%d version=%s",
                          s_status_data[0],
                          s_status_data[1], s_status_data[2],
                          s_status_data[4],
                          (char *)&s_status_data[5]);
            return os_mbuf_append(ctxt->om, s_status_data, sizeof(s_status_data));
        }
        break;

    case BLE_CHAR_LIVE_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, s_live_data, sizeof(s_live_data));
        }
        break;

    case BLE_CHAR_CMD_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint8_t data[256];
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len > sizeof(data)) {
                ESP_LOGW(TAG, "BLE frame truncated: %u -> %u bytes", (unsigned)len, (unsigned)sizeof(data));
                len = sizeof(data);
            }
            ble_hs_mbuf_to_flat(ctxt->om, data, len, NULL);

            /* 帧协议解析（回调中只解析入队，不执行耗时操作） */
            frame_parse_and_dispatch(data, len);
            return 0;
        }
        break;

    case BLE_CHAR_FILELIST_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            /* File List characteristic - only returns SD card file list */
            char buf[512];
            int64_t t0 = esp_timer_get_time();
            s_cbs->sd_get_file_list(buf, sizeof(buf));
            int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
            if (elapsed_ms > 100) {
                ESP_LOGW(TAG, "File list read took %lldms (may cause BLE timeout)", elapsed_ms);
            }
            return os_mbuf_append(ctxt->om, buf, strlen(buf));
        }
        break;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ========== GAP 事件处理 ========== */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            PPG_LOGI(TAG, "gap_event", __LINE__, "BLE connected, handle=%d", s_conn_handle);
            BLE_DEBUG_LOG("Connected: handle=%d", s_conn_handle);
        } else {
            PPG_LOGE(TAG, "gap_event", __LINE__, "BLE connect failed: %d", event->connect.status);
            BLE_DEBUG_LOG("Connect failed: status=%d", event->connect.status);
            ble_svc_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        s_status_subscribed = false;
        PPG_LOGI(TAG, "gap_event", __LINE__, "BLE disconnected, reason=%d", event->disconnect.reason);
        BLE_DEBUG_LOG("Disconnected: reason=%d", event->disconnect.reason);
        ble_svc_start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE Subscribe: attr=%d, val=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        BLE_DEBUG_LOG("Subscribe: attr=0x%04X notify=%d",
                      event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_char_status_handle) {
            s_status_subscribed = (event->subscribe.cur_notify == 1);
            BLE_DEBUG_LOG("Status subscription: %s", s_status_subscribed ? "ON" : "OFF");
        }
        break;
    }

    return 0;
}

/* ========== NimBLE 初始化回调 ========== */

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE synced");
    s_nimble_synced = true;

    int ret = ble_hs_util_ensure_addr(0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Set addr failed: %d", ret);
        return;
    }

    ble_svc_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
    s_nimble_synced = false;
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
}

/* ========== 公共 API ========== */

esp_err_t ble_svc_init(const ble_callbacks_t *callbacks)
{
    s_cbs = callbacks;
    /* Initialize status data with firmware version */
    memset(s_status_data, 0, sizeof(s_status_data));
    strncpy((char *)&s_status_data[5], PPG_FW_VERSION, 15);
    BLE_DEBUG_LOG("Status init: version=%s", PPG_FW_VERSION);

    /* Create command queue */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(ble_cmd_msg_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Create cmd queue failed");
        return ESP_ERR_NO_MEM;
    }

    /* Create command handler task */
    xTaskCreate(ble_cmd_task, "ble_cmd", 4096, NULL, 3, &s_cmd_task_handle);
    ESP_LOGI(TAG, "Cmd task created");

    /* Init NimBLE */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 配置 GAP */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 设置设备名 */
    int gatt_ret = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (gatt_ret != 0) {
        ESP_LOGE(TAG, "Set device name failed: %d", gatt_ret);
        return ESP_FAIL;
    }

    /* 注册 GATT 服务 */
    gatt_ret = ble_gatts_count_cfg(s_gatt_svcs);
    if (gatt_ret != 0) {
        ESP_LOGE(TAG, "GATT count config failed: %d", gatt_ret);
        return ESP_FAIL;
    }

    gatt_ret = ble_gatts_add_svcs(s_gatt_svcs);
    if (gatt_ret != 0) {
        ESP_LOGE(TAG, "GATT service register failed: %d", gatt_ret);
        return ESP_FAIL;
    }

    /* 启动 NimBLE 主机任务 */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE service init done (%s)", BLE_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t ble_svc_start_advertising(void)
{
    if (!s_nimble_synced) {
        BLE_DEBUG_LOG("NimBLE not synced yet, skip advertising");
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0xC8,   /* 200ms */
        .itvl_max = 0xDC,   /* 220ms */
    };

    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (uint8_t *)BLE_DEVICE_NAME,
        .name_len = strlen(BLE_DEVICE_NAME),
        .uuids16 = (ble_uuid16_t[]){
            BLE_UUID16_INIT(BLE_SVC_UUID)
        },
        .num_uuids16 = 1,
    };

    int ret = ble_gap_adv_set_fields(&fields);
    if (ret != 0) {
        ESP_LOGE(TAG, "Set adv fields failed: %d", ret);
        return ESP_FAIL;
    }

    ret = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event, NULL);
    if (ret != 0 && ret != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Start adv failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE advertising started");
    BLE_DEBUG_LOG("Advertising started: name=%s interval=%d-%dms",
                  BLE_DEVICE_NAME, 160, 220);
    return ESP_OK;
}

esp_err_t ble_svc_stop_advertising(void)
{
    int ret = ble_gap_adv_stop();
    if (ret != 0 && ret != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Stop adv failed: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BLE advertising stopped");
    return ESP_OK;
}

esp_err_t ble_svc_notify_live_data(const ppg_result_t *result)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    int32_t hr = result->hr_valid ? result->heart_rate : 0;
    int32_t spo2 = result->spo2_valid ? result->spo2 : 0;

    s_live_data[0] = (hr >> 8) & 0xFF;
    s_live_data[1] = hr & 0xFF;
    s_live_data[2] = (uint8_t)spo2;
    s_live_data[3] = 0;
    s_live_data[4] = (result->hr_valid && result->spo2_valid) ? BLE_QUALITY_VALID : BLE_QUALITY_INVALID;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_live_data, 5);
    if (!om) return ESP_ERR_NO_MEM;

    int ret = ble_gattc_notify_custom(s_conn_handle, s_char_live_handle, om);
    if (ret != 0) {
        ESP_LOGD(TAG, "Live Data notify failed: %d", ret);
    }

    return ESP_OK;
}

esp_err_t ble_svc_update_status(uint8_t batt_pct, uint32_t battery_voltage)
{
    s_status_data[0] = batt_pct;
    s_status_data[1] = (battery_voltage >> 8) & 0xFF;  /* Voltage high byte */
    s_status_data[2] = battery_voltage & 0xFF;          /* Voltage low byte */
    s_status_data[3] = 0;  /* reserved */
    s_status_data[4] = s_connected ? 1 : 0;
    strncpy((char *)&s_status_data[5], PPG_FW_VERSION, 15);

    printf("[BLE] Status: batt_pct=%d%% voltage=%lu.%02luV version=%s\n",
           batt_pct, (unsigned long)(battery_voltage / 100), (unsigned long)(battery_voltage % 100), PPG_FW_VERSION);

    return ESP_OK;
}

bool ble_svc_is_connected(void)
{
    return s_connected;
}

esp_err_t ble_svc_get_peer_addr(uint8_t *addr)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    struct ble_gap_conn_desc desc;
    int ret = ble_gap_conn_find(s_conn_handle, &desc);
    if (ret != 0) return ESP_FAIL;

    memcpy(addr, desc.peer_id_addr.val, 6);
    return ESP_OK;
}
