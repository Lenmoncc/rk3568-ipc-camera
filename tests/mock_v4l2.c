/**
 * @file mock_v4l2.c
 * @brief 仅链接到本机测试二进制的模拟驱动，通过 --wrap 替换设备系统调用。
 *
 * 正式 bin/ipc_camera 永不链接此文件。仅 /dev/mock-video 被模拟；普通文件
 * 仍使用真实系统调用，因此可检查输出内容和文件创建失败。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MOCK_FD 60000
#define WIDTH 1280U
#define HEIGHT 720U
#define STRIDE 1296U
#define SPAN (STRIDE * (HEIGHT + HEIGHT / 2))
#define MAP_BYTES (SPAN + 64U)
#define MUST(x) do { if (!(x)) { fprintf(stderr, "MOCK invariant failed: %s:%d %s\n", __FILE__, __LINE__, #x); _Exit(98); } } while (0)
static void *maps[4];
static bool opened, allocated, streaming, queued[4], saw_open;
static unsigned int sequence, dequeued, qbuf_count;
static int64_t first_timestamp;
static const char *scenario;
int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
void *__real_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset);
int __real_munmap(void *address, size_t length);
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
size_t __real_fwrite(const void *data, size_t size, size_t count, FILE *output);
int __real_fclose(FILE *output);
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument);

/** @brief 注入消费端写入错误；普通情况下仍验证真实文件输出。 */
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *output)
{
    if (scenario != NULL && strcmp(scenario, "write-fail") == 0) {
        errno = ENOSPC;
        return 0;
    }
    return __real_fwrite(data, size, count, output);
}

/** @brief 注入收尾刷盘报错，确认主程序不会把 fclose 错误当成成功。 */
int __wrap_fclose(FILE *output)
{
    int result = __real_fclose(output);
    if (scenario != NULL && strcmp(scenario, "flush-fail") == 0) {
        errno = ENOSPC;
        return EOF;
    }
    return result;
}

/** @brief 注入第一/第二个业务线程创建失败，验证部分启动后的退出唤醒。 */
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument)
{
    static unsigned int attempts;
    unsigned int index = attempts++;
    if (scenario != NULL && ((strcmp(scenario, "consumer-thread-fail") == 0 && index == 0) ||
                            (strcmp(scenario, "producer-thread-fail") == 0 && index == 1)))
        return EAGAIN;
    return __real_pthread_create(thread, attributes, entry, argument);
}

/** @brief 判断当前子进程选择的故障场景。 */
static bool is_case(const char *name)
{
    return scenario != NULL && strcmp(scenario, name) == 0;
}

/** @brief 用系统调用约定返回错误，便于正式代码走真实错误处理分支。 */
static int fail_with(int error)
{
    errno = error;
    return -1;
}

/** @brief 退出时确认初始化回滚/正常收尾已经回收全部模拟资源。 */
static void verify_cleanup(void)
{
    MUST(!opened && !allocated && !streaming);
    for (size_t i = 0; i < 4; ++i) MUST(maps[i] == NULL);
    fprintf(stderr, "MOCK_CLEANUP_OK dequeued=%u qbuf=%u\n", dequeued, qbuf_count);
}

/** @brief 只拦截模拟摄像头的打开，其他文件保持真实独占创建语义。 */
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (strcmp(path, "/dev/mock-video") != 0) return __real_open(path, flags, mode);
    scenario = getenv("IPC_MOCK_CASE");
    if (is_case("open-fail")) return fail_with(EACCES);
    MUST(!saw_open);
    saw_open = opened = true;
    MUST(atexit(verify_cleanup) == 0);
    MUST((flags & O_NONBLOCK) != 0);
    return MOCK_FD;
}

/** @brief 校验调用者已经解除所有映射，模拟设备关闭释放驱动的剩余状态。 */
int __wrap_close(int fd)
{
    if (fd != MOCK_FD) return __real_close(fd);
    for (size_t i = 0; i < 4; ++i) MUST(maps[i] == NULL);
    opened = allocated = streaming = false;
    return 0;
}

/** @brief 分配四个互不共享的模拟 MMAP 缓冲区，可注入第二个映射失败。 */
void *__wrap_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset)
{
    unsigned int index;
    if (fd != MOCK_FD) return __real_mmap(address, length, protection, flags, fd, offset);
    index = (unsigned int)(offset / 4096);
    MUST(index < 4 && length == MAP_BYTES && maps[index] == NULL);
    if (is_case("mmap-fail") && index == 1) { errno = ENOMEM; return MAP_FAILED; }
    maps[index] = malloc(length);
    if (maps[index] == NULL) { errno = ENOMEM; return MAP_FAILED; }
    memset(maps[index], 0xee, length);
    return maps[index];
}

/** @brief 回收模拟映射；统计指针归零，供退出检查检测遗漏。 */
int __wrap_munmap(void *address, size_t length)
{
    for (size_t i = 0; i < 4; ++i) {
        if (maps[i] == address && address != NULL) {
            MUST(length == MAP_BYTES);
            free(maps[i]);
            maps[i] = NULL;
            return 0;
        }
    }
    return __real_munmap(address, length);
}

/** @brief 提供低速就绪通知或真实有限超时，不把模拟帧率当成硬件帧率。 */
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    struct timespec delay;
    if (count != 1 || fds[0].fd != MOCK_FD) return __real_poll(fds, count, timeout);
    MUST(streaming);
    delay = (struct timespec){.tv_sec = 0, .tv_nsec = is_case("timeout") ? timeout * 1000000L : 5000000L};
    nanosleep(&delay, NULL);
    if (is_case("timeout")) return 0;
    fds[0].revents = is_case("poll-error") ? POLLERR : POLLIN;
    return 1;
}

/** @brief 兼容启用 FORTIFY 的优化构建：保留长度检查，再进入相同模拟 poll。 */
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes)
{
    MUST(count <= bytes / sizeof(*fds));
    return __wrap_poll(fds, count, timeout);
}

/** @brief 生成带行填充、非零数据偏移的帧，可注入坏帧、截断及时间戳错误。 */
static int dequeue_mock(struct v4l2_buffer *buffer)
{
    struct v4l2_plane *plane = buffer->m.planes;
    struct timespec now;
    unsigned int index = sequence % 4;
    int64_t stamp;
    static bool eagain_sent;
    if (is_case("eagain") && !eagain_sent) { eagain_sent = true; return fail_with(EAGAIN); }
    MUST(streaming && queued[index] && maps[index] != NULL);
    queued[index] = false;
    buffer->index = index;
    buffer->length = 1;
    buffer->field = V4L2_FIELD_NONE;
    if (is_case("rkisp-field-any") || (is_case("generic-field-any") && sequence == 0))
        buffer->field = V4L2_FIELD_ANY;
    if (is_case("interlaced-once") && sequence == 0) buffer->field = V4L2_FIELD_INTERLACED;
    if (is_case("plane-count-once") && sequence == 0) buffer->length = 0;
    buffer->sequence = sequence;
    buffer->flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    clock_gettime(CLOCK_MONOTONIC, &now);
    stamp = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
    if (sequence == 0) first_timestamp = stamp;
    if (is_case("timestamp-backward") && sequence == 1) stamp = first_timestamp - 1;
    buffer->timestamp.tv_sec = stamp / 1000000;
    buffer->timestamp.tv_usec = stamp % 1000000;
    if (is_case("unknown-clock")) buffer->flags = 0;
    if ((is_case("bad-once") && sequence == 0) || is_case("all-bad")) buffer->flags |= V4L2_BUF_FLAG_ERROR;
    plane->length = MAP_BYTES;
    plane->data_offset = 32;
    /* 最后 UV 行不包含尾部 padding，验证采集代码不会读取其外字节。 */
    plane->bytesused = 32 + SPAN - STRIDE + WIDTH;
    if (is_case("short-once") && sequence == 0) plane->bytesused = 64;
    if (is_case("bad-offset") && sequence == 0) plane->data_offset = plane->bytesused + 1;
    if (is_case("bad-size") && sequence == 0) plane->bytesused = MAP_BYTES + 1;
    if (is_case("invalid-index")) buffer->index = 100;
    memset(maps[index], 0xee, MAP_BYTES);
    for (size_t row = 0; row < HEIGHT + HEIGHT / 2; ++row)
        memset((unsigned char *)maps[index] + 32 + row * STRIDE,
               row < HEIGHT ? sequence % 200 : 128, WIDTH);
    ++sequence;
    ++dequeued;
    return 0;
}

/** @brief 模拟 V4L2 生命周期并校验顺序；只在本机测试链接时替代 ioctl。 */
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    void *argument;
    va_list arguments;
    MUST(fd == MOCK_FD && opened);
    va_start(arguments, request);
    argument = va_arg(arguments, void *);
    va_end(arguments);
    switch (request) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *cap = argument;
        snprintf((char *)cap->driver, sizeof(cap->driver), "%s",
                 is_case("rkisp-field-any") || is_case("interlaced-once") ? "rkisp_v5" : "mock-v4l2");
        cap->capabilities = V4L2_CAP_DEVICE_CAPS;
        cap->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
        if (is_case("capability")) cap->device_caps = V4L2_CAP_VIDEO_CAPTURE;
        return 0;
    }
    case VIDIOC_S_FMT:
        MUST(!allocated);
        if (is_case("set-format")) return fail_with(EBUSY);
        return 0;
    case VIDIOC_G_FMT: {
        struct v4l2_format *format = argument;
        MUST(format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        format->fmt.pix_mp.width = is_case("format") ? 1920 : WIDTH;
        format->fmt.pix_mp.height = HEIGHT;
        format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        format->fmt.pix_mp.field = V4L2_FIELD_NONE;
        format->fmt.pix_mp.num_planes = is_case("two-planes") ? 2 : 1;
        format->fmt.pix_mp.plane_fmt[0].bytesperline = STRIDE;
        format->fmt.pix_mp.plane_fmt[0].sizeimage = SPAN;
        return 0;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *buffers = argument;
        if (buffers->count == 0) {
            if (streaming) return fail_with(EBUSY);
            for (size_t i = 0; i < 4; ++i) MUST(maps[i] == NULL);
            allocated = false;
        } else {
            MUST(!allocated);
            allocated = true;
            buffers->count = is_case("zero-buffers") ? 0 : 4;
        }
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *buffer = argument;
        MUST(allocated && buffer->length == 1);
        if (is_case("query-fail") && buffer->index == 1) return fail_with(EIO);
        buffer->m.planes[0].length = MAP_BYTES;
        buffer->m.planes[0].m.mem_offset = buffer->index * 4096;
        return 0;
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer *buffer = argument;
        unsigned int index = buffer->index;
        MUST(index < 4 && maps[index] != NULL && !queued[index]);
        if (is_case("qbuf-fail") && dequeued > 0) return fail_with(EIO);
        /* 立即覆盖驱动区：验证已入队的应用帧不是 MMAP 的悬空别名。 */
        memset(maps[index], 0xcc, MAP_BYTES);
        queued[index] = true;
        ++qbuf_count;
        return 0;
    }
    case VIDIOC_STREAMON:
        for (size_t i = 0; i < 4; ++i) MUST(queued[i]);
        if (is_case("streamon-fail")) return fail_with(EIO);
        streaming = true;
        return 0;
    case VIDIOC_STREAMOFF:
        if (is_case("streamoff-fail")) return fail_with(EIO);
        streaming = false;
        return 0;
    case VIDIOC_DQBUF:
        return dequeue_mock(argument);
    default:
        return fail_with(ENOTTY);
    }
}
