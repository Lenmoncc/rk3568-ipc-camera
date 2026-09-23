/** @file alsa_capture.c
 * @brief ALSA 采集实现：严格协商参数、有限等待、独立块所有权与样本时间线。
 * 不调用 arecord 或 demo；正式程序直接链接 SDK libasound。
 */
#define _POSIX_C_SOURCE 200809L
#include "alsa_capture.h"
#include "frame_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifndef IPC_WITH_ALSA
#define IPC_WITH_ALSA 0
#endif
#if IPC_WITH_ALSA
#include <alsa/asoundlib.h>

struct IpcAudioCapture {
    snd_pcm_t *pcm;
    IpcAudioCaptureFormat format;
    IpcAudioCaptureStats stats;
    int64_t offset_us;
    bool started, ever_started;
};

/** @brief 保留 ALSA 的负错误码并输出失败操作；附加时间线中断诊断。 */
static int audio_error(IpcAudioCapture *capture, const char *operation, int result)
{
    if (result == -EPIPE) {
        ++capture->stats.xruns;
        ipc_log_write(IPC_LOG_ERROR, "alsa", "capture overrun: samples lost; stopping instead of hiding a discontinuity");
    } else if (result == -ESTRPIPE) {
        ++capture->stats.suspends;
        ipc_log_write(IPC_LOG_ERROR, "alsa", "capture suspended; sample continuity is unknown");
    }
    ipc_log_write(IPC_LOG_ERROR, "alsa", "%s: %s (%d)", operation, snd_strerror(result), result);
    return result;
}

/** @brief 配置交错 S16_LE，精确设置速率/声道；周期可协商，最终参数必须读回。 */
static int configure_pcm(IpcAudioCapture *capture, const IpcConfig *config)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_uframes_t period = config->audio_sample_rate / 100, buffer;
    unsigned int rate = 0, channels = 0;
    snd_pcm_format_t format;
    snd_pcm_access_t access;
    int result, direction = 0;
    const char *operation = "hw_params_malloc";
#define AUDIO_TRY(label, expression) do { operation = label; result = (expression); if (result < 0) goto done; } while (0)
    AUDIO_TRY("hw_params_malloc", snd_pcm_hw_params_malloc(&hw));
    AUDIO_TRY("hw_params_any", snd_pcm_hw_params_any(capture->pcm, hw));
    AUDIO_TRY("set_access RW_INTERLEAVED", snd_pcm_hw_params_set_access(capture->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    AUDIO_TRY("set_format S16_LE", snd_pcm_hw_params_set_format(capture->pcm, hw, SND_PCM_FORMAT_S16_LE));
    AUDIO_TRY("set_channels (RK809 hw:0,0 requires at least 2)", snd_pcm_hw_params_set_channels(capture->pcm, hw, config->audio_channels));
    AUDIO_TRY("set_rate", snd_pcm_hw_params_set_rate(capture->pcm, hw, config->audio_sample_rate, 0));
    AUDIO_TRY("set_period_size_near", snd_pcm_hw_params_set_period_size_near(capture->pcm, hw, &period, &direction));
    if (!period || period > config->audio_sample_rate) { result = -ERANGE; goto done; }
    buffer = period * 4;
    AUDIO_TRY("set_buffer_size_near", snd_pcm_hw_params_set_buffer_size_near(capture->pcm, hw, &buffer));
    AUDIO_TRY("apply_hw_params", snd_pcm_hw_params(capture->pcm, hw));
    AUDIO_TRY("read_hw_params", snd_pcm_hw_params_current(capture->pcm, hw));
    AUDIO_TRY("get_rate", snd_pcm_hw_params_get_rate(hw, &rate, &direction));
    AUDIO_TRY("get_channels", snd_pcm_hw_params_get_channels(hw, &channels));
    AUDIO_TRY("get_format", snd_pcm_hw_params_get_format(hw, &format));
    AUDIO_TRY("get_access", snd_pcm_hw_params_get_access(hw, &access));
    AUDIO_TRY("get_period_size", snd_pcm_hw_params_get_period_size(hw, &period, &direction));
    AUDIO_TRY("get_buffer_size", snd_pcm_hw_params_get_buffer_size(hw, &buffer));
    operation = "verify actual PCM parameters";
    if (rate != config->audio_sample_rate || channels != config->audio_channels ||
        format != SND_PCM_FORMAT_S16_LE || access != SND_PCM_ACCESS_RW_INTERLEAVED ||
        !period || period > rate || buffer < period * 2 || buffer > rate * 4UL) {
        result = -EINVAL; goto done;
    }
    capture->format = (IpcAudioCaptureFormat){rate, channels, (unsigned int)period, (unsigned int)buffer};
    AUDIO_TRY("sw_params_malloc", snd_pcm_sw_params_malloc(&sw));
    AUDIO_TRY("sw_params_current", snd_pcm_sw_params_current(capture->pcm, sw));
    AUDIO_TRY("set_avail_min", snd_pcm_sw_params_set_avail_min(capture->pcm, sw, period));
    AUDIO_TRY("apply_sw_params", snd_pcm_sw_params(capture->pcm, sw));
    AUDIO_TRY("prepare", snd_pcm_prepare(capture->pcm));
    ipc_log_write(IPC_LOG_INFO, "alsa", "actual: device=%s rate=%u channels=%u format=S16_LE access=RW_INTERLEAVED period_frames=%lu buffer_frames=%lu",
                  config->audio_device, rate, channels, (unsigned long)period, (unsigned long)buffer);
done:
    if (sw) snd_pcm_sw_params_free(sw);
    if (hw) snd_pcm_hw_params_free(hw);
    if (result < 0) audio_error(capture, operation, result);
#undef AUDIO_TRY
    return result;
}
#endif

/** @brief 告知调用者该构建是否具备 ALSA 采集能力。 */
bool ipc_audio_capture_available(void) { return IPC_WITH_ALSA != 0; }

/** @brief 创建采集器；所有失败均回滚已创建的 ALSA 对象和应用内存。 */
int ipc_audio_capture_init(IpcAudioCapture **context, const IpcConfig *config)
{
#if IPC_WITH_ALSA
    IpcAudioCapture *capture;
    int result;
    if (!context || *context || !config || !config->audio_device[0] ||
        config->audio_channels < 1 || config->audio_channels > 2 ||
        config->audio_sample_rate < 8000 || config->audio_sample_rate > 96000 ||
        strcmp(config->audio_sample_format, "S16_LE")) return -EINVAL;
    capture = calloc(1, sizeof(*capture));
    if (!capture) return -ENOMEM;
    capture->stats.first_pts_us = capture->stats.last_pts_us = -1;
    result = snd_pcm_open(&capture->pcm, config->audio_device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (result < 0) audio_error(capture, "open capture", result);
    else result = configure_pcm(capture, config);
    if (result < 0) { ipc_audio_capture_deinit(&capture); return result; }
    *context = capture;
    return 0;
#else
    (void)context; (void)config; return -ENOTSUP;
#endif
}

/** @brief 返回实际采样格式的独立副本。 */
int ipc_audio_capture_get_format(const IpcAudioCapture *capture, IpcAudioCaptureFormat *format)
{
#if IPC_WITH_ALSA
    if (!capture || !format) return -EINVAL;
    *format = capture->format; return 0;
#else
    (void)capture; (void)format; return -ENOTSUP;
#endif
}

/** @brief 启动一次采集会话；首样本时间采用软件启动时刻估计，后续按样本数推进。 */
int ipc_audio_capture_start(IpcAudioCapture *capture, int64_t epoch_us)
{
#if IPC_WITH_ALSA
    int64_t now;
    int result;
    if (!capture || capture->ever_started || epoch_us < 0) return -EINVAL;
    now = ipc_monotonic_us();
    if (now < epoch_us) return -EINVAL;
    result = snd_pcm_start(capture->pcm);
    if (result < 0) return audio_error(capture, "start", result);
    capture->started = capture->ever_started = true;
    capture->offset_us = now - epoch_us;
    ipc_log_write(IPC_LOG_INFO, "alsa", "timestamp source: software start estimate + sample count; start_offset_us=%" PRId64, capture->offset_us);
    return 0;
#else
    (void)capture; (void)epoch_us; return -ENOTSUP;
#endif
}

/** @brief 先非阻塞读取，暂无数据才有限等待；返回独立内存中的完整 PCM 样本帧。 */
int ipc_audio_capture_read(IpcAudioCapture *capture, IpcRawFrame **output, unsigned int max_samples, int timeout_ms)
{
#if IPC_WITH_ALSA
    IpcRawFrame *frame;
    snd_pcm_sframes_t read_count;
    unsigned int requested;
    int result;
    if (!capture || !capture->started || !output || *output || !max_samples || timeout_ms < 0 || timeout_ms > 1000) return -EINVAL;
    requested = max_samples < capture->format.period_frames ? max_samples : capture->format.period_frames;
    frame = calloc(1, sizeof(*frame));
    if (!frame) return -ENOMEM;
    frame->size = (size_t)requested * capture->format.channels * 2;
    frame->data = malloc(frame->size);
    if (!frame->data) { ipc_raw_frame_free(&frame); return -ENOMEM; }
    read_count = snd_pcm_readi(capture->pcm, frame->data, requested);
    if (read_count == -EAGAIN || read_count == 0) {
        ++capture->stats.would_block;
        result = snd_pcm_wait(capture->pcm, timeout_ms);
        if (result == 0) ++capture->stats.wait_timeouts;
        else if (result < 0 && result != -EINTR) audio_error(capture, "wait", result);
        ipc_raw_frame_free(&frame);
        return result < 0 && result != -EINTR ? result : 0;
    }
    if (read_count < 0) {
        ipc_raw_frame_free(&frame);
        return read_count == -EINTR ? 0 : audio_error(capture, "readi", (int)read_count);
    }
    if ((uint64_t)read_count > requested || capture->stats.samples > UINT64_MAX - (uint64_t)read_count) {
        ipc_raw_frame_free(&frame); return -EOVERFLOW;
    }
    if ((unsigned int)read_count < requested) ++capture->stats.short_reads;
    frame->type = IPC_MEDIA_AUDIO;
    frame->size = (size_t)read_count * capture->format.channels * 2;
    frame->info.audio.sample_rate = capture->format.sample_rate;
    frame->info.audio.channels = capture->format.channels;
    frame->info.audio.sample_format = IPC_AUDIO_FORMAT_S16_LE;
    frame->info.audio.samples_per_channel = (unsigned int)read_count;
    frame->info.audio.sample_index = capture->stats.samples;
    result = ipc_audio_pts_us(capture->offset_us, capture->stats.samples, capture->format.sample_rate, &frame->pts_us);
    if (result < 0) { ipc_raw_frame_free(&frame); return result; }
    if (!capture->stats.blocks) capture->stats.first_pts_us = frame->pts_us;
    capture->stats.last_pts_us = frame->pts_us;
    ++capture->stats.blocks;
    capture->stats.samples += (uint64_t)read_count;
    *output = frame;
    return 1;
#else
    (void)capture; (void)output; (void)max_samples; (void)timeout_ms; return -ENOTSUP;
#endif
}

/** @brief 停止采集，应用队列的数据仍保留；同一会话不能停止后重新启动。 */
int ipc_audio_capture_stop(IpcAudioCapture *capture)
{
#if IPC_WITH_ALSA
    int result;
    if (!capture || !capture->started) return 0;
    capture->started = false;
    result = snd_pcm_drop(capture->pcm);
    return result < 0 ? audio_error(capture, "drop", result) : 0;
#else
    (void)capture; return 0;
#endif
}

/** @brief 在单线程访问或 join 后取得最终统计。 */
int ipc_audio_capture_get_stats(const IpcAudioCapture *capture, IpcAudioCaptureStats *stats)
{
#if IPC_WITH_ALSA
    if (!capture || !stats) return -EINVAL;
    *stats = capture->stats; return 0;
#else
    (void)capture; (void)stats; return -ENOTSUP;
#endif
}

/** @brief 尽力关闭设备并释放句柄，失败时仍清空应用指针，避免重复释放。 */
int ipc_audio_capture_deinit(IpcAudioCapture **context)
{
#if IPC_WITH_ALSA
    int result, closed;
    IpcAudioCapture *capture;
    if (!context || !*context) return 0;
    capture = *context;
    result = ipc_audio_capture_stop(capture);
    if (capture->pcm) {
        closed = snd_pcm_close(capture->pcm);
        if (closed < 0) { audio_error(capture, "close", closed); if (!result) result = closed; }
    }
    free(capture); *context = NULL; return result;
#else
    (void)context; return 0;
#endif
}
