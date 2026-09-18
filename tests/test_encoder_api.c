/** @file test_encoder_api.c
 * @brief 编码公开接口边界测试：输入布局/PTS、EOS 幂等及所有权；不访问硬件。
 */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "video_encoder.h"
#include <errno.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUST(x) do { if (!(x)) { fprintf(stderr, "API invariant: line %d: %s\n", __LINE__, #x); abort(); } } while (0)

/** @brief 同步检查回调数据；只读取，不缓存借用指针。 */
static int check_packet(void *opaque, const IpcH264Packet *packet)
{
    unsigned int *count = opaque;
    MUST(packet->size == 0 || packet->data != NULL);
    if (!packet->header && !packet->eos) { MUST(packet->pts_us == 100000); ++*count; }
    return 0;
}

/** @brief 构造带不等 Y/UV 步长的有效图像，验证编码模块并非假设紧凑布局。 */
static IpcRawFrame make_frame(void)
{
    IpcRawFrame frame = {.type = IPC_MEDIA_VIDEO, .pts_us = 100000};
    frame.info.video.width = 1280; frame.info.video.height = 720;
    frame.info.video.fourcc = V4L2_PIX_FMT_NV12;
    frame.info.video.y_stride = 1296; frame.info.video.uv_stride = 1312;
    frame.info.video.uv_offset = 1296U * 720U + 32;
    frame.size = frame.info.video.uv_offset + 1312U * 359U + 1280;
    frame.data = malloc(frame.size); MUST(frame.data);
    memset(frame.data, 0xee, frame.size);
    for (size_t row = 0; row < 720; ++row) memset(frame.data + row * 1296, 2, 1280);
    for (size_t row = 0; row < 360; ++row) memset(frame.data + frame.info.video.uv_offset + row * 1312, 128, 1280);
    return frame;
}

/** @brief 验证初始化参数、有效编码、结束幂等和每种非法布局的独立回滚。 */
int main(int argc, char **argv)
{
    IpcConfig config;
    IpcVideoEncoder *encoder = NULL;
    IpcVideoEncoderStats stats;
    IpcRawFrame frame = make_frame();
    unsigned int calls = 0;
    MUST(argc == 2 && ipc_config_load(argv[1], &config) == 0);
    MUST(ipc_video_encoder_init(NULL, &config, 25, check_packet, &calls) == -EINVAL);
    MUST(ipc_video_encoder_init(&encoder, &config, 0, check_packet, &calls) == -EINVAL);
    MUST(ipc_video_encoder_init(&encoder, &config, 31, check_packet, &calls) == -EINVAL);
    MUST(ipc_video_encoder_init(&encoder, &config, 25, NULL, &calls) == -EINVAL);
    MUST(ipc_video_encoder_init(&encoder, &config, 25, check_packet, &calls) == 0);
    MUST(ipc_video_encoder_send(encoder, &frame) == 0 && calls == 1);
    MUST(ipc_video_encoder_finish(encoder) == 0 && ipc_video_encoder_finish(encoder) == 0);
    MUST(ipc_video_encoder_send(encoder, &frame) == -EINVAL);
    MUST(ipc_video_encoder_get_stats(encoder, &stats) == 0 && stats.encoded == 1 && stats.eos);
    MUST(ipc_video_encoder_deinit(&encoder) == 0 && !encoder);
    MUST(ipc_video_encoder_deinit(&encoder) == 0 && ipc_video_encoder_deinit(NULL) == 0);
    for (unsigned int mode = 0; mode < 9; ++mode) {
        IpcRawFrame broken = frame;
        switch (mode) {
        case 0: broken.data = NULL; break;
        case 1: broken.size--; break;
        case 2: broken.info.video.y_stride = SIZE_MAX; break;
        case 3: broken.info.video.uv_stride = SIZE_MAX; break;
        case 4: broken.info.video.uv_offset = SIZE_MAX; break;
        case 5: broken.info.video.uv_offset = 0; break;
        case 6: broken.pts_us = -1; break;
        case 7: broken.info.video.fourcc = V4L2_PIX_FMT_YUYV; break;
        case 8: broken.type = IPC_MEDIA_AUDIO; break;
        }
        MUST(ipc_video_encoder_init(&encoder, &config, 25, check_packet, &calls) == 0);
        MUST(ipc_video_encoder_send(encoder, &broken) == -EINVAL);
        MUST(ipc_video_encoder_finish(encoder) == -EINVAL);
        MUST(ipc_video_encoder_deinit(&encoder) == 0);
    }
    MUST(ipc_video_encoder_init(&encoder, &config, 25, check_packet, &calls) == 0);
    MUST(ipc_video_encoder_send(encoder, &frame) == 0);
    MUST(ipc_video_encoder_send(encoder, &frame) == -EINVAL); /* 拒绝重复时间戳。 */
    MUST(ipc_video_encoder_deinit(&encoder) == 0);
    free(frame.data);
    puts("PASS: encoder API bounds, PTS, EOS and ownership checks.");
    return 0;
}
