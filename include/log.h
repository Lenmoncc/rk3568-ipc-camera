/* log.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_LOG_H
#define IPC_LOG_H

typedef enum { IPC_LOG_DEBUG, IPC_LOG_INFO, IPC_LOG_WARN, IPC_LOG_ERROR } IpcLogLevel;

/* 待实现：只在普通线程上下文调用，不可在信号处理函数中调用。 */
void ipc_log_set_level(IpcLogLevel level);
void ipc_log_write(IpcLogLevel level, const char *module, const char *format, ...);

#endif /* IPC_LOG_H */
