/**
 * @file compress.h
 * @brief 数据压缩工具（基于 miniz deflate）
 *
 * 提供简单的内存到内存压缩/解压接口
 * 压缩率约 2-3 倍（PPG 数据/日志/CSV）
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 压缩数据
 *
 * @param src       输入数据
 * @param src_len   输入数据长度
 * @param dst       输出缓冲区
 * @param dst_len   [in] 输出缓冲区大小 [out] 实际压缩后大小
 * @param level     压缩等级 1-9（1=最快，9=最高压缩率，6=默认平衡）
 * @return ESP_OK 成功，ESP_ERR_NO_SIZE 输出缓冲区太小
 */
esp_err_t compress_data(const void *src, size_t src_len,
                        void *dst, size_t *dst_len, int level);

/**
 * @brief 解压数据
 *
 * @param src       压缩数据
 * @param src_len   压缩数据长度
 * @param dst       输出缓冲区
 * @param dst_len   [in] 输出缓冲区大小 [out] 实际解压后大小
 * @return ESP_OK 成功，ESP_ERR_NO_SIZE 输出缓冲区太小
 */
esp_err_t decompress_data(const void *src, size_t src_len,
                          void *dst, size_t *dst_len);

/**
 * @brief 计算压缩后最大可能大小
 */
size_t compress_bound(size_t src_len);

#ifdef __cplusplus
}
#endif
