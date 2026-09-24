/** @file h264_bridge.h
 * @brief 将借用的 MPP Annex B 分片组装为拥有独立存储的 AVPacket。
 */
#ifndef IPC_H264_BRIDGE_H
#define IPC_H264_BRIDGE_H
#include "video_encoder.h"
#include "packet_queue.h"
typedef struct IpcH264Bridge IpcH264Bridge;
/** @brief 创建桥接器，借用输出队列；失败保持 *context=NULL。 */
int ipc_h264_bridge_init(IpcH264Bridge **context, const IpcConfig *config, unsigned int fps, IpcPacketQueue *queue);
/** @brief MPP 同步回调；复制 SPS/PPS 与图像分片，完整图像才生成一个包。 */
int ipc_h264_bridge_sink(void *context, const IpcH264Packet *packet);
/** @brief 借用含 Annex B SPS/PPS 的流参数，须在 MPP 初始化输出头之后调用。 */
int ipc_h264_bridge_get_params(const IpcH264Bridge *context, const IpcStreamParams **params);
/** @brief 确认分片已齐并输出暂存末帧，其时长按配置帧率估计；重复调用安全。 */
int ipc_h264_bridge_finish(IpcH264Bridge *context);
/** @brief 释放头信息、分片和暂存包，不关闭/释放外部队列。 */
void ipc_h264_bridge_deinit(IpcH264Bridge **context);
#endif
