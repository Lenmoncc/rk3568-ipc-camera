/** @file alsa_capture.h
 * @brief 非阻塞 ALSA PCM 采集；交付应用独占的 S16_LE 交错音频块。
 * 设备句柄由采集线程独占；主线程只在启动前/线程 join 后访问。
 */
#ifndef IPC_ALSA_CAPTURE_H
#define IPC_ALSA_CAPTURE_H
#include "config.h"
#include "media_types.h"

typedef struct IpcAudioCapture IpcAudioCapture;
typedef struct {
    unsigned int sample_rate, channels;
    unsigned int period_frames, buffer_frames; /**< ALSA frame = 每声道各一个样本。 */
} IpcAudioCaptureFormat;
typedef struct {
    uint64_t blocks, samples, short_reads, would_block, wait_timeouts;
    uint64_t xruns, suspends; /**< 时间线可能中断时停止采集，不静默重置计数。 */
    int64_t first_pts_us, last_pts_us;
} IpcAudioCaptureStats;

/** @brief 返回本二进制是否链接正式 ALSA 支持。 */
bool ipc_audio_capture_available(void);
/** @brief 打开并严格核对参数；失败回滚，*context 必须为 NULL。返回 0 或负 errno。 */
int ipc_audio_capture_init(IpcAudioCapture **context, const IpcConfig *config);
/** @brief 读取协商后的实际采样参数，调用方接收独立副本。 */
int ipc_audio_capture_get_format(const IpcAudioCapture *context, IpcAudioCaptureFormat *format);
/** @brief 显式启动录音，以启动前的单调时刻估计首样本偏移；不等同硬件精确时间戳。 */
int ipc_audio_capture_start(IpcAudioCapture *context, int64_t epoch_us);
/** @brief 有限等待并读取不超过 max_samples 个每声道样本；1 成功，0 暂无数据，负值失败。
 * @param frame 必须指向 NULL；成功后由调用者或接收队列负责 ipc_raw_frame_free。
 * @note max_samples 必须大于 0，timeout_ms 为 0..1000。短读直接返回有效部分。
 *       XRUN/挂起返回错误，不自动恢复并伪造连续采样时间线。
 */
int ipc_audio_capture_read(IpcAudioCapture *context, IpcRawFrame **frame,
                           unsigned int max_samples, int timeout_ms);
/** @brief 停止硬件采集并丢弃驱动尚未读取的样本；不影响应用已入队副本。 */
int ipc_audio_capture_stop(IpcAudioCapture *context);
/** @brief 在线程停止后复制统计，避免与采集线程数据竞争。 */
int ipc_audio_capture_get_stats(const IpcAudioCapture *context, IpcAudioCaptureStats *stats);
/** @brief 尽力停设备并关闭句柄，清空指针；NULL 安全，返回首个清理错误。 */
int ipc_audio_capture_deinit(IpcAudioCapture **context);
#endif
