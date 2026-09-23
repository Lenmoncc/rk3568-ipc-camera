/**
 * @file timestamp.c
 * @brief 提供应用单调时钟与音频累计样本换算；通用时间基换算留待封装阶段。
 */
#define _POSIX_C_SOURCE 200809L
#include "timestamp.h"
#include <time.h>
#include <errno.h>

/** @brief 获取 CLOCK_MONOTONIC 微秒值；时钟调用失败或数值溢出返回 -1。 */
int64_t ipc_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)(INT64_MAX - now.tv_nsec / 1000) / 1000000)
        return -1;
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/** @brief 用商和余数避免 sample_count*1000000 中间溢出，并保留累计取整精度。 */
int ipc_audio_pts_us(int64_t start_offset_us, uint64_t sample_count,
                     unsigned int sample_rate, int64_t *pts_us)
{
    uint64_t seconds, fraction, available;
    if (!pts_us || start_offset_us < 0 || !sample_rate) return -EINVAL;
    seconds = sample_count / sample_rate;
    fraction = (sample_count % sample_rate) * UINT64_C(1000000) / sample_rate;
    available = (uint64_t)(INT64_MAX - start_offset_us);
    if (fraction > available || seconds > (available - fraction) / UINT64_C(1000000))
        return -EOVERFLOW;
    *pts_us = start_offset_us + (int64_t)(seconds * UINT64_C(1000000) + fraction);
    return 0;
}
