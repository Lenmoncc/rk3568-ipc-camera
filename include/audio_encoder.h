/* audio_encoder.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_AUDIO_ENCODER_H
#define IPC_AUDIO_ENCODER_H

#include "config.h"
#include "media_types.h"
#include "app.h"

typedef struct IpcAudioEncoder IpcAudioEncoder;

/* 以下均为待实现接口。初始化失败应清理本次已分配的资源，*context 保持 NULL。
 * deinit 在对应线程结束后调用；模块内部负责自己的资源。
 * thread 的 arg 为 IpcApp *，完整上下文在实现 main 时统一连接。 */
int ipc_audio_encoder_init(IpcAudioEncoder **context, const IpcConfig *config);
/* 流参数尚未就绪时返回错误；成功返回的参数借用自编码器，不得由调用方释放。 */
int ipc_audio_encoder_get_params(const IpcAudioEncoder *context, const IpcStreamParams **params);
void *ipc_audio_encoder_thread(void *arg);
void ipc_audio_encoder_deinit(IpcAudioEncoder **context);

#endif /* IPC_AUDIO_ENCODER_H */
