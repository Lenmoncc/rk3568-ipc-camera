/** @file packet_queue.h
 * @brief 有界编码包 FIFO；入队转移描述对象所有权，音视频分别使用独立实例。
 */
#ifndef IPC_PACKET_QUEUE_H
#define IPC_PACKET_QUEUE_H
#include "media_types.h"
typedef struct IpcPacketQueue IpcPacketQueue;
/** @brief 创建容量大于零的队列；*queue 必须为 NULL，失败不改变输出。 */
int ipc_packet_queue_create(IpcPacketQueue **queue, size_t capacity);
/** @brief 尝试入队；0 转移所有权，-EAGAIN 满，-EPIPE 已关闭；失败不接管包。 */
int ipc_packet_queue_try_push(IpcPacketQueue *queue, IpcEncodedPacket *packet);
/** @brief 等待出队；1 转移给调用者，0 关闭且空，负值错误；*packet 必须为 NULL。 */
int ipc_packet_queue_pop(IpcPacketQueue *queue, IpcEncodedPacket **packet);
/** @brief 限时出队，超时返回 -EAGAIN；0ms 只检查，-1 无限等待，其余范围 1..1000ms。 */
int ipc_packet_queue_pop_timed(IpcPacketQueue *queue, IpcEncodedPacket **packet, int timeout_ms);
/** @brief 幂等关闭并唤醒全部消费者；保留剩余包供排空，NULL 安全。 */
void ipc_packet_queue_close(IpcPacketQueue *queue);
/** @brief 使用者全部 join 后释放残留包和队列并置空；不能与其他接口并发。 */
void ipc_packet_queue_destroy(IpcPacketQueue **queue);
/** @brief 创建独立 AVPacket 引用和描述；元数据独立，payload 只读共享；失败保持 *dest=NULL。 */
int ipc_encoded_packet_ref(IpcEncodedPacket **dest, const IpcEncodedPacket *source);
/** @brief 释放描述及其 AVPacket 引用并置空；NULL/重复清理安全。 */
void ipc_encoded_packet_free(IpcEncodedPacket **packet);
#endif
