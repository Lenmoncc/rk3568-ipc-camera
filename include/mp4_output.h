/* mp4_output.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_MP4_OUTPUT_H
#define IPC_MP4_OUTPUT_H

#include "config.h"
#include "media_types.h"
#include "app.h"

typedef struct IpcMp4Output IpcMp4Output;

/* 以下均为待实现接口。初始化失败应清理本次已分配的资源，*context 保持 NULL。
 * deinit 在对应线程结束后调用；模块内部负责自己的资源。
 * thread 的 arg 为 IpcApp *，完整上下文在实现 main 时统一连接。 */
int ipc_mp4_output_init(IpcMp4Output **context, const IpcConfig *config,
    const IpcStreamParams *video, const IpcStreamParams *audio);
void *ipc_mp4_output_thread(void *arg);
void ipc_mp4_output_deinit(IpcMp4Output **context);

#endif /* IPC_MP4_OUTPUT_H */
