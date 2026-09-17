/**
 * @file log.c
 * @brief 使用 pthread 互斥锁保护日志等级和单条日志，统一写入 stderr。
 *
 * 日历时间只用于阅读日志，不作为后续媒体 PTS 的时钟来源。
 * 锁覆盖等级判断、格式化和输出，避免多线程把一条记录拆成交错片段。
 */
#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static IpcLogLevel g_log_level = IPC_LOG_INFO;
static const char *const g_level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void ipc_log_set_level(IpcLogLevel level)
{
    if (level < IPC_LOG_DEBUG || level > IPC_LOG_ERROR)
        level = IPC_LOG_INFO;
    pthread_mutex_lock(&g_log_mutex);
    g_log_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

void ipc_log_write(IpcLogLevel level, const char *module, const char *format, ...)
{
    struct timespec now = {0, 0};
    struct tm local_time;
    char date[32] = "time-unavailable";
    va_list args;

    if (!format || level < IPC_LOG_DEBUG || level > IPC_LOG_ERROR)
        return;
    pthread_mutex_lock(&g_log_mutex);
    if (level < g_log_level) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 &&
        localtime_r(&now.tv_sec, &local_time) != NULL) {
        (void)strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local_time);
    }
    fprintf(stderr, "%s.%03ld [%s] [%s] ", date, now.tv_nsec / 1000000L,
            g_level_names[level], module ? module : "app");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mutex);
}
