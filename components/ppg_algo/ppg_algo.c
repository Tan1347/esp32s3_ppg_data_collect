/**
 * @file ppg_algo.c
 * @brief PPG 心率/血氧算法实现
 *
 * 参考 Maxim 官方算法 (maxim_heart_rate_and_oxygen_saturation)
 * 算法与硬件完全解耦
 *
 * 处理流程：
 * 1. 去除 DC 分量 (均值)
 * 2. 4 点移动平均平滑
 * 3. 差分运算
 * 4. Hamming 窗滤波
 * 5. 峰值检测 → 心率
 * 6. 精确定位波谷 → AC/DC 比值 → SpO2 查表
 */

#include "ppg_algo.h"
#include <string.h>

/* ==================== Hamming 窗系数 ==================== */
static const int16_t auw_hamm[PPG_ALGO_HAMMING_SIZE] = {41, 276, 512, 276, 41};

/* ==================== SpO2 查找表 ==================== */
/* 公式: -45.060*ratio^2 + 30.354*ratio + 94.845 */
static const uint8_t uch_spo2_table[184] = {
    95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98, 99, 99, 99, 99,
    99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 99, 99, 99, 99, 99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97,
    97, 97, 96, 96, 96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92, 91, 91,
    90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84, 84, 83, 82, 82, 81, 81,
    80, 80, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73, 72, 72, 71, 70, 69, 69, 68, 67,
    66, 66, 65, 64, 63, 62, 62, 61, 60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50,
    49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31, 30, 29,
    28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12, 11, 10, 9, 7, 6, 5,
    3, 2, 1
};

/* ==================== 辅助函数 ==================== */

static void sort_ascend(int32_t *pn_x, int32_t n_size)
{
    int32_t i, j, n_temp;
    for (i = 1; i < n_size; i++) {
        n_temp = pn_x[i];
        for (j = i; j > 0 && n_temp < pn_x[j-1]; j--)
            pn_x[j] = pn_x[j-1];
        pn_x[j] = n_temp;
    }
}

static void sort_indices_descend(int32_t *pn_x, int32_t *pn_indx, int32_t n_size)
{
    int32_t i, j, n_temp;
    for (i = 1; i < n_size; i++) {
        n_temp = pn_indx[i];
        for (j = i; j > 0 && pn_x[n_temp] > pn_x[pn_indx[j-1]]; j--)
            pn_indx[j] = pn_indx[j-1];
        pn_indx[j] = n_temp;
    }
}

static int32_t min_val(int32_t x, int32_t y)
{
    return (x < y) ? x : y;
}

/* ==================== 峰值检测 ==================== */

static void peaks_above_min_height(int32_t *pn_locs, int32_t *pn_npks,
                                    int32_t *pn_x, int32_t n_size, int32_t n_min_height)
{
    int32_t i = 1, n_width;
    *pn_npks = 0;

    while (i < n_size - 1) {
        if (pn_x[i] > n_min_height && pn_x[i] > pn_x[i-1]) {
            n_width = 1;
            while (i + n_width < n_size && pn_x[i] == pn_x[i + n_width])
                n_width++;
            if (pn_x[i] > pn_x[i + n_width] && (*pn_npks) < PPG_ALGO_MAX_PEAKS) {
                pn_locs[(*pn_npks)++] = i;
                i += n_width + 1;
            } else {
                i += n_width;
            }
        } else {
            i++;
        }
    }
}

static void remove_close_peaks(int32_t *pn_locs, int32_t *pn_npks,
                                int32_t *pn_x, int32_t n_min_distance)
{
    int32_t i, j, n_old_npks, n_dist;

    sort_indices_descend(pn_x, pn_locs, *pn_npks);

    for (i = -1; i < *pn_npks; i++) {
        n_old_npks = *pn_npks;
        *pn_npks = i + 1;
        for (j = i + 1; j < n_old_npks; j++) {
            n_dist = pn_locs[j] - (i == -1 ? -1 : pn_locs[i]);
            if (n_dist > n_min_distance || n_dist < -n_min_distance)
                pn_locs[(*pn_npks)++] = pn_locs[j];
        }
    }

    sort_ascend(pn_locs, *pn_npks);
}

void ppg_algo_find_peaks(int32_t *pn_locs, int32_t *pn_npks, int32_t *pn_x,
                          int32_t n_size, int32_t n_min_height,
                          int32_t n_min_distance, int32_t n_max_num)
{
    peaks_above_min_height(pn_locs, pn_npks, pn_x, n_size, n_min_height);
    remove_close_peaks(pn_locs, pn_npks, pn_x, n_min_distance);
    *pn_npks = min_val(*pn_npks, n_max_num);
}

/* ==================== 心率计算 ==================== */

void ppg_algo_calc_hr(uint32_t *ir_buffer, int32_t buffer_length,
                       int32_t *heart_rate, int8_t *hr_valid)
{
    static int32_t an_x[PPG_ALGO_BUFFER_SIZE];
    static int32_t an_dx[PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE];

    uint32_t un_ir_mean;
    int32_t k, n_th1, n_npks, n_peak_interval_sum;
    int32_t an_dx_peak_locs[PPG_ALGO_MAX_PEAKS];

    /* 去除 DC */
    un_ir_mean = 0;
    for (k = 0; k < buffer_length; k++)
        un_ir_mean += ir_buffer[k];
    un_ir_mean = un_ir_mean / buffer_length;
    for (k = 0; k < buffer_length; k++)
        an_x[k] = (int32_t)ir_buffer[k] - (int32_t)un_ir_mean;

    /* 4 点移动平均 */
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE; k++) {
        an_x[k] = (an_x[k] + an_x[k+1] + an_x[k+2] + an_x[k+3]) / 4;
    }

    /* 差分 */
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE - 1; k++)
        an_dx[k] = an_x[k+1] - an_x[k];

    /* 2 点移动平均 */
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE - 2; k++)
        an_dx[k] = (an_dx[k] + an_dx[k+1]) / 2;

    /* Hamming 窗 (翻转信号用于波谷检测) */
    for (int32_t i = 0; i < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE - PPG_ALGO_MA4_SIZE - 2; i++) {
        int32_t s = 0;
        for (k = i; k < i + PPG_ALGO_HAMMING_SIZE; k++)
            s -= an_dx[k] * auw_hamm[k - i];
        an_dx[i] = s / 1146;
    }

    /* 计算阈值 */
    n_th1 = 0;
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE; k++)
        n_th1 += (an_dx[k] > 0) ? an_dx[k] : (0 - an_dx[k]);
    n_th1 = n_th1 / (PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE);

    /* 峰值检测 */
    ppg_algo_find_peaks(an_dx_peak_locs, &n_npks, an_dx,
                         PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE,
                         n_th1, 8, 5);

    /* 计算心率 */
    n_peak_interval_sum = 0;
    if (n_npks >= 2) {
        for (k = 1; k < n_npks; k++)
            n_peak_interval_sum += (an_dx_peak_locs[k] - an_dx_peak_locs[k-1]);
        n_peak_interval_sum = n_peak_interval_sum / (n_npks - 1);
        *heart_rate = 6000 / n_peak_interval_sum;  /* bpm */
        *hr_valid = 1;
    } else {
        *heart_rate = -999;
        *hr_valid = 0;
    }
}

/* ==================== SpO2 计算 ==================== */

void ppg_algo_calc_spo2(uint32_t *ir_buffer, uint32_t *red_buffer,
                         int32_t buffer_length, int32_t *spo2, int8_t *spo2_valid)
{
    static int32_t an_x[PPG_ALGO_BUFFER_SIZE];
    static int32_t an_y[PPG_ALGO_BUFFER_SIZE];
    static int32_t an_dx[PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE];

    uint32_t un_ir_mean;
    int32_t k, n_th1, n_npks;
    int32_t an_dx_peak_locs[PPG_ALGO_MAX_PEAKS];
    int32_t an_ir_valley_locs[PPG_ALGO_MAX_PEAKS];
    int32_t an_exact_ir_valley_locs[PPG_ALGO_MAX_PEAKS];
    int32_t n_exact_ir_valley_locs_count;

    /* Step 1: 去除 DC + 4 点移动平均 + 差分 + Hamming 滤波 */
    un_ir_mean = 0;
    for (k = 0; k < buffer_length; k++)
        un_ir_mean += ir_buffer[k];
    un_ir_mean = un_ir_mean / buffer_length;
    for (k = 0; k < buffer_length; k++)
        an_x[k] = (int32_t)ir_buffer[k] - (int32_t)un_ir_mean;

    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE; k++)
        an_x[k] = (an_x[k] + an_x[k+1] + an_x[k+2] + an_x[k+3]) / 4;

    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE - 1; k++)
        an_dx[k] = an_x[k+1] - an_x[k];

    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE - 2; k++)
        an_dx[k] = (an_dx[k] + an_dx[k+1]) / 2;

    for (int32_t i = 0; i < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE - PPG_ALGO_MA4_SIZE - 2; i++) {
        int32_t s = 0;
        for (k = i; k < i + PPG_ALGO_HAMMING_SIZE; k++)
            s -= an_dx[k] * auw_hamm[k - i];
        an_dx[i] = s / 1146;
    }

    /* Step 2: 峰值检测 */
    n_th1 = 0;
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE; k++)
        n_th1 += (an_dx[k] > 0) ? an_dx[k] : (0 - an_dx[k]);
    n_th1 = n_th1 / (PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE);

    ppg_algo_find_peaks(an_dx_peak_locs, &n_npks, an_dx,
                         PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE,
                         n_th1, 8, 5);

    /* Step 3: 定位波谷 */
    for (k = 0; k < n_npks; k++)
        an_ir_valley_locs[k] = an_dx_peak_locs[k] + PPG_ALGO_HAMMING_SIZE / 2;

    /* Step 4: 使用原始数据计算 AC/DC */
    for (k = 0; k < buffer_length; k++) {
        an_x[k] = (int32_t)ir_buffer[k];
        an_y[k] = (int32_t)red_buffer[k];
    }

    /* Step 5: 精确定位波谷 */
    n_exact_ir_valley_locs_count = 0;
    for (k = 0; k < n_npks; k++) {
        int32_t m = an_ir_valley_locs[k];
        int32_t n_c_min = 16777216;  /* 2^24 */
        int found = 0;
        if (m + 5 < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_HAMMING_SIZE && m - 5 > 0) {
            for (int32_t i = m - 5; i < m + 5; i++) {
                if (an_x[i] < n_c_min) {
                    n_c_min = an_x[i];
                    an_exact_ir_valley_locs[k] = i;
                    found = 1;
                }
            }
            if (found)
                n_exact_ir_valley_locs_count++;
        }
    }

    if (n_exact_ir_valley_locs_count < 2) {
        *spo2 = -999;
        *spo2_valid = 0;
        return;
    }

    /* Step 6: 4 点移动平均平滑 */
    for (k = 0; k < PPG_ALGO_BUFFER_SIZE - PPG_ALGO_MA4_SIZE; k++) {
        an_x[k] = (an_x[k] + an_x[k+1] + an_x[k+2] + an_x[k+3]) / 4;
        an_y[k] = (an_y[k] + an_y[k+1] + an_y[k+2] + an_y[k+3]) / 4;
    }

    /* Step 7: 计算 AC/DC 比值 */
    int32_t an_ratio[5], n_i_ratio_count = 0;
    for (k = 0; k < 5; k++) an_ratio[k] = 0;

    for (k = 0; k < n_exact_ir_valley_locs_count - 1; k++) {
        int32_t n_y_dc_max = -16777216, n_x_dc_max = -16777216;
        int32_t n_y_dc_max_idx = 0, n_x_dc_max_idx = 0;

        if (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k] > 10) {
            for (int32_t i = an_exact_ir_valley_locs[k]; i < an_exact_ir_valley_locs[k+1]; i++) {
                if (an_x[i] > n_x_dc_max) { n_x_dc_max = an_x[i]; n_x_dc_max_idx = i; }
                if (an_y[i] > n_y_dc_max) { n_y_dc_max = an_y[i]; n_y_dc_max_idx = i; }
            }

            /* 线性插值去除 DC */
            int32_t n_y_ac = (an_y[an_exact_ir_valley_locs[k+1]] - an_y[an_exact_ir_valley_locs[k]]) *
                             (n_y_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_y_ac = an_y[an_exact_ir_valley_locs[k]] + n_y_ac /
                     (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k]);
            n_y_ac = an_y[n_y_dc_max_idx] - n_y_ac;

            int32_t n_x_ac = (an_x[an_exact_ir_valley_locs[k+1]] - an_x[an_exact_ir_valley_locs[k]]) *
                             (n_x_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_x_ac = an_x[an_exact_ir_valley_locs[k]] + n_x_ac /
                     (an_exact_ir_valley_locs[k+1] - an_exact_ir_valley_locs[k]);
            n_x_ac = an_x[n_y_dc_max_idx] - n_x_ac;

            /* 计算比值 */
            int32_t n_nume = (n_y_ac * n_x_dc_max) >> 7;
            int32_t n_denom = (n_x_ac * n_y_dc_max) >> 7;
            if (n_denom > 0 && n_i_ratio_count < 5 && n_nume != 0) {
                an_ratio[n_i_ratio_count] = (n_nume * 20) / n_denom;
                n_i_ratio_count++;
            }
        }
    }

    /* Step 8: 取中位数 */
    sort_ascend(an_ratio, n_i_ratio_count);
    int32_t n_middle_idx = n_i_ratio_count / 2;
    int32_t n_ratio_average;
    if (n_middle_idx > 1)
        n_ratio_average = (an_ratio[n_middle_idx-1] + an_ratio[n_middle_idx]) / 2;
    else
        n_ratio_average = an_ratio[n_middle_idx];

    /* Step 9: 查表获取 SpO2 */
    if (n_ratio_average > 2 && n_ratio_average < 184) {
        *spo2 = uch_spo2_table[n_ratio_average];
        *spo2_valid = 1;
    } else {
        *spo2 = -999;
        *spo2_valid = 0;
    }
}

/* ==================== 算法上下文 API ==================== */

void ppg_algo_init(ppg_algo_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(ppg_algo_ctx_t));
    ctx->result.heart_rate = -999;
    ctx->result.spo2 = -999;
    ctx->result.hr_valid = 0;
    ctx->result.spo2_valid = 0;
}

void ppg_algo_add_sample(ppg_algo_ctx_t *ctx, uint32_t red, uint32_t ir)
{
    ctx->red_buffer[ctx->buffer_index] = red;
    ctx->ir_buffer[ctx->buffer_index] = ir;
    ctx->buffer_index++;

    if (ctx->buffer_index >= PPG_ALGO_BUFFER_SIZE) {
        ctx->buffer_index = 0;
        ctx->buffer_full = 1;
    }
}

bool ppg_algo_process(ppg_algo_ctx_t *ctx, ppg_algo_result_t *result)
{
    if (!ctx->buffer_full) return false;

    /* 计算心率 */
    ppg_algo_calc_hr(ctx->ir_buffer, PPG_ALGO_BUFFER_SIZE,
                      &ctx->result.heart_rate, &ctx->result.hr_valid);

    /* 计算 SpO2 */
    ppg_algo_calc_spo2(ctx->ir_buffer, ctx->red_buffer, PPG_ALGO_BUFFER_SIZE,
                        &ctx->result.spo2, &ctx->result.spo2_valid);

#if PPG_DEBUG_ENABLE
    /* Signal quality */
    ctx->result.quality = ppg_algo_get_quality(ctx);

    /* IR DC mean and amplitude */
    uint32_t ir_max = 0, ir_min = 0xFFFFFFFF;
    uint32_t ir_sum = 0, red_sum = 0;
    for (int i = 0; i < PPG_ALGO_BUFFER_SIZE; i++) {
        if (ctx->ir_buffer[i] > ir_max) ir_max = ctx->ir_buffer[i];
        if (ctx->ir_buffer[i] < ir_min) ir_min = ctx->ir_buffer[i];
        ir_sum += ctx->ir_buffer[i];
        red_sum += ctx->red_buffer[i];
    }
    ctx->result.ir_dc_mean = ir_sum / PPG_ALGO_BUFFER_SIZE;
    ctx->result.ir_amplitude = ir_max - ir_min;
    ctx->result.red_dc_mean = red_sum / PPG_ALGO_BUFFER_SIZE;

    /* Peak count: zero crossings of AC-coupled IR (each pair = one peak) */
    int32_t crossings = 0;
    int32_t prev = 0;
    for (int k = 0; k < PPG_ALGO_BUFFER_SIZE; k++) {
        int32_t ac = (int32_t)ctx->ir_buffer[k] - (int32_t)ctx->result.ir_dc_mean;
        if (k > 0 && ((prev >= 0 && ac < 0) || (prev < 0 && ac >= 0))) {
            crossings++;
        }
        prev = ac;
    }
    ctx->result.peak_count = (crossings > 0) ? (crossings / 2) : 0;

    /* SpO2 ratio (R value) — stored as ratio * 100 for readability */
    if (ctx->result.spo2_valid && ctx->result.ir_dc_mean > 0 && ctx->result.red_dc_mean > 0) {
        /* Simplified R: using AC/DC from peak-to-peak amplitude */
        uint32_t red_ac = 0;
        uint32_t ir_ac = ctx->result.ir_amplitude;
        /* Estimate red AC from red buffer amplitude */
        uint32_t red_max = 0, red_min = 0xFFFFFFFF;
        for (int i = 0; i < PPG_ALGO_BUFFER_SIZE; i++) {
            if (ctx->red_buffer[i] > red_max) red_max = ctx->red_buffer[i];
            if (ctx->red_buffer[i] < red_min) red_min = ctx->red_buffer[i];
        }
        red_ac = red_max - red_min;
        if (ir_ac > 0 && red_ac > 0) {
            ctx->result.spo2_ratio = (int32_t)((red_ac * ctx->result.ir_dc_mean * 100) /
                                                (ir_ac * ctx->result.red_dc_mean));
        } else {
            ctx->result.spo2_ratio = -1;
        }
    } else {
        ctx->result.spo2_ratio = -1;
    }
#endif

    *result = ctx->result;
    return true;
}

void ppg_algo_reset(ppg_algo_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(ppg_algo_ctx_t));
    ctx->result.heart_rate = -999;
    ctx->result.spo2 = -999;
    ctx->result.hr_valid = 0;
    ctx->result.spo2_valid = 0;
}

uint8_t ppg_algo_get_quality(ppg_algo_ctx_t *ctx)
{
    if (!ctx->buffer_full) return 0;

    /* 简单质量评估: 信号幅度 */
    uint32_t max_val = 0, min_val = 0xFFFFFFFF;
    for (int i = 0; i < PPG_ALGO_BUFFER_SIZE; i++) {
        if (ctx->ir_buffer[i] > max_val) max_val = ctx->ir_buffer[i];
        if (ctx->ir_buffer[i] < min_val) min_val = ctx->ir_buffer[i];
    }

    uint32_t amplitude = max_val - min_val;
    if (amplitude > 50000) return 100;
    if (amplitude > 20000) return 80;
    if (amplitude > 10000) return 60;
    if (amplitude > 5000) return 40;
    if (amplitude > 1000) return 20;
    return 0;
}
