/** @file audio_pipeline.c
 * @brief 正式音频链路：ALSA→独占 PCM 块→有界队列→文件；不依赖 demo。
 * 采集线程独占 ALSA，保存线程独占 FILE；主线程在 join 后汇总普通统计。
 * 音频不采用视频的丢帧策略：队列满即报错停止，已入队数据仍被排空。
 */
#define _POSIX_C_SOURCE 200809L
#include "audio_pipeline.h"
#include "alsa_capture.h"
#include "frame_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    IpcAudioCapture *capture;
    IpcFrameQueue *queue;
    IpcAudioCaptureFormat format;
    IpcAudioRunOptions options;
    FILE *file;
    atomic_bool stop, producer_done, consumer_done;
    uint64_t target_samples, captured, enqueued, consumed, saved, queue_full;
    unsigned int peak[2];
    int producer_error, consumer_error;
    int64_t epoch_us;
} AudioPipeline;

/** @brief 采集并转交独占音频块；短读保留，队列满/设备中断停止，最后关闭队列。 */
static void *audio_producer(void *argument)
{
    AudioPipeline *p = argument;
    int64_t last_valid = ipc_monotonic_us();
    p->producer_error = ipc_audio_capture_start(p->capture, p->epoch_us);
    if (last_valid < 0 && !p->producer_error) p->producer_error = -EIO;
    while (!p->producer_error && !atomic_load(&p->stop) &&
           (!p->target_samples || p->captured < p->target_samples)) {
        IpcRawFrame *frame = NULL;
        unsigned int request = p->format.period_frames;
        int result;
        int64_t now;
        if (p->target_samples && p->target_samples - p->captured < request)
            request = (unsigned int)(p->target_samples - p->captured);
        result = ipc_audio_capture_read(p->capture, &frame, request, 100);
        now = ipc_monotonic_us();
        if (now < 0) { ipc_raw_frame_free(&frame); p->producer_error = -EIO; break; }
        if (result == 0) {
            if (now - last_valid >= INT64_C(3000000)) {
                p->producer_error = -ETIMEDOUT;
                ipc_log_write(IPC_LOG_ERROR, "audio", "no PCM samples for 3 seconds");
            }
            continue;
        }
        if (result < 0) { p->producer_error = result; break; }
        last_valid = now;
        unsigned int samples = frame->info.audio.samples_per_channel;
        p->captured += samples;
        result = ipc_frame_queue_try_push(p->queue, frame);
        if (result == 0) {
            frame = NULL; /* 队列已接管；消费者可能立即释放，禁止继续解引用。 */
            p->enqueued += samples;
        } else {
            if (result == -EAGAIN) {
                ++p->queue_full;
                ipc_log_write(IPC_LOG_ERROR, "audio", "raw queue full; stopping, lost_samples=%u", samples);
            }
            p->producer_error = result;
            ipc_raw_frame_free(&frame);
        }
    }
    {
        int result = ipc_audio_capture_stop(p->capture);
        if (!p->producer_error) p->producer_error = result;
    }
    ipc_frame_queue_close(p->queue);
    atomic_store(&p->producer_done, true);
    return NULL;
}

/** @brief 检查采样布局、连续样本下标和采样计数生成的 PTS；不以块数代替样本数。 */
static int validate_audio(const AudioPipeline *p, const IpcRawFrame *frame, int64_t first_pts)
{
    int64_t expected;
    if (!frame || !frame->data || frame->type != IPC_MEDIA_AUDIO ||
        frame->info.audio.sample_rate != p->format.sample_rate ||
        frame->info.audio.channels != p->format.channels ||
        frame->info.audio.sample_format != IPC_AUDIO_FORMAT_S16_LE ||
        !frame->info.audio.samples_per_channel || frame->info.audio.samples_per_channel > p->format.period_frames ||
        frame->size != (size_t)frame->info.audio.samples_per_channel * p->format.channels * 2 ||
        frame->info.audio.sample_index != p->consumed || frame->pts_us < 0) return -EBADMSG;
    if (first_pts < 0) first_pts = frame->pts_us;
    if (ipc_audio_pts_us(first_pts, p->consumed, p->format.sample_rate, &expected) < 0 || expected != frame->pts_us)
        return -EBADMSG;
    return 0;
}

/** @brief 解读小端有符号样本，累计每声道绝对峰值，帮助发现全零或单侧无信号。 */
static void measure_peaks(AudioPipeline *p, const IpcRawFrame *frame)
{
    for (size_t i = 0; i < frame->size / 2; ++i) {
        unsigned int raw = frame->data[i * 2] | ((unsigned int)frame->data[i * 2 + 1] << 8);
        int value = raw >= 32768 ? (int)raw - 65536 : (int)raw;
        unsigned int magnitude = (unsigned int)(value < 0 ? -value : value);
        unsigned int channel = (unsigned int)(i % p->format.channels);
        if (magnitude > p->peak[channel]) p->peak[channel] = magnitude;
    }
}

/** @brief 消费并同步写 PCM；首次写错后请求停止，但仍释放队列中全部剩余块。 */
static void *audio_consumer(void *argument)
{
    AudioPipeline *p = argument;
    int64_t first_pts = -1;
    for (;;) {
        IpcRawFrame *frame = NULL;
        int result = ipc_frame_queue_pop(p->queue, &frame);
        if (result <= 0) {
            if (result < 0 && !p->consumer_error) p->consumer_error = result;
            break;
        }
        if (!p->consumer_error) {
            result = validate_audio(p, frame, first_pts);
            if (result == 0) {
                if (first_pts < 0) first_pts = frame->pts_us;
                p->consumed += frame->info.audio.samples_per_channel;
                measure_peaks(p, frame);
                errno = 0;
                if (fwrite(frame->data, 1, frame->size, p->file) != frame->size)
                    result = errno ? -errno : -EIO;
                else {
                    p->saved += frame->info.audio.samples_per_channel;
                    if (frame->info.audio.sample_index == 0)
                        ipc_log_write(IPC_LOG_INFO, "audio", "first block: samples=%u pts_us=%" PRId64,
                                      frame->info.audio.samples_per_channel, frame->pts_us);
                }
            }
            if (result < 0) { p->consumer_error = result; atomic_store(&p->stop, true); }
            /* 仅用于模拟慢磁盘；生产者入队不阻塞，故队列满能明确终止。 */
            if (!p->consumer_error && p->options.consumer_delay_ms) {
                struct timespec delay = {.tv_sec = p->options.consumer_delay_ms / 1000,
                                        .tv_nsec = (long)(p->options.consumer_delay_ms % 1000) * 1000000};
                while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
            }
        }
        ipc_raw_frame_free(&frame);
    }
    if (p->consumer_error) atomic_store(&p->stop, true);
    atomic_store(&p->consumer_done, true);
    return NULL;
}

/** @brief 独占创建 PCM 文件；fdopen 失败也关闭 fd，绝不覆盖历史录音。 */
static int open_pcm(const char *path, FILE **file)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    *file = fdopen(fd, "wb");
    if (!*file) { int error = errno; close(fd); return -error; }
    return 0;
}

/** @brief 初始化设备和线程、同步接收停止信号、排空应用队列并核对样本计数。 */
int ipc_audio_pipeline_run(const IpcConfig *config, const IpcAudioRunOptions *options)
{
    AudioPipeline p = {0};
    IpcAudioCaptureStats stats = {0};
    pthread_t producer, consumer;
    bool producer_started = false, consumer_started = false, interrupted = false;
    sigset_t signals, old_mask;
    int result = 0, current;
    if (!config || !options || !options->pcm_path || !*options->pcm_path ||
        options->seconds > 86400 || options->consumer_delay_ms > 1000) return 1;
    p.options = *options;
    atomic_init(&p.stop, false);
    atomic_init(&p.producer_done, false);
    atomic_init(&p.consumer_done, false);
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    current = pthread_sigmask(SIG_BLOCK, &signals, &old_mask);
    if (current) return 1;
    /* 配置失败发生在创建文件之前，避免错误声道数留下空录音。 */
    result = ipc_audio_capture_init(&p.capture, config);
    if (result < 0) goto cleanup;
    result = ipc_audio_capture_get_format(p.capture, &p.format);
    if (result < 0) goto cleanup;
    p.target_samples = (uint64_t)options->seconds * p.format.sample_rate;
    result = ipc_frame_queue_create(&p.queue, config->audio_raw_capacity);
    if (result < 0) goto cleanup;
    result = open_pcm(options->pcm_path, &p.file);
    if (result < 0) goto cleanup;
    p.epoch_us = ipc_monotonic_us();
    if (p.epoch_us < 0) { result = -EIO; goto cleanup; }
    /* 信号在主线程同步处理；工作线程继承屏蔽状态，不安装异步清理处理器。 */
    current = pthread_create(&consumer, NULL, audio_consumer, &p);
    if (current) { result = -current; goto cleanup; }
    consumer_started = true;
    current = pthread_create(&producer, NULL, audio_producer, &p);
    if (current) { result = -current; goto cleanup; }
    producer_started = true;
    while (!atomic_load(&p.producer_done) || !atomic_load(&p.consumer_done)) {
        const struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
        current = sigtimedwait(&signals, NULL, &timeout);
        if (current == SIGINT || current == SIGTERM) {
            interrupted = true; atomic_store(&p.stop, true);
        } else if (current < 0 && errno != EAGAIN && errno != EINTR) {
            result = -errno; atomic_store(&p.stop, true);
        }
    }
cleanup:
    atomic_store(&p.stop, true);
    if (producer_started) pthread_join(producer, NULL);
    else ipc_frame_queue_close(p.queue);
    if (consumer_started) pthread_join(consumer, NULL);
    if (!result) result = p.producer_error;
    if (!result) result = p.consumer_error;
    if (p.file && fclose(p.file) != 0 && !result) result = errno ? -errno : -EIO;
    if (p.capture) {
        ipc_audio_capture_get_stats(p.capture, &stats);
        if (!result && (stats.samples != p.captured || p.captured != p.enqueued || p.enqueued != p.consumed ||
            p.consumed != p.saved || (!interrupted && p.target_samples && p.saved != p.target_samples))) result = -EIO;
    }
    current = ipc_audio_capture_deinit(&p.capture);
    if (!result) result = current;
    ipc_frame_queue_destroy(&p.queue);
    ipc_log_write(IPC_LOG_INFO, "audio", "summary: captured_samples=%" PRIu64 " enqueued_samples=%" PRIu64
                  " consumed_samples=%" PRIu64 " saved_samples=%" PRIu64 " bytes=%" PRIu64
                  " blocks=%" PRIu64 " queue_full=%" PRIu64 " xruns=%" PRIu64 " suspends=%" PRIu64,
                  p.captured, p.enqueued, p.consumed, p.saved, p.saved * p.format.channels * 2,
                  stats.blocks, p.queue_full, stats.xruns, stats.suspends);
    ipc_log_write(IPC_LOG_INFO, "audio", "rate=%u channels=%u short_reads=%" PRIu64 " eagain=%" PRIu64
                  " wait_timeouts=%" PRIu64 " duration=%.3f first_pts_us=%" PRId64 " last_pts_us=%" PRId64
                  " peak_ch0=%u peak_ch1=%u output=%s",
                  p.format.sample_rate, p.format.channels, stats.short_reads, stats.would_block, stats.wait_timeouts,
                  p.format.sample_rate ? (double)p.saved / p.format.sample_rate : 0.0,
                  stats.first_pts_us, stats.last_pts_us, p.peak[0], p.peak[1], options->pcm_path);
    if (p.saved && !p.peak[0] && !p.peak[1])
        ipc_log_write(IPC_LOG_WARN, "audio", "all saved PCM samples are zero; check capture input/mixer/microphone");
    current = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (!result && current) result = -current;
    if (result < 0) {
        ipc_log_write(IPC_LOG_ERROR, "audio", "audio pipeline failed: %s", strerror(-result));
        return 1;
    }
    ipc_log_write(IPC_LOG_INFO, "audio", "%s; queue drained and resources released",
                  interrupted ? "audio interrupted" : "audio complete");
    return interrupted ? 130 : 0;
}
