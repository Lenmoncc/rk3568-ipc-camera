/* RK3568 最小完整预览 B：OpenCV VideoCapture(CAP_V4L2) -> BGR -> 显示。
 * 单文件、应用单线程；必须使用 V4L2 后端，不回退到 GStreamer。
 * NV12 到 BGR 交给 VideoCapture 后端；检查实际返回的 Mat。
 */
// 模块分工：core管理Mat，videoio负责采集后端，highgui负责窗口与按键；本文件不直接调用libv4lconvert。
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <string>

// 日志统一写入stderr，方便用“2>文件名.log”保存；do/while(0)让宏作为单条语句使用。
#define DEBUG(...) do { std::fprintf(stderr, "[DEBUG] "); \
    std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define ERROR(...) do { std::fprintf(stderr, "[ERROR] "); \
    std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)

// 采集节点与窗口名称集中配置。窗口尺寸只影响显示，不改变摄像头输出分辨率。
static const char *DEVICE = "/dev/video0";
static const char *WINDOW = "B - OpenCV V4L2 preview (q / ESC to exit)";
// 请求摄像头输出1280×720；窗口的初始960×540大小仅影响显示区域。
static const int WIDTH = 1280, HEIGHT = 720;
// 信号处理函数与主循环共用的退出标志；sig_atomic_t适合在信号处理函数中赋值。
static volatile sig_atomic_t stop_requested = 0;
// 信号回调仅设置标志，不在回调内打印日志、调用OpenCV或释放设备，清理由正常流程完成。
static void on_signal(int) { stop_requested = 1; }

// 设置一个VideoCapture属性并记录请求值、是否被后端接受、查询返回值。
// property是属性编号，value是目标值，name用于可读日志；本函数不自行判定整个采集配置成功。
static void set_property(cv::VideoCapture &capture, int property, double value,
                         const char *name)
{
    // 后端支持情况存在差异，set返回值与get报告值都要记录，最终还需核对取回的图像。
    const bool accepted = capture.set(property, value);
    DEBUG("set %s requested=%.0f accepted=%s reported=%.3f", name, value,
          accepted ? "YES" : "NO", capture.get(property));
    // 某些后端属性查询不完整；最后统一检查真实配置和第一帧。
}

// 程序入口：注册退出信号 → 打开V4L2后端 → 请求并核对格式 → 持续读取与显示 → 释放资源。
// 设备控制和缓冲区操作由VideoCapture封装，本文件不手写REQBUFS、mmap、DQBUF、QBUF。
int main()
{
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
    // 采集对象放在try块外，使正常结束与异常结束都能经过下方显式release流程。
    cv::VideoCapture capture;
    try {
        // 限制OpenCV计算线程并关闭OpenCL，便于本阶段观察CPU处理流程；依赖库仍可能有内部线程。
        cv::setNumThreads(1);
        cv::ocl::setUseOpenCL(false);
        DEBUG("OpenCV=%s; open %s with explicit CAP_V4L2", CV_VERSION, DEVICE);
        // 明确指定CAP_V4L2，避免自动选择其他后端导致实验结果混淆。
        // 能否使用当前多平面节点取决于板端OpenCV后端实现；打开失败时直接记录并退出。
        if (!capture.open(DEVICE, cv::CAP_V4L2))
            throw std::runtime_error("CAP_V4L2 open failed. Check device ownership and "
                "this OpenCV build's VIDEO_CAPTURE_MPLANE support. No backend fallback.");
        DEBUG("backend=%s", capture.getBackendName().c_str());
        // 查询实际后端再次确认采集路径，不能只凭设备打开成功认定使用了V4L2。
        if (static_cast<int>(capture.get(cv::CAP_PROP_BACKEND)) != cv::CAP_V4L2)
            throw std::runtime_error("Unexpected backend; CAP_V4L2 is required");

        // FOURCC由四个字符编码为整数；此处请求设备输出NV12，交给后端转换为显示所需图像。
        const int nv12 = cv::VideoWriter::fourcc('N', 'V', '1', '2');
        set_property(capture, cv::CAP_PROP_FOURCC, nv12, "FOURCC(NV12)");
        // 宽高分别请求，驱动/后端可能调整，以下会重新读取并核对最终报告值。
        set_property(capture, cv::CAP_PROP_FRAME_WIDTH, WIDTH, "WIDTH");
        set_property(capture, cv::CAP_PROP_FRAME_HEIGHT, HEIGHT, "HEIGHT");
        // 期望后端使用4个采集缓冲区；是否支持该属性以日志为准，不假定请求必然生效。
        set_property(capture, cv::CAP_PROP_BUFFERSIZE, 4, "BUFFERSIZE");
        // OpenCV 的 CONVERT_RGB 名称虽写 RGB，常规彩色输出 Mat 是 BGR。
        set_property(capture, cv::CAP_PROP_CONVERT_RGB, 1, "CONVERT_RGB");
        // 在所有请求完成后读取配置；FPS仅查询，不主动设置，不将报告值视为实测处理帧率。
        const int actual_fourcc = static_cast<int>(capture.get(cv::CAP_PROP_FOURCC));
        const double actual_width = capture.get(cv::CAP_PROP_FRAME_WIDTH);
        const double actual_height = capture.get(cv::CAP_PROP_FRAME_HEIGHT);
        DEBUG("actual=%.0fx%.0f fourcc=%c%c%c%c fps_reported=%.3f buffers_reported=%.0f",
              actual_width, actual_height, actual_fourcc & 255, (actual_fourcc >> 8) & 255,
              (actual_fourcc >> 16) & 255, (actual_fourcc >> 24) & 255,
              capture.get(cv::CAP_PROP_FPS), capture.get(cv::CAP_PROP_BUFFERSIZE));
        // 本demo要求后端报告的尺寸和格式与请求一致，避免将其他输出布局按NV12条件解释。
        if (actual_width != WIDTH || actual_height != HEIGHT || actual_fourcc != nv12)
            throw std::runtime_error("Backend did not report requested 1280x720 NV12");

        // 创建可调整大小的窗口；Qt/Wayland连接由运行环境提供，程序本身不启动Weston。
        cv::namedWindow(WINDOW, cv::WINDOW_NORMAL);
        window_created = true;
        cv::resizeWindow(WINDOW, 960, 540);
        // read负责填充图像Mat；应用不直接持有或归还驱动MMAP缓冲区。
        cv::Mat bgr;
        // frames为累计完成帧数，batch_frames用于约一秒统计窗口内的吞吐计算。
        unsigned long long frames = 0, batch_frames = 0;
        // 累计read耗时，包括等帧、采集和后端内部处理，不能直接与手写版纯转换耗时比较。
        double read_ms = 0;
        // steady_clock为单调时钟，适合测量耗时，不受系统日期时间校准影响。
        auto batch_start = std::chrono::steady_clock::now();
        DEBUG("preview running; q / ESC / Ctrl+C to stop; read wait is backend-controlled");
        while (!stop_requested) {
            // read内部完成取帧及图像提取；是否阻塞和等待多久由后端决定。
            // Ctrl+C只设置标志，通常需等read返回后才能进入退出流程。
            const auto read_start = std::chrono::steady_clock::now();
            const bool got_frame = capture.read(bgr);
            if (stop_requested) break;
            // 同时检查读取状态和数据是否为空，避免将无效图像交给imshow。
            if (!got_frame || bgr.empty())
                throw std::runtime_error("VideoCapture.read failed or returned an empty frame");
            read_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - read_start).count();
            // 第一帧及每30帧打印图像属性；step是相邻两行起点之间的字节跨度，可能包含行填充。
            if (frames == 0 || (frames + 1) % 30 == 0)
                DEBUG("read frame=%llu Mat=%dx%d type=%d channels=%d step=%zu",
                      frames + 1, bgr.cols, bgr.rows, bgr.type(), bgr.channels(),
                      static_cast<size_t>(bgr.step));
            // 要求8位3通道、1280×720的图像；通道顺序依赖后端转换约定，类型检查本身不能证明颜色正确。
            if (bgr.cols != WIDTH || bgr.rows != HEIGHT || bgr.type() != CV_8UC3)
                throw std::runtime_error("Expected 1280x720 CV_8UC3 BGR after backend "
                    "conversion. Raw/other layouts are not silently reinterpreted.");
            // 提交本帧给窗口显示；后面的waitKey既等待按键，也驱动窗口事件处理。
            cv::imshow(WINDOW, bgr);
            // 短暂处理窗口事件，取按键低8位；未按键时不会匹配q或ESC。
            const int key = cv::waitKey(1) & 0xff;
            // 只有本轮取帧、转换/显示等步骤未抛异常，才计入完成帧数。
            ++frames; ++batch_frames;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - batch_start).count();
            // 按实际经过时间统计循环吞吐；loop_fps不是摄像头标称帧率，也不是屏幕刷新率。
            if (seconds >= 1.0) {
                DEBUG("frames=%llu loop_fps=%.2f avg_read_ms=%.3f", frames,
                      batch_frames / seconds, read_ms / batch_frames);
                batch_frames = 0; read_ms = 0; batch_start = now;
            }
            // 按q/Q或ESC（键值27）退出；窗口需获得键盘焦点。
            if (key == 'q' || key == 'Q' || key == 27) break;
            // 用户关闭窗口也结束循环，随后走与按键退出相同的清理流程。
            if (cv::getWindowProperty(WINDOW, cv::WND_PROP_VISIBLE) < 1) break;
        }
        DEBUG("preview stopped; total_frames=%llu", frames);
        // 循环由正常退出请求结束时记为成功；执行过程中抛异常则保留默认失败状态。
        result = 0;
    // 捕获OpenCV异常与常规C++异常，记录错误后进入公共清理阶段。
    } catch (const cv::Exception &exception) {
        ERROR("OpenCV: %s", exception.what());
    } catch (const std::exception &exception) {
        ERROR("%s", exception.what());
    }
    // 退出时由 VideoCapture 后端停止采集、释放内部缓冲区和设备。
    try {
        // 由后端停止设备采集并释放内部缓冲区和设备句柄；应用无需再手动调用STREAMOFF或munmap。
        capture.release();
        DEBUG("VideoCapture.release OK");
    } catch (const cv::Exception &exception) {
        ERROR("VideoCapture.release: %s", exception.what()); result = 1;
    }
    // 仅在窗口确实创建后销毁窗口；清理异常也记录为失败，避免掩盖退出问题。
    if (window_created) {
        try { cv::destroyAllWindows(); }
        catch (const cv::Exception &exception) {
            ERROR("destroyAllWindows: %s", exception.what()); result = 1;
        }
    }
    // 进程退出码：0为正常预览结束，1为采集、显示或清理失败。
    DEBUG("exit=%d", result);
    return result;
}
