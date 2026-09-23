/** @file audio_pipeline.h
 * @brief ALSA→原始音频队列→PCM/AAC 文件的阶段入口，当前独立于视频运行。
 */
#ifndef IPC_AUDIO_PIPELINE_H
#define IPC_AUDIO_PIPELINE_H
#include "config.h"
typedef struct {
    const char *aac_path; /**< AAC 模式输出路径；与 pcm_path 恰好指定一个。 */
    const char *pcm_path; /**< PCM 模式输出路径；独占创建新文件，拒绝覆盖。 */
    unsigned int seconds; /**< 按每声道样本数限长，默认 10；0 持续录音，最大 86400。 */
    unsigned int consumer_delay_ms; /**< 故障验证用途，默认 0，最大 1000ms。 */
} IpcAudioRunOptions;
/** @brief 运行采集与保存线程，处理信号及排空；只能从尚无业务线程的主线程调用。
 * @return 0 达到样本上限且收尾成功；130 信号停止且收尾成功；1 出错。
 * @note XRUN/挂起/队列满停止并报告错误，保留已写文件供排查。工作线程不更改 mixer。
 */
int ipc_audio_pipeline_run(const IpcConfig *config, const IpcAudioRunOptions *options);
#endif
