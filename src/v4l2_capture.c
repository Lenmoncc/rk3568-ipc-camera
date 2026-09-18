/**
 * @file v4l2_capture.c
 * @brief 正式工程采集实现：MPLANE + MMAP -> 应用独占 NV12 帧。
 *
 * 独立实现设备生命周期，不调用、不包含、不链接 demo 文件。
 * 标准 NV12 的 Y/UV 连续存储；多平面 API 不等于必须有多个内存平面。
 * 帧数据复制并 QBUF 后才交给上层，耗时消费不会直接占用驱动缓冲区。
 */
#define _POSIX_C_SOURCE 200809L
#include "v4l2_capture.h"
#include "frame_queue.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CAPTURE_TYPE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define REQUEST_BUFFERS 4U
#define MAX_BUFFERS 32U
#define MAX_IMAGE_BYTES (64U * 1024U * 1024U)

typedef struct {
    void *address; /**< MMAP 地址，仅归采集模块持有。 */
    size_t length;
    bool mapped;  /**< 不以地址是否为 NULL 判断 mmap 成功。 */
} MappedBuffer;

struct IpcVideoCapture {
    int fd;
    bool allocated, streaming, started;
    MappedBuffer *maps;
    IpcVideoFormat format;
    IpcVideoCaptureStats stats;
    int64_t epoch_us, last_pts_us;
    int timestamp_mode; /**< 0 未选定，1 驱动单调时间，2 出队单调时间。 */
    uint32_t last_sequence;
    bool have_sequence;
    bool rkisp_field_compat; /**< 仅为 RKISP 逐行协商结果兼容 DQBUF 未填写 field。 */
    bool field_warning_sent; /**< 兼容提示仅打印一次，不逐帧刷屏。 */
};

/** @brief 执行 ioctl 并处理 EINTR；失败返回负 errno，便于跨层传播。 */
static int capture_ioctl(int fd, unsigned long request, void *argument, const char *name)
{
    int result;
    do { result = ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    if (result < 0) {
        int error = errno;
        if (error != EAGAIN)
            ipc_log_write(IPC_LOG_ERROR, "v4l2", "%s: %s", name, strerror(error));
        return -error;
    }
    return 0;
}
#define CAP_IOCTL(c, request, arg) capture_ioctl((c)->fd, request, arg, #request)

/** @brief 构造干净的多平面缓冲区描述，避免将 DQBUF 返回标志重新提交驱动。 */
static void prepare_buffer(struct v4l2_buffer *buffer, struct v4l2_plane *plane,
                           unsigned int index)
{
    memset(buffer, 0, sizeof(*buffer));
    memset(plane, 0, sizeof(*plane));
    buffer->type = CAPTURE_TYPE;
    buffer->memory = V4L2_MEMORY_MMAP;
    buffer->index = index;
    buffer->length = 1;
    buffer->m.planes = plane;
}

/** @brief 把指定 MMAP 缓冲区交给驱动；调用之后禁止再读取其内容。 */
static int queue_buffer(IpcVideoCapture *context, unsigned int index)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane plane;
    prepare_buffer(&buffer, &plane, index);
    plane.length = (unsigned int)context->maps[index].length;
    return CAP_IOCTL(context, VIDIOC_QBUF, &buffer);
}

/** @brief 检查节点能力，设置并通过 G_FMT 读回 720P NV12 实际布局。 */
static int configure_format(IpcVideoCapture *context, const IpcConfig *config)
{
    struct v4l2_capability cap = {0};
    struct v4l2_format format = {0};
    struct v4l2_pix_format_mplane *pix = &format.fmt.pix_mp;
    uint32_t caps;
    int result = CAP_IOCTL(context, VIDIOC_QUERYCAP, &cap);
    if (result < 0) return result;
    context->rkisp_field_compat = strncmp((const char *)cap.driver, "rkisp", 5) == 0;
    ipc_log_write(IPC_LOG_INFO, "v4l2", "driver=%.16s card=%.32s", cap.driver, cap.card);
    caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING))
        return -ENOTSUP;
    format.type = CAPTURE_TYPE;
    pix->width = config->video_width;
    pix->height = config->video_height;
    pix->pixelformat = V4L2_PIX_FMT_NV12;
    pix->field = V4L2_FIELD_NONE;
    result = CAP_IOCTL(context, VIDIOC_S_FMT, &format);
    if (result < 0) return result;
    memset(&format, 0, sizeof(format));
    format.type = CAPTURE_TYPE;
    result = CAP_IOCTL(context, VIDIOC_G_FMT, &format);
    if (result < 0) return result;
    ipc_log_write(IPC_LOG_INFO, "v4l2",
        "actual: %ux%u fourcc=%c%c%c%c planes=%u field=%u stride=%u sizeimage=%u",
        pix->width, pix->height, (int)(pix->pixelformat & 255),
        (int)((pix->pixelformat >> 8) & 255), (int)((pix->pixelformat >> 16) & 255),
        (int)((pix->pixelformat >> 24) & 255), pix->num_planes, pix->field,
        pix->plane_fmt[0].bytesperline, pix->plane_fmt[0].sizeimage);
    if (pix->width != 1280 || pix->height != 720 ||
        pix->pixelformat != V4L2_PIX_FMT_NV12 || pix->num_planes != 1 ||
        pix->field != V4L2_FIELD_NONE || pix->plane_fmt[0].bytesperline < pix->width)
        return -ENOTSUP;
    context->format.width = pix->width;
    context->format.height = pix->height;
    context->format.stride = pix->plane_fmt[0].bytesperline;
    /* 先用除法限制乘法大小，也对驱动异常返回值设立内存分配上限。 */
    if (context->format.stride > MAX_IMAGE_BYTES / (pix->height + pix->height / 2))
        return -EOVERFLOW;
    context->format.uv_offset = context->format.stride * pix->height;
    context->format.frame_size = context->format.stride * (pix->height + pix->height / 2);
    context->format.sizeimage = pix->plane_fmt[0].sizeimage;
    if (context->format.sizeimage < context->format.frame_size ||
        context->format.sizeimage > MAX_IMAGE_BYTES)
        return -EINVAL;
    /* 该 ISP 节点曾确认不支持 G_PARM；不把配置目标冒充驱动已设置的帧率。 */
    ipc_log_write(IPC_LOG_INFO, "v4l2", "target_fps=%u (not forced); actual rate measured at dequeue",
                  config->video_fps);
    return 0;
}

/** @brief 申请并映射全部驱动缓冲区；部分失败由统一 deinit 逆序回收。 */
static int allocate_buffers(IpcVideoCapture *context)
{
    struct v4l2_requestbuffers request = {0};
    int result;
    request.type = CAPTURE_TYPE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = REQUEST_BUFFERS;
    result = CAP_IOCTL(context, VIDIOC_REQBUFS, &request);
    if (result < 0) return result;
    context->allocated = true;
    if (request.count == 0 || request.count > MAX_BUFFERS) return -ENOMEM;
    context->maps = calloc(request.count, sizeof(*context->maps));
    if (context->maps == NULL) return -ENOMEM;
    context->format.buffers = request.count;
    for (unsigned int i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane plane;
        prepare_buffer(&buffer, &plane, i);
        result = CAP_IOCTL(context, VIDIOC_QUERYBUF, &buffer);
        if (result < 0) return result;
        if (buffer.length != 1 || plane.length < context->format.sizeimage ||
            plane.length > MAX_IMAGE_BYTES) return -EINVAL;
        context->maps[i].length = plane.length;
        context->maps[i].address = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, context->fd, plane.m.mem_offset);
        if (context->maps[i].address == MAP_FAILED) return -errno;
        context->maps[i].mapped = true;
        result = queue_buffer(context, i);
        if (result < 0) return result;
    }
    ipc_log_write(IPC_LOG_INFO, "v4l2", "MMAP buffers requested=%u granted=%u",
                  REQUEST_BUFFERS, request.count);
    return 0;
}

/** @brief 创建已配置但未 STREAMON 的上下文，失败保持调用者指针不变。 */
int ipc_video_capture_init(IpcVideoCapture **context, const IpcConfig *config)
{
    IpcVideoCapture *created;
    int result;
    if (context == NULL || *context != NULL || config == NULL ||
        config->video_width != 1280 || config->video_height != 720 ||
        memchr(config->video_device, '\0', sizeof(config->video_device)) == NULL ||
        config->video_device[0] == '\0') return -EINVAL;
    created = calloc(1, sizeof(*created));
    if (created == NULL) return -ENOMEM;
    created->fd = -1;
    created->last_pts_us = -1;
    created->fd = open(config->video_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (created->fd < 0) { result = -errno; goto fail; }
    result = configure_format(created, config);
    if (result < 0) goto fail;
    result = allocate_buffers(created);
    if (result < 0) goto fail;
    *context = created;
    return 0;
fail:
    ipc_log_write(IPC_LOG_ERROR, "v4l2", "initialization failed: %s", strerror(-result));
    ipc_video_capture_deinit(&created);
    return result;
}

/** @brief 启动流并记录共同时间起点，不重置已经运行过的统计。 */
int ipc_video_capture_start(IpcVideoCapture *context, int64_t epoch_us)
{
    enum v4l2_buf_type type = CAPTURE_TYPE;
    int result;
    if (context == NULL || context->started || epoch_us < 0) return -EINVAL;
    context->epoch_us = epoch_us;
    result = CAP_IOCTL(context, VIDIOC_STREAMON, &type);
    if (result < 0) return result;
    context->streaming = context->started = true;
    return 0;
}

/** @brief 安全地把驱动 timeval 转为微秒；不合法或溢出时返回 -1。 */
static int64_t driver_time_us(const struct timeval *timestamp)
{
    if (timestamp->tv_sec < 0 || timestamp->tv_usec < 0 || timestamp->tv_usec >= 1000000 ||
        (uint64_t)timestamp->tv_sec > (uint64_t)(INT64_MAX - timestamp->tv_usec) / 1000000)
        return -1;
    return (int64_t)timestamp->tv_sec * 1000000 + timestamp->tv_usec;
}

/** @brief 为一次采集固定时间戳来源，生成相对起点 PTS 并拒绝倒退。 */
static int assign_timestamp(IpcVideoCapture *context, const struct v4l2_buffer *buffer,
                            int64_t arrival, IpcRawFrame *frame)
{
    int64_t raw = driver_time_us(&buffer->timestamp);
    bool usable = (buffer->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
                  raw >= context->epoch_us && raw <= arrival;
    int64_t absolute;
    if (context->timestamp_mode == 0) {
        context->timestamp_mode = usable ? 1 : 2;
        ipc_log_write(usable ? IPC_LOG_INFO : IPC_LOG_WARN, "v4l2", "timestamp source: %s",
                      usable ? "driver CLOCK_MONOTONIC" : "dequeue CLOCK_MONOTONIC fallback (approximate)");
    }
    /* 不在两种时间源之间逐帧切换，避免未知偏移使时间线跳变。
     * 驱动源一旦选定却返回非法时间戳，则丢弃该帧并计数。 */
    if (context->timestamp_mode == 1 && !usable) return -EBADMSG;
    absolute = context->timestamp_mode == 1 ? raw : arrival;
    frame->pts_us = absolute - context->epoch_us;
    if (frame->pts_us < 0 || frame->pts_us <= context->last_pts_us) return -EBADMSG;
    frame->info.video.capture_timestamp_us = raw;
    frame->info.video.dequeue_monotonic_us = arrival;
    frame->info.video.timestamp_flags = buffer->flags &
        (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
    frame->info.video.timestamp_fallback = context->timestamp_mode == 2;
    return 0;
}

/** @brief 从有效 MMAP 帧逐行复制像素，保留步长并将应用侧行填充清零。 */
static int copy_frame(IpcVideoCapture *context, const struct v4l2_buffer *buffer,
                      const struct v4l2_plane *plane, int64_t arrival, IpcRawFrame **output)
{
    IpcRawFrame *frame;
    const IpcVideoFormat *format = &context->format;
    const uint8_t *source;
    /* bytesused 含 data_offset；只要求最后一行的有效像素存在，不读取尾部填充。 */
    size_t required = format->frame_size - format->stride + format->width;
    /* G_FMT 已严格验证为 FIELD_NONE。若 RKISP 在 DQBUF 中留下未填写的
     * FIELD_ANY(0)，它是未指定值，不等于隔行。仅对此驱动、此协商结果兼容；
     * 明确的 INTERLACED/TOP/BOTTOM 等以及其他驱动的 ANY 仍拒绝。
     * 这是平台兼容处理，不把 ANY 当成标准接口允许的正常逐帧返回值。 */
    bool field_ok = buffer->field == V4L2_FIELD_NONE ||
                    (context->rkisp_field_compat && buffer->field == V4L2_FIELD_ANY);
    int result;
    if (buffer->length != 1 || (buffer->flags & V4L2_BUF_FLAG_ERROR) ||
        !field_ok || plane->bytesused > context->maps[buffer->index].length ||
        plane->data_offset > plane->bytesused || plane->bytesused - plane->data_offset < required)
        return -EBADMSG;
    if (buffer->field == V4L2_FIELD_ANY && !context->field_warning_sent) {
        ipc_log_write(IPC_LOG_WARN, "v4l2", "RKISP DQBUF field=ANY(0); using negotiated progressive FIELD_NONE(1)");
        context->field_warning_sent = true;
    }
    frame = calloc(1, sizeof(*frame));
    if (frame == NULL) return -ENOMEM;
    result = assign_timestamp(context, buffer, arrival, frame);
    if (result < 0) {
        ++context->stats.timestamp_rejected;
        ipc_raw_frame_free(&frame);
        return result;
    }
    frame->data = calloc(1, format->frame_size);
    if (frame->data == NULL) { ipc_raw_frame_free(&frame); return -ENOMEM; }
    frame->type = IPC_MEDIA_VIDEO;
    frame->size = format->frame_size;
    frame->info.video.width = format->width;
    frame->info.video.height = format->height;
    frame->info.video.fourcc = V4L2_PIX_FMT_NV12;
    frame->info.video.y_stride = frame->info.video.uv_stride = format->stride;
    frame->info.video.uv_offset = format->uv_offset;
    frame->info.video.sequence = buffer->sequence;
    source = (const uint8_t *)context->maps[buffer->index].address + plane->data_offset;
    for (size_t row = 0; row < format->height + format->height / 2; ++row)
        memcpy(frame->data + row * format->stride, source + row * format->stride, format->width);
    *output = frame;
    return 0;
}

/** @brief 等待、出队、复制、归还一帧；错误帧也在安全索引范围内归还驱动。 */
int ipc_video_capture_read(IpcVideoCapture *context, IpcRawFrame **frame, int timeout_ms)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane plane;
    struct pollfd descriptor;
    IpcRawFrame *copied = NULL;
    int result, returned;
    int64_t arrival;
    if (context == NULL || !context->streaming || frame == NULL || *frame != NULL ||
        timeout_ms < 1 || timeout_ms > 1000) return -EINVAL;
    descriptor = (struct pollfd){.fd = context->fd, .events = POLLIN};
    result = poll(&descriptor, 1, timeout_ms);
    if (result < 0) return errno == EINTR ? 0 : -errno;
    if (result == 0) { ++context->stats.poll_timeouts; return 0; }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
    if (!(descriptor.revents & POLLIN)) return 0;
    prepare_buffer(&buffer, &plane, 0);
    result = CAP_IOCTL(context, VIDIOC_DQBUF, &buffer);
    if (result == -EAGAIN) { ++context->stats.would_block; return 0; }
    if (result < 0) return result;
    ++context->stats.dequeued;
    /* 驱动给出不可信索引时不能用于数组访问或 QBUF，交给停流统一回收。 */
    if (buffer.index >= context->format.buffers) return -EIO;
    if (context->stats.dequeued == 1)
        ipc_log_write(IPC_LOG_INFO, "v4l2",
                      "first DQBUF: index=%u planes=%u field=%u bytesused=%u offset=%u mapped=%zu flags=0x%x",
                      buffer.index, buffer.length, buffer.field, plane.bytesused,
                      plane.data_offset, context->maps[buffer.index].length, buffer.flags);
    if (context->have_sequence) {
        uint32_t delta = buffer.sequence - context->last_sequence;
        if (delta > 0 && delta < UINT32_C(0x80000000))
            context->stats.sequence_gaps += delta - 1;
        else
            ++context->stats.sequence_resets;
    }
    context->last_sequence = buffer.sequence;
    context->have_sequence = true;
    arrival = ipc_monotonic_us();
    result = arrival < 0 ? -EIO : copy_frame(context, &buffer, &plane, arrival, &copied);
    /* 先归还驱动，再发布应用帧；QBUF 失败时不能继续假装采集链路正常。 */
    returned = queue_buffer(context, buffer.index);
    if (returned < 0) { ipc_raw_frame_free(&copied); return returned; }
    if (result == -EBADMSG) {
        ++context->stats.invalid;
        if (context->stats.invalid == 1 || context->stats.invalid % 30 == 0)
            ipc_log_write(IPC_LOG_WARN, "v4l2", "discard invalid frame seq=%u planes=%u field=%u bytesused=%u offset=%u mapped=%zu required=%zu flags=0x%x",
                          buffer.sequence, buffer.length, buffer.field, plane.bytesused, plane.data_offset,
                          context->maps[buffer.index].length,
                          context->format.frame_size - context->format.stride + context->format.width, buffer.flags);
    }
    if (result < 0) return result;
    context->last_pts_us = copied->pts_us;
    if (context->stats.captured == 0) context->stats.first_arrival_us = arrival;
    context->stats.last_arrival_us = arrival;
    ++context->stats.captured;
    context->stats.timestamp_fallback += copied->info.video.timestamp_fallback ? 1 : 0;
    *frame = copied;
    return 1;
}

/** @brief 在采集线程退出后停止流；停流失败保留状态，deinit 会再次尝试。 */
int ipc_video_capture_stop(IpcVideoCapture *context)
{
    enum v4l2_buf_type type = CAPTURE_TYPE;
    int result;
    if (context == NULL) return -EINVAL;
    if (!context->streaming) return 0;
    result = CAP_IOCTL(context, VIDIOC_STREAMOFF, &type);
    if (result == 0) context->streaming = false;
    return result;
}

/** @brief 复制已协商布局，不把内部映射指针暴露给调用者。 */
int ipc_video_capture_get_format(const IpcVideoCapture *context, IpcVideoFormat *format)
{
    if (context == NULL || format == NULL) return -EINVAL;
    *format = context->format;
    return 0;
}

/** @brief 采集停止后复制统计；调用者负责避免与 read 并发。 */
int ipc_video_capture_get_stats(const IpcVideoCapture *context, IpcVideoCaptureStats *stats)
{
    if (context == NULL || stats == NULL) return -EINVAL;
    *stats = context->stats;
    return 0;
}

/** @brief 尽力回收全部资源并返回首个错误，既用于正常收尾，也用于初始化回滚。 */
int ipc_video_capture_deinit(IpcVideoCapture **context)
{
    IpcVideoCapture *destroyed;
    int result = 0, current;
    if (context == NULL || *context == NULL) return 0;
    destroyed = *context;
    if (destroyed->streaming) result = ipc_video_capture_stop(destroyed);
    for (unsigned int i = 0; i < destroyed->format.buffers; ++i) {
        if (destroyed->maps[i].mapped && munmap(destroyed->maps[i].address, destroyed->maps[i].length) < 0 && result == 0)
            result = -errno;
    }
    free(destroyed->maps);
    if (destroyed->allocated) {
        struct v4l2_requestbuffers request = {.type = CAPTURE_TYPE, .memory = V4L2_MEMORY_MMAP};
        current = CAP_IOCTL(destroyed, VIDIOC_REQBUFS, &request);
        if (result == 0) result = current;
    }
    if (destroyed->fd >= 0 && close(destroyed->fd) < 0 && result == 0) result = -errno;
    free(destroyed);
    *context = NULL;
    return result;
}
