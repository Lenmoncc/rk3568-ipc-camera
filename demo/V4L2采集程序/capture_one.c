/* RK3568 / rkisp_mainpath: 单线程、单文件、MMAP 采集。
 * 多平面 API + NV12 单内存平面；固定 1280x720，不设置传感器帧率。
 * 流程：open -> QUERYCAP -> S_FMT -> REQBUFS -> QUERYBUF/mmap
 *       -> QBUF -> STREAMON -> poll/DQBUF/保存/QBUF -> 清理。
 * 输出是紧凑 NV12 原始数据；已有同名输出文件会被覆盖。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define DEVICE "/dev/video0"
#define WIDTH 1280U
#define HEIGHT 720U
#define FRAME_COUNT 1U
#define OUTPUT "frame_1280x720_nv12.yuv"
#define BUFFER_COUNT 4U
#define TIMEOUT_MS 3000
#define DEBUG(...) do { fprintf(stderr, "[DEBUG] "); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define ERROR(...) do { fprintf(stderr, "[ERROR] "); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

struct mapped_buffer { void *addr; size_t length; };
static volatile sig_atomic_t stop_requested;

/* 信号处理函数只设置标志，资源由主线程统一释放。 */
static void handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

/* 每个 ioctl 都输出名称；EAGAIN 表示暂时无帧，由采集循环重新等待。 */
static int debug_ioctl(int fd, unsigned long request, void *arg, const char *name)
{
    int ret;
    do { ret = ioctl(fd, request, arg); } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        int saved_errno = errno;
        if (saved_errno != EAGAIN)
            ERROR("%s: errno=%d (%s)", name, saved_errno, strerror(saved_errno));
        errno = saved_errno;
    } else {
        DEBUG("%s OK", name);
    }
    return ret;
}
#define XIOCTL(command, arg) debug_ioctl(fd, command, arg, #command)

int main(void)
{
    int fd = -1, streaming = 0, allocated = 0, result = EXIT_FAILURE;
    unsigned int mapped_count = 0, saved = 0;
    FILE *output = NULL;
    struct mapped_buffer *buffers = NULL;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {0};
    struct v4l2_requestbuffers req = {0};
    struct sigaction sa = {0};
    const size_t frame_size = (size_t)WIDTH * HEIGHT * 3 / 2;

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        ERROR("sigaction: %s", strerror(errno));
        goto cleanup;
    }

    /* 1. 打开设备。非阻塞方式配合 poll，避免没有图像时一直卡住。 */
    DEBUG("open %s; request NV12 %ux%u; frames=%u; output=%s",
          DEVICE, WIDTH, HEIGHT, FRAME_COUNT, OUTPUT);
    fd = open(DEVICE, O_RDWR | O_NONBLOCK);
    if (fd < 0) { ERROR("open: %s", strerror(errno)); goto cleanup; }

    /* 2. 查询当前节点能力，优先使用 device_caps。 */
    if (XIOCTL(VIDIOC_QUERYCAP, &cap) < 0) goto cleanup;
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                    ? cap.device_caps : cap.capabilities;
    DEBUG("driver=%.16s card=%.32s caps=0x%08x", cap.driver, cap.card, caps);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
        ERROR("Need VIDEO_CAPTURE_MPLANE and STREAMING capabilities");
        goto cleanup;
    }

    /* 3. S_FMT 会返回实际生效的配置，后续必须检查返回值。 */
    fmt.type = type;
    fmt.fmt.pix_mp.width = WIDTH;
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (XIOCTL(VIDIOC_S_FMT, &fmt) < 0) goto cleanup;
    struct v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
    DEBUG("actual: %ux%u fourcc=%c%c%c%c planes=%u stride=%u sizeimage=%u",
          pix->width, pix->height,
          (int)(pix->pixelformat & 255), (int)((pix->pixelformat >> 8) & 255),
          (int)((pix->pixelformat >> 16) & 255), (int)((pix->pixelformat >> 24) & 255),
          pix->num_planes, pix->plane_fmt[0].bytesperline, pix->plane_fmt[0].sizeimage);
    /* 最小 demo 只接受紧凑 NV12；不悄悄把不支持的布局写成错误画面。 */
    if (pix->width != WIDTH || pix->height != HEIGHT ||
        pix->pixelformat != V4L2_PIX_FMT_NV12 || pix->num_planes != 1 ||
        pix->field != V4L2_FIELD_NONE || pix->plane_fmt[0].bytesperline != WIDTH ||
        pix->plane_fmt[0].sizeimage < frame_size) {
        ERROR("Demo requires %ux%u NV12, one memory plane, stride=%u, progressive",
              WIDTH, HEIGHT, WIDTH);
        goto cleanup;
    }

    /* 4. 申请驱动缓冲区；实际数量以 REQBUFS 返回值为准。 */
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = BUFFER_COUNT;
    if (XIOCTL(VIDIOC_REQBUFS, &req) < 0) goto cleanup;
    allocated = 1;
    DEBUG("buffers requested=%u granted=%u", BUFFER_COUNT, req.count);
    if (!req.count) { ERROR("No buffers allocated"); goto cleanup; }
    buffers = calloc(req.count, sizeof(*buffers));
    if (!buffers) { ERROR("calloc: %s", strerror(errno)); goto cleanup; }

    /* 5. 查询、映射每个缓冲区，再把它交给驱动。
     * 即使只有一个内存平面，也必须传 v4l2_plane 数组。
     * buf.length 是数组元素数；plane.length 才是内存字节数。
     */
    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = {0};
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = &plane;
        if (XIOCTL(VIDIOC_QUERYBUF, &buf) < 0) goto cleanup;
        buffers[i].length = plane.length;
        buffers[i].addr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, plane.m.mem_offset);
        if (buffers[i].addr == MAP_FAILED) {
            ERROR("mmap index=%u: %s", i, strerror(errno));
            goto cleanup;
        }
        ++mapped_count;
        DEBUG("mmap index=%u offset=%u length=%zu addr=%p",
              i, plane.m.mem_offset, buffers[i].length, buffers[i].addr);
        if (XIOCTL(VIDIOC_QBUF, &buf) < 0) goto cleanup;
    }

    output = fopen(OUTPUT, "wb");
    if (!output) { ERROR("fopen %s: %s", OUTPUT, strerror(errno)); goto cleanup; }
    /* 6. 已有缓冲区入队后，再启动视频流。 */
    if (XIOCTL(VIDIOC_STREAMON, &type) < 0) goto cleanup;
    streaming = 1;

    /* 7. 单线程循环：等待 -> 取帧 -> 写文件 -> 归还。 */
    while (saved < FRAME_COUNT && !stop_requested) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, TIMEOUT_MS);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { ERROR("poll: %s", strerror(errno)); goto cleanup; }
        if (!ready) { ERROR("No frame within %d ms", TIMEOUT_MS); goto cleanup; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ERROR("poll revents=0x%x", (unsigned int)pfd.revents);
            goto cleanup;
        }
        if (!(pfd.revents & POLLIN)) continue;

        struct v4l2_plane plane = {0};
        struct v4l2_buffer buf = {0};
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = &plane;
        if (XIOCTL(VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            goto cleanup;
        }
        DEBUG("frame=%u/%u index=%u sequence=%u timestamp=%lld.%06lld "
              "bytesused=%u data_offset=%u flags=0x%x",
              saved + 1, FRAME_COUNT, buf.index, buf.sequence,
              (long long)buf.timestamp.tv_sec, (long long)buf.timestamp.tv_usec,
              plane.bytesused, plane.data_offset, buf.flags);
        if (buf.index >= req.count || (buf.flags & V4L2_BUF_FLAG_ERROR) ||
            plane.bytesused > buffers[buf.index].length ||
            plane.data_offset > plane.bytesused ||
            plane.bytesused - plane.data_offset < frame_size) {
            ERROR("Invalid/incomplete frame; see metadata above");
            goto cleanup;
        }
        /* bytesused 含 data_offset；排除起始偏移及尾部多余容量。
         * 已检查 stride == WIDTH，只写紧凑 NV12 的一帧大小。
         */
        unsigned char *data = (unsigned char *)buffers[buf.index].addr + plane.data_offset;
        if (fwrite(data, 1, frame_size, output) != frame_size) {
            ERROR("fwrite: %s", strerror(errno));
            goto cleanup;
        }
        ++saved;
        DEBUG("saved=%u frame_bytes=%zu total_bytes=%zu", saved, frame_size,
              (size_t)saved * frame_size);
        if (XIOCTL(VIDIOC_QBUF, &buf) < 0) goto cleanup;
    }
    if (stop_requested) DEBUG("Interrupted; keeping partial output");
    else result = EXIT_SUCCESS;

cleanup:
    /* 8. 正常结束与中途失败共用出口；只清理成功创建的资源。 */
    DEBUG("cleanup begin");
    if (streaming && XIOCTL(VIDIOC_STREAMOFF, &type) < 0) result = EXIT_FAILURE;
    for (unsigned int i = 0; i < mapped_count; ++i) {
        if (munmap(buffers[i].addr, buffers[i].length) < 0) {
            ERROR("munmap index=%u: %s", i, strerror(errno));
            result = EXIT_FAILURE;
        } else DEBUG("munmap index=%u OK", i);
    }
    free(buffers);
    if (allocated) {
        req.count = 0;  /* 所有映射解除后，显式释放驱动缓冲区。 */
        if (XIOCTL(VIDIOC_REQBUFS, &req) < 0) result = EXIT_FAILURE;
    }
    if (output && fclose(output) != 0) {
        ERROR("fclose (flush output): %s", strerror(errno));
        result = EXIT_FAILURE;
    }
    if (fd >= 0 && close(fd) < 0) {
        ERROR("close device: %s", strerror(errno));
        result = EXIT_FAILURE;
    }
    DEBUG("%s: saved=%u/%u frames, output=%s",
          result == EXIT_SUCCESS ? "DONE" : "FAILED/STOPPED", saved, FRAME_COUNT, OUTPUT);
    return result;
}
