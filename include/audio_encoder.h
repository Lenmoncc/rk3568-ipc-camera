/** @file audio_encoder.h
 * @brief 单线程 AAC-LC 编码器；PCM 借用输入，编码包仅在同步回调内有效。
 */
#ifndef IPC_AUDIO_ENCODER_H
#define IPC_AUDIO_ENCODER_H
#include <stdbool.h>
#include "config.h"
#include "media_types.h"
typedef struct IpcAudioEncoder IpcAudioEncoder;
/** @brief 同步消费借用的编码包；需异步保存时必须另行 av_packet_ref，负值中止编码。 */
typedef int (*IpcAudioPacketSink)(void *opaque, const IpcEncodedPacket *packet);
typedef struct {
    uint64_t input_samples, converted_samples, submitted_samples, padding_samples;
    uint64_t packets, payload_bytes;
    unsigned int frame_samples, initial_padding;
    int64_t first_pts_us, last_packet_pts;
    bool drained;
} IpcAudioEncoderStats;
/** @brief 查询构建是否包含 FFmpeg AAC 支持，不打开设备。 */
bool ipc_audio_encoder_available(void);
/** @brief 创建 AAC-LC 编码、重采样和 FIFO；失败保持 *context=NULL，禁止重复初始化。 */
int ipc_audio_encoder_init(IpcAudioEncoder **context, const IpcConfig *config);
/** @brief 借用已就绪的编码参数和时间基；有效期至 deinit，调用方不可释放。 */
int ipc_audio_encoder_get_params(const IpcAudioEncoder *context, const IpcStreamParams **params);
/** @brief 校验并转换连续 S16_LE PCM，提交完整编码帧；不接管输入；参数错误不消费，转换/输出中途失败后仅可释放。 */
int ipc_audio_encoder_push(IpcAudioEncoder *context, const IpcRawFrame *frame, IpcAudioPacketSink sink, void *opaque);
/** @brief 排空转换器、补零末帧、发送 EOF 并取完延迟包；成功后重复调用无副作用。 */
int ipc_audio_encoder_finish(IpcAudioEncoder *context, IpcAudioPacketSink sink, void *opaque);
/** @brief 复制样本和编码包统计；样本计数均按每声道计算。 */
int ipc_audio_encoder_get_stats(const IpcAudioEncoder *context, IpcAudioEncoderStats *stats);
/** @brief 释放全部编码资源并置空；不隐式排空，也不释放调用方的 PCM 和 sink。 */
void ipc_audio_encoder_deinit(IpcAudioEncoder **context);
#endif
