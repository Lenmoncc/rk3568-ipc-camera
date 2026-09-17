/* app.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_APP_H
#define IPC_APP_H

/* 应用上下文暂保持不透明，避免过早固定内部同步结构。
 * 后续由 main.c 定义：配置、2 个原始队列、4 个编码包队列、
 * 6 个业务线程句柄、模块上下文、流参数就绪状态和退出状态。
 * 线程入口统一接收 IpcApp *，不得自行持有无保护的跨线程全局变量。 */
typedef struct IpcApp IpcApp;

#endif /* IPC_APP_H */
