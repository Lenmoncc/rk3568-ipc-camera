/* RK3568 最小完整预览 A：手写 V4L2 -> libv4lconvert -> BGR -> 保存/显示。
 * 单文件、应用单线程；不录像、不推流。q / ESC / Ctrl+C 退出。
 * 使用多平面 API，YU12/NV12 均要求一个内存平面。
 */
// 模块分工：core管理Mat，imgcodecs保存BMP，highgui管理窗口；libv4lconvert负责像素转换。
// Linux/V4L2头文件提供设备控制、内存映射、等待就绪及信号处理接口。
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>
#include <libv4lconvert.h>
#include <opencv2/highgui.hpp>
#include <ctime>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

// 日志统一写入stderr，方便用“2>文件名.log”保存；do/while(0)让宏作为单条语句使用。
#define DEBUG(...) do { std::fprintf(stderr, "[DEBUG] "); \
    std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define ERROR(...) do { std::fprintf(stderr, "[ERROR] "); \
    std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)

// 采集节点与窗口名称集中配置。窗口尺寸只影响显示，不改变摄像头输出分辨率。
static const char *DEVICE = "/dev/video0";
static const char *WINDOW = "A - libv4lconvert preview (q / ESC to exit)";
// 固定采集1280×720，请求4个MMAP缓冲区；实际分配数以驱动返回为准。
static const unsigned WIDTH = 1280, HEIGHT = 720, BUFFER_COUNT = 4;
// 手写采集用poll等待帧，超时单位为毫秒；超时后报错退出，避免无限等待。
static const int WAIT_MS = 3000;
// 信号处理函数与主循环共用的退出标志；sig_atomic_t适合在信号处理函数中赋值。
static volatile sig_atomic_t stop_requested = 0;
// 信号回调仅设置标志，不在回调内打印日志、调用OpenCV或释放设备，清理由正常流程完成。
static void on_signal(int) { stop_requested = 1; }

// 将系统调用错误转换为C++异常，由main统一记录。先保存errno，避免构造错误消息时丢失原因。
// operation是出错操作的名称；本函数通过throw退出，不正常返回。
static void system_error(const char *operation)
{
    const int saved_errno = errno;
    throw std::runtime_error(std::string(operation) + ": errno=" +
        std::to_string(saved_errno) + " (" + std::strerror(saved_errno) + ")");
}

// 执行V4L2控制请求。仅在信号打断（EINTR）且尚未请求退出时重试。
// 返回底层ioctl结果，错误码仍通过errno提供；不会把设备错误当作可重试错误。
static int ioctl_retry(int fd, unsigned long command, void *argument)
{
    int result;
    do { result = ioctl(fd, command, argument); }
    while (result < 0 && errno == EINTR && !stop_requested);
    return result;
}

// 带错误处理和成功日志的ioctl封装，主要用于初始化阶段。
// 失败时抛异常；采集循环需要自行区分EAGAIN，因此使用ioctl_retry直接判断。
static void checked_ioctl(int fd, unsigned long command, void *argument,
                          const char *name)
{
    if (ioctl_retry(fd, command, argument) < 0) system_error(name);
    DEBUG("%s OK", name);
}
// #command将请求常量转换为文字，日志会直接显示VIDIOC_S_FMT等名称。
// 宏使用当前作用域的fd，供Capture成员函数调用。
#define CHECK_IOCTL(command, argument) checked_ioctl(fd, command, argument, #command)

// 记录一个驱动缓冲区的用户态映射地址与映射长度；本结构自身不拥有独立分配的像素数组。
// MAP_FAILED表示尚未成功映射，供初始化失败后的清理逻辑判断。
struct Buffer {
    void *address = MAP_FAILED;
    size_t length = 0;
};

/* RAII：异常发生在初始化中途或采集中途，都释放已经获得的资源。 */
struct Capture {
    // Capture集中管理设备、MMAP映射与转换上下文的生命周期；fd=-1表示未打开或已关闭。
    int fd = -1;
    // 转换上下文在打开设备后创建，在关闭设备前销毁；它不替代V4L2取帧过程。
    v4lconvert_data *converter = nullptr;
    // 这两个结构描述转换函数输入/输出的连续像素内存，与设备使用的多平面格式结构分开。
    v4l2_format source_format = {}, destination_format = {};
    uint32_t pixel_format = V4L2_PIX_FMT_YUV420; // FOURCC 为 YU12。

    // 分别记录驱动缓冲区是否已申请、是否已启动流，支持在初始化中途失败时按状态释放。
    bool allocated = false, streaming = false;
    // 设备能力要求使用MPLANE API；即使YU12/NV12只有一个内存平面，也不能换成普通采集API。
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    std::vector<Buffer> buffers;
    // 本demo限定无行填充的YUV420：Y占宽×高字节，U/V合计占一半，故每帧为宽×高×3/2。
    size_t frame_bytes = static_cast<size_t>(WIDTH) * HEIGHT * 3 / 2;

    /* 每个查询/入队/出队操作都附带一个 v4l2_plane。
     * buffer.length 是平面数组元素数，不是图像字节数。
     */
    // 构造QUERYBUF/QBUF/DQBUF共用的描述结构；index用于指定查询或入队的缓冲区。
    // plane由调用方提供，必须在使用返回的buffer执行ioctl期间保持有效。
    // DQBUF时缓冲区编号由驱动填写，而不是由默认index决定。
    v4l2_buffer describe(v4l2_plane &plane, unsigned index = 0) const
    {
        // 清零结构及保留字段，避免把未初始化数据传入内核；buffer.m.planes指向调用方的plane。
        plane = v4l2_plane{};
        v4l2_buffer buffer = {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = &plane;
        return buffer;
    }

    // 初始化完整采集链路：打开设备、核对能力/格式、创建转换上下文、准备缓冲区、启动流。
    // 任何一步失败均抛异常；已获得的资源由Capture析构函数统一回收。
    void start()
    {
        DEBUG("open %s (O_RDWR | O_NONBLOCK)", DEVICE);
        // O_RDWR用于设备控制；O_NONBLOCK让无帧时的出队返回EAGAIN，等待工作交给poll。
        fd = open(DEVICE, O_RDWR | O_NONBLOCK);
        if (fd < 0) system_error("open");

        // 查询驱动能力；若声明DEVICE_CAPS，则使用当前节点的device_caps而不是整个设备的能力合集。
        v4l2_capability capability = {};
        CHECK_IOCTL(VIDIOC_QUERYCAP, &capability);
        const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps : capability.capabilities;
        DEBUG("driver=%.16s card=%.32s device_caps=0x%08x",
              capability.driver, capability.card, caps);
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING))
            throw std::runtime_error("Need VIDEO_CAPTURE_MPLANE + STREAMING");

        // S_FMT是请求并协商格式：驱动可能调整参数，不能假设请求值就是最终配置。
        // V4L2_FIELD_NONE表示逐行扫描；这里不调用S_PARM主动设置帧率。
        v4l2_format format = {};
        format.type = type;
        format.fmt.pix_mp.width = WIDTH;
        format.fmt.pix_mp.height = HEIGHT;
        format.fmt.pix_mp.pixelformat = pixel_format;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        CHECK_IOCTL(VIDIOC_S_FMT, &format);
        // S_FMT返回结构包含实际配置。FOURCC按四个字符打印，便于区分YU12、NV12等格式。
        const v4l2_pix_format_mplane &pix = format.fmt.pix_mp;
        DEBUG("actual=%ux%u fourcc=%c%c%c%c planes=%u stride=%u sizeimage=%u",
              pix.width, pix.height, int(pix.pixelformat & 255),
              int((pix.pixelformat >> 8) & 255), int((pix.pixelformat >> 16) & 255),
              int((pix.pixelformat >> 24) & 255), pix.num_planes,
              pix.plane_fmt[0].bytesperline, pix.plane_fmt[0].sizeimage);
        DEBUG("colorspace=%u ycbcr_enc=%u quantization=%u",
              pix.colorspace, pix.ycbcr_enc, pix.quantization);
        // 只接收本demo能够直接解释的布局：尺寸/格式匹配、1个内存平面、逐行且无行填充。
        // sizeimage是驱动要求的容量，可大于有效像素字节数；不能小于一帧有效数据。
        if (pix.width != WIDTH || pix.height != HEIGHT ||
            pix.pixelformat != pixel_format || pix.num_planes != 1 ||
            pix.field != V4L2_FIELD_NONE || pix.plane_fmt[0].bytesperline != WIDTH ||
            pix.plane_fmt[0].sizeimage < frame_bytes)
            throw std::runtime_error("Demo requires requested progressive 1280x720 YU12/NV12, "
                                     "one memory plane, stride=1280");

        // 设备始终通过 MPLANE API 采集。以下 fmt.pix 仅描述连续内存，
        // 交给 libv4lconvert；不可把设备的 fmt.pix_mp 直接当 fmt.pix 使用。
        source_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        source_format.fmt.pix.width = pix.width;
        source_format.fmt.pix.height = pix.height;
        source_format.fmt.pix.pixelformat = pix.pixelformat;
        source_format.fmt.pix.field = pix.field;
        source_format.fmt.pix.bytesperline = pix.plane_fmt[0].bytesperline;
        source_format.fmt.pix.sizeimage = static_cast<unsigned>(frame_bytes);
        source_format.fmt.pix.colorspace = pix.colorspace;
        source_format.fmt.pix.ycbcr_enc = pix.ycbcr_enc;
        source_format.fmt.pix.quantization = pix.quantization;
        source_format.fmt.pix.xfer_func = pix.xfer_func;
        // 输出为BGR24：每像素3字节，行跨度为宽×3，总容量为宽×高×3。
        // 复制色彩描述不等于库一定按全部色彩元数据选择转换矩阵，实际颜色仍需观察确认。
        destination_format = source_format;
        destination_format.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
        destination_format.fmt.pix.bytesperline = WIDTH * 3;
        destination_format.fmt.pix.sizeimage = WIDTH * HEIGHT * 3;
        // 明确给定内存布局，避免用面向设备协商的 try_format 改动采集格式。
        converter = v4lconvert_create(fd);
        if (!converter) throw std::runtime_error("v4lconvert_create failed");
        DEBUG("converter=libv4lconvert; destination=BGR24 stride=%u bytes=%u",
              destination_format.fmt.pix.bytesperline,
              destination_format.fmt.pix.sizeimage);

        // 请求驱动分配MMAP缓冲区；count只是期望数量，之后按返回数量建立映射记录。
        v4l2_requestbuffers request = {};
        request.type = type;
        request.memory = V4L2_MEMORY_MMAP;
        request.count = BUFFER_COUNT;
        CHECK_IOCTL(VIDIOC_REQBUFS, &request);
        allocated = true;
        DEBUG("buffers requested=%u granted=%u", BUFFER_COUNT, request.count);
        if (!request.count) throw std::runtime_error("No capture buffers");
        buffers.resize(request.count);
        // 逐个查询缓冲区，取得驱动给出的长度与映射偏移；偏移不是物理地址，不自行计算。
        for (unsigned i = 0; i < request.count; ++i) {
            v4l2_plane plane;
            v4l2_buffer buffer = describe(plane, i);
            CHECK_IOCTL(VIDIOC_QUERYBUF, &buffer);
            buffers[i].length = plane.length;
            // 将驱动缓冲区映射到当前进程，采集时可直接访问，避免先read到另一个原始数据数组。
            // MAP_SHARED建立共享映射；映射长度使用QUERYBUF返回的plane.length。
            buffers[i].address = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd, plane.m.mem_offset);
            if (buffers[i].address == MAP_FAILED) system_error("mmap");
            DEBUG("mmap index=%u length=%zu offset=%u address=%p", i,
                  buffers[i].length, plane.m.mem_offset, buffers[i].address);
            // 首次入队把空缓冲区交给驱动填充；正式启动前先排好所有可用缓冲区。
            CHECK_IOCTL(VIDIOC_QBUF, &buffer);
        }
        // 队列准备完成后启动采集流；只有成功后才标记streaming，便于准确清理。
        CHECK_IOCTL(VIDIOC_STREAMON, &type);
        streaming = true;
    }

    // 按状态清理资源，不抛异常：先停流，再解除映射、释放驱动缓冲区、销毁转换器、关闭设备。
    // 返回是否全部清理成功；某一步失败仍继续尝试后续步骤。状态复位后可安全再次调用。
    bool close_all() noexcept
    {
        bool ok = true;
        if (fd < 0) return ok;
        DEBUG("capture cleanup begin");
        // 停止驱动对采集队列的使用，再处理用户态映射；失败时记录错误并继续尽力清理。
        if (streaming) {
            if (ioctl_retry(fd, VIDIOC_STREAMOFF, &type) < 0) {
                ERROR("STREAMOFF: %s", std::strerror(errno)); ok = false;
            } else DEBUG("VIDIOC_STREAMOFF OK");
            streaming = false;
        }
        // 只解除已成功建立的映射，支持初始化过程中部分mmap成功、部分失败的情况。
        for (size_t i = 0; i < buffers.size(); ++i) {
            if (buffers[i].address != MAP_FAILED) {
                if (munmap(buffers[i].address, buffers[i].length) < 0) {
                    ERROR("munmap index=%zu: %s", i, std::strerror(errno)); ok = false;
                } else DEBUG("munmap index=%zu OK", i);
                buffers[i].address = MAP_FAILED;
            }
        }
        // REQBUFS(count=0)请求释放驱动分配的缓冲区；与munmap解除用户态映射是不同操作。
        if (allocated) {
            v4l2_requestbuffers request = {};
            request.type = type;
            request.memory = V4L2_MEMORY_MMAP;
            request.count = 0;
            if (ioctl_retry(fd, VIDIOC_REQBUFS, &request) < 0) {
                ERROR("REQBUFS(count=0): %s", std::strerror(errno)); ok = false;
            } else DEBUG("VIDIOC_REQBUFS(count=0) OK");
            allocated = false;
        }
        // 销毁转换器及其内部工作内存，此时fd仍有效；转换上下文不能跨设备关闭后继续使用。
        if (converter) {
            v4lconvert_destroy(converter);
            converter = nullptr;
            DEBUG("v4lconvert_destroy OK");
        }
        if (close(fd) < 0) { ERROR("close: %s", std::strerror(errno)); ok = false; }
        else DEBUG("close device OK");
        fd = -1;
        return ok;
    }
    // 析构兜底：异常离开作用域时执行清理；正常路径已清理后再次调用也会快速返回。
    ~Capture() { close_all(); }
};

// 程序入口：解析模式/格式，设置退出信号，然后运行单线程采集、转换与保存/显示循环。
// 默认YU12实时预览；--convert-only转换60帧并保存第60帧；--nv12明确请求NV12。
int main(int argc, char **argv)
{
    bool convert_only = false;
    uint32_t requested_format = V4L2_PIX_FMT_YUV420;
    // 参数可组合使用；--help打印说明后立即退出，未知参数不进入设备初始化。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--convert-only") == 0) convert_only = true;
        else if (std::strcmp(argv[i], "--nv12") == 0) requested_format = V4L2_PIX_FMT_NV12;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("Usage: preview_libv4l [--convert-only] [--nv12]\n"
                      "Default: YU12 -> libv4lconvert -> BGR live preview.\n"
                      "--convert-only: capture and convert 60 frames, save frame 60, no window.\n"
                      "--nv12: explicitly request NV12; conversion support depends on target library.");
            return 0;
        } else { ERROR("Unknown argument: %s", argv[i]); return 2; }
    }
    // 注册SIGINT（终端Ctrl+C）和SIGTERM处理函数，将退出请求交给主循环。
    struct sigaction action = {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) < 0 ||
        sigaction(SIGTERM, &action, nullptr) < 0) {
        ERROR("sigaction: %s", std::strerror(errno)); return 1;
    }
    // 默认返回失败；只有正常执行结束才置0，后续清理失败仍可将结果改回1。
    int result = 1;
    bool window_created = false;
    try {
        cv::setNumThreads(1);             // 应用及 OpenCV 计算保持单线程策略。
        cv::ocl::setUseOpenCL(false);     // 本阶段先用 CPU 格式转换。
        DEBUG("OpenCV=%s; mode=%s; pixel conversion=libv4lconvert", CV_VERSION,
              convert_only ? "convert 60 frames, save last" : "live preview");
        Capture capture;
        capture.pixel_format = requested_format;
        capture.start();
        // CV_8UC3表示8位无符号、3通道；此处为连续BGR内存，可直接作为转换函数输出。
        cv::Mat bgr(HEIGHT, WIDTH, CV_8UC3); // 独立缓冲区，库直接写入 BGR。
        // 仅在本次第60帧BMP保存成功后置true，避免提前Ctrl+C仍被记为任务4完成。
        bool saved = false;
        // frames为累计完成帧数，batch_frames用于约一秒统计窗口内的吞吐计算。
        unsigned long long frames = 0, batch_frames = 0;
        double conversion_ms = 0;
        // steady_clock为单调时钟，适合测量耗时，不受系统日期时间校准影响。
        auto batch_start = std::chrono::steady_clock::now();
        DEBUG("capture loop running; mode=%s; Ctrl+C to stop",
              convert_only ? "save frame 60" : "preview, q/ESC to exit");
        while (!stop_requested) {
            // POLLIN表示等待可读帧。poll只负责等待，不取走图像，真正取帧要继续调用DQBUF。
            struct pollfd poll_fd = {capture.fd, POLLIN, 0};
            const int ready = poll(&poll_fd, 1, WAIT_MS);
            if (stop_requested) break;
            // 区分信号打断、等待超时和设备异常；退出信号已在上一行优先处理。
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) system_error("poll");
            if (ready == 0) throw std::runtime_error("poll: no frame within 3000 ms");
            if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("poll: device error, revents=" +
                                         std::to_string(poll_fd.revents));
            if (!(poll_fd.revents & POLLIN)) continue;

            // DQBUF将一个已填充缓冲区交给应用；返回编号对应启动时映射好的buffers数组。
            // 非阻塞情况下EAGAIN表示这次没有可出队帧，回到poll等待，不将其当作致命错误。
            v4l2_plane plane;
            v4l2_buffer buffer = capture.describe(plane);
            if (ioctl_retry(capture.fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (stop_requested) break;
                if (errno == EAGAIN) continue;
                system_error("VIDIOC_DQBUF");
            }
            // 第一帧和每30帧打印一次详细日志，避免逐帧日志过多影响采集循环。
            const bool log_frame = (frames == 0 || (frames + 1) % 30 == 0);
            if (log_frame)
                DEBUG("DQBUF frame=%llu index=%u sequence=%u timestamp=%lld.%06lld "
                      "bytesused=%u offset=%u flags=0x%x", frames + 1, buffer.index,
                      buffer.sequence, (long long)buffer.timestamp.tv_sec,
                      (long long)buffer.timestamp.tv_usec, plane.bytesused,
                      plane.data_offset, buffer.flags);
            // 检查编号后再索引数组；bytesused包含data_offset，二者差值才是本次有效数据长度。
            // 同时检查驱动错误标志及长度边界，防止读取越界或将不完整帧交给转换库。
            if (buffer.index >= capture.buffers.size() ||
                (buffer.flags & V4L2_BUF_FLAG_ERROR) ||
                plane.bytesused > capture.buffers[buffer.index].length ||
                plane.data_offset > plane.bytesused ||
                plane.bytesused - plane.data_offset < capture.frame_bytes)
                throw std::runtime_error("Invalid/incomplete YUV420 frame");

            // 输入借用摄像头内存，输出写入独立 BGR；整个转换不调用 cvtColor。
            unsigned char *data = static_cast<unsigned char *>(
                capture.buffers[buffer.index].address) + plane.data_offset;
            const auto conversion_start = std::chrono::steady_clock::now();
            // 将已校验的原始帧交给转换库，src_size传有效像素长度，dest_size传BGR缓冲区容量。
            // 返回值为转换输出字节数；负数表示失败，此处不改用OpenCV转换来掩盖错误。
            const int converted = v4lconvert_convert(capture.converter,
                &capture.source_format, &capture.destination_format,
                data, static_cast<int>(capture.frame_bytes),
                bgr.data, static_cast<int>(capture.destination_format.fmt.pix.sizeimage));
            if (converted < 0)
                throw std::runtime_error(std::string("v4lconvert_convert failed: ") +
                    v4lconvert_get_error_message(capture.converter) +
                    "; no OpenCV conversion fallback. If using --nv12, retry default YU12.");
            // 当前固定尺寸BGR24必须输出完整宽×高×3字节；不足时不能直接显示未写满的图像。
            if (converted != static_cast<int>(capture.destination_format.fmt.pix.sizeimage))
                throw std::runtime_error("libv4lconvert returned unexpected BGR byte count");
            if (log_frame) DEBUG("v4lconvert_convert OK output_bytes=%d", converted);
            conversion_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - conversion_start).count();
            // 此后显示只访问 BGR，驱动可以立即复用原 YUV 缓冲区。
            // 转换成功后，原始帧不再被后续流程访问，此时归还缓冲区让驱动继续采下一帧。
            if (ioctl_retry(capture.fd, VIDIOC_QBUF, &buffer) < 0)
                system_error("VIDIOC_QBUF");
            if (log_frame) DEBUG("QBUF index=%u OK; BGR=%dx%d channels=%d",
                                 buffer.index, bgr.cols, bgr.rows, bgr.channels());
            // 转换保存模式也直接采摄像头，不读取前期文件；逐帧转换60帧。
            if (convert_only && frames + 1 == 60) {
                const char *filename = requested_format == V4L2_PIX_FMT_NV12
                    ? "frame_nv12_to_bgr.bmp" : "frame_yu12_to_bgr.bmp";
                // 文件编码使用已经转换好的BGR，不再做YU12/NV12颜色转换；输出到当前工作目录。
                // 同名BMP会覆盖，保存失败则抛异常。
                if (!cv::imwrite(filename, bgr))
                    throw std::runtime_error("BMP write failed");
                saved = true;
                DEBUG("saved frame=60 file=%s dimensions=%ux%u", filename, WIDTH, HEIGHT);
            }
            int key = -1;
            if (!convert_only) {
                // 第一帧转换成功后再创建窗口，先获得真实转换结果。
                if (!window_created) {
                    cv::namedWindow(WINDOW, cv::WINDOW_NORMAL);
                    window_created = true;
                    cv::resizeWindow(WINDOW, 960, 540);
                }
                // imshow提交本轮图像，waitKey同时处理窗口事件并读取按键；不能只显示而不处理事件。
                cv::imshow(WINDOW, bgr);
                key = cv::waitKey(1) & 0xff;
            }
            // 只有本轮取帧、转换/显示等步骤未抛异常，才计入完成帧数。
            ++frames; ++batch_frames;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - batch_start).count();
            // 按实际经过时间统计循环吞吐；loop_fps不是摄像头标称帧率，也不是屏幕刷新率。
            if (seconds >= 1.0) {
                DEBUG("frames=%llu loop_fps=%.2f avg_convert_ms=%.3f", frames,
                      batch_frames / seconds, conversion_ms / batch_frames);
                batch_frames = 0; conversion_ms = 0; batch_start = now;
            }
            // 按q/Q或ESC（键值27）退出；窗口需获得键盘焦点。
            if (key == 'q' || key == 'Q' || key == 27) break;
            // 保存模式完成60帧即退出；预览模式还检查用户是否关闭了窗口。
            if (convert_only && frames >= 60) break;
            if (window_created && cv::getWindowProperty(WINDOW, cv::WND_PROP_VISIBLE) < 1) break;
        }
        DEBUG("preview stopped; total_frames=%llu", frames);
        // 保存模式提前中断算未完成；预览模式收到正常退出请求则可返回0。
        if (convert_only && !saved) {
            DEBUG("conversion task interrupted before 60 frames; no completed output");
            result = 1;
        } else result = 0;
        // 正常路径显式清理以取得清理结果；异常路径由Capture析构函数兜底。
        if (!capture.close_all()) result = 1;
    // 捕获OpenCV异常与常规C++异常，记录错误后进入公共清理阶段。
    } catch (const cv::Exception &exception) {
        ERROR("OpenCV: %s", exception.what());
    } catch (const std::exception &exception) {
        ERROR("%s", exception.what());
    }
    // 仅在窗口确实创建后销毁窗口；清理异常也记录为失败，避免掩盖退出问题。
    if (window_created) {
        try { cv::destroyAllWindows(); }
        catch (const cv::Exception &exception) {
            ERROR("destroyAllWindows: %s", exception.what()); result = 1;
        }
    }
    // 进程退出码：0为正常结束，1为执行/清理失败；命令行参数错误另行返回2（若支持参数）。
    DEBUG("exit=%d", result);
    return result;
}
