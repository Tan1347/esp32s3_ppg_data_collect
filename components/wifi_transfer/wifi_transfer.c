/**
 * @file wifi_transfer.c
 * @brief WiFi HTTP 传输服务实现
 *
 * HTTP API:
 *   GET  /api/files           - TF 卡文件列表 JSON
 *   GET  /api/download?file=x - 分块下载 (4KB buffer)
 *   GET  /api/status          - 设备状态 JSON
 *   GET  /api/ota             - OTA 升级页面
 *   POST /api/ota             - 上传固件执行 OTA
 *   GET  /api/logs            - 日志文件列表
 *   GET  /api/logs/download   - 下载日志文件
 *   POST /api/shutdown        - 关闭 WiFi, 回 Deep-sleep
 */

#include "wifi_transfer.h"
#include "ppg_config.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ff.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "wifi_transfer";
static const http_callbacks_t *s_cbs = NULL;

static httpd_handle_t s_server = NULL;
static bool s_running = false;
static uint32_t s_timeout_sec = WIFI_HTTP_TIMEOUT_SEC;

/* ========== 文件下载处理 ========== */

static esp_err_t api_download_handler(httpd_req_t *req)
{
    /* 获取文件名参数 */
    char query[128];
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char filename[64];
    if (httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    /* Path traversal protection */
    if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
        ESP_LOGW(TAG, "Path traversal attempt: %s", filename);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    /* 构建完整路径 */
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, filename);

    /* 打开文件 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    /* Get file size */
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat failed: %s", path);
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stat failed");
        return ESP_FAIL;
    }

    /* Set response headers */
    char content_len[32];
    snprintf(content_len, sizeof(content_len), "%jd", (intmax_t)st.st_size);
    httpd_resp_set_hdr(req, "Content-Length", content_len);
    httpd_resp_set_type(req, "application/octet-stream");

    /* Allocate read buffer */
    char *buf = malloc(OTA_BUFFER_SIZE);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* Pass 1: compute CRC32 over entire file */
    uint32_t crc = 0xFFFFFFFF;
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, OTA_BUFFER_SIZE, f)) > 0) {
        crc = esp_rom_crc32_le(crc, (const uint8_t *)buf, read_bytes);
    }
    fseek(f, 0, SEEK_SET);

    /* Set CRC32 header BEFORE sending chunks (headers are flushed on first chunk) */
    char crc_hex[9];
    snprintf(crc_hex, sizeof(crc_hex), "%08lX", (unsigned long)crc);
    httpd_resp_set_hdr(req, "X-File-CRC32", crc_hex);

    /* Pass 2: send file data in chunks */
    while ((read_bytes = fread(buf, 1, OTA_BUFFER_SIZE, f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_bytes);
    }

    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  /* end chunked transfer */

    ESP_LOGI(TAG, "File download done: %s (%jd bytes) CRC32=%s", filename, (intmax_t)st.st_size, crc_hex);
    return ESP_OK;
}

/* ========== 文件列表处理 ========== */

static esp_err_t api_files_handler(httpd_req_t *req)
{
    char *buf = malloc(1024);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    s_cbs->sd_get_file_list(buf, 1024);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    free(buf);
    return ESP_OK;
}

/* ========== 设备状态处理 ========== */

static esp_err_t api_status_handler(httpd_req_t *req)
{
    char buf[256];
    uint32_t voltage = s_cbs->get_voltage();
    uint8_t batt_pct = s_cbs->voltage_to_soc(voltage);

    char ip[16];
    s_cbs->wifi_get_ip(ip, sizeof(ip));

    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"battery\":{\"batt_pct\":%u,\"voltage\":%lu},"
             "\"ip\":\"%s\",\"sd_free_mb\":%lu}",
             PPG_FW_VERSION, batt_pct, (unsigned long)voltage,
             ip, (unsigned long)s_cbs->sd_get_free_mb());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}

/* ========== OTA 处理 ========== */

static esp_err_t api_ota_page_handler(httpd_req_t *req)
{
    /* 现代化 Web OTA 页面，参考 ESP-WebOTA / ESPAsyncHTTPUpdateServer */
    /* 分段发送避免 % 转义问题 */
    static const char html_head[] =
        "<!DOCTYPE html><html lang='zh'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>PPG Monitor - OTA</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;"
        "display:flex;align-items:center;justify-content:center;padding:20px}"
        ".card{background:#fff;border-radius:16px;box-shadow:0 20px 60px rgba(0,0,0,0.3);"
        "padding:40px;max-width:460px;width:100%}"
        "h1{font-size:24px;color:#333;margin-bottom:8px;text-align:center}"
        ".subtitle{color:#666;text-align:center;margin-bottom:24px;font-size:14px}"
        ".info{background:#f8f9fa;border-radius:8px;padding:12px 16px;margin-bottom:24px;"
        "font-size:13px;color:#555}"
        ".info span{color:#667eea;font-weight:600}"
        ".drop-zone{border:2px dashed #ccc;border-radius:12px;padding:40px 20px;"
        "text-align:center;cursor:pointer;transition:all .3s;margin-bottom:16px}"
        ".drop-zone:hover,.drop-zone.active{border-color:#667eea;background:#f0f2ff}"
        ".drop-zone p{color:#888;font-size:14px;margin-top:8px}"
        ".drop-zone .icon{font-size:48px}"
        ".file-info{display:none;background:#e8f5e9;border-radius:8px;padding:12px;"
        "margin-bottom:16px;font-size:13px;color:#2e7d32;word-break:break-all}"
        ".progress-container{display:none;margin-bottom:16px}"
        ".progress-bar{width:100%;height:24px;background:#e0e0e0;border-radius:12px;"
        "overflow:hidden;position:relative}"
        ".progress-fill{height:100%;background:linear-gradient(90deg,#667eea,#764ba2);"
        "width:0;transition:width .3s;border-radius:12px}"
        ".progress-text{position:absolute;top:0;left:0;right:0;text-align:center;"
        "line-height:24px;font-size:12px;font-weight:600;color:#333}"
        ".btn{width:100%;padding:14px;border:none;border-radius:8px;font-size:16px;"
        "font-weight:600;cursor:pointer;transition:all .3s}"
        ".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff}"
        ".btn-primary:hover{transform:translateY(-2px);box-shadow:0 4px 15px rgba(102,126,234,0.4)}"
        ".btn-primary:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}"
        ".status{margin-top:16px;padding:12px;border-radius:8px;font-size:14px;"
        "text-align:center;display:none}"
        ".status.success{display:block;background:#e8f5e9;color:#2e7d32}"
        ".status.error{display:block;background:#ffebee;color:#c62828}"
        ".status.info{display:block;background:#e3f2fd;color:#1565c0}"
        ".reboot-info{display:none;margin-top:16px;text-align:center}"
        ".reboot-info .countdown{font-size:36px;font-weight:700;color:#667eea}"
        ".reboot-info p{color:#666;font-size:13px;margin-top:4px}"
        "input[type=file]{display:none}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>PPG Monitor</h1>"
        "<p class='subtitle'>固件升级 (OTA)</p>"
        "<div class='info'>"
        "当前版本: <span id='ver'>";

    static const char html_mid[] =
        "</span><br>"
        "编译时间: <span id='build'>";

    static const char html_mid2[] =
        "</span></div>"
        "<div class='drop-zone' id='dropZone' onclick='document.getElementById(\"fileInput\").click()'>"
        "<div class='icon'>&#128230;</div>"
        "<p>拖拽 .bin 固件到此处<br>或点击选择文件</p>"
        "</div>"
        "<div class='file-info' id='fileInfo'></div>"
        "<div class='progress-container' id='progressContainer'>"
        "<div class='progress-bar'>"
        "<div class='progress-fill' id='progressFill'></div>"
        "<div class='progress-text' id='progressText'>0%</div>"
        "</div></div>"
        "<button class='btn btn-primary' id='uploadBtn' disabled onclick='startUpload()'>"
        "开始升级</button>"
        "<div class='status' id='status'></div>"
        "<div class='reboot-info' id='rebootInfo'>"
        "<div class='countdown' id='countdown'>5</div>"
        "<p>秒后自动刷新页面</p>"
        "</div></div>"
        "<input type='file' id='fileInput' accept='.bin' onchange='onFileSelect(this)'>"
        "<script>"
        "var selectedFile=null;"
        "var dropZone=document.getElementById('dropZone');"
        "dropZone.addEventListener('dragover',function(e){e.preventDefault();this.classList.add('active')});"
        "dropZone.addEventListener('dragleave',function(){this.classList.remove('active')});"
        "dropZone.addEventListener('drop',function(e){"
        "e.preventDefault();this.classList.remove('active');"
        "if(e.dataTransfer.files.length)handleFile(e.dataTransfer.files[0])});"
        "function onFileSelect(input){if(input.files.length)handleFile(input.files[0])}"
        "function handleFile(file){"
        "if(!file.name.endsWith('.bin')){showStatus('请选择 .bin 固件文件','error');return}"
        "selectedFile=file;"
        "document.getElementById('fileInfo').style.display='block';"
        "document.getElementById('fileInfo').innerHTML='&#128196; '+file.name+' ('+formatSize(file.size)+')';"
        "document.getElementById('uploadBtn').disabled=false;"
        "showStatus('文件已选择，点击开始升级','info')}"
        "function formatSize(bytes){"
        "if(bytes<1024)return bytes+'B';"
        "if(bytes<1048576)return (bytes/1024).toFixed(1)+'KB';"
        "return (bytes/1048576).toFixed(2)+'MB'}"
        "function startUpload(){"
        "if(!selectedFile)return;"
        "document.getElementById('uploadBtn').disabled=true;"
        "document.getElementById('progressContainer').style.display='block';"
        "showStatus('正在上传...','info');"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/api/ota',true);"
        "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
        "var pct=Math.round(e.loaded/e.total*100);"
        "document.getElementById('progressFill').style.width=pct+'%%';"
        "document.getElementById('progressText').textContent=pct+'%%'}};"
        "xhr.onload=function(){"
        "if(xhr.status===200){"
        "showStatus('升级成功！设备即将重启...','success');"
        "document.getElementById('progressFill').style.width='100%%';"
        "document.getElementById('progressText').textContent='100%%';"
        "startCountdown()}else{"
        "showStatus('升级失败: '+xhr.responseText,'error');"
        "document.getElementById('uploadBtn').disabled=false}};"
        "xhr.onerror=function(){"
        "showStatus('网络错误，请检查连接','error');"
        "document.getElementById('uploadBtn').disabled=false};"
        "xhr.send(selectedFile)}"
        "function showStatus(msg,type){"
        "var el=document.getElementById('status');"
        "el.className='status '+type;el.textContent=msg}"
        "function startCountdown(){"
        "document.getElementById('rebootInfo').style.display='block';"
        "var sec=5;"
        "var timer=setInterval(function(){"
        "sec--;document.getElementById('countdown').textContent=sec;"
        "if(sec<=0){clearInterval(timer);location.reload()}},1000)}"
        "</script></body></html>";

    /* 分段发送 */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, html_head, sizeof(html_head) - 1);

    /* 版本号 */
    const char *ver = s_cbs->ota_get_version();
    httpd_resp_send_chunk(req, ver, strlen(ver));

    httpd_resp_send_chunk(req, html_mid, sizeof(html_mid) - 1);

    /* 编译时间 */
    const char *build_time = s_cbs->ota_get_build_time();
    httpd_resp_send_chunk(req, build_time, strlen(build_time));

    httpd_resp_send_chunk(req, html_mid2, sizeof(html_mid2) - 1);
    httpd_resp_send_chunk(req, NULL, 0);  /* 结束 */

    return ESP_OK;
}

/* ========== OTA 分区信息 ========== */

static esp_err_t api_ota_info_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"current_version\":\"%s\","
             "\"build_time\":\"%s %s\","
             "\"current_size\":%lu,"
             "\"partition_label\":\"%s\","
             "\"partition_offset\":\"0x%08X\","
             "\"partition_size\":%lu,"
             "\"next_partition\":\"%s\"}",
             desc ? desc->version : "unknown",
             desc ? desc->date : "", desc ? desc->time : "",
             running ? (unsigned long)running->size : 0,
             running ? running->label : "unknown",
             running ? (unsigned)running->address : 0,
             running ? (unsigned long)running->size : 0,
             next ? next->label : "none");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    ESP_LOGI(TAG, "OTA info sent");
    return ESP_OK;
}

/* HTTP 读取回调 */
static int http_read_func(void *ctx, void *buf, size_t len)
{
    httpd_req_t *req = (httpd_req_t *)ctx;
    return httpd_req_recv(req, buf, len);
}

static esp_err_t api_ota_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Start Web OTA, fw size: %d bytes", req->content_len);

    /* 使用 ota_upgrade 回调处理 OTA */
    esp_err_t ret = s_cbs->ota_upgrade_from_http(req->content_len, http_read_func, req);

    if (ret == ESP_OK) {
        const char *msg = "OTA 升级成功，即将重启...";
        httpd_resp_send(req, msg, strlen(msg));
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_OK;
    }

    /* OTA 失败 */
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg), "OTA failed: %s", esp_err_to_name(ret));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
    return ESP_FAIL;
}

/* ========== 日志处理 ========== */

static esp_err_t api_logs_handler(httpd_req_t *req)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/log", SD_MOUNT_POINT);

    char *buf = malloc(512);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int pos = snprintf(buf, 512, "{\"logs\":[");

    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && pos < 480) {
            if (entry->d_name[0] == '.') continue;
            if (pos > 10) buf[pos++] = ',';
            pos += snprintf(buf + pos, 512 - pos, "\"%s\"", entry->d_name);
        }
        closedir(dir);
    }

    pos += snprintf(buf + pos, 512 - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ========== Web Manager API Handlers ========== */

static esp_err_t api_system_info_handler(httpd_req_t *req)
{
    char buf[512];
    int uptime_sec = (int)(esp_timer_get_time() / 1000000);

    int len = snprintf(buf, sizeof(buf),
        "{\"firmware_version\":\"%s\","
        "\"build_time\":\"%s\","
        "\"uptime_seconds\":%d,"
        "\"chip_model\":\"ESP32-S3\","
        "\"cpu_freq_mhz\":%d}",
        PPG_FW_VERSION,
        PPG_FW_BUILD_TS,
        uptime_sec,
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t api_storage_info_handler(httpd_req_t *req)
{
    char buf[1024];
    int pos = 0;

    /* TF card info */
    FATFS *fs;
    DWORD fre_clust, total_sect, free_sect;
    int total_mb = 0, free_mb = 0, used_mb = 0;

    if (f_getfree("0:", &fre_clust, &fs) == FR_OK) {
        total_sect = (fs->n_fatent - 2) * fs->csize;
        free_sect = fre_clust * fs->csize;
        total_mb = (int)(total_sect / 2048);
        free_mb = (int)(free_sect / 2048);
        used_mb = total_mb - free_mb;
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"sd_card\":{\"total_mb\":%d,\"used_mb\":%d,\"free_mb\":%d,\"used_percent\":%.1f},"
        "\"directories\":{",
        total_mb, used_mb, free_mb,
        total_mb > 0 ? (100.0 * used_mb / total_mb) : 0);

    /* Scan directories */
    const char *dirs[] = {"raw", "csv", "log", "uart0"};
    for (int d = 0; d < 4; d++) {
        char dir_path[64];
        snprintf(dir_path, sizeof(dir_path), "%s/%s", SD_MOUNT_POINT, dirs[d]);

        DIR *dir = opendir(dir_path);
        int dir_size = 0, file_count = 0;
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char file_path[320];
                snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);
                struct stat st;
                if (stat(file_path, &st) == 0) {
                    dir_size += (int)st.st_size;
                    file_count++;
                }
            }
            closedir(dir);
        }

        if (d > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"%s\":{\"size_mb\":%.1f,\"file_count\":%d}",
            dirs[d], dir_size / (1024.0 * 1024.0), file_count);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "}}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

static esp_err_t api_file_delete_handler(httpd_req_t *req)
{
    /* Get file path parameter */
    char query[128];
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char filepath[128];
    if (httpd_query_key_value(query, "path", filepath, sizeof(filepath)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
        return ESP_FAIL;
    }

    /* Path traversal protection */
    if (strstr(filepath, "..") || strchr(filepath, '/') || strchr(filepath, '\\')) {
        ESP_LOGW(TAG, "Path traversal attempt: %s", filepath);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    /* Ensure path is under SD card mount point */
    if (strncmp(filepath, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) != 0) {
        ESP_LOGW(TAG, "Path outside SD card: %s", filepath);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path outside SD card");
        return ESP_FAIL;
    }

    /* Check if file exists */
    struct stat st;
    if (stat(filepath, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    /* Delete file */
    if (remove(filepath) != 0) {
        ESP_LOGE(TAG, "Delete failed: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleted: %s", filepath);

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"deleted\":\"%s\"}", filepath);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t api_tasks_info_handler(httpd_req_t *req)
{
    char buf[1024];
    int pos = 0;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    /* Get task count */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_status = malloc(sizeof(TaskStatus_t) * task_count);
    uint32_t total_runtime;

    if (task_status) {
        task_count = uxTaskGetSystemState(task_status, task_count, &total_runtime);

        pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"tasks\":[");

        for (int i = 0; i < task_count; i++) {
            if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");

            float cpu_percent = 0;
            if (total_runtime > 0) {
                cpu_percent = 100.0f * task_status[i].ulRunTimeCounter / total_runtime;
            }

            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"name\":\"%s\",\"state\":%d,\"priority\":%u,\"stack_free\":%lu,\"cpu_percent\":%.1f}",
                task_status[i].pcTaskName,
                task_status[i].eCurrentState,
                task_status[i].uxCurrentPriority,
                task_status[i].usStackHighWaterMark * sizeof(StackType_t),
                cpu_percent);
        }

        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        free(task_status);
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"tasks\":[]}");
    }
#else
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"tasks\":[],\"note\":\"Trace facility disabled\"}");
#endif

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    return ESP_OK;
}

static esp_err_t api_memory_info_handler(httpd_req_t *req)
{
    char buf[512];

    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    int len = snprintf(buf, sizeof(buf),
        "{\"heap\":{\"total\":%u,\"free\":%u,\"used\":%u,\"used_percent\":%.1f,"
        "\"min_free_ever\":%u,\"largest_block\":%u}}",
        (unsigned)total_heap,
        (unsigned)free_heap,
        (unsigned)(total_heap - free_heap),
        100.0 * (total_heap - free_heap) / total_heap,
        (unsigned)min_free,
        (unsigned)largest_block);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* Web Manager main page */
static esp_err_t api_web_manager_handler(httpd_req_t *req)
{
    /* Embedded HTML page - compact single page app */
    static const char html[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PPG Monitor Manager</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,sans-serif;background:#f5f5f5;color:#333}"
    ".header{background:#1976d2;color:#fff;padding:16px;text-align:center}"
    ".tabs{display:flex;background:#fff;border-bottom:1px solid #ddd}"
    ".tab{flex:1;padding:12px;text-align:center;cursor:pointer;border-bottom:2px solid transparent}"
    ".tab.active{border-bottom-color:#1976d2;color:#1976d2;font-weight:bold}"
    ".content{padding:16px;max-width:800px;margin:0 auto}"
    ".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 1px 3px rgba(0,0,0,0.1)}"
    ".card h3{margin-bottom:12px;color:#1976d2}"
    ".info-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #eee}"
    ".progress-bar{height:20px;background:#e0e0e0;border-radius:10px;overflow:hidden;margin:8px 0}"
    ".progress-fill{height:100%;background:#4caf50;transition:width 0.3s}"
    ".progress-fill.warning{background:#ff9800}"
    ".progress-fill.danger{background:#f44336}"
    "table{width:100%;border-collapse:collapse}"
    "th,td{padding:8px;text-align:left;border-bottom:1px solid #eee}"
    "th{background:#f5f5f5;font-weight:bold}"
    ".btn{padding:6px 12px;border:none;border-radius:4px;cursor:pointer;font-size:12px}"
    ".btn-danger{background:#f44336;color:#fff}"
    ".btn-download{background:#1976d2;color:#fff}"
    ".hidden{display:none}"
    ".refresh-btn{position:fixed;bottom:20px;right:20px;width:50px;height:50px;border-radius:50%;background:#1976d2;color:#fff;border:none;font-size:20px;cursor:pointer;box-shadow:0 2px 5px rgba(0,0,0,0.3)}"
    "</style></head><body>"
    "<div class='header'><h1>PPG Monitor</h1><p>Web Manager</p></div>"
    "<div class='tabs'>"
    "<div class='tab active' onclick='showTab(0)'>System</div>"
    "<div class='tab' onclick='showTab(1)'>Storage</div>"
    "<div class='tab' onclick='showTab(2)'>Tasks</div>"
    "<div class='tab' onclick='showTab(3)'>Memory</div>"
    "</div>"
    "<div class='content'>"
    "<div id='tab0'></div><div id='tab1' class='hidden'></div>"
    "<div id='tab2' class='hidden'></div><div id='tab3' class='hidden'></div>"
    "</div>"
    "<button class='refresh-btn' onclick='refresh()'>↻</button>"
    "<script>"
    "let currentTab=0;"
    "function showTab(n){"
    "  document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',i===n));"
    "  document.querySelectorAll('[id^=tab]').forEach((t,i)=>t.classList.toggle('hidden',i!==n));"
    "  currentTab=n;refresh();"
    "}"
    "async function fetchJson(url){try{const r=await fetch(url);return await r.json()}catch(e){return null}}"
    "function progressBar(pct){"
    "  const cls=pct>80?'danger':pct>60?'warning':'';"
    "  return `<div class='progress-bar'><div class='progress-fill ${cls}' style='width:${pct}%'></div></div>`;"
    "}"
    "async function refresh(){"
    "  if(currentTab===0){"
    "    const d=await fetchJson('/api/system');"
    "    if(!d)return;"
    "    document.getElementById('tab0').innerHTML=`"
    "      <div class='card'><h3>System Info</h3>"
    "      <div class='info-row'><span>Firmware</span><span>${d.firmware_version}</span></div>"
    "      <div class='info-row'><span>Build</span><span>${d.build_time}</span></div>"
    "      <div class='info-row'><span>Uptime</span><span>${Math.floor(d.uptime_seconds/3600)}h ${Math.floor(d.uptime_seconds%3600/60)}m</span></div>"
    "      <div class='info-row'><span>Chip</span><span>${d.chip_model}</span></div>"
    "      <div class='info-row'><span>CPU</span><span>${d.cpu_freq_mhz} MHz</span></div>"
    "      </div>`;"
    "  }else if(currentTab===1){"
    "    const d=await fetchJson('/api/storage');"
    "    if(!d)return;"
    "    let html=`<div class='card'><h3>TF Card</h3>"
    "      <div class='info-row'><span>Total</span><span>${d.sd_card.total_mb} MB</span></div>"
    "      <div class='info-row'><span>Used</span><span>${d.sd_card.used_mb} MB (${d.sd_card.used_percent}%)</span></div>"
    "      <div class='info-row'><span>Free</span><span>${d.sd_card.free_mb} MB</span></div>"
    "      ${progressBar(d.sd_card.used_percent)}</div>`;"
    "    html+=`<div class='card'><h3>Directories</h3><table><tr><th>Name</th><th>Size</th><th>Files</th></tr>`;"
    "    for(const[name,info]of Object.entries(d.directories)){"
    "      html+=`<tr><td>${name}</td><td>${info.size_mb.toFixed(1)} MB</td><td>${info.file_count}</td></tr>`;"
    "    }"
    "    html+=`</table></div><div id='fileList'></div>`;"
    "    document.getElementById('tab1').innerHTML=html;"
    "    loadFiles();"
    "  }else if(currentTab===2){"
    "    const d=await fetchJson('/api/tasks');"
    "    if(!d)return;"
    "    let html=`<div class='card'><h3>FreeRTOS Tasks</h3><table>"
    "      <tr><th>Name</th><th>Priority</th><th>Stack Free</th><th>CPU %</th></tr>`;"
    "    d.tasks.forEach(t=>{"
    "      const state=['Running','Ready','Blocked','Suspended','Deleted'][t.state]||'Unknown';"
    "      html+=`<tr><td>${t.name}</td><td>${t.priority}</td><td>${t.stack_free} B</td><td>${t.cpu_percent.toFixed(1)}%</td></tr>`;"
    "    });"
    "    html+=`</table></div>`;"
    "    document.getElementById('tab2').innerHTML=html;"
    "  }else if(currentTab===3){"
    "    const d=await fetchJson('/api/memory');"
    "    if(!d)return;"
    "    document.getElementById('tab3').innerHTML=`"
    "      <div class='card'><h3>Heap Memory</h3>"
    "      <div class='info-row'><span>Total</span><span>${(d.heap.total/1024).toFixed(1)} KB</span></div>"
    "      <div class='info-row'><span>Used</span><span>${(d.heap.used/1024).toFixed(1)} KB (${d.heap.used_percent.toFixed(1)}%)</span></div>"
    "      <div class='info-row'><span>Free</span><span>${(d.heap.free/1024).toFixed(1)} KB</span></div>"
    "      <div class='info-row'><span>Min Free Ever</span><span>${(d.heap.min_free_ever/1024).toFixed(1)} KB</span></div>"
    "      <div class='info-row'><span>Largest Block</span><span>${(d.heap.largest_block/1024).toFixed(1)} KB</span></div>"
    "      ${progressBar(d.heap.used_percent)}</div>`;"
    "  }"
    "}"
    "async function loadFiles(){"
    "  const d=await fetchJson('/api/files');"
    "  if(!d||!d.files)return;"
    "  let html=`<div class='card'><h3>Files</h3><table>"
    "    <tr><th>Name</th><th>Size</th><th>Action</th></tr>`;"
    "  d.files.forEach(f=>{"
    "    const size=f.size>1048576?(f.size/1048576).toFixed(1)+' MB':(f.size/1024).toFixed(1)+' KB';"
    "    html+=`<tr><td>${f.name}</td><td>${size}</td>"
    "      <td><button class='btn btn-download' onclick=\"window.open('/api/download?file=${f.name}')\">Download</button> "
    "      <button class='btn btn-danger' onclick=\"deleteFile('${f.name}')\">Delete</button></td></tr>`;"
    "  });"
    "  html+=`</table></div>`;"
    "  document.getElementById('fileList').innerHTML=html;"
    "}"
    "async function deleteFile(name){"
    "  if(!confirm('Delete '+name+'?'))return;"
    "  try{"
    "    const r=await fetch('/api/files/delete?path=/sdcard/'+name,{method:'POST'});"
    "    const d=await r.json();"
    "    if(d.status==='ok'){alert('Deleted');loadFiles();refresh();}"
    "    else alert('Delete failed');"
    "  }catch(e){alert('Error: '+e);}"
    "}"
    "refresh();setInterval(refresh,5000);loadFiles();"
    "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, sizeof(html) - 1);
    return ESP_OK;
}

/* ========== 关闭处理 ========== */

static esp_err_t api_shutdown_handler(httpd_req_t *req)
{
    const char *msg = "WiFi closing soon";
    httpd_resp_send(req, msg, strlen(msg));

    /* 通知主循环关闭 WiFi */
    s_cbs->set_state(STATE_WIFI_SHUTDOWN);

    return ESP_OK;
}

/* ========== 根路径：Web 管理界面 ========== */

static esp_err_t root_handler(httpd_req_t *req)
{
    return api_web_manager_handler(req);
}

/* ========== 公共 API ========== */

esp_err_t wifi_transfer_start(const http_callbacks_t *callbacks)
{
    s_cbs = callbacks;
    if (s_running) return ESP_OK;

    /* 连接 WiFi */
    esp_err_t ret = s_cbs->wifi_auto_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        return ret;
    }

    /* 等待连接 */
    int retry = 0;
    while (!s_cbs->wifi_is_connected() && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }

    if (!s_cbs->wifi_is_connected()) {
        ESP_LOGE(TAG, "WiFi connect timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* 启动 HTTP Server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 18;
    config.stack_size = 8192;

    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 注册 URI 处理器 */
    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET,
        .handler = root_handler
    };
    httpd_uri_t uri_files = {
        .uri = "/api/files", .method = HTTP_GET,
        .handler = api_files_handler
    };
    httpd_uri_t uri_download = {
        .uri = "/api/download", .method = HTTP_GET,
        .handler = api_download_handler
    };
    httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = api_status_handler
    };
    httpd_uri_t uri_ota_page = {
        .uri = "/api/ota", .method = HTTP_GET,
        .handler = api_ota_page_handler
    };
    httpd_uri_t uri_ota_info = {
        .uri = "/api/ota/info", .method = HTTP_GET,
        .handler = api_ota_info_handler
    };
    httpd_uri_t uri_ota_upload = {
        .uri = "/api/ota", .method = HTTP_POST,
        .handler = api_ota_upload_handler
    };
    httpd_uri_t uri_logs = {
        .uri = "/api/logs", .method = HTTP_GET,
        .handler = api_logs_handler
    };
    httpd_uri_t uri_shutdown = {
        .uri = "/api/shutdown", .method = HTTP_POST,
        .handler = api_shutdown_handler
    };
    httpd_uri_t uri_system = {
        .uri = "/api/system", .method = HTTP_GET,
        .handler = api_system_info_handler
    };
    httpd_uri_t uri_storage = {
        .uri = "/api/storage", .method = HTTP_GET,
        .handler = api_storage_info_handler
    };
    httpd_uri_t uri_tasks = {
        .uri = "/api/tasks", .method = HTTP_GET,
        .handler = api_tasks_info_handler
    };
    httpd_uri_t uri_memory = {
        .uri = "/api/memory", .method = HTTP_GET,
        .handler = api_memory_info_handler
    };
    httpd_uri_t uri_file_delete = {
        .uri = "/api/files/delete", .method = HTTP_POST,
        .handler = api_file_delete_handler
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_files);
    httpd_register_uri_handler(s_server, &uri_download);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_ota_page);
    httpd_register_uri_handler(s_server, &uri_ota_info);
    httpd_register_uri_handler(s_server, &uri_ota_upload);
    httpd_register_uri_handler(s_server, &uri_logs);
    httpd_register_uri_handler(s_server, &uri_shutdown);
    httpd_register_uri_handler(s_server, &uri_system);
    httpd_register_uri_handler(s_server, &uri_storage);
    httpd_register_uri_handler(s_server, &uri_tasks);
    httpd_register_uri_handler(s_server, &uri_memory);
    httpd_register_uri_handler(s_server, &uri_file_delete);

    s_running = true;

    char ip[16];
    s_cbs->wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "HTTP Server started: http://%s", ip);
    ESP_LOGI(TAG, "  File list: http://%s/api/files", ip);
    ESP_LOGI(TAG, "  Status: http://%s/api/status", ip);
    ESP_LOGI(TAG, "  OTA: http://%s/api/ota", ip);
    ESP_LOGI(TAG, "  OTA Info: http://%s/api/ota/info", ip);

    return ESP_OK;
}

esp_err_t wifi_transfer_stop(void)
{
    if (!s_running) return ESP_OK;

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    s_cbs->wifi_disconnect();
    s_running = false;

    ESP_LOGI(TAG, "WiFi HTTP Server stopped");
    return ESP_OK;
}

void wifi_transfer_set_timeout(uint32_t seconds)
{
    s_timeout_sec = seconds;
    ESP_LOGI(TAG, "WiFi timeout set to %ds", seconds);
}

esp_err_t wifi_transfer_start_ota(void)
{
    /* s_cbs should already be set from previous wifi_transfer_start call */
    return wifi_transfer_start(s_cbs);
}

bool wifi_transfer_is_running(void)
{
    return s_running;
}
