/** @file audio_adts.h
 * @brief AAC-LC 的最小 ADTS 文件接收端；只借用 FILE，不负责关闭或覆盖文件。
 */
#ifndef IPC_AUDIO_ADTS_H
#define IPC_AUDIO_ADTS_H
#include <stdio.h>
#include "media_types.h"
typedef struct {
    FILE *file;
    unsigned int frequency_index, channels;
    uint64_t packets, bytes;
} IpcAudioAdts;
/** @brief 根据编码器 AudioSpecificConfig 校验 AAC-LC 与 ADTS 参数；失败不写文件。 */
int ipc_audio_adts_init(IpcAudioAdts *writer, FILE *file, const IpcStreamParams *params);
/** @brief 同步写入七字节 ADTS 头和一个 AAC 包；包仅借用，写错返回负 errno。 */
int ipc_audio_adts_write(void *writer, const IpcEncodedPacket *packet);
#endif
