/** @file audio_encoder.c
 * @brief S16_LE→FLTP→采样 FIFO→FFmpeg AAC-LC；所有操作由音频消费者串行调用。
 * 采样率不变，声道不变；微秒起点只换算一次，之后按样本数累加，避免逐块舍入漂移。
 */
#include "audio_encoder.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifndef IPC_WITH_FFMPEG
#define IPC_WITH_FFMPEG 0
#endif
#if IPC_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
struct IpcAudioEncoder {
    AVCodecContext *codec;
    SwrContext *swr;
    AVAudioFifo *fifo;
    AVPacket *packet;
    IpcStreamParams params;
    IpcAudioEncoderStats stats;
    int64_t start_pts;
    bool failed;
};

/** @brief 记录 FFmpeg 错误原文，对外统一返回 errno 风格错误。 */
static int codec_error(const char *operation, int error)
{
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, message, sizeof(message));
    ipc_log_write(IPC_LOG_ERROR, "aac", "%s: %s (%d)", operation, message, error);
    return error == AVERROR(ENOMEM) ? -ENOMEM : -EIO;
}

/** @brief 申请可写 FLTP 帧；数据引用可被编码器保留，释放帧不破坏其引用。 */
static AVFrame *allocate_frame(const IpcAudioEncoder *e, int samples)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) return NULL;
    frame->format = e->codec->sample_fmt;
    frame->channel_layout = e->codec->channel_layout;
    frame->sample_rate = e->codec->sample_rate;
    frame->nb_samples = samples;
    if (av_frame_get_buffer(frame, 0) < 0) av_frame_free(&frame);
    return frame;
}

/** @brief 取完当前可用包并同步分发；EOF 与 EAGAIN 分别通过返回值告知调用者。 */
static int receive_packets(IpcAudioEncoder *e, IpcAudioPacketSink sink, void *opaque)
{
    for (;;) {
        int result = avcodec_receive_packet(e->codec, e->packet);
        if (result == AVERROR(EAGAIN)) return 0;
        if (result == AVERROR_EOF) { e->stats.drained = true; return 1; }
        if (result < 0) return codec_error("receive_packet", result);
        IpcEncodedPacket packet = {IPC_MEDIA_AUDIO, e->packet, e->params.time_base};
        result = sink(opaque, &packet);
        if (result == 0) {
            ++e->stats.packets;
            e->stats.payload_bytes += e->packet->size;
            e->stats.last_packet_pts = e->packet->pts;
        }
        av_packet_unref(e->packet); /* 回调结束即释放本方引用，禁止 sink 借用至下一次调用。 */
        if (result != 0) return result < 0 ? result : -EIO;
    }
}

/** @brief 发送一帧或 EOF；EAGAIN 时先取包，再重试同一帧，确保输入不丢失。 */
static int send_frame(IpcAudioEncoder *e, AVFrame *frame, IpcAudioPacketSink sink, void *opaque)
{
    int result = avcodec_send_frame(e->codec, frame);
    if (result == AVERROR(EAGAIN)) {
        uint64_t before = e->stats.packets;
        result = receive_packets(e, sink, opaque);
        if (result < 0) return result;
        if (result != 0 || e->stats.packets == before) return -EIO;
        result = avcodec_send_frame(e->codec, frame);
    }
    if (result < 0) return codec_error("send_frame", result);
    if (frame) e->stats.submitted_samples += frame->nb_samples;
    result = receive_packets(e, sink, opaque);
    if (result < 0) return result;
    if ((!frame && result != 1) || (frame && result != 0)) return -EIO;
    return 0;
}

/** @brief 从 FIFO 提取完整帧；收尾时将不足一帧的尾部补零并单独统计。 */
static int encode_fifo(IpcAudioEncoder *e, bool finishing, IpcAudioPacketSink sink, void *opaque)
{
    int available;
    while ((available = av_audio_fifo_size(e->fifo)) >= e->codec->frame_size || (finishing && available > 0)) {
        int count = available < e->codec->frame_size ? available : e->codec->frame_size;
        AVFrame *frame = allocate_frame(e, e->codec->frame_size);
        int result;
        if (!frame) return -ENOMEM;
        if (av_audio_fifo_read(e->fifo, (void **)frame->extended_data, count) != count) {
            av_frame_free(&frame); return -EIO;
        }
        if (count < frame->nb_samples) {
            result = av_samples_set_silence(frame->extended_data, count, frame->nb_samples - count,
                                           e->codec->channels, e->codec->sample_fmt);
            if (result < 0) { av_frame_free(&frame); return codec_error("tail silence", result); }
            e->stats.padding_samples += frame->nb_samples - count;
        }
        /* 最多运行 86400 秒；仍检查溢出，支持公共 API 的独立调用者。 */
        if (e->start_pts < 0 || e->start_pts > INT64_MAX - frame->nb_samples ||
            e->stats.submitted_samples > (uint64_t)(INT64_MAX - e->start_pts - frame->nb_samples)) {
            av_frame_free(&frame); return -EOVERFLOW;
        }
        frame->pts = e->start_pts + (int64_t)e->stats.submitted_samples;
        result = send_frame(e, frame, sink, opaque);
        av_frame_free(&frame);
        if (result < 0) return result;
    }
    return 0;
}

/** @brief 转换一块 PCM 或排空转换器；每次最多一秒输入，FIFO 随后立即消费。 */
static int convert_samples(IpcAudioEncoder *e, const uint8_t *input, int samples)
{
    int capacity = swr_get_out_samples(e->swr, samples), converted, result = 0;
    if (capacity < 0) return codec_error("swr_get_out_samples", capacity);
    if (capacity == 0) return 0;
    AVFrame *frame = allocate_frame(e, capacity);
    if (!frame) return -ENOMEM;
    const uint8_t *planes[] = {input};
    converted = swr_convert(e->swr, frame->extended_data, capacity, input ? planes : NULL, samples);
    if (converted < 0) result = codec_error("swr_convert", converted);
    else if (converted > 0) {
        if (av_audio_fifo_write(e->fifo, (void **)frame->extended_data, converted) != converted) result = -ENOMEM;
        else e->stats.converted_samples += converted;
    }
    av_frame_free(&frame);
    return result < 0 ? result : converted;
}

/** @brief 返回编译时启用状态；运行时是否存在 AAC 编码器由 init 检查。 */
bool ipc_audio_encoder_available(void) { return true; }

/** @brief 配置原生 AAC-LC、FLTP、同采样率同声道转换并导出独立流参数。 */
int ipc_audio_encoder_init(IpcAudioEncoder **context, const IpcConfig *config)
{
    IpcAudioEncoder *e;
    int result = -ENOMEM;
    if (!context || *context || !config || config->audio_channels < 1 || config->audio_channels > 2 ||
        config->audio_sample_rate < 8000 || config->audio_sample_rate > 96000 ||
        !config->audio_bitrate || strcmp(config->audio_codec, "aac") ||
        strcmp(config->audio_sample_format, "S16_LE")) return -EINVAL;
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec) { ipc_log_write(IPC_LOG_ERROR, "aac", "native AAC encoder unavailable in FFmpeg"); return -ENOTSUP; }
    /* RK3568 与验证主机均为小端；拒绝将 S16_LE 在大端机器上错误解释为本机 S16。 */
    const uint16_t endian = 1;
    if (*(const uint8_t *)&endian != 1) return -ENOTSUP;
    e = calloc(1, sizeof(*e));
    if (!e) return -ENOMEM;
    e->stats.first_pts_us = -1;
    e->stats.last_packet_pts = AV_NOPTS_VALUE;
    e->codec = avcodec_alloc_context3(codec);
    e->packet = av_packet_alloc();
    e->params.codecpar = avcodec_parameters_alloc();
    if (!e->codec || !e->packet || !e->params.codecpar) goto fail;
    e->codec->sample_rate = (int)config->audio_sample_rate;
    e->codec->channels = (int)config->audio_channels;
    e->codec->channel_layout = av_get_default_channel_layout(e->codec->channels);
    e->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    e->codec->bit_rate = config->audio_bitrate;
    e->codec->profile = FF_PROFILE_AAC_LOW;
    e->codec->time_base = (AVRational){1, e->codec->sample_rate};
    e->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    result = avcodec_open2(e->codec, codec, NULL);
    if (result < 0) { result = codec_error("avcodec_open2", result); goto fail; }
    if (e->codec->frame_size <= 0 || e->codec->frame_size > e->codec->sample_rate ||
        e->codec->profile != FF_PROFILE_AAC_LOW || e->codec->sample_fmt != AV_SAMPLE_FMT_FLTP) {
        result = -ENOTSUP; goto fail;
    }
    e->swr = swr_alloc_set_opts(NULL, e->codec->channel_layout, e->codec->sample_fmt, e->codec->sample_rate,
                               e->codec->channel_layout, AV_SAMPLE_FMT_S16, e->codec->sample_rate, 0, NULL);
    e->fifo = av_audio_fifo_alloc(e->codec->sample_fmt, e->codec->channels, e->codec->frame_size);
    if (!e->swr || !e->fifo) { result = -ENOMEM; goto fail; }
    result = swr_init(e->swr);
    if (result < 0) { result = codec_error("swr_init", result); goto fail; }
    result = avcodec_parameters_from_context(e->params.codecpar, e->codec);
    if (result < 0) { result = codec_error("codec parameters", result); goto fail; }
    e->params.type = IPC_MEDIA_AUDIO;
    e->params.time_base = (IpcTimeBase){1, e->codec->sample_rate};
    e->stats.frame_samples = (unsigned int)e->codec->frame_size;
    e->stats.initial_padding = (unsigned int)e->codec->initial_padding;
    ipc_log_write(IPC_LOG_INFO, "aac", "AAC-LC: rate=%d channels=%d bitrate=%u frame_samples=%u initial_padding=%u",
                  e->codec->sample_rate, e->codec->channels, config->audio_bitrate, e->stats.frame_samples, e->stats.initial_padding);
    *context = e;
    return 0;
fail:
    ipc_audio_encoder_deinit(&e);
    return result;
}

/** @brief 读取借用的编码参数；初始化后即有 AudioSpecificConfig，可用于 ADTS/后续封装。 */
int ipc_audio_encoder_get_params(const IpcAudioEncoder *e, const IpcStreamParams **params)
{
    if (!e || !params) return -EINVAL;
    *params = &e->params;
    return 0;
}

/** @brief 校验连续下标和 PTS 后转换输入，任何中途失败锁定实例，避免重复提交部分数据。 */
int ipc_audio_encoder_push(IpcAudioEncoder *e, const IpcRawFrame *frame, IpcAudioPacketSink sink, void *opaque)
{
    int64_t expected;
    int result;
    if (!e || !frame || !sink || e->failed || e->stats.drained) return -EINVAL;
    if (frame->type != IPC_MEDIA_AUDIO || !frame->data || frame->pts_us < 0 ||
        frame->info.audio.sample_format != IPC_AUDIO_FORMAT_S16_LE ||
        frame->info.audio.sample_rate != (unsigned int)e->codec->sample_rate ||
        frame->info.audio.channels != (unsigned int)e->codec->channels ||
        !frame->info.audio.samples_per_channel || frame->info.audio.samples_per_channel > (unsigned int)e->codec->sample_rate ||
        frame->size != (size_t)frame->info.audio.samples_per_channel * e->codec->channels * 2 ||
        frame->info.audio.sample_index != e->stats.input_samples) return -EINVAL;
    int64_t first = e->stats.first_pts_us < 0 ? frame->pts_us : e->stats.first_pts_us;
    if (ipc_audio_pts_us(first, e->stats.input_samples, e->codec->sample_rate, &expected) < 0 || expected != frame->pts_us)
        return -EINVAL;
    if (e->stats.first_pts_us < 0) {
        e->stats.first_pts_us = first;
        e->start_pts = av_rescale_q(first, (AVRational){1, 1000000}, e->codec->time_base);
    }
    result = convert_samples(e, frame->data, (int)frame->info.audio.samples_per_channel);
    if (result >= 0) {
        e->stats.input_samples += frame->info.audio.samples_per_channel;
        result = encode_fifo(e, false, sink, opaque);
    }
    if (result < 0) e->failed = true;
    return result;
}

/** @brief 先排空重采样与 FIFO，最后只发送一次 EOF；验证输入样本全部进入编码器。 */
int ipc_audio_encoder_finish(IpcAudioEncoder *e, IpcAudioPacketSink sink, void *opaque)
{
    int result;
    if (!e || !sink || e->failed) return -EINVAL;
    if (e->stats.drained) return 0;
    do {
        result = convert_samples(e, NULL, 0);
        if (result < 0) goto fail;
        if (encode_fifo(e, false, sink, opaque) < 0) { result = -EIO; goto fail; }
    } while (result > 0);
    result = encode_fifo(e, true, sink, opaque);
    if (result < 0) goto fail;
    result = send_frame(e, NULL, sink, opaque);
    if (result < 0) goto fail;
    if (e->stats.converted_samples != e->stats.input_samples ||
        e->stats.submitted_samples != e->stats.input_samples + e->stats.padding_samples) { result = -EIO; goto fail; }
    return 0;
fail:
    e->failed = true;
    return result;
}

/** @brief 复制统计快照；调用者须与编码操作串行。 */
int ipc_audio_encoder_get_stats(const IpcAudioEncoder *e, IpcAudioEncoderStats *stats)
{
    if (!e || !stats) return -EINVAL;
    *stats = e->stats;
    return 0;
}

/** @brief 按资源依赖释放，支持初始化中途失败及重复释放空指针。 */
void ipc_audio_encoder_deinit(IpcAudioEncoder **context)
{
    if (!context || !*context) return;
    IpcAudioEncoder *e = *context;
    *context = NULL;
    av_packet_free(&e->packet);
    avcodec_parameters_free(&e->params.codecpar);
    av_audio_fifo_free(e->fifo);
    swr_free(&e->swr);
    avcodec_free_context(&e->codec);
    free(e);
}
#else
/** @brief 无 FFmpeg 构建显式报告不可用。 */
bool ipc_audio_encoder_available(void) { return false; }
/** @brief 无 FFmpeg 构建拒绝初始化，不伪造成功。 */
int ipc_audio_encoder_init(IpcAudioEncoder **e, const IpcConfig *c) { (void)e; (void)c; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建无有效流参数。 */
int ipc_audio_encoder_get_params(const IpcAudioEncoder *e, const IpcStreamParams **p) { (void)e; (void)p; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建拒绝编码。 */
int ipc_audio_encoder_push(IpcAudioEncoder *e, const IpcRawFrame *f, IpcAudioPacketSink s, void *o)
{ (void)e; (void)f; (void)s; (void)o; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建拒绝排空。 */
int ipc_audio_encoder_finish(IpcAudioEncoder *e, IpcAudioPacketSink s, void *o)
{ (void)e; (void)s; (void)o; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建无统计。 */
int ipc_audio_encoder_get_stats(const IpcAudioEncoder *e, IpcAudioEncoderStats *s) { (void)e; (void)s; return -ENOTSUP; }
/** @brief 无 FFmpeg 构建允许空上下文清理。 */
void ipc_audio_encoder_deinit(IpcAudioEncoder **e) { if (e) *e = NULL; }
#endif
