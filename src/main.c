/* main.c — 骨架构建入口，尚未创建任何业务线程。
 * 当前仅显示状态，不读取配置、不打开设备、不推流、不录像。
 * 返回 0 仅表示骨架程序已运行，不表示任何 IPC 功能通过验证。
 * 后续顺序：配置/日志 → 队列 → 模块初始化 → 线程 → 有序退出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "v4l2_capture.h"
#include "alsa_capture.h"
#include "video_encoder.h"
#include "audio_encoder.h"
#include "rtmp_output.h"
#include "mp4_output.h"
#include "timestamp.h"
#include "log.h"

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
            puts("Usage: ipc_camera [--help]");
            puts("Scaffold only. Configuration and IPC functions are not implemented.");
            return EXIT_SUCCESS;
        }
        fputs("Unsupported option: scaffold accepts only --help.\n", stderr);
        return EXIT_FAILURE;
    }
    puts("RK3568 IPC scaffold: build and executable startup only.");
    puts("Target: 1280x720 NV12 -> MPP H.264; ALSA -> AAC; RTMP + MP4.");
    puts("NOT IMPLEMENTED: config loading, queues, capture, encoding and outputs.");
    return EXIT_SUCCESS;
}
