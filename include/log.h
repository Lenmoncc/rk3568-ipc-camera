/**
 * @file log.h
 * @brief 线程安全的分级日志接口，当前统一写入 stderr。
 * @note 仅供普通线程调用，不具备异步信号安全性。
 */
#ifndef IPC_LOG_H
#define IPC_LOG_H

typedef enum { IPC_LOG_DEBUG, IPC_LOG_INFO, IPC_LOG_WARN, IPC_LOG_ERROR } IpcLogLevel;

/** @brief 设置最低输出级别。非法级别按 INFO 处理；默认级别为 INFO。 */
void ipc_log_set_level(IpcLogLevel level);

/**
 * @brief 输出一条完整日志，包含本地时间、级别和模块名。
 * @param level 本条日志级别；低于当前阈值的记录不输出。
 * @param module 模块名称；NULL 时使用 "app"。
 * @param format printf 格式字符串，调用者不需要添加末尾换行。
 * @note format 为 NULL 或 level 非法时不输出。全局互斥锁保护级别和整条记录。
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void ipc_log_write(IpcLogLevel level, const char *module, const char *format, ...);

#endif /* IPC_LOG_H */
