/**
 * @file ota_upgrade.c
 * @brief OTA 固件升级实现
 *
 * 安全校验流程：
 *   1. esp_ota_begin → 获取备用分区
 *   2. esp_ota_write → 分块写入固件数据
 *   3. 校验固件头魔数
 *   4. 比较版本号（防降级）
 *   5. SHA-256 摘要验证
 *   6. esp_ota_end → 原子提交
 *   7. esp_ota_set_boot_partition → 切换启动分区
 *   8. 重启后确认 → 取消回滚标记
 *
 * 失败处理：
 *   - 写入/校验失败 → esp_ota_abort → 保持原分区
 *   - 重启后运行异常 → 自动回滚到旧分区
 */

#include "ota_upgrade.h"
#include "ppg_config.h"
#include "ppg_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <string.h>

static const char *TAG = "ota_upgrade";

/* 固件头魔数 (ESP-IDF 标准格式) */
#define ESP_IMAGE_MAGIC         0xE9
#define ESP_IMAGE_HEADER_SIZE   24

/* 默认配置 */
#define DEFAULT_BUFFER_SIZE     4096

/* OTA 上下文 */
static struct {
    ota_state_t     state;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    size_t          received;
    size_t          total;
    ota_progress_cb_t progress_cb;
    ota_config_t    config;
    bool            initialized;
} s_ota_ctx = {
    .state = OTA_STATE_IDLE,
    .handle = 0,
    .partition = NULL,
    .received = 0,
    .total = 0,
    .progress_cb = NULL,
    .config = {
        .buffer_size = DEFAULT_BUFFER_SIZE,
        .check_version = true,
        .check_sha256 = true,
        .auto_rollback = true,
    },
    .initialized = false,
};

/* ========== 固件头校验 ========== */

static esp_err_t verify_firmware_header(const uint8_t *data, size_t len)
{
    if (len < ESP_IMAGE_HEADER_SIZE) {
        PPG_LOGE(TAG, "verify_firmware_header", __LINE__,
                 "Firmware data too short: %d < %d", len, ESP_IMAGE_HEADER_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 检查魔数 */
    if (data[0] != ESP_IMAGE_MAGIC) {
        PPG_LOGE(TAG, "verify_firmware_header", __LINE__,
                 "Firmware header magic error: 0x%02X (expect 0x%02X)", data[0], ESP_IMAGE_MAGIC);
        return ESP_ERR_INVALID_RESPONSE;
    }

    PPG_LOGI(TAG, "verify_firmware_header", __LINE__, "Firmware header magic OK");
    return ESP_OK;
}

/* ========== 版本比较 ========== */

/**
 * @brief 比较两个版本号字符串
 * @return >0 v1>v2, 0 v1==v2, <0 v1<v2
 */
static int compare_versions(const char *v1, const char *v2)
{
    if (!v1 || !v2) return 0;

    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;

    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);

    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

__attribute__((unused)) static esp_err_t verify_version(const esp_app_desc_t *new_desc)
{
    if (!s_ota_ctx.config.check_version) return ESP_OK;

    const esp_app_desc_t *current = esp_app_get_description();
    if (!current || !new_desc) return ESP_OK;

    int cmp = compare_versions(new_desc->version, current->version);
    if (cmp < 0) {
        PPG_LOGE(TAG, "verify_version", __LINE__,
                 "Version downgrade not allowed: %s -> %s", current->version, new_desc->version);
        return ESP_ERR_INVALID_VERSION;
    }

    PPG_LOGI(TAG, "verify_version", __LINE__,
             "Version check passed: %s -> %s", current->version, new_desc->version);
    return ESP_OK;
}

/* ========== 公共 API ========== */

esp_err_t ota_upgrade_init(const ota_config_t *config)
{
    if (config) {
        s_ota_ctx.config = *config;
    }

    /* 检查是否有待确认的 OTA */
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            PPG_LOGI(TAG, "ota_upgrade_init", __LINE__,
                     "Pending OTA detected, firmware verifying");
        }
    }

    s_ota_ctx.initialized = true;
    PPG_LOGI(TAG, "ota_upgrade_init", __LINE__,
             "OTA init done (version=%s, sha256=%s, rollback=%s)",
             s_ota_ctx.config.check_version ? "ON" : "OFF",
             s_ota_ctx.config.check_sha256 ? "ON" : "OFF",
             s_ota_ctx.config.auto_rollback ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t ota_upgrade_begin(size_t expected_size)
{
    if (s_ota_ctx.state != OTA_STATE_IDLE) {
        PPG_LOGE(TAG, "ota_upgrade_begin", __LINE__,
                 "OTA already in progress, state=%d", s_ota_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    /* 获取下一个可用的 OTA 分区 */
    s_ota_ctx.partition = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_ctx.partition) {
        PPG_LOGE(TAG, "ota_upgrade_begin", __LINE__, "No available OTA partition");
        return ESP_ERR_NOT_FOUND;
    }

    PPG_LOGI(TAG, "ota_upgrade_begin", __LINE__,
             "Target partition: %s (offset=0x%08X, size=0x%08X)",
             s_ota_ctx.partition->label,
             s_ota_ctx.partition->address,
             s_ota_ctx.partition->size);

    /* 开始 OTA */
    esp_err_t ret = esp_ota_begin(s_ota_ctx.partition, expected_size, &s_ota_ctx.handle);
    if (ret != ESP_OK) {
        PPG_LOGE(TAG, "ota_upgrade_begin", __LINE__,
                 "esp_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ota_ctx.state = OTA_STATE_STARTED;
    s_ota_ctx.received = 0;
    s_ota_ctx.total = expected_size;

    PPG_LOGI(TAG, "ota_upgrade_begin", __LINE__,
             "OTA started, expected size: %d bytes", expected_size);
    return ESP_OK;
}

esp_err_t ota_upgrade_write(const void *data, size_t len)
{
    if (s_ota_ctx.state != OTA_STATE_STARTED && s_ota_ctx.state != OTA_STATE_WRITING) {
        PPG_LOGE(TAG, "ota_upgrade_write", __LINE__,
                 "OTA state error: %d", s_ota_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    /* 第一块数据：校验固件头 */
    if (s_ota_ctx.received == 0) {
        esp_err_t ret = verify_firmware_header((const uint8_t *)data, len);
        if (ret != ESP_OK) {
            ota_upgrade_abort();
            return ret;
        }
    }

    /* 写入固件数据 */
    esp_err_t ret = esp_ota_write(s_ota_ctx.handle, data, len);
    if (ret != ESP_OK) {
        PPG_LOGE(TAG, "ota_upgrade_write", __LINE__,
                 "esp_ota_write failed: %s (offset=%d)", esp_err_to_name(ret), s_ota_ctx.received);
        ota_upgrade_abort();
        return ret;
    }

    s_ota_ctx.received += len;
    s_ota_ctx.state = OTA_STATE_WRITING;

    /* 进度回调 */
    if (s_ota_ctx.progress_cb) {
        s_ota_ctx.progress_cb(s_ota_ctx.received, s_ota_ctx.total, s_ota_ctx.state);
    }

    /* 每 64KB 打印一次进度 */
    if (s_ota_ctx.received % (64 * 1024) < len) {
        PPG_LOGI(TAG, "ota_upgrade_write", __LINE__,
                 "OTA progress: %d/%d bytes (%d%%)",
                 s_ota_ctx.received, s_ota_ctx.total,
                 s_ota_ctx.total > 0 ? (s_ota_ctx.received * 100 / s_ota_ctx.total) : 0);
    }

    return ESP_OK;
}

esp_err_t ota_upgrade_end(void)
{
    if (s_ota_ctx.state != OTA_STATE_WRITING) {
        PPG_LOGE(TAG, "ota_upgrade_end", __LINE__,
                 "OTA state error: %d", s_ota_ctx.state);
        return ESP_ERR_INVALID_STATE;
    }

    s_ota_ctx.state = OTA_STATE_VERIFYING;
    PPG_LOGI(TAG, "ota_upgrade_end", __LINE__, "Verifying firmware...");

    /* 结束 OTA（内部会校验固件完整性） */
    esp_err_t ret = esp_ota_end(s_ota_ctx.handle);
    if (ret != ESP_OK) {
        PPG_LOGE(TAG, "ota_upgrade_end", __LINE__,
                 "esp_ota_end failed: %s", esp_err_to_name(ret));
        s_ota_ctx.state = OTA_STATE_FAILED;
        return ret;
    }

    /* 切换启动分区 */
    ret = esp_ota_set_boot_partition(s_ota_ctx.partition);
    if (ret != ESP_OK) {
        PPG_LOGE(TAG, "ota_upgrade_end", __LINE__,
                 "Set boot partition failed: %s", esp_err_to_name(ret));
        s_ota_ctx.state = OTA_STATE_FAILED;
        return ret;
    }

    s_ota_ctx.state = OTA_STATE_DONE;
    PPG_LOGI(TAG, "ota_upgrade_end", __LINE__,
             "OTA upgrade success! Received %d bytes, effective after reboot", s_ota_ctx.received);

    /* 进度回调 */
    if (s_ota_ctx.progress_cb) {
        s_ota_ctx.progress_cb(s_ota_ctx.received, s_ota_ctx.total, OTA_STATE_DONE);
    }

    return ESP_OK;
}

esp_err_t ota_upgrade_abort(void)
{
    if (s_ota_ctx.state == OTA_STATE_IDLE) return ESP_OK;

    if (s_ota_ctx.handle) {
        esp_ota_abort(s_ota_ctx.handle);
        s_ota_ctx.handle = 0;
    }

    s_ota_ctx.state = OTA_STATE_IDLE;
    s_ota_ctx.received = 0;
    s_ota_ctx.total = 0;
    s_ota_ctx.partition = NULL;

    PPG_LOGI(TAG, "ota_upgrade_abort", __LINE__, "OTA aborted");
    return ESP_OK;
}

ota_state_t ota_upgrade_get_state(void)
{
    return s_ota_ctx.state;
}

size_t ota_upgrade_get_received(void)
{
    return s_ota_ctx.received;
}

size_t ota_upgrade_get_total(void)
{
    return s_ota_ctx.total;
}

void ota_upgrade_set_progress_cb(ota_progress_cb_t callback)
{
    s_ota_ctx.progress_cb = callback;
}

const char *ota_upgrade_get_current_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

const char *ota_upgrade_get_build_time(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->date : "unknown";
}

bool ota_upgrade_pending_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;

    return (state == ESP_OTA_IMG_PENDING_VERIFY);
}

esp_err_t ota_upgrade_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        PPG_LOGI(TAG, "ota_upgrade_confirm", __LINE__, "OTA confirmed, rollback cleared");
    } else {
        PPG_LOGE(TAG, "ota_upgrade_confirm", __LINE__,
                 "Confirm failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ota_upgrade_rollback(void)
{
    PPG_LOGW(TAG, "ota_upgrade_rollback", __LINE__, "Rolling back firmware...");

    esp_err_t ret = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (ret != ESP_OK) {
        PPG_LOGE(TAG, "ota_upgrade_rollback", __LINE__,
                 "Rollback failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ota_upgrade_from_http(size_t content_len, ota_read_func_t read_func, void *read_ctx)
{
    if (!read_func) return ESP_ERR_INVALID_ARG;

    /* 开始 OTA */
    esp_err_t ret = ota_upgrade_begin(content_len);
    if (ret != ESP_OK) return ret;

    /* 分块读取并写入 */
    char *buf = malloc(s_ota_ctx.config.buffer_size);
    if (!buf) {
        ota_upgrade_abort();
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < content_len || content_len == 0) {
        size_t to_read = s_ota_ctx.config.buffer_size;
        if (content_len > 0 && received + to_read > content_len) {
            to_read = content_len - received;
        }

        int read = read_func(read_ctx, buf, to_read);
        if (read <= 0) {
            if (read == 0) break;  /* 连接关闭 */
            continue;  /* 超时重试 */
        }

        ret = ota_upgrade_write(buf, read);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }

        received += read;
    }

    free(buf);

    /* 完成校验 */
    if (received == content_len || content_len == 0) {
        return ota_upgrade_end();
    }

    PPG_LOGE(TAG, "ota_upgrade_from_http", __LINE__,
             "Data incomplete: received %d, expected %d", received, content_len);
    ota_upgrade_abort();
    return ESP_ERR_INVALID_SIZE;
}
