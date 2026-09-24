/** @file record_pipeline.c
 * @brief V4L2/ALSA 双采集、MPP/AAC 双编码和独立 MP4/RTMP 输出线程。
 * 主线程同步处理信号；原子变量只传停止/错误状态，普通统计在 join 后读取。
 */
#define _POSIX_C_SOURCE 200809L
#include "record_pipeline.h"
#include "v4l2_capture.h"
#include "alsa_capture.h"
#include "video_encoder.h"
#include "audio_encoder.h"
#include "h264_bridge.h"
#include "mp4_output.h"
#include "rtmp_output.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#if IPC_WITH_FFMPEG
#include <libavcodec/packet.h>
#include <libavutil/mathematics.h>
enum { VIDEO_CAPTURE, AUDIO_CAPTURE, VIDEO_ENCODE, AUDIO_ENCODE, MP4_WRITE, RTMP_WRITE, WORKERS };
typedef struct {
    IpcVideoCapture *video_capture;
    IpcAudioCapture *audio_capture;
    IpcVideoEncoder *video_encoder;
    IpcAudioEncoder *audio_encoder;
    IpcH264Bridge *bridge;
    IpcMp4Output *mp4;
    IpcRtmpOutput *rtmp;
    int network_init_error; /**< 初始化/线程创建错误由主线程独占，join 后汇总。 */
    IpcFrameQueue *video_raw, *audio_raw;
    IpcPacketQueue *video_packets, *audio_packets;
    IpcEncodedPacket *audio_pending; /**< 延迟一个包，收尾时才能扣除显式尾部补零时长。 */
    IpcAudioCaptureFormat audio_format;
    IpcRecordOptions options;
    atomic_bool stop, done[WORKERS];
    atomic_int error;
    int64_t epoch_us, limit_us;
    uint64_t video_enqueued, video_consumed, audio_enqueued, audio_consumed, audio_published;
} RecordPipeline;
/** @brief 只保留首个错误并请求采集停止；不在异步信号处理器里操作队列或资源。 */
static void fail_record(RecordPipeline *p, const char *stage, int error)
{
    if (error >= 0) return;
    int expected = 0;
    if (atomic_compare_exchange_strong(&p->error, &expected, error))
        ipc_log_write(IPC_LOG_ERROR, "record", "%s failed: %s (%d)", stage, strerror(-error), error);
    atomic_store(&p->stop, true);
}
/** @brief 检查停止请求与共同单调截止时刻；墙上时间调整不影响录制时长。 */
static bool capture_stopping(RecordPipeline *p)
{
    if (atomic_load(&p->stop)) return true;
    int64_t now = ipc_monotonic_us();
    if (now < 0) { fail_record(p, "clock", -EIO); return true; }
    if (p->limit_us && now - p->epoch_us >= p->limit_us) {
        atomic_store(&p->stop, true); return true;
    }
    return false;
}
/** @brief 视频采集按原始 PTS 入队；录像模式队列满即停止，避免无声丢帧伪装完整。 */
static void *record_video_capture(void *opaque)
{
    RecordPipeline *p = opaque;
    int result = 0;
    int64_t last = ipc_monotonic_us();
    if (!capture_stopping(p)) result = ipc_video_capture_start(p->video_capture, p->epoch_us);
    while (!result && !capture_stopping(p)) {
        IpcRawFrame *frame = NULL;
        result = ipc_video_capture_read(p->video_capture, &frame, 100);
        int64_t now = ipc_monotonic_us();
        if (now < 0) { ipc_raw_frame_free(&frame); result = -EIO; break; }
        if (!result || result == -EBADMSG) {
            result = now - last >= 3000000 ? -ETIMEDOUT : 0;
            continue;
        }
        if (result < 0) break;
        last = now;
        if (p->limit_us && frame->pts_us >= p->limit_us) {
            ipc_raw_frame_free(&frame); atomic_store(&p->stop, true); result = 0; break;
        }
        result = ipc_frame_queue_try_push(p->video_raw, frame);
        if (!result) { ++p->video_enqueued; frame = NULL; }
        ipc_raw_frame_free(&frame);
    }
    fail_record(p, "video capture/raw queue", result);
    fail_record(p, "video stop", ipc_video_capture_stop(p->video_capture));
    ipc_frame_queue_close(p->video_raw); atomic_store(&p->done[VIDEO_CAPTURE], true);
    return NULL;
}
/** @brief 音频采集保持样本连续性，最后一块按共同截止 PTS 截短，不把驱动块大小当时长。 */
static void *record_audio_capture(void *opaque)
{
    RecordPipeline *p = opaque;
    int result = 0;
    int64_t last = ipc_monotonic_us();
    if (!capture_stopping(p)) result = ipc_audio_capture_start(p->audio_capture, p->epoch_us);
    while (!result && !capture_stopping(p)) {
        IpcRawFrame *frame = NULL;
        result = ipc_audio_capture_read(p->audio_capture, &frame, p->audio_format.period_frames, 100);
        int64_t now = ipc_monotonic_us();
        if (now < 0) { ipc_raw_frame_free(&frame); result = -EIO; break; }
        if (!result) { result = now - last >= 3000000 ? -ETIMEDOUT : 0; continue; }
        if (result < 0) break;
        last = now;
        if (p->limit_us) {
            int64_t left = p->limit_us - frame->pts_us;
            uint64_t allowed = left > 0 ? (uint64_t)left * p->audio_format.sample_rate / 1000000 : 0;
            if (!allowed) { ipc_raw_frame_free(&frame); atomic_store(&p->stop, true); result = 0; break; }
            if (allowed < frame->info.audio.samples_per_channel) {
                frame->info.audio.samples_per_channel = (unsigned int)allowed;
                frame->size = (size_t)allowed * p->audio_format.channels * 2;
                atomic_store(&p->stop, true);
            }
        }
        unsigned int count = frame->info.audio.samples_per_channel;
        result = ipc_frame_queue_try_push(p->audio_raw, frame);
        if (!result) { p->audio_enqueued += count; frame = NULL; }
        ipc_raw_frame_free(&frame);
    }
    fail_record(p, "audio capture/raw queue", result);
    fail_record(p, "audio stop", ipc_audio_capture_stop(p->audio_capture));
    ipc_frame_queue_close(p->audio_raw); atomic_store(&p->done[AUDIO_CAPTURE], true);
    return NULL;
}
/** @brief 复制引用给本地队列后分发网络引用；网络关闭不传播为本地编码错误。 */
static int publish_packet(void *opaque, const IpcEncodedPacket *packet)
{
    RecordPipeline *p = opaque;
    if (!p->options.stream_only) {
        IpcEncodedPacket *local = NULL;
        int result = ipc_encoded_packet_ref(&local,packet);
        if (!result) result = ipc_packet_queue_try_push(packet->type == IPC_MEDIA_VIDEO ? p->video_packets : p->audio_packets,local);
        if (result < 0) { ipc_encoded_packet_free(&local); return result; }
    }
    ipc_rtmp_output_submit(p->rtmp,packet);
    return 0;
}
/** @brief 网络输出独立运行并上报结束；只推流模式的故障由主线程请求采集停止。 */
static void *record_network(void *opaque)
{
    RecordPipeline *p = opaque;
    ipc_rtmp_output_thread(p->rtmp);
    atomic_store(&p->done[RTMP_WRITE],true);
    return NULL;
}
/** @brief 视频编码线程排空原始队列，之后发送 EOS 并排出暂存末帧，再关闭编码包队列。 */
static void *record_video_encode(void *opaque)
{
    RecordPipeline *p = opaque;
    int result, error = 0;
    IpcRawFrame *frame = NULL;
    while ((result = ipc_frame_queue_pop(p->video_raw, &frame)) == 1) {
        if (!error) {
            error = ipc_video_encoder_send(p->video_encoder, frame);
            if (!error) ++p->video_consumed;
            else fail_record(p, "video encode/packet queue", error);
        }
        ipc_raw_frame_free(&frame);
    }
    if (result < 0 && !error) error = result;
    if (!error) error = ipc_video_encoder_finish(p->video_encoder);
    if (!error) error = ipc_h264_bridge_finish(p->bridge);
    fail_record(p, "video drain", error);
    ipc_rtmp_output_close(p->rtmp,IPC_MEDIA_VIDEO);
    ipc_packet_queue_close(p->video_packets); atomic_store(&p->done[VIDEO_ENCODE], true);
    return NULL;
}
/** @brief 转交前一个 AAC 包，当前包创建独立引用暂存，以便 finish 时修正最终 duration。 */
static int enqueue_audio(void *opaque, const IpcEncodedPacket *packet)
{
    RecordPipeline *p = opaque;
    IpcEncodedPacket *next = NULL;
    int result = ipc_encoded_packet_ref(&next, packet);
    if (result < 0) return result;
    if (p->audio_pending) {
        result = publish_packet(p,p->audio_pending);
        if (result < 0) { ipc_encoded_packet_free(&next); return result; }
        ++p->audio_published;
        ipc_encoded_packet_free(&p->audio_pending);
    }
    p->audio_pending = next;
    return 0;
}
/** @brief AAC 编码线程按样本重分帧，排空后只修正末包的显式补零时长，不修改前端真实 PTS。 */
static void *record_audio_encode(void *opaque)
{
    RecordPipeline *p = opaque;
    int result, error = 0;
    IpcRawFrame *frame = NULL;
    IpcAudioEncoderStats stats = {0};
    while ((result = ipc_frame_queue_pop(p->audio_raw, &frame)) == 1) {
        if (!error) {
            error = ipc_audio_encoder_push(p->audio_encoder, frame, enqueue_audio, p);
            if (!error) p->audio_consumed += frame->info.audio.samples_per_channel;
            else fail_record(p, "audio encode/packet queue", error);
        }
        ipc_raw_frame_free(&frame);
    }
    if (result < 0 && !error) error = result;
    if (!error) error = ipc_audio_encoder_finish(p->audio_encoder, enqueue_audio, p);
    if (!error) {
        ipc_audio_encoder_get_stats(p->audio_encoder, &stats);
        if (p->audio_pending) {
            if (stats.padding_samples >= (uint64_t)p->audio_pending->packet->duration) error = -EBADMSG;
            else {
                p->audio_pending->packet->duration -= (int64_t)stats.padding_samples;
                error = publish_packet(p,p->audio_pending);
                if (!error) { ipc_encoded_packet_free(&p->audio_pending); ++p->audio_published; }
            }
        }
    }
    fail_record(p, "audio drain", error);
    ipc_encoded_packet_free(&p->audio_pending);
    ipc_rtmp_output_close(p->rtmp,IPC_MEDIA_AUDIO);
    ipc_packet_queue_close(p->audio_packets); atomic_store(&p->done[AUDIO_ENCODE], true);
    return NULL;
}
/** @brief 有限等待两路队首，按 DTS 交错写入；一条关闭后仍持续排空另一条。 */
static void *record_output(void *opaque)
{
    RecordPipeline *p = opaque;
    IpcPacketQueue *queues[2] = {p->video_packets,p->audio_packets};
    IpcEncodedPacket *head[2] = {NULL,NULL};
    bool eof[2] = {false,false};
    int error = 0;
    while (!eof[0] || !eof[1] || head[0] || head[1]) {
        for (unsigned int i = 0; i < 2; ++i) if (!head[i] && !eof[i]) {
            int result = ipc_packet_queue_pop_timed(queues[i], &head[i], 10);
            if (!result) eof[i] = true;
            else if (result < 0 && result != -EAGAIN) { error = result; eof[i] = true; }
        }
        if (error) {
            fail_record(p, "MP4 output", error);
            ipc_packet_queue_close(queues[0]); ipc_packet_queue_close(queues[1]);
            ipc_encoded_packet_free(&head[0]); ipc_encoded_packet_free(&head[1]);
            continue;
        }
        if ((!head[0] && !eof[0]) || (!head[1] && !eof[1])) continue;
        if (!head[0] && !head[1]) continue;
        unsigned int chosen = !head[0] ? 1 : 0;
        if (head[0] && head[1]) {
            AVRational v = {head[0]->time_base.num,head[0]->time_base.den};
            AVRational a = {head[1]->time_base.num,head[1]->time_base.den};
            chosen = av_compare_ts(head[0]->packet->dts,v,head[1]->packet->dts,a) <= 0 ? 0 : 1;
        }
        error = ipc_mp4_output_write(p->mp4, head[chosen]);
        ipc_encoded_packet_free(&head[chosen]);
        if (!error && p->options.output_delay_ms) {
            struct timespec delay = {.tv_sec=p->options.output_delay_ms/1000,
                                    .tv_nsec=(long)(p->options.output_delay_ms%1000)*1000000};
            while (nanosleep(&delay,&delay) < 0 && errno == EINTR) {}
        }
    }
    if (!error) error = ipc_mp4_output_finish(p->mp4);
    fail_record(p, "MP4 finalize", error);
    atomic_store(&p->done[MP4_WRITE], true);
    return NULL;
}
/** @brief 初始化双路设备/编码和容器，启动所选输出线程，共同停止后按依赖 join 并核对包数。 */
int ipc_record_pipeline_run(const IpcConfig *config, const IpcRecordOptions *options)
{
    RecordPipeline p = {0};
    pthread_t threads[WORKERS];
    bool started[WORKERS] = {false}, interrupted = false;
    sigset_t signals, old_mask;
    int result = 0, current;
    unsigned int fps;
    const IpcStreamParams *video_params = NULL, *audio_params = NULL;
    IpcVideoEncoderStats video = {0}; IpcAudioEncoderStats audio = {0}; IpcMp4Stats mux = {0};
    IpcRtmpStats network = {0};
    IpcVideoCaptureStats vc = {0}; IpcAudioCaptureStats ac = {0};
    if (!config || !options || options->seconds > 86400 || options->encode_fps > 30 || options->output_delay_ms > 1000) return 1;
    if (options->stream_only && !config->rtmp_enabled) return 1;
    if (!ipc_video_encoder_available() || !ipc_audio_capture_available() || !ipc_audio_encoder_available()) {
        ipc_log_write(IPC_LOG_ERROR,"record","record requires MPP, ALSA and FFmpeg support"); return 1;
    }
    p.options = *options; fps = options->encode_fps ? options->encode_fps : config->video_fps;
    const char *path = options->mp4_path ? options->mp4_path : config->record_path;
    atomic_init(&p.stop,false); atomic_init(&p.error,0);
    for (unsigned int i=0;i<WORKERS;++i) atomic_init(&p.done[i],false);
    sigemptyset(&signals); sigaddset(&signals,SIGINT); sigaddset(&signals,SIGTERM);
    current = pthread_sigmask(SIG_BLOCK,&signals,&old_mask);
    if (current) return 1;
    /* 资源先全部就绪，再启动采集；编码头必须在 MP4 写头之前拿到。 */
    result = ipc_video_capture_init(&p.video_capture,config); if (result < 0) goto shutdown;
    result = ipc_audio_capture_init(&p.audio_capture,config); if (result < 0) goto shutdown;
    result = ipc_audio_capture_get_format(p.audio_capture,&p.audio_format); if (result < 0) goto shutdown;
    result = ipc_frame_queue_create(&p.video_raw,config->video_raw_capacity); if (result < 0) goto shutdown;
    result = ipc_frame_queue_create(&p.audio_raw,config->audio_raw_capacity); if (result < 0) goto shutdown;
    if (!options->stream_only) {
        result = ipc_packet_queue_create(&p.video_packets,config->video_packet_capacity); if (result < 0) goto shutdown;
        result = ipc_packet_queue_create(&p.audio_packets,config->audio_packet_capacity); if (result < 0) goto shutdown;
    }
    result = ipc_h264_bridge_init(&p.bridge,config,fps,p.video_packets); if (result < 0) goto shutdown;
    result = ipc_h264_bridge_set_sink(p.bridge,publish_packet,&p); if (result < 0) goto shutdown;
    result = ipc_video_encoder_init(&p.video_encoder,config,fps,ipc_h264_bridge_sink,p.bridge); if (result < 0) goto shutdown;
    result = ipc_audio_encoder_init(&p.audio_encoder,config); if (result < 0) goto shutdown;
    result = ipc_h264_bridge_get_params(p.bridge,&video_params); if (result < 0) goto shutdown;
    result = ipc_audio_encoder_get_params(p.audio_encoder,&audio_params); if (result < 0) goto shutdown;
    if (!options->stream_only) {
        result = ipc_mp4_output_init(&p.mp4,path,video_params,audio_params,fps); if (result < 0) goto shutdown;
    }
    if (config->rtmp_enabled) {
        p.network_init_error = ipc_rtmp_output_init(&p.rtmp,config,video_params,audio_params,fps);
        if (p.network_init_error < 0) {
            ipc_log_write(IPC_LOG_WARN,"rtmp","initialization failed: %d",p.network_init_error);
            if (options->stream_only) { result = p.network_init_error; goto shutdown; }
        }
    }
    p.epoch_us = ipc_monotonic_us();
    if (p.epoch_us < 0) { result = -EIO; goto shutdown; }
    p.limit_us = (int64_t)options->seconds * 1000000;
    ipc_log_write(IPC_LOG_INFO,"record","starting: common_epoch_us=%" PRId64 " seconds=%u fps=%u output=%s",p.epoch_us,options->seconds,fps,options->stream_only ? "RTMP only" : path);
    void *(*workers[WORKERS])(void *) = {record_video_capture,record_audio_capture,record_video_encode,record_audio_encode,record_output,record_network};
    const unsigned int order[WORKERS] = {MP4_WRITE,RTMP_WRITE,VIDEO_ENCODE,AUDIO_ENCODE,VIDEO_CAPTURE,AUDIO_CAPTURE};
    for (unsigned int i=0;i<WORKERS;++i) {
        unsigned int index = order[i];
        if ((index == MP4_WRITE && !p.mp4) || (index == RTMP_WRITE && !p.rtmp)) {
            atomic_store(&p.done[index],true); continue;
        }
        current = pthread_create(&threads[index],NULL,workers[index],&p);
        if (current) {
            if (index == RTMP_WRITE && !options->stream_only) {
                p.network_init_error = -current; ipc_rtmp_output_abort(p.rtmp,-current);
                atomic_store(&p.done[index],true); continue;
            }
            result = -current; goto shutdown;
        }
        started[index] = true;
    }
    for (;;) {
        bool done = true;
        for (unsigned int i=0;i<WORKERS;++i) done &= atomic_load(&p.done[i]);
        if (done) break;
        const struct timespec wait = {.tv_sec=0,.tv_nsec=100000000};
        current = sigtimedwait(&signals,NULL,&wait);
        if (current == SIGINT || current == SIGTERM) { interrupted = true; atomic_store(&p.stop,true); }
        else if (current < 0 && errno != EAGAIN && errno != EINTR) fail_record(&p,"signal wait",-errno);
        if (options->stream_only && ipc_rtmp_output_error(p.rtmp)) fail_record(&p,"RTMP",ipc_rtmp_output_error(p.rtmp));
        (void)capture_stopping(&p);
        if (atomic_load(&p.stop)) ipc_rtmp_output_begin_drain(p.rtmp);
    }
shutdown:
    fail_record(&p,"initialization/thread creation",result);
    atomic_store(&p.stop,true);
    ipc_rtmp_output_begin_drain(p.rtmp);
    /* 未成功创建的生产者不会关闭队列，管理线程补齐；成功创建者自行排空/关闭。 */
    if (!started[VIDEO_CAPTURE]) ipc_frame_queue_close(p.video_raw);
    if (!started[AUDIO_CAPTURE]) ipc_frame_queue_close(p.audio_raw);
    if (!started[VIDEO_ENCODE] || !started[MP4_WRITE]) ipc_packet_queue_close(p.video_packets);
    if (!started[AUDIO_ENCODE] || !started[MP4_WRITE]) ipc_packet_queue_close(p.audio_packets);
    if (!started[VIDEO_ENCODE]) ipc_rtmp_output_close(p.rtmp,IPC_MEDIA_VIDEO);
    if (!started[AUDIO_ENCODE]) ipc_rtmp_output_close(p.rtmp,IPC_MEDIA_AUDIO);
    if (!started[RTMP_WRITE] && p.rtmp) ipc_rtmp_output_abort(p.rtmp,p.network_init_error ? p.network_init_error : -ECANCELED);
    for (unsigned int i=0;i<WORKERS;++i) if (started[i]) pthread_join(threads[i],NULL);
    if (p.video_encoder) ipc_video_encoder_get_stats(p.video_encoder,&video);
    if (p.audio_encoder) ipc_audio_encoder_get_stats(p.audio_encoder,&audio);
    if (p.mp4) ipc_mp4_output_get_stats(p.mp4,&mux);
    if (p.rtmp) ipc_rtmp_output_get_stats(p.rtmp,&network);
    if (p.network_init_error) network.error = p.network_init_error;
    if (p.video_capture) ipc_video_capture_get_stats(p.video_capture,&vc);
    if (p.audio_capture) ipc_audio_capture_get_stats(p.audio_capture,&ac);
    if (!atomic_load(&p.error) && (!video.eos || !audio.drained ||
        p.video_enqueued != p.video_consumed || video.submitted != video.encoded ||
        p.audio_enqueued != p.audio_consumed || audio.input_samples != p.audio_consumed || audio.packets != p.audio_published ||
        (!options->stream_only && (!mux.trailer_written || video.encoded != mux.video_packets || p.audio_published != mux.audio_packets)))) fail_record(&p,"final counters",-EIO);
    if (config->rtmp_enabled && !network.error && (!network.completed ||
        network.video_accepted != video.encoded || network.video_packets != video.encoded ||
        network.audio_accepted != audio.packets || network.audio_packets != audio.packets)) {
        network.error = -EIO; network.completed = false;
    }
    if (options->stream_only && network.error) fail_record(&p,"RTMP final counters",network.error);
    if (config->rtmp_enabled) ipc_log_write(IPC_LOG_INFO,"rtmp",
        "summary: video_accepted=%" PRIu64 " audio_accepted=%" PRIu64 " video_packets=%" PRIu64
        " audio_packets=%" PRIu64 " bytes=%" PRIu64 " header=%u completed=%u error=%d",
        network.video_accepted,network.audio_accepted,network.video_packets,network.audio_packets,
        network.bytes,(unsigned int)network.header_written,(unsigned int)network.completed,network.error);
    ipc_rtmp_output_deinit(&p.rtmp);
    fail_record(&p,"MPP cleanup",ipc_video_encoder_deinit(&p.video_encoder));
    ipc_audio_encoder_deinit(&p.audio_encoder); ipc_h264_bridge_deinit(&p.bridge);
    fail_record(&p,"MP4 close",ipc_mp4_output_deinit(&p.mp4));
    fail_record(&p,"V4L2 cleanup",ipc_video_capture_deinit(&p.video_capture));
    fail_record(&p,"ALSA cleanup",ipc_audio_capture_deinit(&p.audio_capture));
    ipc_frame_queue_destroy(&p.video_raw); ipc_frame_queue_destroy(&p.audio_raw);
    ipc_packet_queue_destroy(&p.video_packets); ipc_packet_queue_destroy(&p.audio_packets);
    ipc_encoded_packet_free(&p.audio_pending);
    ipc_log_write(IPC_LOG_INFO,"record","summary: video_enqueued=%" PRIu64 " video_encoded=%" PRIu64 " video_packets=%" PRIu64
                  " video_eos=%u audio_samples=%" PRIu64 " audio_packets=%" PRIu64 " audio_drained=%u trailer=%u",
                  p.video_enqueued,video.encoded,mux.video_packets,(unsigned int)video.eos,p.audio_consumed,mux.audio_packets,
                  (unsigned int)audio.drained,(unsigned int)mux.trailer_written);
    ipc_log_write(IPC_LOG_INFO,"record","timeline: first_video_us=%" PRId64 " first_audio_sample_us=%" PRId64
                  " last_video_us=%" PRId64 " last_audio_block_us=%" PRId64 " invalid=%" PRIu64 " sequence_gaps=%" PRIu64
                  " timestamp_fallback=%" PRIu64 " xruns=%" PRIu64 " suspends=%" PRIu64,
                  video.first_pts_us,ac.first_pts_us,video.last_pts_us,ac.last_pts_us,vc.invalid,vc.sequence_gaps,
                  vc.timestamp_fallback,ac.xruns,ac.suspends);
    /* 用真实输入样本末端与末帧 PTS 辅助板端观察漂移；不是校准后的音画同步误差。 */
    int64_t audio_end_us = -1;
    if (p.audio_consumed && p.audio_format.sample_rate)
        (void)ipc_audio_pts_us(ac.first_pts_us,p.audio_consumed,p.audio_format.sample_rate,&audio_end_us);
    double measured_fps = vc.captured > 1 && vc.last_arrival_us > vc.first_arrival_us ?
        (double)(vc.captured-1)*1000000.0/(double)(vc.last_arrival_us-vc.first_arrival_us) : 0.0;
    ipc_log_write(IPC_LOG_INFO,"record","timing: capture_fps=%.3f audio_end_us=%" PRId64
                  " aac_initial_padding=%u aac_tail_padding=%" PRIu64,
                  measured_fps,audio_end_us,audio.initial_padding,audio.padding_samples);
    current = pthread_sigmask(SIG_SETMASK,&old_mask,NULL);
    if (current) fail_record(&p,"restore signals",-current);
    if (atomic_load(&p.error)) {
        ipc_log_write(IPC_LOG_ERROR,"record","record failed; output may be incomplete: %s",options->stream_only ? "RTMP" : path); return 1;
    }
    if (network.error) {
        ipc_log_write(IPC_LOG_WARN,"record","MP4 finalized; RTMP failed (exit=3): %s",path); return 3;
    }
    if (options->stream_only) {
        ipc_log_write(IPC_LOG_INFO,"record","stream complete; both streams drained"); return interrupted ? 130 : 0;
    }
    ipc_log_write(IPC_LOG_INFO,"record","%s; both streams drained and MP4 finalized",interrupted ? "record interrupted" : "record complete");
    return interrupted ? 130 : 0;
}
#else
/** @brief 无 FFmpeg 构建明确拒绝录像，保留其他独立调试模式。 */
int ipc_record_pipeline_run(const IpcConfig *c,const IpcRecordOptions *o)
{(void)c;(void)o;ipc_log_write(IPC_LOG_ERROR,"record","record requires FFmpeg support");return 1;}
#endif
