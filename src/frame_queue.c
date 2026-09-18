/**
 * @file frame_queue.c
 * @brief 用环形指针数组、互斥锁和两个条件变量实现有界原始数据队列。
 *
 * 本模块只搬移 IpcRawFrame 指针，不复制图像/PCM，也不解释媒体格式。
 * capacity/head/tail/count/closed 和槽位均在 mutex 保护下访问；唯一例外
 * 是尚未发布对象的 create，以及已确保所有线程退出的 destroy。
 *
 * 正常退出：close -> 消费者排空 -> join 全部使用者 -> destroy。
 * close 不能替代 join，closed 也不意味着对象可以立即释放。
 */
#include "frame_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

struct IpcFrameQueue {
    IpcRawFrame **slots;       /**< 指针槽位；非 NULL 槽位中的帧归队列所有。 */
    size_t capacity;          /**< 槽位总数，创建后保持不变。 */
    size_t head;              /**< 下一次出队的位置。 */
    size_t tail;              /**< 下一次入队的位置。 */
    size_t count;             /**< 有效元素数；区分 head == tail 时的空和满。 */
    bool closed;              /**< 单向状态：false -> true，不支持重新打开。 */
    pthread_mutex_t mutex;    /**< 同时保护数据和等待条件，防止检查与睡眠间漏通知。 */
    pthread_cond_t not_empty; /**< 消费者等待条件：count > 0 或 closed。 */
    pthread_cond_t not_full;  /**< 生产者等待条件：count < capacity 或 closed。 */
};

void ipc_raw_frame_free(IpcRawFrame **frame)
{
    if (frame == NULL || *frame == NULL)
        return;
    /* 数据和描述对象是两个独立分配。仅清空此指针，不能使其他别名失效；
     * 因而“唯一所有者”约定比重复调用安全性更重要。 */
    free((*frame)->data);
    free(*frame);
    *frame = NULL;
}

int ipc_frame_queue_create(IpcFrameQueue **queue, size_t capacity)
{
    IpcFrameQueue *created;
    int error;

    if (queue == NULL || *queue != NULL || capacity == 0)
        return -EINVAL;
    /* 在 capacity * sizeof(pointer) 之前检查，防止溢出造成少分配内存。 */
    if (capacity > SIZE_MAX / sizeof(IpcRawFrame *))
        return -EOVERFLOW;
    created = calloc(1, sizeof(*created));
    if (created == NULL)
        return -ENOMEM;
    created->slots = calloc(capacity, sizeof(*created->slots));
    if (created->slots == NULL) {
        free(created);
        return -ENOMEM;
    }
    created->capacity = capacity;

    /* pthread 接口直接返回错误码，而不是通过 errno 报错。
     * 每个失败出口只销毁此前成功初始化的对象，不能销毁未初始化的锁。 */
    error = pthread_mutex_init(&created->mutex, NULL);
    if (error != 0)
        goto fail_slots;
    error = pthread_cond_init(&created->not_empty, NULL);
    if (error != 0)
        goto fail_mutex;
    error = pthread_cond_init(&created->not_full, NULL);
    if (error != 0)
        goto fail_not_empty;

    /* 全部就绪后才发布句柄：调用者不会拿到半初始化的对象。 */
    *queue = created;
    return 0;

fail_not_empty:
    pthread_cond_destroy(&created->not_empty);
fail_mutex:
    pthread_mutex_destroy(&created->mutex);
fail_slots:
    free(created->slots);
    free(created);
    return -error;
}

/**
 * @brief 两种入队接口共用同一条所有权转移路径。
 * @param wait_for_space true 时等待空槽，false 时满队列返回 -EAGAIN。
 * @note 在槽位写入之前发生的任何错误均不接管帧。
 */
static int push_frame(IpcFrameQueue *queue, IpcRawFrame *frame, bool wait_for_space)
{
    int error;
    int result = 0;

    if (queue == NULL || frame == NULL)
        return -EINVAL;
    error = pthread_mutex_lock(&queue->mutex);
    if (error != 0)
        return -error;

    /* 必须用 while 而非 if：条件变量可虚假唤醒，且另一个生产者可能先抢到
     * 空槽。cond_wait 原子地释放锁并等待，返回时重新持锁，再检查真实状态。 */
    while (queue->count == queue->capacity && !queue->closed) {
        if (!wait_for_space) {
            result = -EAGAIN;
            goto unlock;
        }
        error = pthread_cond_wait(&queue->not_full, &queue->mutex);
        if (error != 0) {
            result = -error;
            goto unlock;
        }
    }
    /* 即使关闭时有空槽也禁止入队，保证关闭后的消费者最终能够排空退出。 */
    if (queue->closed) {
        result = -EPIPE;
        goto unlock;
    }

    queue->slots[queue->tail] = frame;
    queue->tail = (queue->tail + 1) % queue->capacity;
    ++queue->count;
    /* 这里完成所有权转移。只新增一个元素，唤醒一个消费者即可。
     * 已初始化且未销毁的条件变量通知不改变本次成功结果，不能把已接管的帧
     * 再以“入队失败”返回给生产者，否则可能引发重复释放。 */
    pthread_cond_signal(&queue->not_empty);
unlock:
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

int ipc_frame_queue_try_push(IpcFrameQueue *queue, IpcRawFrame *frame)
{
    return push_frame(queue, frame, false);
}

int ipc_frame_queue_push(IpcFrameQueue *queue, IpcRawFrame *frame)
{
    return push_frame(queue, frame, true);
}

int ipc_frame_queue_pop(IpcFrameQueue *queue, IpcRawFrame **frame)
{
    int error;
    int result = 0;

    if (queue == NULL || frame == NULL || *frame != NULL)
        return -EINVAL;
    error = pthread_mutex_lock(&queue->mutex);
    if (error != 0)
        return -error;
    while (queue->count == 0 && !queue->closed) {
        error = pthread_cond_wait(&queue->not_empty, &queue->mutex);
        if (error != 0) {
            result = -error;
            goto unlock;
        }
    }
    /* 关闭且为空才是 EOF；关闭但还有数据时必须继续交付，不能提前返回 0。 */
    if (queue->count != 0) {
        *frame = queue->slots[queue->head];
        queue->slots[queue->head] = NULL;
        queue->head = (queue->head + 1) % queue->capacity;
        --queue->count;
        result = 1;
        /* 帧归消费者了。清空槽位避免 destroy 把已经交付的帧再次释放。 */
        pthread_cond_signal(&queue->not_full);
    }
unlock:
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

void ipc_frame_queue_close(IpcFrameQueue *queue)
{
    if (queue == NULL)
        return;
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    /* 关闭是全局状态变化，不能只 signal 一个线程：其余等待者可能再也收不到
     * 通知。分别广播两个条件，让生产者失败返回、消费者排空或返回 EOF。 */
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

void ipc_frame_queue_destroy(IpcFrameQueue **queue)
{
    IpcFrameQueue *destroyed;

    if (queue == NULL || *queue == NULL)
        return;
    destroyed = *queue;
    /* 调用者已 join 全部使用者，因此这里不需要加锁。加锁也不能代替 join：
     * 其他线程可能正在等锁，解锁后释放对象会使它们访问失效地址。 */
    while (destroyed->count != 0) {
        ipc_raw_frame_free(&destroyed->slots[destroyed->head]);
        destroyed->head = (destroyed->head + 1) % destroyed->capacity;
        --destroyed->count;
    }
    pthread_cond_destroy(&destroyed->not_full);
    pthread_cond_destroy(&destroyed->not_empty);
    pthread_mutex_destroy(&destroyed->mutex);
    free(destroyed->slots);
    free(destroyed);
    *queue = NULL;
}
