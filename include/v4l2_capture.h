/* v4l2_capture.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_V4L2_CAPTURE_H
#define IPC_V4L2_CAPTURE_H

#include "config.h"
#include "media_types.h"
#include "app.h"

typedef struct IpcVideoCapture IpcVideoCapture;

/* 以下均为待实现接口。初始化失败应清理本次已分配的资源，*context 保持 NULL。
 * deinit 在对应线程结束后调用；模块内部负责自己的资源。
 * thread 的 arg 为 IpcApp *，完整上下文在实现 main 时统一连接。 */
int ipc_video_capture_init(IpcVideoCapture **context, const IpcConfig *config);
void *ipc_video_capture_thread(void *arg);
void ipc_video_capture_deinit(IpcVideoCapture **context);

#endif /* IPC_V4L2_CAPTURE_H */
