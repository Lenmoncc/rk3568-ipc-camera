/* packet_queue.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_PACKET_QUEUE_H
#define IPC_PACKET_QUEUE_H

#include "media_types.h"

typedef struct IpcPacketQueue IpcPacketQueue;

/* 接口约定（待实现）：
 * create 成功后队列归调用方管理；capacity 必须大于 0。
 * try_push 不无限等待：0 表示接管元素，负值表示失败且所有权仍归调用方。
 * pop 等待数据：1 表示取出元素且所有权交给消费者，0 表示关闭且已空，
 * 负值表示错误。关闭后仍可取出已入队的数据，以支持正常排空。
 * close 唤醒等待者并禁止新入队；destroy 仅在使用者全部退出后调用，
 * 同时释放残留元素。具体错误码与同步实现将在队列实现阶段补齐。
 */
int ipc_packet_queue_create(IpcPacketQueue **queue, size_t capacity);
int ipc_packet_queue_try_push(IpcPacketQueue *queue, IpcEncodedPacket *packet);
int ipc_packet_queue_pop(IpcPacketQueue *queue, IpcEncodedPacket **packet);
void ipc_packet_queue_close(IpcPacketQueue *queue);
void ipc_packet_queue_destroy(IpcPacketQueue **queue);
/* 为两路输出创建独立包引用；失败时 source 的所有权不变。 */
int ipc_encoded_packet_ref(IpcEncodedPacket **dest, const IpcEncodedPacket *source);
void ipc_encoded_packet_free(IpcEncodedPacket **packet);

#endif /* IPC_PACKET_QUEUE_H */
