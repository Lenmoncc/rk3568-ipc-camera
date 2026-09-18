/**
 * @file timestamp.c
 * @brief 提供应用单调时钟。音频采样换算和通用时间基换算留待编码阶段实现。
 */
#define _POSIX_C_SOURCE 200809L
#include "timestamp.h"
#include <time.h>

/** @brief 获取 CLOCK_MONOTONIC 微秒值；时钟调用失败或数值溢出返回 -1。 */
int64_t ipc_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)(INT64_MAX - now.tv_nsec / 1000) / 1000000)
        return -1;
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
