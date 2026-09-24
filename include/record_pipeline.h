/** @file record_pipeline.h
 * @brief 音视频并行录像入口；不通过外部进程采集或编码。
 */
#ifndef IPC_RECORD_PIPELINE_H
#define IPC_RECORD_PIPELINE_H
#include "config.h"
typedef struct {
    const char *mp4_path; /**< NULL 使用 config.record_path；独占新建，不自动创建父目录。 */
    unsigned int seconds; /**< 共同时间起点后的录制时长，默认 30，0 直到信号停止。 */
    unsigned int encode_fps; /**< 0 使用 config.video_fps；当前板端建议显式传 25。 */
    unsigned int output_delay_ms; /**< 仅故障验证，人为放慢输出，0..1000ms。 */
} IpcRecordOptions;
/** @brief 启动五个工作线程，共用时间轴并按依赖排空；主线程无其他业务线程时调用。
 * @return 0 正常录像完成，130 信号停止且收尾成功，1 任一阶段失败。
 */
int ipc_record_pipeline_run(const IpcConfig *config, const IpcRecordOptions *options);
#endif
