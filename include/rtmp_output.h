/* rtmp_output.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_RTMP_OUTPUT_H
#define IPC_RTMP_OUTPUT_H

#include "config.h"
#include "media_types.h"
#include "app.h"

typedef struct IpcRtmpOutput IpcRtmpOutput;

/* 以下均为待实现接口。初始化失败应清理本次已分配的资源，*context 保持 NULL。
 * deinit 在对应线程结束后调用；模块内部负责自己的资源。
 * thread 的 arg 为 IpcApp *，完整上下文在实现 main 时统一连接。 */
int ipc_rtmp_output_init(IpcRtmpOutput **context, const IpcConfig *config,
    const IpcStreamParams *video, const IpcStreamParams *audio);
void *ipc_rtmp_output_thread(void *arg);
void ipc_rtmp_output_deinit(IpcRtmpOutput **context);

#endif /* IPC_RTMP_OUTPUT_H */
