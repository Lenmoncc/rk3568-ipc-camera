/**
 * @file frame_queue.h
 * @brief 应用自有原始帧的有界 FIFO；视频和音频分别创建实例。
 *
 * 多生产者/多消费者可并发 push、try_push、pop 和 close；create/destroy
 * 由管理线程串行调用。接口返回负 errno 值，不依赖全局 errno。
 * try_push 只是不等待空槽，仍会短暂等待互斥锁，并非无锁接口。
 *
 * 所有权：生产者 --入队成功--> 队列 --出队成功--> 消费者。
 * 入队失败时帧仍归生产者；同一帧不能重复入队或放入两个队列。
 * 不支持 pthread_cancel；通过 close 唤醒工作线程，再 join、destroy。
 * 所有接口均不可在异步信号处理函数中调用。
 */
#ifndef IPC_FRAME_QUEUE_H
#define IPC_FRAME_QUEUE_H

#include "media_types.h"

typedef struct IpcFrameQueue IpcFrameQueue;

/**
 * @brief 创建固定容量队列，成功后交由调用者管理。
 * @param queue 输出句柄地址；必须非 NULL 且 *queue == NULL，防止覆盖旧句柄。
 * @param capacity 可容纳的帧/音频块数量，必须大于零；不是字节数。
 * @return 0 成功；-EINVAL 参数错误；-EOVERFLOW 槽位大小溢出；
 *         -ENOMEM 分配失败；或 pthread 初始化返回值的相反数。
 * @note 失败不改变 *queue，并逆序释放已成功初始化的资源。
 */
int ipc_frame_queue_create(IpcFrameQueue **queue, size_t capacity);

/**
 * @brief 尝试入队，满队列立即返回，不丢弃任何已有帧。
 * @param frame 独占持有的堆帧；frame 和 frame->data 均须可用 free 释放。
 * @return 0 成功并转移所有权；-EAGAIN 队列已满；-EPIPE 队列已关闭；
 *         -EINVAL 空指针；或 pthread 加锁错误的相反数。
 * @note 任何失败均不接管 frame。成功后调用者应自行将本地指针置 NULL，
 *       不得再读取、修改或释放原指针，消费者可能已立即取出并释放它。
 */
int ipc_frame_queue_try_push(IpcFrameQueue *queue, IpcRawFrame *frame);

/**
 * @brief 阻塞入队：满时等待空槽；关闭时停止等待并返回 -EPIPE。
 * @return 0 成功；失败返回负 errno，含义及所有权与 try_push 一致。
 * @note 没有超时；管理线程必须确保消费者持续工作或最终调用 close。
 *       采集线程是否适合等待由上层决定，不应持有驱动缓冲区长期等待。
 */
int ipc_frame_queue_push(IpcFrameQueue *queue, IpcRawFrame *frame);

/**
 * @brief 等待并取出最早入队的帧；队列关闭后仍先排空剩余帧。
 * @param frame 输出地址；必须非 NULL 且 *frame == NULL，避免覆盖未释放帧。
 * @return 1 成功并将所有权交给消费者；0 已关闭且已空；负 errno 表示错误。
 * @note 返回 0 或错误时不改变 *frame。取出的帧最终由消费者释放。
 */
int ipc_frame_queue_pop(IpcFrameQueue *queue, IpcRawFrame **frame);

/**
 * @brief 幂等关闭，禁止新入队并唤醒全部等待生产者和消费者。
 * @note 不释放内存，不丢弃剩余帧，不等待线程退出；NULL 是安全的空操作。
 */
void ipc_frame_queue_close(IpcFrameQueue *queue);

/**
 * @brief 释放残留帧、同步对象与队列，并将 *queue 置 NULL。
 * @pre 所有使用者已经退出并 join；本函数不能与任意队列操作并发。
 * @note 不会自动 join；残留帧会被丢弃并释放。queue 或 *queue 为 NULL 可重复调用。
 */
void ipc_frame_queue_destroy(IpcFrameQueue **queue);

/**
 * @brief 依次 free((*frame)->data)、free(*frame)，然后清空调用者指针。
 * @note 只能释放调用者当前持有的帧；不能释放已经成功入队的帧。
 *       不接受栈帧、MMAP、DMA-BUF 或共享数据；它们需要另行设计释放回调。
 *       frame、*frame 或 data 为 NULL 均可安全处理。
 */
void ipc_raw_frame_free(IpcRawFrame **frame);

#endif /* IPC_FRAME_QUEUE_H */
