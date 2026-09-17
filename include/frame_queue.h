/* frame_queue.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_FRAME_QUEUE_H
#define IPC_FRAME_QUEUE_H

#include "media_types.h"

typedef struct IpcFrameQueue IpcFrameQueue;

/* 接口约定（待实现）：
 * create 成功后队列归调用方管理；capacity 必须大于 0。
 * try_push 不无限等待：0 表示接管元素，负值表示失败且所有权仍归调用方。
 * pop 等待数据：1 表示取出元素且所有权交给消费者，0 表示关闭且已空，
 * 负值表示错误。关闭后仍可取出已入队的数据，以支持正常排空。
 * close 唤醒等待者并禁止新入队；destroy 仅在使用者全部退出后调用，
 * 同时释放残留元素。具体错误码与同步实现将在队列实现阶段补齐。
 */
int ipc_frame_queue_create(IpcFrameQueue **queue, size_t capacity);
int ipc_frame_queue_try_push(IpcFrameQueue *queue, IpcRawFrame *frame);
int ipc_frame_queue_pop(IpcFrameQueue *queue, IpcRawFrame **frame);
void ipc_frame_queue_close(IpcFrameQueue *queue);
void ipc_frame_queue_destroy(IpcFrameQueue **queue);
/* 释放原始帧及其 data；调用后将 *frame 清空。 */
void ipc_raw_frame_free(IpcRawFrame **frame);

#endif /* IPC_FRAME_QUEUE_H */
