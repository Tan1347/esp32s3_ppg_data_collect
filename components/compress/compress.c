/**
 * @file compress.c
 * @brief 数据压缩工具实现（基于 LZ4）
 *
 * LZ4 特点：
 *   - 压缩速度极快（~800MB/s）
 *   - 解压速度极快（~4GB/s）
 *   - 内存占用低（8-16KB）
 *   - 压缩率适中（2-3x）
 */

#include "compress.h"
#include "lz4.h"
#include "esp_log.h"

static const char *TAG = "compress";

esp_err_t compress_data(const void *src, size_t src_len,
                        void *dst, size_t *dst_len, int level)
{
    if (!src || !dst || !dst_len || src_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* LZ4 压缩等级通过 acceleration 参数控制：
     * acceleration=1: 最高压缩率（但比 miniz 仍然快很多）
     * acceleration=1: 默认（推荐）
     * acceleration>1: 更快但压缩率更低
     */
    int accel = 1;
    if (level <= 3) accel = 1;      /* 高压缩率 */
    else if (level <= 6) accel = 1; /* 默认 */
    else accel = 2;                 /* 更快速度 */

    int max_dst_size = (int)*dst_len;
    int result = LZ4_compress_fast((const char *)src, (char *)dst,
                                   (int)src_len, max_dst_size, accel);

    if (result <= 0) {
        ESP_LOGW(TAG, "LZ4 compress failed (in=%u, out_buf=%u)",
                 (unsigned)src_len, (unsigned)*dst_len);
        return ESP_ERR_NO_MEM;
    }

    *dst_len = (size_t)result;
    return ESP_OK;
}

esp_err_t decompress_data(const void *src, size_t src_len,
                          void *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len || src_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int result = LZ4_decompress_safe((const char *)src, (char *)dst,
                                     (int)src_len, (int)*dst_len);

    if (result < 0) {
        ESP_LOGW(TAG, "LZ4 decompress failed (in=%u, out_buf=%u)",
                 (unsigned)src_len, (unsigned)*dst_len);
        return ESP_ERR_NO_MEM;
    }

    *dst_len = (size_t)result;
    return ESP_OK;
}

size_t compress_bound(size_t src_len)
{
    return (size_t)LZ4_compressBound((int)src_len);
}
