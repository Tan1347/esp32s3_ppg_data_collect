/**
 * @file sd_storage.c
 * @brief TF 卡存储实现
 *
 * SPI 模式连接 TF 卡, FAT32 文件系统
 * 主备缓冲策略: 主缓冲 64KB + 备缓冲 16KB
 * 主缓冲写入文件时，备缓冲继续接收数据
 */

#include "sd_storage.h"
#include "ppg_config.h"
#include "compress.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "sd_storage";

/* 挂载点 */
#define MOUNT_POINT SD_MOUNT_POINT

/* 主备缓冲 — S3 has 8MB PSRAM, use it for large buffers */
#define PRIMARY_BUF_SIZE    (32 * 1024)     /* 32KB 主缓冲 (40秒填充, PSRAM) */
#define BACKUP_BUF_SIZE     (8 * 1024)      /* 8KB 备缓冲 (10秒填充, PSRAM) */
#define CSV_BUF_SIZE        (8 * 1024)      /* 8KB CSV 缓冲 (PSRAM) */
#define COMPRESS_LEVEL      6               /* 压缩等级 1-9（6=平衡） */

/* 缓冲区结构 */
typedef struct {
    uint8_t *data;          /* 数据指针 */
    size_t   size;          /* 缓冲区大小 */
    size_t   pos;           /* 当前写入位置 */
    bool     is_writing;    /* 是否正在写入文件 */
} buffer_t;

static buffer_t s_primary_buf = {0};    /* 主缓冲 */
static buffer_t s_backup_buf = {0};     /* 备缓冲 */
static buffer_t *s_active_buf = NULL;   /* 当前活跃缓冲 */

static char    *s_csv_buf = NULL;
static size_t   s_csv_buf_pos = 0;
static int      s_raw_fd = -1;
static int      s_csv_fd = -1;

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

/* Current file names */
static char s_current_raw_path[64];
static char s_current_csv_path[64];
static char s_current_dht11_path[64];

/* DHT11 buffer */
static char    *s_dht11_buf = NULL;
static size_t   s_dht11_buf_pos = 0;
static int      s_dht11_fd = -1;

/* Mutex for buffer switching */
static SemaphoreHandle_t s_buf_mutex = NULL;

/* Mutex for CSV and DHT11 buffer access */
static SemaphoreHandle_t s_csv_mutex = NULL;
static SemaphoreHandle_t s_dht11_mutex = NULL;

/* ========== 文件名生成 ========== */

static void generate_raw_filename(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info && tm_info->tm_year > 100) {
        snprintf(buf, len, "%s/raw/%04d%02d%02d_%02d.bin",
                 MOUNT_POINT, tm_info->tm_year + 1900, tm_info->tm_mon + 1,
                 tm_info->tm_mday, tm_info->tm_hour);
    } else {
        snprintf(buf, len, "%s/raw/unknown.bin", MOUNT_POINT);
    }
}

static void generate_csv_filename(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info && tm_info->tm_year > 100) {
        snprintf(buf, len, "%s/csv/%04d%02d%02d.csv",
                 MOUNT_POINT, tm_info->tm_year + 1900, tm_info->tm_mon + 1,
                 tm_info->tm_mday);
    } else {
        snprintf(buf, len, "%s/csv/unknown.csv", MOUNT_POINT);
    }
}

static void generate_dht11_filename(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info && tm_info->tm_year > 100) {
        snprintf(buf, len, "%s/env/%04d%02d%02d.bin",
                 MOUNT_POINT, tm_info->tm_year + 1900, tm_info->tm_mon + 1,
                 tm_info->tm_mday);
    } else {
        snprintf(buf, len, "%s/env/unknown.bin", MOUNT_POINT);
    }
}

/* ========== 目录创建 ========== */

static esp_err_t ensure_directories(void)
{
    const char *dirs[] = {"/raw", "/csv", "/log", "/env"};
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "%s%s", MOUNT_POINT, dirs[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            if (mkdir(path, 0777) != 0) {
                ESP_LOGW(TAG, "Create dir failed: %s", path);
            }
        }
    }
    return ESP_OK;
}

/* ========== Safe write (POSIX) ========== */

/**
 * @brief Safe write with retry on partial writes
 * @return total bytes written, or -1 on error
 */
static ssize_t safe_write(int fd, const void *ptr, size_t len)
{
    const uint8_t *p = (const uint8_t *)ptr;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t w = write(fd, p, remaining);
        if (w < 0) {
            ESP_LOGE(TAG, "write failed: errno=%d", errno);
            return -1;
        }
        p += w;
        remaining -= w;
    }
    return (ssize_t)len;
}

/* ========== 缓冲区操作 ========== */

/**
 * @brief 初始化缓冲区 (prefer PSRAM for large buffers)
 */
static esp_err_t init_buffer(buffer_t *buf, size_t size)
{
    /* Try PSRAM first for large buffers, fallback to internal SRAM */
    if (size >= 4096) {
        buf->data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        buf->data = malloc(size);
    }
    if (!buf->data) {
        buf->data = malloc(size);  /* Fallback to internal SRAM */
    }
    if (!buf->data) {
        ESP_LOGE(TAG, "Buffer alloc failed (%u bytes)", (unsigned)size);
        return ESP_ERR_NO_MEM;
    }
    buf->size = size;
    buf->pos = 0;
    buf->is_writing = false;
    return ESP_OK;
}

/**
 * @brief 向缓冲区写入数据
 * @return 写入的字节数，0 表示缓冲区满
 */
static size_t buffer_write(buffer_t *buf, const void *data, size_t len)
{
    if (buf->is_writing) {
        return 0;  /* 正在写入文件，不能写入 */
    }

    size_t free_space = buf->size - buf->pos;
    size_t to_write = (len > free_space) ? free_space : len;

    if (to_write > 0) {
        memcpy(buf->data + buf->pos, data, to_write);
        buf->pos += to_write;
    }

    return to_write;
}

/**
 * @brief 切换主备缓冲区
 */
static buffer_t* switch_buffer(void)
{
    buffer_t *new_active = NULL;

    if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_active_buf == &s_primary_buf) {
            s_active_buf = &s_backup_buf;
        } else {
            s_active_buf = &s_primary_buf;
        }
        new_active = s_active_buf;
        xSemaphoreGive(s_buf_mutex);
    }

    return new_active;
}

/**
 * @brief Flush buffer to file (POSIX write)
 */
static esp_err_t flush_buffer(buffer_t *buf, int fd)
{
    if (buf->pos == 0 || fd < 0) {
        return ESP_OK;
    }

    buf->is_writing = true;

    /* Compress if possible */
    size_t comp_bound = compress_bound(buf->pos);
    uint8_t *comp_buf = malloc(comp_bound);

    if (comp_buf) {
        size_t comp_len = comp_bound;
        esp_err_t ret = compress_data(buf->data, buf->pos,
                                      comp_buf, &comp_len, COMPRESS_LEVEL);
        if (ret == ESP_OK && comp_len < buf->pos) {
            uint32_t orig_size = (uint32_t)buf->pos;
            uint32_t comp_size = (uint32_t)comp_len;
            safe_write(fd, &orig_size, 4);
            safe_write(fd, &comp_size, 4);
            safe_write(fd, comp_buf, comp_len);
            ESP_LOGD(TAG, "Compressed: %u -> %u (%.1f%%)",
                     (unsigned)buf->pos, (unsigned)comp_len,
                     100.0 * comp_len / buf->pos);
        } else {
            safe_write(fd, buf->data, buf->pos);
        }
        free(comp_buf);
    } else {
        safe_write(fd, buf->data, buf->pos);
    }

    fsync(fd);
    buf->pos = 0;
    buf->is_writing = false;

    return ESP_OK;
}

/* ========== 公共 API ========== */

esp_err_t sd_storage_init(void)
{
    /* SD_CARD_CD disabled: GPIO8 is a strapping pin, using it for card detection
     * could cause boot issues when SD card is inserted (GPIO8 pulled low).
     * Card presence is detected at mount time instead. */

    /* Create mutex */
    s_buf_mutex = xSemaphoreCreateMutex();
    if (!s_buf_mutex) {
        ESP_LOGE(TAG, "Create mutex failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD storage init done (no card detect pin)");
    return ESP_OK;
}

bool sd_storage_card_inserted(void)
{
    /* No hardware card detection - assume card is present.
     * Actual presence is verified at mount time. */
    return true;
}

esp_err_t sd_storage_mount(void)
{
    if (s_mounted) return ESP_OK;

    /* 检测 TF 卡是否插入 */
    if (!sd_storage_card_inserted()) {
        ESP_LOGW(TAG, "SD card not inserted");
        return ESP_ERR_NOT_FOUND;
    }

    /* SPI configuration - start at 400kHz for SD card init */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 400;  /* 400kHz for initialization */

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_SPI_MOSI_PIN,
        .miso_io_num = SD_SPI_MISO_PIN,
        .sclk_io_num = SD_SPI_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SD card configuration */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_SPI_CS_PIN;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,  /* 16KB 簇 */
    };

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                   &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted: %s, model: %s",
             MOUNT_POINT, s_card->cid.name);

    /* Speed up SPI after successful init */
    s_card->host.max_freq_khz = 20000;  /* 20MHz for normal operation */
    ESP_LOGI(TAG, "SPI speed: 400kHz -> 20MHz");

    /* 创建目录 */
    ensure_directories();

    /* 分配主备缓冲 */
    ret = init_buffer(&s_primary_buf, PRIMARY_BUF_SIZE);
    if (ret != ESP_OK) {
        sd_storage_unmount();
        return ret;
    }

    ret = init_buffer(&s_backup_buf, BACKUP_BUF_SIZE);
    if (ret != ESP_OK) {
        sd_storage_unmount();
        return ret;
    }

    /* Allocate CSV buffer (prefer PSRAM) */
    s_csv_buf = heap_caps_malloc(CSV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_csv_buf) s_csv_buf = malloc(CSV_BUF_SIZE);  /* Fallback */
    if (!s_csv_buf) {
        ESP_LOGE(TAG, "CSV buffer alloc failed");
        sd_storage_unmount();
        return ESP_ERR_NO_MEM;
    }

    /* Allocate DHT11 buffer (prefer PSRAM) */
    s_dht11_buf = heap_caps_malloc(CSV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dht11_buf) s_dht11_buf = malloc(CSV_BUF_SIZE);  /* Fallback */
    if (!s_dht11_buf) {
        ESP_LOGE(TAG, "DHT11 buffer alloc failed");
        sd_storage_unmount();
        return ESP_ERR_NO_MEM;
    }

    s_csv_buf_pos = 0;
    s_dht11_buf_pos = 0;
    s_active_buf = &s_primary_buf;  /* Default to primary buffer */

    /* Create CSV and DHT11 mutexes */
    s_csv_mutex = xSemaphoreCreateMutex();
    s_dht11_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Buffers: primary=%uKB, backup=%uKB, csv=%uKB, dht11=%uKB",
             PRIMARY_BUF_SIZE/1024, BACKUP_BUF_SIZE/1024, CSV_BUF_SIZE/1024, CSV_BUF_SIZE/1024);

    return ESP_OK;
}

esp_err_t sd_storage_unmount(void)
{
    if (!s_mounted) return ESP_OK;

    /* Set mounted=false FIRST to prevent other tasks from entering write functions
     * while we are tearing down resources (avoids use-after-free on mutexes) */
    s_mounted = false;

    /* Flush remaining data */
    sd_storage_flush();

    /* Close files */
    if (s_raw_fd >= 0) { close(s_raw_fd); s_raw_fd = -1; }
    if (s_csv_fd >= 0) { close(s_csv_fd); s_csv_fd = -1; }
    if (s_dht11_fd >= 0) { close(s_dht11_fd); s_dht11_fd = -1; }

    /* Free buffers */
    if (s_primary_buf.data) { free(s_primary_buf.data); s_primary_buf.data = NULL; }
    if (s_backup_buf.data) { free(s_backup_buf.data); s_backup_buf.data = NULL; }
    if (s_csv_buf) { free(s_csv_buf); s_csv_buf = NULL; }
    if (s_dht11_buf) { free(s_dht11_buf); s_dht11_buf = NULL; }

    /* Delete mutexes */
    if (s_csv_mutex) { vSemaphoreDelete(s_csv_mutex); s_csv_mutex = NULL; }
    if (s_dht11_mutex) { vSemaphoreDelete(s_dht11_mutex); s_dht11_mutex = NULL; }

    /* Unmount */
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

bool sd_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_storage_write_raw(const max30102_sample_t *sample)
{
    if (!s_mounted || !s_active_buf) return ESP_ERR_INVALID_STATE;

    /* Check if need to create new file */
    char new_path[64];
    generate_raw_filename(new_path, sizeof(new_path));
    if (strcmp(new_path, s_current_raw_path) != 0 || s_raw_fd < 0) {
        xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
        /* Flush old buffer */
        if (s_active_buf->pos > 0 && s_raw_fd >= 0) {
            flush_buffer(s_active_buf, s_raw_fd);
        }
        if (s_raw_fd >= 0) close(s_raw_fd);
        strncpy(s_current_raw_path, new_path, sizeof(s_current_raw_path) - 1);
        s_current_raw_path[sizeof(s_current_raw_path) - 1] = '\0';
        s_raw_fd = open(new_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        xSemaphoreGive(s_buf_mutex);
        if (s_raw_fd < 0) {
            ESP_LOGE(TAG, "Open file failed: %s", new_path);
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Prepare binary record */
    ppg_raw_record_t record;
    record.timestamp = (uint32_t)time(NULL);
    record.red = sample->red;
    record.ir = sample->ir;
    record.checksum = calc_checksum(&record, sizeof(record));

    /* Write to active buffer (protected by s_buf_mutex) */
    xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
    size_t written = buffer_write(s_active_buf, &record, sizeof(record));

    if (written < sizeof(record)) {
        /* Primary buffer full, switch to backup */
        ESP_LOGI(TAG, "Primary buffer full, switching to backup");
        buffer_t *new_buf = switch_buffer();
        if (new_buf) {
            /* Write remaining data to new buffer */
            size_t remaining = sizeof(record) - written;
            buffer_write(new_buf, (uint8_t *)&record + written, remaining);

            /* Flush old buffer */
            buffer_t *old_buf = (new_buf == &s_primary_buf) ? &s_backup_buf : &s_primary_buf;
            flush_buffer(old_buf, s_raw_fd);
        }
    }
    xSemaphoreGive(s_buf_mutex);

    return ESP_OK;
}

esp_err_t sd_storage_write_csv(const ppg_result_t *result)
{
    if (!s_mounted || !s_csv_buf) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_csv_mutex, portMAX_DELAY);

    /* Check if need to create new file */
    char new_path[64];
    generate_csv_filename(new_path, sizeof(new_path));
    if (strcmp(new_path, s_current_csv_path) != 0 || s_csv_fd < 0) {
        if (s_csv_buf_pos > 0 && s_csv_fd >= 0) {
            safe_write(s_csv_fd, s_csv_buf, s_csv_buf_pos);
            s_csv_buf_pos = 0;
        }
        if (s_csv_fd >= 0) close(s_csv_fd);
        strncpy(s_current_csv_path, new_path, sizeof(s_current_csv_path) - 1);
        s_current_csv_path[sizeof(s_current_csv_path) - 1] = '\0';
        s_csv_fd = open(new_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (s_csv_fd < 0) {
            ESP_LOGE(TAG, "Open CSV file failed: %s", new_path);
            xSemaphoreGive(s_csv_mutex);
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Prepare binary record */
    ppg_result_record_t record;
    record.timestamp = (uint32_t)time(NULL);
    record.heart_rate = result->heart_rate;
    record.spo2 = result->spo2;
    record.hr_valid = result->hr_valid;
    record.spo2_valid = result->spo2_valid;
    record.checksum = calc_checksum(&record, sizeof(record));

    /* Write to buffer */
    if (s_csv_buf_pos + sizeof(record) > CSV_BUF_SIZE) {
        safe_write(s_csv_fd, s_csv_buf, s_csv_buf_pos);
        s_csv_buf_pos = 0;
    }

    memcpy(s_csv_buf + s_csv_buf_pos, &record, sizeof(record));
    s_csv_buf_pos += sizeof(record);

    xSemaphoreGive(s_csv_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_write_dht11(int temperature, int humidity)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_dht11_mutex, portMAX_DELAY);

    /* Check if need to create new file */
    char new_path[64];
    generate_dht11_filename(new_path, sizeof(new_path));
    if (strcmp(new_path, s_current_dht11_path) != 0 || s_dht11_fd < 0) {
        if (s_dht11_buf_pos > 0 && s_dht11_fd >= 0) {
            safe_write(s_dht11_fd, s_dht11_buf, s_dht11_buf_pos);
            s_dht11_buf_pos = 0;
        }
        if (s_dht11_fd >= 0) close(s_dht11_fd);
        strncpy(s_current_dht11_path, new_path, sizeof(s_current_dht11_path) - 1);
        s_current_dht11_path[sizeof(s_current_dht11_path) - 1] = '\0';
        s_dht11_fd = open(new_path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (s_dht11_fd < 0) {
            ESP_LOGE(TAG, "Open DHT11 file failed: %s", new_path);
            xSemaphoreGive(s_dht11_mutex);
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Prepare binary record */
    dht11_record_t record;
    record.timestamp = (uint32_t)time(NULL);
    record.temperature = (int16_t)(temperature * 10);  /* x10 */
    record.humidity = (int16_t)(humidity * 10);        /* x10 */
    record.checksum = calc_checksum(&record, sizeof(record));

    /* Write to buffer */
    if (s_dht11_buf_pos + sizeof(record) > CSV_BUF_SIZE) {
        safe_write(s_dht11_fd, s_dht11_buf, s_dht11_buf_pos);
        s_dht11_buf_pos = 0;
    }

    memcpy(s_dht11_buf + s_dht11_buf_pos, &record, sizeof(record));
    s_dht11_buf_pos += sizeof(record);

    xSemaphoreGive(s_dht11_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_flush(void)
{
    if (!s_mounted) return ESP_OK;

    /* Flush raw buffers (protected by s_buf_mutex) */
    if (s_buf_mutex && s_primary_buf.data && s_raw_fd >= 0) {
        xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
        if (s_primary_buf.pos > 0) {
            flush_buffer(&s_primary_buf, s_raw_fd);
        }
        if (s_backup_buf.pos > 0 && s_backup_buf.data) {
            flush_buffer(&s_backup_buf, s_raw_fd);
        }
        xSemaphoreGive(s_buf_mutex);
    }

    /* Flush CSV buffer */
    if (s_csv_mutex && s_csv_buf && s_csv_fd >= 0) {
        xSemaphoreTake(s_csv_mutex, portMAX_DELAY);
        if (s_csv_buf_pos > 0) {
            safe_write(s_csv_fd, s_csv_buf, s_csv_buf_pos);
            fsync(s_csv_fd);
            s_csv_buf_pos = 0;
        }
        xSemaphoreGive(s_csv_mutex);
    }

    /* Flush DHT11 buffer */
    if (s_dht11_mutex && s_dht11_buf && s_dht11_fd >= 0) {
        xSemaphoreTake(s_dht11_mutex, portMAX_DELAY);
        if (s_dht11_buf_pos > 0) {
            safe_write(s_dht11_fd, s_dht11_buf, s_dht11_buf_pos);
            fsync(s_dht11_fd);
            s_dht11_buf_pos = 0;
        }
        xSemaphoreGive(s_dht11_mutex);
    }

    return ESP_OK;
}

esp_err_t sd_storage_get_file_list(char *buf, size_t len)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    int pos = snprintf(buf, len, "{\"files\":[");

    /* 扫描 /raw/ /csv/ /log/ 目录 */
    const char *dirs[] = {"raw", "csv", "log"};
    for (int d = 0; d < 3; d++) {
        char path[32];
        snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, dirs[d]);
        DIR *dir = opendir(path);
        if (!dir) continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && pos < (int)len - 64) {
            if (entry->d_name[0] == '.') continue;
            if (pos > 10) buf[pos++] = ',';
            pos += snprintf(buf + pos, len - pos, "\"%s/%s\"", dirs[d], entry->d_name);
        }
        closedir(dir);
    }

    if (pos < (int)len - 3) {
        pos += snprintf(buf + pos, len - pos, "]}");
    }

    return ESP_OK;
}

uint32_t sd_storage_get_free_space_mb(void)
{
    if (!s_mounted || !s_card) return 0;
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree(MOUNT_POINT, &fre_clust, &fs) != FR_OK) return 0;
    return (uint32_t)(fre_clust * fs->csize * 512 / (1024 * 1024));
}

uint32_t sd_storage_get_total_space_mb(void)
{
    if (!s_mounted || !s_card) return 0;
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree(MOUNT_POINT, &fre_clust, &fs) != FR_OK) return 0;
    return (uint32_t)((fs->n_fatent - 2) * fs->csize * 512 / (1024 * 1024));
}

void sd_storage_release_buffers(void)
{
    /* Set mounted=false FIRST to prevent other tasks from entering write functions */
    s_mounted = false;

    /* Flush remaining data if buffers are still valid */
    if (s_primary_buf.data || s_csv_buf || s_dht11_buf) {
        sd_storage_flush();
    }

    if (s_buf_mutex) {
        xSemaphoreTake(s_buf_mutex, portMAX_DELAY);
        if (s_primary_buf.data) { free(s_primary_buf.data); s_primary_buf.data = NULL; }
        if (s_backup_buf.data) { free(s_backup_buf.data); s_backup_buf.data = NULL; }
        s_active_buf = NULL;
        xSemaphoreGive(s_buf_mutex);
    }

    if (s_csv_mutex) {
        xSemaphoreTake(s_csv_mutex, portMAX_DELAY);
        if (s_csv_buf) { free(s_csv_buf); s_csv_buf = NULL; }
        s_csv_buf_pos = 0;
        if (s_csv_fd >= 0) { close(s_csv_fd); s_csv_fd = -1; }
        xSemaphoreGive(s_csv_mutex);
    }

    if (s_dht11_mutex) {
        xSemaphoreTake(s_dht11_mutex, portMAX_DELAY);
        if (s_dht11_buf) { free(s_dht11_buf); s_dht11_buf = NULL; }
        s_dht11_buf_pos = 0;
        if (s_dht11_fd >= 0) { close(s_dht11_fd); s_dht11_fd = -1; }
        xSemaphoreGive(s_dht11_mutex);
    }

    if (s_raw_fd >= 0) { close(s_raw_fd); s_raw_fd = -1; }

    puts("[SD] Write buffers released");
}
