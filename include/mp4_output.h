/** @file mp4_output.h
 * @brief 单线程 MP4 封装器；复制编码参数/输入包，独占新建本地文件。
 */
#ifndef IPC_MP4_OUTPUT_H
#define IPC_MP4_OUTPUT_H
#include <stdbool.h>
#include "media_types.h"
typedef struct IpcMp4Output IpcMp4Output;
typedef struct {
    uint64_t video_packets, audio_packets;
    int64_t first_video_us, last_video_us, first_audio_us, last_audio_us;
    bool trailer_written;
} IpcMp4Stats;
/** @brief 创建新 MP4 并写头；路径不得存在，失败保留可能产生的残片供排查。 */
int ipc_mp4_output_init(IpcMp4Output **context, const char *path,
                        const IpcStreamParams *video, const IpcStreamParams *audio, unsigned int fps);
/** @brief 借用输入包并复制引用，按输出流时间基重标定后交错写入；失败后不得继续写。 */
int ipc_mp4_output_write(IpcMp4Output *context, const IpcEncodedPacket *packet);
/** @brief 写 trailer、刷新、关闭文件；缺少任一轨数据时返回错误，不宣称完整双轨录像。 */
int ipc_mp4_output_finish(IpcMp4Output *context);
/** @brief 复制统计，须在写入线程停止后调用。 */
int ipc_mp4_output_get_stats(const IpcMp4Output *context, IpcMp4Stats *stats);
/** @brief 释放部分或完整上下文并置空；不自动写 trailer，返回首个关闭错误。 */
int ipc_mp4_output_deinit(IpcMp4Output **context);
#endif
