/** @file packet_queue.c
 * @brief 互斥锁保护环形队列，单调时钟条件变量避免墙上时钟变化干扰超时。
 */
#define _POSIX_C_SOURCE 200809L
#include "packet_queue.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#ifndef IPC_WITH_FFMPEG
#define IPC_WITH_FFMPEG 0
#endif
#if IPC_WITH_FFMPEG
#include <libavcodec/packet.h>
#endif
struct IpcPacketQueue {
    IpcEncodedPacket **slots;
    size_t capacity, head, tail, count;
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
};
/** @brief 引用编码包；每个输出可独立修改自己的 PTS/stream_index，不修改共享 payload。 */
int ipc_encoded_packet_ref(IpcEncodedPacket **dest, const IpcEncodedPacket *source)
{
#if IPC_WITH_FFMPEG
    if (!dest || *dest || !source || !source->packet || source->time_base.num <= 0 || source->time_base.den <= 0 ||
        (source->type != IPC_MEDIA_VIDEO && source->type != IPC_MEDIA_AUDIO)) return -EINVAL;
    IpcEncodedPacket *copy = calloc(1, sizeof(*copy));
    if (!copy) return -ENOMEM;
    copy->packet = av_packet_clone(source->packet);
    if (!copy->packet) { free(copy); return -ENOMEM; }
    copy->type = source->type; copy->time_base = source->time_base;
    *dest = copy;
    return 0;
#else
    (void)dest; (void)source;
    return -ENOTSUP;
#endif
}
/** @brief 先减少 AVPacket 数据引用，再释放外层对象；不会释放其他输出持有的引用。 */
void ipc_encoded_packet_free(IpcEncodedPacket **packet)
{
    if (!packet || !*packet) return;
#if IPC_WITH_FFMPEG
    av_packet_free(&(*packet)->packet);
#endif
    free(*packet); *packet = NULL;
}
/** @brief 完整初始化后发布句柄；逐级回滚锁、条件变量属性与内存。 */
int ipc_packet_queue_create(IpcPacketQueue **queue, size_t capacity)
{
    pthread_condattr_t attr;
    int error;
    if (!queue || *queue || !capacity) return -EINVAL;
    if (capacity > SIZE_MAX / sizeof(IpcEncodedPacket *)) return -EOVERFLOW;
    IpcPacketQueue *q = calloc(1, sizeof(*q));
    if (!q) return -ENOMEM;
    q->slots = calloc(capacity, sizeof(*q->slots));
    if (!q->slots) { free(q); return -ENOMEM; }
    q->capacity = capacity;
    error = pthread_mutex_init(&q->mutex, NULL);
    if (error) goto memory;
    error = pthread_condattr_init(&attr);
    if (error) goto mutex;
    error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!error) error = pthread_cond_init(&q->ready, &attr);
    pthread_condattr_destroy(&attr);
    if (error) goto mutex;
    *queue = q; return 0;
mutex:
    pthread_mutex_destroy(&q->mutex);
memory:
    free(q->slots); free(q); return -error;
}
/** @brief 满时立即失败，禁止对压缩视频随意丢包；业务层决定停止整段录像。 */
int ipc_packet_queue_try_push(IpcPacketQueue *q, IpcEncodedPacket *packet)
{
    if (!q || !packet) return -EINVAL;
    int error = pthread_mutex_lock(&q->mutex), result = 0;
    if (error) return -error;
    if (q->closed) result = -EPIPE;
    else if (q->count == q->capacity) result = -EAGAIN;
    else {
        q->slots[q->tail] = packet; q->tail = (q->tail + 1) % q->capacity; ++q->count;
        pthread_cond_signal(&q->ready);
    }
    pthread_mutex_unlock(&q->mutex); return result;
}
/** @brief 等待至有包、关闭或同一个绝对截止时刻；虚假唤醒不重置超时时间。 */
int ipc_packet_queue_pop_timed(IpcPacketQueue *q, IpcEncodedPacket **packet, int timeout_ms)
{
    struct timespec deadline = {0};
    int error, result = 0;
    if (!q || !packet || *packet || timeout_ms < -1 || timeout_ms > 1000) return -EINVAL;
    if (timeout_ms > 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return -errno;
        deadline.tv_nsec += (long)timeout_ms * 1000000;
        deadline.tv_sec += deadline.tv_nsec / 1000000000;
        deadline.tv_nsec %= 1000000000;
    }
    error = pthread_mutex_lock(&q->mutex);
    if (error) return -error;
    while (!q->count && !q->closed) {
        if (!timeout_ms) { result = -EAGAIN; goto unlock; }
        error = timeout_ms < 0 ? pthread_cond_wait(&q->ready, &q->mutex) :
                                pthread_cond_timedwait(&q->ready, &q->mutex, &deadline);
        if (error == ETIMEDOUT && !q->count && !q->closed) { result = -EAGAIN; goto unlock; }
        if (error && error != ETIMEDOUT) { result = -error; goto unlock; }
    }
    if (q->count) {
        *packet = q->slots[q->head]; q->slots[q->head] = NULL;
        q->head = (q->head + 1) % q->capacity; --q->count; result = 1;
    }
unlock:
    pthread_mutex_unlock(&q->mutex); return result;
}
/** @brief 提供单队列无限等待入口；多流输出应使用限时出队协调两路。 */
int ipc_packet_queue_pop(IpcPacketQueue *q, IpcEncodedPacket **packet)
{ return ipc_packet_queue_pop_timed(q, packet, -1); }
/** @brief 广播关闭状态，使每个等待消费者最终收到 EOF。 */
void ipc_packet_queue_close(IpcPacketQueue *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->mutex); q->closed = true;
    pthread_cond_broadcast(&q->ready); pthread_mutex_unlock(&q->mutex);
}
/** @brief 在线程已退出的前提下释放残留包，不把 close 当作 join。 */
void ipc_packet_queue_destroy(IpcPacketQueue **queue)
{
    if (!queue || !*queue) return;
    IpcPacketQueue *q = *queue; *queue = NULL;
    for (size_t i = 0; i < q->capacity; ++i) ipc_encoded_packet_free(&q->slots[i]);
    pthread_cond_destroy(&q->ready); pthread_mutex_destroy(&q->mutex);
    free(q->slots); free(q);
}
