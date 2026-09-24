/** @file h264_bridge.c
 * @brief 视频分片组帧与所有权转换；保留采集 PTS，用下一帧 PTS 确定前帧 duration。
 */
#include "h264_bridge.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if IPC_WITH_FFMPEG
#include <libavcodec/avcodec.h>
struct IpcH264Bridge {
    IpcStreamParams params;
    IpcPacketQueue *queue;
    AVPacket *parts;
    IpcEncodedPacket *pending;
    unsigned int fps;
    bool ready, finished, failed, key;
    int64_t pts;
};
/** @brief 检查 Annex B 头至少包含 SPS 与 PPS；不把普通图像包误当配置数据。 */
static bool has_parameter_sets(const uint8_t *data, size_t size)
{
    bool sps = false, pps = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t n = 0;
        if (!data[i] && !data[i+1] && data[i+2] == 1) n = i+3;
        else if (!data[i] && !data[i+1] && !data[i+2] && data[i+3] == 1) n = i+4;
        if (n) { sps |= (data[n] & 31) == 7; pps |= (data[n] & 31) == 8; }
    }
    return sps && pps;
}
/** @brief 配置视频描述，码流头待 MPP 回调补齐；只支持当前无 B 帧的 Baseline。 */
int ipc_h264_bridge_init(IpcH264Bridge **context, const IpcConfig *config, unsigned int fps, IpcPacketQueue *queue)
{
    if (!context || *context || !config || !queue || !fps || fps > 30) return -EINVAL;
    IpcH264Bridge *b = calloc(1, sizeof(*b));
    if (!b) return -ENOMEM;
    b->params.codecpar = avcodec_parameters_alloc(); b->parts = av_packet_alloc();
    if (!b->params.codecpar || !b->parts) { ipc_h264_bridge_deinit(&b); return -ENOMEM; }
    AVCodecParameters *p = b->params.codecpar;
    p->codec_type = AVMEDIA_TYPE_VIDEO; p->codec_id = AV_CODEC_ID_H264;
    p->width = (int)config->video_width; p->height = (int)config->video_height;
    p->format = AV_PIX_FMT_YUV420P; p->profile = FF_PROFILE_H264_BASELINE;
    p->bit_rate = config->video_bitrate;
    b->params.type = IPC_MEDIA_VIDEO; b->params.time_base = (IpcTimeBase){1,1000000};
    b->queue = queue; b->fps = fps; b->pts = -1;
    *context = b; return 0;
}
/** @brief 转交暂存完整帧；仅成功时清空本方指针，满队列不丢弃旧数据。 */
static int publish_pending(IpcH264Bridge *b)
{
    if (!b->pending) return 0;
    int result = ipc_packet_queue_try_push(b->queue, b->pending);
    if (!result) b->pending = NULL;
    return result;
}
/** @brief 复制借用的 MPP 数据并按 frame_end 组帧；错误后锁定实例，防止重复片段。 */
int ipc_h264_bridge_sink(void *context, const IpcH264Packet *packet)
{
    IpcH264Bridge *b = context;
    int result = 0;
    if (!b || !packet || b->failed || b->finished || (packet->size && !packet->data)) return -EINVAL;
    if (packet->header) {
        if (b->ready || !packet->size || packet->size > 1024*1024 || !has_parameter_sets(packet->data, packet->size)) return -EBADMSG;
        b->params.codecpar->extradata = av_mallocz(packet->size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!b->params.codecpar->extradata) return -ENOMEM;
        memcpy(b->params.codecpar->extradata, packet->data, packet->size);
        b->params.codecpar->extradata_size = (int)packet->size; b->ready = true;
        return 0;
    }
    if (!b->ready) return -EINVAL;
    if (!packet->size) return packet->eos && !b->parts->size ? 0 : -EBADMSG;
    /* 当前 720p 限制单帧压缩大小为 16MiB，避免异常分片无限累计。 */
    if (packet->eos || packet->pts_us < 0 || packet->size > 16*1024*1024 - (size_t)b->parts->size ||
        (b->parts->size && packet->pts_us != b->pts)) { result = -EBADMSG; goto fail; }
    if (!b->parts->size) { b->pts = packet->pts_us; b->key = false; }
    int old_size = b->parts->size;
    if (av_grow_packet(b->parts, (int)packet->size) < 0) { result = -ENOMEM; goto fail; }
    memcpy(b->parts->data + old_size, packet->data, packet->size);
    b->key |= packet->key_frame;
    if (packet->frame_end) {
        if (b->pending) {
            if (b->pts <= b->pending->packet->pts) { result = -EBADMSG; goto fail; }
            b->pending->packet->duration = b->pts - b->pending->packet->pts;
            result = publish_pending(b); if (result < 0) goto fail;
        }
        b->pending = calloc(1, sizeof(*b->pending));
        if (!b->pending) { result = -ENOMEM; goto fail; }
        b->pending->packet = av_packet_alloc();
        if (!b->pending->packet) { result = -ENOMEM; goto fail; }
        av_packet_move_ref(b->pending->packet, b->parts);
        b->pending->type = IPC_MEDIA_VIDEO; b->pending->time_base = b->params.time_base;
        b->pending->packet->pts = b->pending->packet->dts = b->pts;
        b->pending->packet->duration = (1000000 + b->fps / 2) / b->fps;
        if (b->key) b->pending->packet->flags |= AV_PKT_FLAG_KEY;
    }
    return 0;
fail:
    b->failed = true; return result;
}
/** @brief SPS/PPS 已就绪后借用参数；调用者应在桥接器释放前复制。 */
int ipc_h264_bridge_get_params(const IpcH264Bridge *b, const IpcStreamParams **params)
{
    if (!b || !params || !b->ready) return -EINVAL;
    *params = &b->params; return 0;
}
/** @brief 排出暂存末帧，拒绝以缺少尾片的图像完成录像。 */
int ipc_h264_bridge_finish(IpcH264Bridge *b)
{
    if (!b || b->failed) return -EINVAL;
    if (b->finished) return 0;
    if (b->parts->size) { b->failed = true; return -EBADMSG; }
    int result = publish_pending(b);
    if (result < 0) b->failed = true;
    else b->finished = true;
    return result;
}
/** @brief 回收全部内部引用，适用于部分初始化和失败后的清理。 */
void ipc_h264_bridge_deinit(IpcH264Bridge **context)
{
    if (!context || !*context) return;
    IpcH264Bridge *b = *context; *context = NULL;
    ipc_encoded_packet_free(&b->pending); av_packet_free(&b->parts);
    avcodec_parameters_free(&b->params.codecpar); free(b);
}
#else
/** @brief 禁用 FFmpeg 时拒绝初始化桥接器。 */
int ipc_h264_bridge_init(IpcH264Bridge **b,const IpcConfig *c,unsigned int f,IpcPacketQueue *q)
{ (void)b;(void)c;(void)f;(void)q;return -ENOTSUP; }
/** @brief 禁用 FFmpeg 时无编码包转换能力。 */
int ipc_h264_bridge_sink(void *b,const IpcH264Packet *p) { (void)b;(void)p;return -ENOTSUP; }
/** @brief 禁用 FFmpeg 时无有效流参数。 */
int ipc_h264_bridge_get_params(const IpcH264Bridge *b,const IpcStreamParams **p) { (void)b;(void)p;return -ENOTSUP; }
/** @brief 禁用 FFmpeg 时拒绝排空。 */
int ipc_h264_bridge_finish(IpcH264Bridge *b) { (void)b;return -ENOTSUP; }
/** @brief 禁用 FFmpeg 时允许空指针清理。 */
void ipc_h264_bridge_deinit(IpcH264Bridge **b) { if(b)*b=NULL; }
#endif
