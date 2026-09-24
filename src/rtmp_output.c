/** @file rtmp_output.c
 * @brief 双轨按 DTS 排序，单次编码包分发的网络分支；与 MP4 无共享可变封装状态。
 */
#include "rtmp_output.h"
#include "rtmp_transport.h"
#include "packet_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if IPC_WITH_FFMPEG
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
struct IpcRtmpOutput {
    char url[512];
    IpcPacketQueue *queues[2];
    AVFormatContext *format;
    AVIOContext *io;
    AVPacket *scratch;
    IpcRtmpTransport *transport;
    atomic_bool cancel;
    atomic_int error;
    atomic_llong drain_deadline;
    int io_error;
    int64_t last_dts[2], shift_ms;
    bool timeline_ready;
    IpcRtmpStats stats;
};
/** @brief 记录首个失败并关闭两个队列，唤醒消费者；允许两个生产者同时发现异常。 */
void ipc_rtmp_output_abort(IpcRtmpOutput *r, int error)
{
    if (!r) return;
    if (error >= 0) error = -ECANCELED;
    int expected = 0;
    if (atomic_compare_exchange_strong(&r->error,&expected,error)) {
        char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(error,message,sizeof(message));
        ipc_log_write(IPC_LOG_WARN,"rtmp","output disabled: %s (%d); no automatic reconnect",message,error);
    }
    atomic_store(&r->cancel,true);
    ipc_packet_queue_close(r->queues[0]); ipc_packet_queue_close(r->queues[1]);
}
/** @brief 自定义 AVIO 只发送有界块；独立记录错误防止封装器的返回值掩盖底层失败。 */
static int write_flv(void *opaque, uint8_t *data, int size)
{
    IpcRtmpOutput *r = opaque;
    if (r->io_error) return r->io_error;
    int total = size;
    while (size > 0) {
        int count = size > 32768 ? 32768 : size;
        int result = ipc_rtmp_transport_write(r->transport,data,(size_t)count);
        if (result < 0) { r->io_error = result; return result; }
        data += count; size -= count; r->stats.bytes += (unsigned int)count;
    }
    return total;
}
/** @brief 复制独立流参数，保留 SPS/PPS 与 AAC ASC，由 FLV muxer 生成 sequence header。 */
static int add_stream(IpcRtmpOutput *r, const IpcStreamParams *params, unsigned int fps)
{
    AVStream *stream = avformat_new_stream(r->format,NULL);
    if (!stream) return -ENOMEM;
    int result = avcodec_parameters_copy(stream->codecpar,params->codecpar);
    if (result < 0) return result;
    stream->codecpar->codec_tag = 0;
    stream->time_base = (AVRational){params->time_base.num,params->time_base.den};
    if (params->type == IPC_MEDIA_VIDEO) stream->avg_frame_rate = (AVRational){(int)fps,1};
    return 0;
}
/** @brief 初始化不连接网络，使本地 MP4 可以独立启动；初始化失败不残留队列或引用。 */
int ipc_rtmp_output_init(IpcRtmpOutput **context, const IpcConfig *config,
                        const IpcStreamParams *video, const IpcStreamParams *audio, unsigned int fps)
{
    if (!context || *context || !config || !config->rtmp_enabled || ipc_config_validate(config) < 0 ||
        !video || !audio || !video->codecpar || !audio->codecpar ||
        video->type != IPC_MEDIA_VIDEO || audio->type != IPC_MEDIA_AUDIO ||
        video->codecpar->codec_id != AV_CODEC_ID_H264 || audio->codecpar->codec_id != AV_CODEC_ID_AAC ||
        !video->codecpar->extradata || video->codecpar->extradata_size <= 0 ||
        !audio->codecpar->extradata || audio->codecpar->extradata_size <= 0 ||
        video->time_base.num <= 0 || video->time_base.den <= 0 ||
        audio->time_base.num <= 0 || audio->time_base.den <= 0 || !fps || fps > 30) return -EINVAL;
    IpcRtmpOutput *r = calloc(1,sizeof(*r));
    if (!r) return -ENOMEM;
    atomic_init(&r->cancel,false); atomic_init(&r->error,0); atomic_init(&r->drain_deadline,0);
    memcpy(r->url,config->rtmp_url,sizeof(r->url));
    r->last_dts[0] = r->last_dts[1] = AV_NOPTS_VALUE;
    int result = ipc_packet_queue_create(&r->queues[0],config->video_packet_capacity);
    if (!result) result = ipc_packet_queue_create(&r->queues[1],config->audio_packet_capacity);
    if (!result) result = avformat_alloc_output_context2(&r->format,NULL,"flv",NULL);
    if (!result) result = add_stream(r,video,fps);
    if (!result) result = add_stream(r,audio,fps);
    if (result < 0) goto fail;
    r->scratch = av_packet_alloc();
    uint8_t *buffer = av_malloc(32768);
    if (!r->scratch || !buffer) { av_free(buffer); result = -ENOMEM; goto fail; }
    r->io = avio_alloc_context(buffer,32768,1,r,NULL,write_flv,NULL);
    if (!r->io) { av_free(buffer); result = -ENOMEM; goto fail; }
    r->io->seekable = 0;
    r->format->pb = r->io; r->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    /* 自己统一平移必要的负 DTS，禁止库按输出重新选择时间原点。 */
    r->format->avoid_negative_ts = 0;
    *context = r; return 0;
fail:
    ipc_rtmp_output_deinit(&r); return result;
}
/** @brief 新建只读 payload 引用；MP4 和网络拥有不同 AVPacket，任何一侧重标定互不影响。 */
void ipc_rtmp_output_submit(IpcRtmpOutput *r, const IpcEncodedPacket *packet)
{
    if (!r || atomic_load(&r->cancel)) return;
    if (!packet || (packet->type != IPC_MEDIA_VIDEO && packet->type != IPC_MEDIA_AUDIO)) {
        ipc_rtmp_output_abort(r,-EINVAL); return;
    }
    unsigned int index = packet->type == IPC_MEDIA_VIDEO ? 0 : 1;
    IpcEncodedPacket *copy = NULL;
    int result = ipc_encoded_packet_ref(&copy,packet);
    if (!result) result = ipc_packet_queue_try_push(r->queues[index],copy);
    if (result < 0) { ipc_encoded_packet_free(&copy); ipc_rtmp_output_abort(r,result); }
    else if (!index) ++r->stats.video_accepted;
    else ++r->stats.audio_accepted;
}
/** @brief 关闭单条生产队列，正常关闭不会中断网络，使 EOS 后剩余包仍可写完。 */
void ipc_rtmp_output_close(IpcRtmpOutput *r, IpcMediaType type)
{ if (r && (type == IPC_MEDIA_VIDEO || type == IPC_MEDIA_AUDIO)) ipc_packet_queue_close(r->queues[type == IPC_MEDIA_VIDEO ? 0 : 1]); }
/** @brief 设置一次共享排空期限，反复主循环检查不会延长退出等待。 */
void ipc_rtmp_output_begin_drain(IpcRtmpOutput *r)
{
    if (!r) return;
    long long expected = 0, now = ipc_monotonic_us();
    if (now < 0) { ipc_rtmp_output_abort(r,-EIO); return; }
    (void)atomic_compare_exchange_strong(&r->drain_deadline,&expected,now+3000000);
}
/** @brief 并发读取首个网络故障；NULL 视为未创建输出。 */
int ipc_rtmp_output_error(const IpcRtmpOutput *r) { return r ? atomic_load(&r->error) : 0; }
/** @brief 刷新封装数据并优先保留实际网络错误。 */
static int flush_result(IpcRtmpOutput *r, int result)
{
    avio_flush(r->io);
    if (r->io_error < 0) return r->io_error;
    if (r->io->error < 0) return r->io->error;
    return result;
}
/** @brief 重标定独立包到 FLV 实际时间基，两轨共用一个毫秒偏移，保持相对时间差。 */
static int write_packet(IpcRtmpOutput *r, const IpcEncodedPacket *input)
{
    unsigned int index = input->type == IPC_MEDIA_VIDEO ? 0 : 1;
    const AVPacket *packet = input->packet;
    if (!packet || !packet->data || packet->size <= 0 || packet->dts == AV_NOPTS_VALUE ||
        packet->pts == AV_NOPTS_VALUE || packet->duration <= 0 || input->time_base.num <= 0 ||
        input->time_base.den <= 0) return -EINVAL;
    if (!index && !r->stats.video_packets && !(packet->flags & AV_PKT_FLAG_KEY)) return -EBADMSG;
    int result = av_packet_ref(r->scratch,packet);
    if (result < 0) return result;
    AVRational base = {input->time_base.num,input->time_base.den};
    av_packet_rescale_ts(r->scratch,base,r->format->streams[index]->time_base);
    if (!r->timeline_ready) {
        r->shift_ms = r->scratch->dts < 0 ? -r->scratch->dts : 0;
        r->timeline_ready = true;
        ipc_log_write(IPC_LOG_INFO,"rtmp","common timestamp shift_ms=%" PRId64,r->shift_ms);
    }
    r->scratch->pts += r->shift_ms; r->scratch->dts += r->shift_ms;
    r->scratch->stream_index = (int)index; r->scratch->pos = -1;
    if (r->scratch->dts < 0 || r->scratch->pts < r->scratch->dts ||
        (r->last_dts[index] != AV_NOPTS_VALUE && r->scratch->dts <= r->last_dts[index])) result = -EINVAL;
    else {
        r->last_dts[index] = r->scratch->dts;
        /* 上层已按双轨 DTS 排序，直接写包避免 libavformat 另建交错缓存。 */
        result = flush_result(r,av_write_frame(r->format,r->scratch));
    }
    av_packet_unref(r->scratch);
    if (!result) {
        if (!index) ++r->stats.video_packets;
        else ++r->stats.audio_packets;
    }
    return result;
}
/** @brief 连接、写 FLV 头、按 DTS 排空两条队列；任何网络故障只禁用当前输出。 */
void *ipc_rtmp_output_thread(void *opaque)
{
    IpcRtmpOutput *r = opaque;
    IpcEncodedPacket *head[2] = {NULL,NULL};
    bool eof[2] = {false,false};
    AVDictionary *options = NULL;
    int result = ipc_rtmp_transport_open(&r->transport,r->url,&r->cancel,&r->drain_deadline);
    if (!result) result = av_dict_set(&options,"flvflags","no_duration_filesize",0);
    if (!result) result = flush_result(r,avformat_write_header(r->format,&options));
    av_dict_free(&options);
    if (!result) {
        r->stats.header_written = true;
        ipc_log_write(IPC_LOG_INFO,"rtmp","connected: %s; FLV H.264/AAC, video_tb=%d/%d audio_tb=%d/%d",r->url,
            r->format->streams[0]->time_base.num,r->format->streams[0]->time_base.den,
            r->format->streams[1]->time_base.num,r->format->streams[1]->time_base.den);
    }
    while (!result && !atomic_load(&r->cancel) && (!eof[0] || !eof[1] || head[0] || head[1])) {
        int64_t deadline = atomic_load(&r->drain_deadline);
        if (deadline && ipc_monotonic_us() >= deadline) { result = -ETIMEDOUT; break; }
        for (unsigned int i=0;i<2;++i) if (!head[i] && !eof[i]) {
            int read = ipc_packet_queue_pop_timed(r->queues[i],&head[i],10);
            if (!read) eof[i] = true;
            else if (read < 0 && read != -EAGAIN) result = read;
        }
        if (result) break;
        if ((!head[0] && !eof[0]) || (!head[1] && !eof[1]) || (!head[0] && !head[1])) continue;
        unsigned int chosen = !head[0] ? 1 : 0;
        if (head[0] && head[1]) {
            AVRational v = {head[0]->time_base.num,head[0]->time_base.den};
            AVRational a = {head[1]->time_base.num,head[1]->time_base.den};
            chosen = av_compare_ts(head[0]->packet->dts,v,head[1]->packet->dts,a) <= 0 ? 0 : 1;
        }
        result = write_packet(r,head[chosen]); ipc_encoded_packet_free(&head[chosen]);
    }
    if (!result && atomic_load(&r->cancel)) result = ipc_rtmp_output_error(r);
    if (!result) result = flush_result(r,av_write_trailer(r->format));
    if (!result) result = ipc_rtmp_transport_finish(r->transport);
    if (!result && (!r->stats.video_packets || !r->stats.audio_packets)) result = -ENODATA;
    if (result < 0) ipc_rtmp_output_abort(r,result);
    else r->stats.completed = true;
    ipc_rtmp_transport_destroy(&r->transport);
    /* 禁用分发并释放本分支残留引用；本地 MP4 的引用不受影响。 */
    atomic_store(&r->cancel,true);
    for (unsigned int i=0;i<2;++i) {
        ipc_packet_queue_close(r->queues[i]); ipc_encoded_packet_free(&head[i]);
        while (ipc_packet_queue_pop_timed(r->queues[i],&head[i],0) == 1) ipc_encoded_packet_free(&head[i]);
    }
    return NULL;
}
/** @brief join 后读取普通统计及最终原子错误，不在推流过程中读取非原子计数。 */
int ipc_rtmp_output_get_stats(const IpcRtmpOutput *r, IpcRtmpStats *stats)
{ if (!r || !stats) return -EINVAL; *stats = r->stats; stats->error = atomic_load(&r->error); return 0; }
/** @brief 释放封装、编码参数和队列，先由调用方停止并 join 全部使用者。 */
void ipc_rtmp_output_deinit(IpcRtmpOutput **context)
{
    if (!context || !*context) return;
    IpcRtmpOutput *r = *context; *context = NULL;
    ipc_rtmp_transport_destroy(&r->transport);
    ipc_packet_queue_destroy(&r->queues[0]); ipc_packet_queue_destroy(&r->queues[1]);
    av_packet_free(&r->scratch);
    if (r->format) r->format->pb = NULL;
    avformat_free_context(r->format);
    if (r->io) { av_freep(&r->io->buffer); avio_context_free(&r->io); }
    free(r);
}
#else
/** @brief 无 FFmpeg 构建拒绝网络输出初始化。 */
int ipc_rtmp_output_init(IpcRtmpOutput **r,const IpcConfig *c,const IpcStreamParams *v,const IpcStreamParams *a,unsigned int f)
{(void)r;(void)c;(void)v;(void)a;(void)f;return -ENOTSUP;}
/** @brief 无 FFmpeg 时不接管输入包。 */
void ipc_rtmp_output_submit(IpcRtmpOutput *r,const IpcEncodedPacket *p){(void)r;(void)p;}
/** @brief 无 FFmpeg 时没有待关闭的网络队列。 */
void ipc_rtmp_output_close(IpcRtmpOutput *r,IpcMediaType t){(void)r;(void)t;}
/** @brief 无 FFmpeg 时没有排空过程。 */
void ipc_rtmp_output_begin_drain(IpcRtmpOutput *r){(void)r;}
/** @brief 无 FFmpeg 时没有运行中的网络线程。 */
void ipc_rtmp_output_abort(IpcRtmpOutput *r,int e){(void)r;(void)e;}
/** @brief 禁用构建不应创建输出线程。 */
void *ipc_rtmp_output_thread(void *r){(void)r;return NULL;}
/** @brief 无 FFmpeg 时返回不支持状态。 */
int ipc_rtmp_output_error(const IpcRtmpOutput *r){(void)r;return -ENOTSUP;}
/** @brief 无 FFmpeg 时没有有效统计。 */
int ipc_rtmp_output_get_stats(const IpcRtmpOutput *r,IpcRtmpStats *s){(void)r;(void)s;return -ENOTSUP;}
/** @brief 无 FFmpeg 时允许空清理。 */
void ipc_rtmp_output_deinit(IpcRtmpOutput **r){if(r)*r=NULL;}
#endif
