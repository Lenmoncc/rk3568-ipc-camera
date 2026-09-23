/** @file audio_adts.c
 * @brief 独立 ADTS 接收端：每个 AAC-LC 包一个 raw_data_block，无 CRC。
 * ADTS 不保存应用 PTS/编码器裁剪元数据；播放时长可能包含编码延迟和补零。
 */
#include "audio_adts.h"
#include <errno.h>
#include <string.h>
#ifndef IPC_WITH_FFMPEG
#define IPC_WITH_FFMPEG 0
#endif
#if IPC_WITH_FFMPEG
#include <libavcodec/avcodec.h>
/** @brief 从 ASC 提取对象类型、采样率索引与声道布局，拒绝不支持的扩展结构。 */
int ipc_audio_adts_init(IpcAudioAdts *w, FILE *file, const IpcStreamParams *params)
{
    static const unsigned int rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if (!w || !file || !params || params->type != IPC_MEDIA_AUDIO || !params->codecpar) return -EINVAL;
    const AVCodecParameters *p = params->codecpar;
    if (p->codec_id != AV_CODEC_ID_AAC || p->profile != FF_PROFILE_AAC_LOW ||
        !p->extradata || p->extradata_size < 2) return -ENOTSUP;
    unsigned int object = p->extradata[0] >> 3;
    unsigned int index = ((p->extradata[0] & 7) << 1) | (p->extradata[1] >> 7);
    unsigned int channels = (p->extradata[1] >> 3) & 15;
    if (object != 2 || index >= sizeof(rates)/sizeof(rates[0]) ||
        rates[index] != (unsigned int)p->sample_rate || channels < 1 || channels > 2 ||
        channels != (unsigned int)p->channels || (p->extradata[1] & 7)) return -ENOTSUP;
    *w = (IpcAudioAdts){.file=file, .frequency_index=index, .channels=channels};
    return 0;
}
/** @brief 校验包长度后写 ADTS 头和 payload，只有全部写成功才累计统计。 */
int ipc_audio_adts_write(void *writer, const IpcEncodedPacket *packet)
{
    IpcAudioAdts *w = writer;
    if (!w || !w->file || !packet || packet->type != IPC_MEDIA_AUDIO || !packet->packet ||
        !packet->packet->data || packet->packet->size <= 0) return -EINVAL;
    if (packet->packet->size > 8191 - 7) return -EMSGSIZE;
    unsigned int length = (unsigned int)packet->packet->size + 7;
    uint8_t header[7] = {0xff, 0xf1,
        (uint8_t)((1 << 6) | (w->frequency_index << 2) | (w->channels >> 2)),
        (uint8_t)((w->channels << 6) | (length >> 11)),
        (uint8_t)(length >> 3), (uint8_t)(((length & 7) << 5) | 0x1f), 0xfc};
    errno = 0;
    if (fwrite(header, 1, sizeof(header), w->file) != sizeof(header) ||
        fwrite(packet->packet->data, 1, packet->packet->size, w->file) != (size_t)packet->packet->size)
        return errno ? -errno : -EIO;
    ++w->packets;
    w->bytes += length;
    return 0;
}
#else
/** @brief 无 FFmpeg 构建不初始化 ADTS 接收端。 */
int ipc_audio_adts_init(IpcAudioAdts *w, FILE *f, const IpcStreamParams *p)
{ (void)w; (void)f; (void)p; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建拒绝写编码包。 */
int ipc_audio_adts_write(void *w, const IpcEncodedPacket *p) { (void)w; (void)p; return -ENOTSUP; }
#endif
