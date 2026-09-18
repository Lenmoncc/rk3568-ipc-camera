/**
 * @file video_pipeline.c
 * @brief 正式工程阶段链路：V4L2 -> 有界队列 -> 检查/可选 NV12 保存/MPP 编码 -> 释放。
 *
 * 采集线程独占设备 read 和生产统计，消费线程独占文件写入和消费统计。
 * 管理线程只通过 C11 原子变量请求停止/观察结束，在 join 后读取普通统计。
 * 数据队列与设备模块均为正式 src 实现，demo 始终独立。
 */
#define _POSIX_C_SOURCE 200809L
#include "video_pipeline.h"
#include "v4l2_capture.h"
#include "video_encoder.h"
#include "frame_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define READ_POLL_MS 250
#define NO_VALID_FRAME_US INT64_C(3000000)

typedef struct {
    IpcVideoCapture *capture;
    IpcFrameQueue *queue;
    IpcVideoFormat format;
    IpcVideoRunOptions options;
    FILE *dump, *h264;
    IpcVideoEncoder *encoder;
    atomic_bool stop, producer_done, consumer_done;
    uint64_t captured, enqueued, dropped_full, dropped_stop;
    uint64_t consumed, saved;
    int producer_error, consumer_error;
} VideoPipeline;

/** @brief 采集线程：有限等待、复制后入队；满时释放新帧，退出时关闭队列。 */
static void *capture_worker(void *argument)
{
    VideoPipeline *pipeline = argument;
    int64_t last_valid = ipc_monotonic_us();
    if (last_valid < 0) pipeline->producer_error = -EIO;
    while (pipeline->producer_error == 0 && !atomic_load(&pipeline->stop) &&
           (pipeline->options.frames == 0 || pipeline->captured < pipeline->options.frames)) {
        IpcRawFrame *frame = NULL;
        int result = ipc_video_capture_read(pipeline->capture, &frame, READ_POLL_MS);
        int64_t now = ipc_monotonic_us();
        if (now < 0) {
            ipc_raw_frame_free(&frame);
            pipeline->producer_error = -EIO;
            break;
        }
        if (result == 0 || result == -EBADMSG) {
            /* 损坏帧不能重置这个计时器，否则连续坏帧会让有限帧测试永久运行。 */
            if (now - last_valid >= NO_VALID_FRAME_US) {
                pipeline->producer_error = -ETIMEDOUT;
                ipc_log_write(IPC_LOG_ERROR, "capture", "no valid frame for 3 seconds");
            }
            continue;
        }
        if (result < 0) { pipeline->producer_error = result; break; }
        ++pipeline->captured;
        last_valid = now;
        if (atomic_load(&pipeline->stop)) {
            ++pipeline->dropped_stop;
            ipc_raw_frame_free(&frame);
            break;
        }
        result = ipc_frame_queue_try_push(pipeline->queue, frame);
        if (result == 0) {
            frame = NULL; /* 从这里开始帧可能已被消费端释放，不能再读取它。 */
            ++pipeline->enqueued;
        } else {
            if (result == -EAGAIN) ++pipeline->dropped_full;
            else { ++pipeline->dropped_stop; pipeline->producer_error = result; }
            ipc_raw_frame_free(&frame);
        }
    }
    /* 本线程已结束所有 DQBUF，先停设备，再让消费者排空应用自有副本。 */
    {
        int stopped = ipc_video_capture_stop(pipeline->capture);
        if (pipeline->producer_error == 0 && stopped < 0) pipeline->producer_error = stopped;
    }
    if (pipeline->producer_error != 0)
        ipc_log_write(IPC_LOG_ERROR, "capture", "worker stopped: %s", strerror(-pipeline->producer_error));
    /* 唯一生产者关闭队列，消费者取完已入队帧后收到 EOF。 */
    ipc_frame_queue_close(pipeline->queue);
    atomic_store(&pipeline->producer_done, true);
    return NULL;
}

/** @brief 检查消费端帧的类型、布局、内存边界及递增 PTS。 */
static int validate_frame(const VideoPipeline *pipeline, const IpcRawFrame *frame, int64_t last_pts)
{
    const IpcVideoFormat *format = &pipeline->format;
    if (frame == NULL || frame->data == NULL || frame->type != IPC_MEDIA_VIDEO ||
        frame->size != format->frame_size || frame->info.video.fourcc != V4L2_PIX_FMT_NV12 ||
        frame->info.video.width != format->width || frame->info.video.height != format->height ||
        frame->info.video.y_stride != format->stride || frame->info.video.uv_stride != format->stride ||
        frame->info.video.uv_offset != format->uv_offset || frame->pts_us < 0 || frame->pts_us <= last_pts)
        return -EBADMSG;
    return 0;
}

/** @brief 按 Y、UV 行去掉步长填充，写出可按 1280x720 直接播放的紧凑 NV12。 */
static int save_frame(FILE *output, const IpcRawFrame *frame)
{
    for (size_t row = 0; row < frame->info.video.height; ++row) {
        if (fwrite(frame->data + row * frame->info.video.y_stride, 1,
                   frame->info.video.width, output) != frame->info.video.width)
            return errno ? -errno : -EIO;
    }
    for (size_t row = 0; row < frame->info.video.height / 2; ++row) {
        if (fwrite(frame->data + frame->info.video.uv_offset + row * frame->info.video.uv_stride,
                   1, frame->info.video.width, output) != frame->info.video.width)
            return errno ? -errno : -EIO;
    }
    return 0;
}

/** @brief 仅用于积压测试的人为消费延时；默认不开启，不改变视频时间戳。 */
static int delay_consumer(unsigned int milliseconds)
{
    struct timespec delay = {.tv_sec = milliseconds / 1000,
                            .tv_nsec = (long)(milliseconds % 1000) * 1000000};
    while (nanosleep(&delay, &delay) < 0)
        if (errno != EINTR) return -errno;
    return 0;
}

/** @brief 消费线程：检查/保存原始帧、编码输出，失败后排空释放，正常结束发送 EOS。 */
static void *consume_worker(void *argument)
{
    VideoPipeline *pipeline = argument;
    IpcRawFrame *frame = NULL;
    int64_t last_pts = -1;
    int result;
    while ((result = ipc_frame_queue_pop(pipeline->queue, &frame)) == 1) {
        ++pipeline->consumed;
        if (pipeline->consumer_error == 0) {
            result = validate_frame(pipeline, frame, last_pts);
            if (result == 0 && pipeline->dump != NULL && pipeline->saved < pipeline->options.dump_frames) {
                result = save_frame(pipeline->dump, frame);
                if (result == 0) ++pipeline->saved;
            }
            if (result == 0 && pipeline->encoder != NULL)
                result = ipc_video_encoder_send(pipeline->encoder, frame);
            if (result == 0 && pipeline->options.consumer_delay_ms != 0)
                result = delay_consumer(pipeline->options.consumer_delay_ms);
            if (result < 0) {
                pipeline->consumer_error = result;
                atomic_store(&pipeline->stop, true);
                ipc_log_write(IPC_LOG_ERROR, "consumer", "frame/encode/write failed: %s", strerror(-result));
            }
            last_pts = frame->pts_us;
            if (pipeline->consumed == 1 || pipeline->consumed % 100 == 0)
                ipc_log_write(IPC_LOG_INFO, "consumer", "consumed=%" PRIu64 " sequence=%u pts_us=%" PRId64,
                              pipeline->consumed, frame->info.video.sequence, frame->pts_us);
        }
        ipc_raw_frame_free(&frame);
    }
    if (result < 0) { pipeline->consumer_error = result; atomic_store(&pipeline->stop, true); }
    /* 队列先排空，再发不含图像的 EOS；编码或写入失败则不继续向坏状态送数据。 */
    if (pipeline->encoder != NULL && pipeline->consumer_error == 0) {
        result = ipc_video_encoder_finish(pipeline->encoder);
        if (result < 0) {
            pipeline->consumer_error = result;
            atomic_store(&pipeline->stop, true);
            ipc_log_write(IPC_LOG_ERROR, "encoder", "EOS drain failed: %s", strerror(-result));
        }
    }
    atomic_store(&pipeline->consumer_done, true);
    return NULL;
}

/** @brief 独占创建输出文件；两个输出均采用同一策略，永不覆盖已有内容。 */
static int open_output(const char *path, FILE **output)
{
    int descriptor;
    if (path == NULL) return 0;
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (descriptor < 0) return -errno;
    *output = fdopen(descriptor, "wb");
    if (*output == NULL) {
        int error = errno;
        close(descriptor);
        return -error;
    }
    return 0;
}

/** @brief 在 MPP 包有效期间写入 Annex B 数据；空 EOS 只通知结束，不写入伪数据。 */
static int save_h264_packet(void *opaque, const IpcH264Packet *packet)
{
    VideoPipeline *pipeline = opaque;
    if (packet->size != 0 && fwrite(packet->data, 1, packet->size, pipeline->h264) != packet->size)
        return errno ? -errno : -EIO;
    return 0;
}

/** @brief 在所有线程 join 后输出可核对的数量、丢帧原因及实测到达帧率。 */
static void report_statistics(VideoPipeline *pipeline, const IpcVideoCaptureStats *stats)
{
    double fps = 0.0;
    if (stats->captured > 1 && stats->last_arrival_us > stats->first_arrival_us)
        fps = (double)(stats->captured - 1) * 1000000.0 /
              (double)(stats->last_arrival_us - stats->first_arrival_us);
    ipc_log_write(IPC_LOG_INFO, "capture", "summary: dequeued=%" PRIu64 " captured=%" PRIu64
                  " enqueued=%" PRIu64 " dropped_full=%" PRIu64 " dropped_stop=%" PRIu64
                  " consumed=%" PRIu64 " saved=%" PRIu64,
                  stats->dequeued, stats->captured, pipeline->enqueued, pipeline->dropped_full,
                  pipeline->dropped_stop, pipeline->consumed, pipeline->saved);
    ipc_log_write(IPC_LOG_INFO, "capture", "invalid=%" PRIu64 " timestamp_rejected=%" PRIu64
                  " sequence_gaps=%" PRIu64 " sequence_resets=%" PRIu64
                  " timestamp_fallback=%" PRIu64 " poll_timeouts=%" PRIu64 " eagain=%" PRIu64
                  " capture_fps=%.3f",
                  stats->invalid, stats->timestamp_rejected, stats->sequence_gaps,
                  stats->sequence_resets, stats->timestamp_fallback, stats->poll_timeouts,
                  stats->would_block, fps);
    if (pipeline->dump != NULL)
        ipc_log_write(IPC_LOG_INFO, "capture", "dump=%s packed_NV12=%ux%u frames=%" PRIu64
                      " complete_frame_bytes=%zu", pipeline->options.dump_path,
                      pipeline->format.width, pipeline->format.height, pipeline->saved,
                      (size_t)pipeline->format.width * pipeline->format.height * 3 / 2);
}

/** @brief 运行正式工程的阶段采集模式，统一管理初始化、信号、线程与清理错误。 */
int ipc_video_pipeline_run(const IpcConfig *config, const IpcVideoRunOptions *options)
{
    VideoPipeline pipeline = {0};
    IpcVideoCaptureStats stats = {0};
    IpcVideoEncoderStats encoded = {0};
    pthread_t producer, consumer;
    bool producer_started = false, consumer_started = false, interrupted = false;
    sigset_t signals, old_mask;
    int result = 0, current;
    int64_t epoch;
    if (config == NULL || options == NULL || options->consumer_delay_ms > 1000 ||
        (options->dump_path != NULL && options->dump_frames == 0) || options->encode_fps > 30) return 1;
    pipeline.options = *options;
    atomic_init(&pipeline.stop, false);
    atomic_init(&pipeline.producer_done, false);
    atomic_init(&pipeline.consumer_done, false);
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    /* 工作线程继承屏蔽状态；只由主线程 sigtimedwait 接收信号。
     * 不在信号处理函数中调用日志、锁、队列或设备清理接口。 */
    current = pthread_sigmask(SIG_BLOCK, &signals, &old_mask);
    if (current != 0) return 1;
    result = ipc_frame_queue_create(&pipeline.queue, config->video_raw_capacity);
    if (result < 0) goto cleanup;
    result = ipc_video_capture_init(&pipeline.capture, config);
    if (result < 0) goto cleanup;
    result = ipc_video_capture_get_format(pipeline.capture, &pipeline.format);
    if (result < 0) goto cleanup;
    result = open_output(options->dump_path, &pipeline.dump);
    if (result < 0) goto cleanup;
    if (options->h264_path != NULL) {
        result = open_output(options->h264_path, &pipeline.h264);
        if (result < 0) goto cleanup;
        result = ipc_video_encoder_init(&pipeline.encoder, config,
                    options->encode_fps ? options->encode_fps : config->video_fps, save_h264_packet, &pipeline);
        if (result < 0) goto cleanup;
    }
    epoch = ipc_monotonic_us();
    if (epoch < 0) { result = -EIO; goto cleanup; }
    result = ipc_video_capture_start(pipeline.capture, epoch);
    if (result < 0) goto cleanup;
    current = pthread_create(&consumer, NULL, consume_worker, &pipeline);
    if (current != 0) { result = -current; goto cleanup; }
    consumer_started = true;
    current = pthread_create(&producer, NULL, capture_worker, &pipeline);
    if (current != 0) { result = -current; goto cleanup; }
    producer_started = true;
    while (!atomic_load(&pipeline.producer_done) || !atomic_load(&pipeline.consumer_done)) {
        const struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};
        current = sigtimedwait(&signals, NULL, &timeout);
        if (current == SIGINT || current == SIGTERM) {
            interrupted = true;
            atomic_store(&pipeline.stop, true);
        } else if (current < 0 && errno != EAGAIN && errno != EINTR) {
            result = -errno;
            atomic_store(&pipeline.stop, true);
        }
    }
cleanup:
    atomic_store(&pipeline.stop, true);
    /* 生产者可能正在 poll，有限等待后退出并关闭队列。若未启动生产者，
     * 由管理线程关闭队列，避免先启动的消费者永久等待。 */
    if (producer_started) pthread_join(producer, NULL);
    else ipc_frame_queue_close(pipeline.queue);
    if (consumer_started) pthread_join(consumer, NULL);
    if (result == 0 && pipeline.producer_error != 0) result = pipeline.producer_error;
    if (result == 0 && pipeline.consumer_error != 0) result = pipeline.consumer_error;
    if (pipeline.capture != NULL) {
        ipc_video_capture_get_stats(pipeline.capture, &stats);
        report_statistics(&pipeline, &stats);
        if (result == 0 && (pipeline.enqueued != pipeline.consumed ||
            stats.captured != pipeline.enqueued + pipeline.dropped_full + pipeline.dropped_stop))
            result = -EIO;
    }
    if (pipeline.dump != NULL && fclose(pipeline.dump) != 0 && result == 0)
        result = errno ? -errno : -EIO; /* fclose 可能才报告延迟写入错误，必须检查。 */
    if (pipeline.h264 != NULL && fclose(pipeline.h264) != 0 && result == 0)
        result = errno ? -errno : -EIO;
    if (pipeline.encoder != NULL) {
        ipc_video_encoder_get_stats(pipeline.encoder, &encoded);
        ipc_log_write(IPC_LOG_INFO, "encoder", "summary: submitted=%" PRIu64 " encoded=%" PRIu64
                      " packets=%" PRIu64 " bytes=%" PRIu64 " keyframes=%" PRIu64 " eos=%u fps_config=%u"
                      " first_pts_us=%" PRId64 " last_pts_us=%" PRId64 " output=%s",
                      encoded.submitted, encoded.encoded, encoded.packets, encoded.bytes, encoded.keyframes,
                      encoded.eos ? 1U : 0U, encoded.fps, encoded.first_pts_us, encoded.last_pts_us, options->h264_path);
        if (result == 0 && (encoded.submitted != pipeline.consumed || encoded.encoded != encoded.submitted || !encoded.eos))
            result = -EIO;
        if (stats.captured > 1 && stats.last_arrival_us > stats.first_arrival_us) {
            double actual = (double)(stats.captured - 1) * 1000000.0 / (double)(stats.last_arrival_us - stats.first_arrival_us);
            if (actual < encoded.fps * 0.95 || actual > encoded.fps * 1.05)
                ipc_log_write(IPC_LOG_WARN, "encoder", "capture_fps=%.3f differs from encoder fps=%u; set --encode-fps to measured input rate", actual, encoded.fps);
        }
        if (pipeline.dropped_full != 0)
            ipc_log_write(IPC_LOG_WARN, "encoder", "raw frames dropped before encoding; elementary H.264 cannot preserve capture timestamp gaps");
    }
    current = ipc_video_encoder_deinit(&pipeline.encoder);
    if (result == 0) result = current;
    current = ipc_video_capture_deinit(&pipeline.capture);
    if (result == 0) result = current;
    ipc_frame_queue_destroy(&pipeline.queue);
    current = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    if (result == 0 && current != 0) result = -current;
    if (result < 0) {
        ipc_log_write(IPC_LOG_ERROR, "capture", "capture pipeline failed: %s", strerror(-result));
        return 1;
    }
    ipc_log_write(IPC_LOG_INFO, "capture", "%s; queue drained and resources released",
                  interrupted ? "capture interrupted" : "capture complete");
    return interrupted ? 130 : 0;
}
