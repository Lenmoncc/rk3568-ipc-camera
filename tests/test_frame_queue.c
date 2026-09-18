/**
 * @file test_frame_queue.c
 * @brief 不依赖硬件的队列行为测试，支持本机与 RK3568 独立运行。
 *
 * 使用小块模拟数据验证指针所有权、FIFO、并发与关闭，不模拟实际编码负载。
 * 不用 assert 执行带副作用的操作，避免 -DNDEBUG 使测试被悄悄跳过。
 * 每个测试独立创建资源；检查失败立即退出，成功路径完整 join 并销毁。
 */
#define _POSIX_C_SOURCE 200809L
#include "frame_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

/** 构造可追踪序号和时间戳的模拟块；分配方式与正式队列释放契约相同。 */
static IpcRawFrame *make_frame(IpcMediaType type, unsigned int sequence)
{
    IpcRawFrame *frame = calloc(1, sizeof(*frame));
    CHECK(frame != NULL);
    frame->size = sizeof(sequence);
    frame->data = malloc(frame->size);
    CHECK(frame->data != NULL);
    memcpy(frame->data, &sequence, sizeof(sequence));
    frame->type = type;
    frame->pts_us = (int64_t)sequence * 1000;
    if (type == IPC_MEDIA_VIDEO) {
        frame->info.video.sequence = sequence;
    } else {
        frame->info.audio.sample_rate = 48000;
        frame->info.audio.channels = 1;
        frame->info.audio.samples_per_channel = sizeof(sequence) / 2;
        frame->info.audio.sample_format = IPC_AUDIO_FORMAT_S16_LE;
    }
    return frame;
}

/** 验证字节内容及元数据均未被队列修改；这里的数据不是可显示图像。 */
static void check_frame(const IpcRawFrame *frame, IpcMediaType type, unsigned int sequence)
{
    unsigned int actual;
    CHECK(frame != NULL && frame->type == type);
    CHECK(frame->size == sizeof(actual));
    memcpy(&actual, frame->data, sizeof(actual));
    CHECK(actual == sequence && frame->pts_us == (int64_t)sequence * 1000);
    if (type == IPC_MEDIA_VIDEO)
        CHECK(frame->info.video.sequence == sequence);
    else
        CHECK(frame->info.audio.sample_format == IPC_AUDIO_FORMAT_S16_LE);
}

static void test_invalid_arguments(void)
{
    IpcFrameQueue *queue = NULL;
    IpcFrameQueue *saved;
    IpcRawFrame *frame = make_frame(IPC_MEDIA_VIDEO, 1);
    IpcRawFrame *output = NULL;
    CHECK(ipc_frame_queue_create(NULL, 1) == -EINVAL);
    CHECK(ipc_frame_queue_create(&queue, 0) == -EINVAL && queue == NULL);
    CHECK(ipc_frame_queue_create(&queue, SIZE_MAX) == -EOVERFLOW && queue == NULL);
    CHECK(ipc_frame_queue_create(&queue, 1) == 0);
    saved = queue;
    CHECK(ipc_frame_queue_create(&queue, 2) == -EINVAL && queue == saved);
    CHECK(ipc_frame_queue_try_push(NULL, frame) == -EINVAL);
    CHECK(ipc_frame_queue_push(NULL, frame) == -EINVAL);
    CHECK(ipc_frame_queue_try_push(queue, NULL) == -EINVAL);
    CHECK(ipc_frame_queue_push(queue, NULL) == -EINVAL);
    CHECK(ipc_frame_queue_pop(NULL, &output) == -EINVAL && output == NULL);
    CHECK(ipc_frame_queue_pop(queue, NULL) == -EINVAL);
    CHECK(ipc_frame_queue_pop(queue, &frame) == -EINVAL);
    check_frame(frame, IPC_MEDIA_VIDEO, 1); /* 错误路径不覆盖输出或接管帧。 */
    ipc_raw_frame_free(&frame);
    ipc_raw_frame_free(&frame);
    ipc_raw_frame_free(NULL);
    frame = calloc(1, sizeof(*frame));
    CHECK(frame != NULL);
    ipc_raw_frame_free(&frame); /* data 为 NULL 的部分初始化对象也可释放。 */
    ipc_frame_queue_close(NULL);
    ipc_frame_queue_destroy(NULL);
    ipc_frame_queue_destroy(&queue);
    CHECK(queue == NULL);
    ipc_frame_queue_destroy(&queue);
}

static void test_fifo_full_and_wrap(void)
{
    IpcFrameQueue *queue = NULL;
    IpcRawFrame *frame;
    IpcRawFrame *output = NULL;
    CHECK(ipc_frame_queue_create(&queue, 3) == 0);
    for (unsigned int i = 0; i < 3; ++i)
        CHECK(ipc_frame_queue_try_push(queue, make_frame(IPC_MEDIA_VIDEO, i)) == 0);
    frame = make_frame(IPC_MEDIA_VIDEO, 3);
    CHECK(ipc_frame_queue_try_push(queue, frame) == -EAGAIN);
    check_frame(frame, IPC_MEDIA_VIDEO, 3); /* 满队列时调用者仍可安全访问。 */
    CHECK(ipc_frame_queue_pop(queue, &output) == 1);
    check_frame(output, IPC_MEDIA_VIDEO, 0);
    ipc_raw_frame_free(&output);
    CHECK(ipc_frame_queue_try_push(queue, frame) == 0); /* tail 回绕到起始槽位。 */
    frame = NULL;
    ipc_frame_queue_close(queue);
    ipc_frame_queue_close(queue);
    for (unsigned int i = 1; i < 4; ++i) {
        CHECK(ipc_frame_queue_pop(queue, &output) == 1);
        check_frame(output, IPC_MEDIA_VIDEO, i);
        ipc_raw_frame_free(&output);
    }
    CHECK(ipc_frame_queue_pop(queue, &output) == 0 && output == NULL);
    frame = make_frame(IPC_MEDIA_VIDEO, 4);
    CHECK(ipc_frame_queue_try_push(queue, frame) == -EPIPE);
    CHECK(ipc_frame_queue_push(queue, frame) == -EPIPE);
    ipc_raw_frame_free(&frame);
    ipc_frame_queue_destroy(&queue);
}

static void test_destroy_residual_and_capacity_one(void)
{
    IpcFrameQueue *queue = NULL;
    IpcRawFrame *output = NULL;
    CHECK(ipc_frame_queue_create(&queue, 1) == 0);
    for (unsigned int i = 0; i < 32; ++i) {
        CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_AUDIO, i)) == 0);
        CHECK(ipc_frame_queue_pop(queue, &output) == 1);
        check_frame(output, IPC_MEDIA_AUDIO, i);
        ipc_raw_frame_free(&output);
    }
    CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_AUDIO, 32)) == 0);
    ipc_frame_queue_close(queue);
    ipc_frame_queue_destroy(&queue); /* sanitizer 检查剩余帧是否泄漏/重复释放。 */
    CHECK(ipc_frame_queue_create(&queue, 3) == 0);
    for (unsigned int i = 0; i < 3; ++i)
        CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_VIDEO, i)) == 0);
    CHECK(ipc_frame_queue_pop(queue, &output) == 1);
    ipc_raw_frame_free(&output);
    CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_VIDEO, 3)) == 0);
    ipc_frame_queue_destroy(&queue); /* 无使用线程时，未 close 的回绕队列也能清理。 */
}

/**
 * 单次操作线程与测试控制线程间的握手；这些锁仅用于测试观察，
 * 不读取队列内部字段，也不靠 volatile 或无保护的全局变量判断线程状态。
 */
typedef struct {
    IpcFrameQueue *queue;
    IpcRawFrame *frame;
    bool producer;
    bool started;
    bool done;
    int result;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
} Operation;

static struct timespec deadline_ms(long milliseconds)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += milliseconds * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    return deadline;
}

static void *operate_once(void *argument)
{
    Operation *operation = argument;
    CHECK(pthread_mutex_lock(&operation->mutex) == 0);
    operation->started = true;
    CHECK(pthread_cond_signal(&operation->changed) == 0);
    CHECK(pthread_mutex_unlock(&operation->mutex) == 0);
    if (operation->producer) {
        operation->result = ipc_frame_queue_push(operation->queue, operation->frame);
        if (operation->result == 0)
            operation->frame = NULL; /* 成功后不再访问可能已被消费者释放的帧。 */
    } else {
        operation->result = ipc_frame_queue_pop(operation->queue, &operation->frame);
    }
    CHECK(pthread_mutex_lock(&operation->mutex) == 0);
    operation->done = true;
    CHECK(pthread_cond_signal(&operation->changed) == 0);
    CHECK(pthread_mutex_unlock(&operation->mutex) == 0);
    return NULL;
}

static void start_operation(Operation *operation, IpcFrameQueue *queue, bool producer)
{
    memset(operation, 0, sizeof(*operation));
    operation->queue = queue;
    operation->producer = producer;
    if (producer)
        operation->frame = make_frame(IPC_MEDIA_VIDEO, 99);
    CHECK(pthread_mutex_init(&operation->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&operation->changed, NULL) == 0);
    CHECK(pthread_create(&operation->thread, NULL, operate_once, operation) == 0);
}

/**
 * 确认工作线程已开始，且在条件未满足的观察窗口内没有提前完成。
 * 时间窗口用于观察阻塞行为，不要求线程在某个精确毫秒数被调度。
 */
static void expect_blocked(Operation *operation)
{
    struct timespec deadline = deadline_ms(2000);
    int result;
    CHECK(pthread_mutex_lock(&operation->mutex) == 0);
    while (!operation->started)
        CHECK(pthread_cond_timedwait(&operation->changed, &operation->mutex, &deadline) == 0);
    deadline = deadline_ms(50);
    while (!operation->done) {
        result = pthread_cond_timedwait(&operation->changed, &operation->mutex, &deadline);
        if (result == ETIMEDOUT)
            break;
        CHECK(result == 0);
    }
    CHECK(!operation->done);
    CHECK(pthread_mutex_unlock(&operation->mutex) == 0);
}

static void finish_operation(Operation *operation, int expected)
{
    struct timespec deadline = deadline_ms(2000);
    CHECK(pthread_mutex_lock(&operation->mutex) == 0);
    while (!operation->done)
        CHECK(pthread_cond_timedwait(&operation->changed, &operation->mutex, &deadline) == 0);
    CHECK(pthread_mutex_unlock(&operation->mutex) == 0);
    CHECK(pthread_join(operation->thread, NULL) == 0);
    CHECK(operation->result == expected);
    /* 失败生产者持有原帧，成功消费者持有出队帧；两者都必须最终释放。
     * 成功生产者的指针已清空，此处不会重复释放已移交队列的帧。 */
    if (operation->frame != NULL)
        check_frame(operation->frame, IPC_MEDIA_VIDEO, 99);
    ipc_raw_frame_free(&operation->frame);
    CHECK(pthread_cond_destroy(&operation->changed) == 0);
    CHECK(pthread_mutex_destroy(&operation->mutex) == 0);
}

static void test_wake_consumer(void)
{
    IpcFrameQueue *queue = NULL;
    Operation operation;
    CHECK(ipc_frame_queue_create(&queue, 1) == 0);
    start_operation(&operation, queue, false);
    expect_blocked(&operation);
    CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_VIDEO, 99)) == 0);
    finish_operation(&operation, 1);
    ipc_frame_queue_close(queue);
    ipc_frame_queue_destroy(&queue);
}

static void test_wake_producer(void)
{
    IpcFrameQueue *queue = NULL;
    IpcRawFrame *output = NULL;
    Operation operation;
    CHECK(ipc_frame_queue_create(&queue, 1) == 0);
    CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_VIDEO, 0)) == 0);
    start_operation(&operation, queue, true);
    expect_blocked(&operation);
    CHECK(ipc_frame_queue_pop(queue, &output) == 1);
    check_frame(output, IPC_MEDIA_VIDEO, 0);
    ipc_raw_frame_free(&output);
    finish_operation(&operation, 0);
    CHECK(ipc_frame_queue_pop(queue, &output) == 1);
    check_frame(output, IPC_MEDIA_VIDEO, 99);
    ipc_raw_frame_free(&output);
    ipc_frame_queue_close(queue);
    ipc_frame_queue_destroy(&queue);
}

/** 关闭空队列唤醒所有消费者；关闭满队列唤醒所有生产者。 */
static void test_close_waiters(bool producers)
{
    IpcFrameQueue *queue = NULL;
    Operation operations[3];
    IpcRawFrame *output = NULL;
    CHECK(ipc_frame_queue_create(&queue, 1) == 0);
    if (producers)
        CHECK(ipc_frame_queue_push(queue, make_frame(IPC_MEDIA_AUDIO, 0)) == 0);
    for (size_t i = 0; i < 3; ++i)
        start_operation(&operations[i], queue, producers);
    for (size_t i = 0; i < 3; ++i)
        expect_blocked(&operations[i]);
    ipc_frame_queue_close(queue);
    for (size_t i = 0; i < 3; ++i)
        finish_operation(&operations[i], producers ? -EPIPE : 0);
    if (producers) {
        CHECK(ipc_frame_queue_pop(queue, &output) == 1);
        check_frame(output, IPC_MEDIA_AUDIO, 0);
        ipc_raw_frame_free(&output);
    }
    CHECK(ipc_frame_queue_pop(queue, &output) == 0);
    ipc_frame_queue_destroy(&queue);
}

#define STREAM_FRAMES 10000U
typedef struct {
    IpcFrameQueue *queue;
    IpcMediaType type;
    unsigned int received; /* 仅消费者写，主线程在 join 之后读取。 */
} Stream;

static void *produce_stream(void *argument)
{
    Stream *stream = argument;
    for (unsigned int i = 0; i < STREAM_FRAMES; ++i) {
        IpcRawFrame *frame = make_frame(stream->type, i);
        CHECK(ipc_frame_queue_push(stream->queue, frame) == 0);
        frame = NULL;
    }
    ipc_frame_queue_close(stream->queue);
    return NULL;
}

static void *consume_stream(void *argument)
{
    Stream *stream = argument;
    IpcRawFrame *frame = NULL;
    int result;
    while ((result = ipc_frame_queue_pop(stream->queue, &frame)) == 1) {
        check_frame(frame, stream->type, stream->received);
        ++stream->received;
        ipc_raw_frame_free(&frame);
    }
    CHECK(result == 0);
    return NULL;
}

static void test_two_independent_streams(void)
{
    Stream streams[2] = {{NULL, IPC_MEDIA_VIDEO, 0}, {NULL, IPC_MEDIA_AUDIO, 0}};
    pthread_t producers[2], consumers[2];
    for (size_t i = 0; i < 2; ++i) {
        CHECK(ipc_frame_queue_create(&streams[i].queue, i == 0 ? 3 : 7) == 0);
        CHECK(pthread_create(&consumers[i], NULL, consume_stream, &streams[i]) == 0);
        CHECK(pthread_create(&producers[i], NULL, produce_stream, &streams[i]) == 0);
    }
    for (size_t i = 0; i < 2; ++i) {
        CHECK(pthread_join(producers[i], NULL) == 0);
        CHECK(pthread_join(consumers[i], NULL) == 0);
        CHECK(streams[i].received == STREAM_FRAMES);
        ipc_frame_queue_destroy(&streams[i].queue);
    }
}

#define PARALLEL_PRODUCERS 3U
#define PARALLEL_CONSUMERS 2U
#define PER_PRODUCER 2000U
typedef struct {
    IpcFrameQueue *queue;
    pthread_mutex_t mutex;
    unsigned char seen[PARALLEL_PRODUCERS * PER_PRODUCER];
    unsigned int count;
} ParallelContext;

typedef struct {
    ParallelContext *context;
    unsigned int index;
} ProducerContext;

static void *parallel_producer(void *argument)
{
    ProducerContext *producer = argument;
    for (unsigned int i = 0; i < PER_PRODUCER; ++i) {
        unsigned int id = producer->index * PER_PRODUCER + i;
        CHECK(ipc_frame_queue_push(producer->context->queue,
                                   make_frame(IPC_MEDIA_VIDEO, id)) == 0);
    }
    /* 多生产者共享一个队列，不能由任意单个生产者提前 close。
     * 测试主线程 join 全部生产者后统一关闭。 */
    return NULL;
}

static void *parallel_consumer(void *argument)
{
    ParallelContext *context = argument;
    IpcRawFrame *frame = NULL;
    int result;
    while ((result = ipc_frame_queue_pop(context->queue, &frame)) == 1) {
        unsigned int id = frame->info.video.sequence;
        CHECK(id < PARALLEL_PRODUCERS * PER_PRODUCER);
        check_frame(frame, IPC_MEDIA_VIDEO, id);
        /* 两个消费者的完成顺序可能不同；用独立锁保护计数和去重表。
         * 这里只检查每个包恰好一次交付，不错误地要求线程完成顺序一致。 */
        CHECK(pthread_mutex_lock(&context->mutex) == 0);
        CHECK(context->seen[id] == 0);
        context->seen[id] = 1;
        ++context->count;
        CHECK(pthread_mutex_unlock(&context->mutex) == 0);
        ipc_raw_frame_free(&frame);
    }
    CHECK(result == 0);
    return NULL;
}

static void test_multiple_producers_consumers(void)
{
    ParallelContext context = {0};
    ProducerContext arguments[PARALLEL_PRODUCERS];
    pthread_t producers[PARALLEL_PRODUCERS], consumers[PARALLEL_CONSUMERS];
    CHECK(ipc_frame_queue_create(&context.queue, 7) == 0);
    CHECK(pthread_mutex_init(&context.mutex, NULL) == 0);
    for (size_t i = 0; i < PARALLEL_CONSUMERS; ++i)
        CHECK(pthread_create(&consumers[i], NULL, parallel_consumer, &context) == 0);
    for (unsigned int i = 0; i < PARALLEL_PRODUCERS; ++i) {
        arguments[i].context = &context;
        arguments[i].index = i;
        CHECK(pthread_create(&producers[i], NULL, parallel_producer, &arguments[i]) == 0);
    }
    for (size_t i = 0; i < PARALLEL_PRODUCERS; ++i)
        CHECK(pthread_join(producers[i], NULL) == 0);
    ipc_frame_queue_close(context.queue);
    for (size_t i = 0; i < PARALLEL_CONSUMERS; ++i)
        CHECK(pthread_join(consumers[i], NULL) == 0);
    CHECK(context.count == PARALLEL_PRODUCERS * PER_PRODUCER);
    CHECK(pthread_mutex_destroy(&context.mutex) == 0);
    ipc_frame_queue_destroy(&context.queue);
}

int main(void)
{
    /* 防止错误实现让 CI 或板端测试永久挂起；测试整体超时会以非零状态终止。 */
    alarm(30);
    test_invalid_arguments(); puts("PASS: invalid arguments and safe cleanup");
    test_fifo_full_and_wrap(); puts("PASS: FIFO, full ownership, wraparound and drain");
    test_destroy_residual_and_capacity_one(); puts("PASS: capacity one and residual cleanup");
    test_wake_consumer(); puts("PASS: empty consumer wakes after push");
    test_wake_producer(); puts("PASS: full producer wakes after pop");
    test_close_waiters(false); puts("PASS: close wakes all waiting consumers");
    test_close_waiters(true); puts("PASS: close wakes all waiting producers");
    test_two_independent_streams(); puts("PASS: independent video/audio queues, 20000 frames");
    test_multiple_producers_consumers(); puts("PASS: 3 producers / 2 consumers, 6000 unique frames");
    alarm(0);
    puts("PASS: 9 frame queue scenarios (no hardware required).");
    return EXIT_SUCCESS;
}
