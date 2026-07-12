/**
 * @file ppg_algo.h
 * @brief PPG 心率/血氧算法（与硬件解耦）
 *
 * 参考 Maxim 官方算法 (maxim_heart_rate_and_oxygen_saturation)
 * 算法与驱动完全解耦，只接收原始红光/红外光数据
 *
 * 特点：
 * - 纯定点运算，无浮点依赖
 * - 5 秒滑动窗口
 * - 峰值检测 + Hamming 滤波
 * - SpO2 查表法
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ppg_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 算法配置 ==================== */
#define PPG_ALGO_SAMPLE_RATE    100     /* 采样率 Hz */
#define PPG_ALGO_BUFFER_SIZE    (PPG_ALGO_SAMPLE_RATE * 5)  /* 5 秒缓冲 */
#define PPG_ALGO_HR_FIFO_SIZE   7
#define PPG_ALGO_MA4_SIZE       4       /* 4 点移动平均 */
#define PPG_ALGO_HAMMING_SIZE   5       /* Hamming 窗大小 */
#define PPG_ALGO_MAX_PEAKS      15      /* 最大峰值数 */

/* ==================== 算法结果 ==================== */
typedef struct {
    int32_t heart_rate;         /**< 心率 (bpm), -999 表示无效 */
    int32_t spo2;               /**< 血氧 (%), -999 表示无效 */
    int8_t  hr_valid;           /**< 心率有效标志 (1=有效) */
    int8_t  spo2_valid;         /**< 血氧有效标志 (1=有效) */
#if PPG_DEBUG_ENABLE
    uint8_t quality;            /**< 信号质量 (0-100) */
    uint8_t peak_count;         /**< 检测到的峰值数 */
    uint32_t ir_dc_mean;        /**< IR 信号直流均值 */
    uint32_t ir_amplitude;      /**< IR 信号幅度 (max-min) */
    uint32_t red_dc_mean;       /**< RED 信号直流均值 */
    int32_t spo2_ratio;         /**< SpO2 R 值 (AC_red/DC_red)/(AC_ir/DC_ir) * 100 */
#endif
} ppg_algo_result_t;

/* ==================== 算法上下文 ==================== */
typedef struct {
    /* 原始数据缓冲 */
    uint32_t ir_buffer[PPG_ALGO_BUFFER_SIZE];
    uint32_t red_buffer[PPG_ALGO_BUFFER_SIZE];
    int32_t  buffer_index;
    int32_t  buffer_full;       /* 缓冲区满标志 */

    /* 滤波后数据 */
    int32_t  an_x[PPG_ALGO_BUFFER_SIZE];       /* IR (去 DC) */
    int32_t  an_y[PPG_ALGO_BUFFER_SIZE];       /* RED */
    int32_t  an_dx[PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE];  /* 差分 */

    /* 结果 */
    ppg_algo_result_t result;
} ppg_algo_ctx_t;

/* ==================== 算法 API ==================== */

/**
 * @brief 初始化算法上下文
 * @param ctx 算法上下文
 */
void ppg_algo_init(ppg_algo_ctx_t *ctx);

/**
 * @brief 添加一个采样点
 * @param ctx 算法上下文
 * @param red 红光原始值
 * @param ir 红外光原始值
 */
void ppg_algo_add_sample(ppg_algo_ctx_t *ctx, uint32_t red, uint32_t ir);

/**
 * @brief 处理缓冲区数据，计算心率和血氧
 * @param ctx 算法上下文
 * @param result 输出结果
 * @return true 计算完成, false 缓冲区未满
 */
bool ppg_algo_process(ppg_algo_ctx_t *ctx, ppg_algo_result_t *result);

/**
 * @brief 重置算法上下文
 * @param ctx 算法上下文
 */
void ppg_algo_reset(ppg_algo_ctx_t *ctx);

/**
 * @brief 获取信号质量评估 (0-100)
 * @param ctx 算法上下文
 * @return 信号质量分数
 */
uint8_t ppg_algo_get_quality(ppg_algo_ctx_t *ctx);

/* ==================== 底层算法函数（可单独调用） ==================== */

/**
 * @brief 峰值检测
 * @param pn_locs 输出峰值位置
 * @param pn_npks 输出峰值数量
 * @param pn_x 输入信号
 * @param n_size 信号长度
 * @param n_min_height 最小峰值高度
 * @param n_min_distance 最小峰值间距
 * @param n_max_num 最大峰值数
 */
void ppg_algo_find_peaks(int32_t *pn_locs, int32_t *pn_npks, int32_t *pn_x,
                          int32_t n_size, int32_t n_min_height,
                          int32_t n_min_distance, int32_t n_max_num);

/**
 * @brief 计算 SpO2
 * @param ir_buffer IR 数据
 * @param red_buffer RED 数据
 * @param buffer_length 数据长度
 * @param spo2 输出 SpO2
 * @param spo2_valid 输出有效标志
 */
void ppg_algo_calc_spo2(uint32_t *ir_buffer, uint32_t *red_buffer,
                         int32_t buffer_length, int32_t *spo2, int8_t *spo2_valid);

/**
 * @brief 计算心率
 * @param ir_buffer IR 数据
 * @param buffer_length 数据长度
 * @param heart_rate 输出心率
 * @param hr_valid 输出有效标志
 */
void ppg_algo_calc_hr(uint32_t *ir_buffer, int32_t buffer_length,
                       int32_t *heart_rate, int8_t *hr_valid);

#ifdef __cplusplus
}
#endif
