/** @file rtmp_output.h
 * @brief 两条独立有界队列、FLV 封装和受监督 RTMP 传输；故障仅关闭本输出。
 */
#ifndef IPC_RTMP_OUTPUT_H
#define IPC_RTMP_OUTPUT_H
#include "config.h"
#include "media_types.h"
typedef struct IpcRtmpOutput IpcRtmpOutput;
typedef struct {
    uint64_t video_accepted, audio_accepted, video_packets, audio_packets, bytes;
    int error; /**< 首个负错误，0 无故障；不代表远端播放器的播放结果。 */
    bool header_written, completed;
} IpcRtmpStats;
/** @brief 创建队列并复制双轨参数，不访问网络；队列容量沿用 config 的每输出容量。 */
int ipc_rtmp_output_init(IpcRtmpOutput **context, const IpcConfig *config,
                        const IpcStreamParams *video, const IpcStreamParams *audio, unsigned int fps);
/** @brief 借用只读编码包并克隆引用入队；队列满/分配失败禁用本输出，不阻塞编码。
 * 同一媒体仅允许一个生产者；失败后的后续调用为空操作。
 */
void ipc_rtmp_output_submit(IpcRtmpOutput *context, const IpcEncodedPacket *packet);
/** @brief 对应编码线程排空后关闭该媒体队列，网络线程继续排空已有包。 */
void ipc_rtmp_output_close(IpcRtmpOutput *context, IpcMediaType type);
/** @brief 采集停止时设置一次全局 3 秒网络排空期限；可与输出线程并发调用。 */
void ipc_rtmp_output_begin_drain(IpcRtmpOutput *context);
/** @brief 请求立刻中止网络输出；可并发调用，负 error 作为首个失败原因。 */
void ipc_rtmp_output_abort(IpcRtmpOutput *context, int error);
/** @brief 网络输出线程入口，arg 为本模块上下文；结束前回收网络子进程和残留包。 */
void *ipc_rtmp_output_thread(void *context);
/** @brief 并发读取网络故障状态，0 无故障；不读取未同步的普通统计。 */
int ipc_rtmp_output_error(const IpcRtmpOutput *context);
/** @brief 复制统计，须在生产者及网络线程全部 join 后调用。 */
int ipc_rtmp_output_get_stats(const IpcRtmpOutput *context, IpcRtmpStats *stats);
/** @brief 所有使用者 join 后释放参数、队列和封装器；NULL 安全。 */
void ipc_rtmp_output_deinit(IpcRtmpOutput **context);
#endif
