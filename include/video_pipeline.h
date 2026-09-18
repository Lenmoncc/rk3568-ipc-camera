/** @file video_pipeline.h
 * @brief 视频采集与原始队列的阶段验证入口，无编码、显示和网络依赖。
 */
#ifndef IPC_VIDEO_PIPELINE_H
#define IPC_VIDEO_PIPELINE_H
#include "config.h"

typedef struct {
    unsigned int frames;          /**< 有效采集帧数上限，0 为持续采集；默认 300。 */
    const char *dump_path;         /**< 可选紧凑 NV12 文件，NULL 表示不保存。 */
    unsigned int dump_frames;     /**< 最多保存前多少个消费帧；路径有效时须大于零。 */
    unsigned int consumer_delay_ms; /**< 测试积压的人工延时，默认 0，上限 1000ms。 */
} IpcVideoRunOptions;

/** @brief 创建采集与消费线程，处理信号、排空队列并输出统计。
 * @return 0 达到采集上限且清理成功；130 被 SIGINT/SIGTERM 正常中断；1 运行错误。
 * @pre 从尚未创建其他业务线程的主线程调用，config 已通过静态校验。
 * @note 临时屏蔽并同步接收 SIGINT/SIGTERM，收尾后恢复原信号掩码。
 *       dump 文件以独占创建方式打开，已有文件不会被覆盖。
 */
int ipc_video_pipeline_run(const IpcConfig *config, const IpcVideoRunOptions *options);
#endif
