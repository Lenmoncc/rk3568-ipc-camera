/** @file video_encoder.c
 * @brief 正式 MPP H.264 编码模块：应用 NV12 副本→DMA 缓冲→Annex B 输出。
 *
 * 同时最多一帧在途，收到完整输出才允许改写输入内存。这里优先保证所有权
 * 清晰，后续再基于实测优化 DMA-BUF 零拷贝；不依赖 demo 的任何实现。
 */
#define _POSIX_C_SOURCE 200809L
#include "video_encoder.h"
#include "log.h"
#include "timestamp.h"
#include <errno.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef IPC_WITH_MPP
#define IPC_WITH_MPP 0
#endif
#if IPC_WITH_MPP
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_venc_cfg.h>

#define ENCODER_WAIT_US INT64_C(3000000)
#define HEADER_CAPACITY 4096

struct IpcVideoEncoder {
    MppCtx ctx;
    MppApi *api;
    MppEncCfg cfg;
    MppBufferGroup group;
    MppBuffer input;
    MppFrame pending; /**< 仅持有未提交帧；put 成功后置空，由 MPP 经输出元数据归还。 */
    unsigned int width, height, stride, vertical_stride;
    size_t input_size;
    IpcH264Sink sink;
    void *opaque;
    IpcVideoEncoderStats stats;
    bool failed;
};

/** @brief 记录 MPP 原始错误码，再转换为项目统一的 errno 风格错误。 */
static int mpp_error(const char *operation, MPP_RET result)
{
    ipc_log_write(IPC_LOG_ERROR, "encoder", "%s failed: MPP ret=%d", operation, (int)result);
    return result == MPP_ERR_MALLOC || result == MPP_ERR_NOMEM ? -ENOMEM : -EIO;
}

/** @brief 非阻塞接口未就绪时短暂让出 CPU，并检查单调时钟截止时间。 */
static int wait_retry(int64_t deadline)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 2000000};
    int64_t now = ipc_monotonic_us();
    if (now < 0) return -EIO;
    if (now >= deadline) return -ETIMEDOUT;
    while (nanosleep(&delay, &delay) < 0)
        if (errno != EINTR) return -errno;
    return 0;
}

/** @brief 按键设置并逐项检查配置，避免不支持的键被静默忽略。 */
static int configure_encoder(IpcVideoEncoder *encoder, const IpcConfig *config)
{
    const struct { const char *key; RK_S32 value; } settings[] = {
        {"prep:width", (RK_S32)encoder->width}, {"prep:height", (RK_S32)encoder->height},
        {"prep:hor_stride", (RK_S32)encoder->stride},
        {"prep:ver_stride", (RK_S32)encoder->vertical_stride},
        {"prep:format", MPP_FMT_YUV420SP}, {"rc:mode", MPP_ENC_RC_MODE_CBR},
        /* MPP 的历史公开键名确实拼作 denorm；新版 denom 只是别名。
         * BSP 旧库可能不提供别名，因此输入/输出分母统一使用 denorm=1。 */
        {"rc:fps_in_flex", 0}, {"rc:fps_in_num", (RK_S32)encoder->stats.fps}, {"rc:fps_in_denorm", 1},
        {"rc:fps_out_flex", 0}, {"rc:fps_out_num", (RK_S32)encoder->stats.fps}, {"rc:fps_out_denorm", 1},
        {"rc:bps_target", (RK_S32)config->video_bitrate},
        {"rc:bps_max", (RK_S32)(config->video_bitrate * 17U / 16U)},
        {"rc:bps_min", (RK_S32)(config->video_bitrate * 15U / 16U)},
        {"rc:gop", (RK_S32)config->video_gop},
        {"rc:qp_init", -1}, {"rc:qp_min", 10}, {"rc:qp_max", 51},
        {"rc:qp_min_i", 10}, {"rc:qp_max_i", 51}, {"rc:qp_ip", 2},
        /* Baseline 配合默认简单参考结构只生成 I/P，避免 B 帧重排和额外延迟。 */
        {"codec:type", MPP_VIDEO_CodingAVC}, {"h264:profile", 66}, {"h264:level", 40},
        {"h264:cabac_en", 0}, {"h264:trans8x8", 0}
    };
    MPP_RET ret = mpp_enc_cfg_init(&encoder->cfg);
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (ret != MPP_OK) return mpp_error("mpp_enc_cfg_init", ret);
    ret = encoder->api->control(encoder->ctx, MPP_ENC_GET_CFG, encoder->cfg);
    if (ret != MPP_OK) return mpp_error("MPP_ENC_GET_CFG", ret);
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        ret = mpp_enc_cfg_set_s32(encoder->cfg, settings[i].key, settings[i].value);
        if (ret != MPP_OK) return mpp_error(settings[i].key, ret);
    }
    /* 这两个键在 MPP 中是 u32，必须使用匹配的访问接口。 */
    ret = mpp_enc_cfg_set_u32(encoder->cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    if (ret != MPP_OK) return mpp_error("rc:drop_mode", ret);
    ret = mpp_enc_cfg_set_u32(encoder->cfg, "split:mode", 0);
    if (ret != MPP_OK) return mpp_error("split:mode", ret);
    ret = encoder->api->control(encoder->ctx, MPP_ENC_SET_CFG, encoder->cfg);
    if (ret != MPP_OK) return mpp_error("MPP_ENC_SET_CFG", ret);
    ret = encoder->api->control(encoder->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    return ret == MPP_OK ? 0 : mpp_error("MPP_ENC_SET_HEADER_MODE", ret);
}

/** @brief 获取 Annex B SPS/PPS 并同步交付；栈上头数据不会逃逸回调。 */
static int emit_header(IpcVideoEncoder *encoder)
{
    uint8_t storage[HEADER_CAPACITY];
    MppPacket packet = NULL;
    MPP_RET ret = mpp_packet_init(&packet, storage, sizeof(storage));
    int result;
    if (ret != MPP_OK) return mpp_error("header packet init", ret);
    mpp_packet_set_length(packet, 0);
    ret = encoder->api->control(encoder->ctx, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret != MPP_OK) result = mpp_error("MPP_ENC_GET_HDR_SYNC", ret);
    else {
        IpcH264Packet output = {.data = mpp_packet_get_pos(packet),
                               .size = mpp_packet_get_length(packet), .header = true, .pts_us = -1};
        if (output.data == NULL || output.size == 0 || output.size > sizeof(storage)) result = -EBADMSG;
        else {
            result = encoder->sink(encoder->opaque, &output);
            if (result == 0) encoder->stats.bytes += output.size;
        }
    }
    mpp_packet_deinit(&packet);
    return result;
}

/** @brief 非阻塞提交当前 pending 帧；仅对明确的暂未就绪状态重试。 */
static int submit_frame(IpcVideoEncoder *encoder)
{
    int64_t start = ipc_monotonic_us();
    if (start < 0) return -EIO;
    for (;;) {
        MPP_RET ret = encoder->api->encode_put_frame(encoder->ctx, encoder->pending);
        int result;
        if (ret == MPP_OK) {
            /* 非阻塞 MPP 接管帧描述，随后经输出包 KEY_INPUT_FRAME 归还。
             * 尚未取到输出时的异常清理交给 mpp_destroy，不能再自行 deinit。 */
            encoder->pending = NULL;
            return 0;
        }
        /* MPP 的异步输入在内部队列忙时也可能返回通用 MPP_NOK。 */
        if (ret != MPP_NOK && ret != MPP_ERR_BUFFER_FULL && ret != MPP_ERR_TIMEOUT)
            return mpp_error("encode_put_frame", ret);
        result = wait_retry(start + ENCODER_WAIT_US);
        if (result < 0) return result;
    }
}

/** @brief 释放输出包及异步接口归还的输入帧；返回是否收到输入帧所有权。 */
static bool release_output(MppPacket *packet)
{
    MppFrame returned = NULL;
    bool found = false;
    if (*packet != NULL && mpp_packet_has_meta(*packet) &&
        mpp_meta_get_frame(mpp_packet_get_meta(*packet), KEY_INPUT_FRAME, &returned) == MPP_OK && returned != NULL) {
        mpp_frame_deinit(&returned);
        found = true;
    }
    if (*packet != NULL) mpp_packet_deinit(packet);
    return found;
}

/** @brief 收齐一帧或 EOS，逐包同步交付并检查输出 PTS，分片未结束不复用输入。 */
static int receive_output(IpcVideoEncoder *encoder, bool finishing, int64_t pts)
{
    int64_t start = ipc_monotonic_us();
    bool key_frame = false, had_data = false, frame_returned = false;
    if (start < 0) return -EIO;
    for (;;) {
        MppPacket packet = NULL;
        MPP_RET ret = encoder->api->encode_get_packet(encoder->ctx, &packet);
        int result = 0;
        bool done = false;
        if (ret != MPP_OK && ret != MPP_NOK && ret != MPP_ERR_TIMEOUT) {
            if (packet != NULL) release_output(&packet);
            return mpp_error("encode_get_packet", ret);
        }
        if (packet != NULL) {
            RK_S32 intra = 0;
            IpcH264Packet output = {.data = mpp_packet_get_pos(packet),
                .size = mpp_packet_get_length(packet), .pts_us = mpp_packet_get_pts(packet),
                .eos = mpp_packet_get_eos(packet) != 0,
                .frame_end = !mpp_packet_is_partition(packet) || mpp_packet_is_eoi(packet)};
            if (mpp_packet_has_meta(packet))
                (void)mpp_meta_get_s32(mpp_packet_get_meta(packet), KEY_OUTPUT_INTRA, &intra);
            key_frame = key_frame || intra != 0;
            output.key_frame = key_frame;
            /* 同时只有一帧在途；非 EOS 输出必须对应当前输入，不能伪造 PTS。 */
            if ((!finishing && (output.eos || output.pts_us != pts)) ||
                (finishing && output.size != 0) || (output.size && output.data == NULL)) result = -EBADMSG;
            if (result == 0) result = encoder->sink(encoder->opaque, &output);
            if (result == 0) {
                encoder->stats.bytes += output.size;
                if (output.size) { ++encoder->stats.packets; had_data = true; }
                if (finishing) {
                    done = output.eos;
                    encoder->stats.eos = done;
                } else if (output.frame_end) {
                    if (!had_data) result = -EBADMSG;
                    else {
                        ++encoder->stats.encoded;
                        if (key_frame) ++encoder->stats.keyframes;
                        if (encoder->stats.encoded == 1) encoder->stats.first_pts_us = pts;
                        encoder->stats.last_pts_us = pts;
                        done = true;
                    }
                }
            }
            frame_returned = release_output(&packet) || frame_returned;
            if (result < 0) return result;
            if (done) return frame_returned ? 0 : -EBADMSG;
        }
        result = wait_retry(start + ENCODER_WAIT_US);
        if (result < 0) return result;
    }
}

/** @brief 校验原始 NV12 的可读边界，再逐行复制到 16 字节对齐的 DMA 布局。 */
static int copy_input(IpcVideoEncoder *encoder, const IpcRawFrame *frame)
{
    size_t y_end, uv_end;
    uint8_t *destination;
    if (!frame || !frame->data || frame->type != IPC_MEDIA_VIDEO ||
        frame->info.video.fourcc != V4L2_PIX_FMT_NV12 || frame->info.video.width != encoder->width ||
        frame->info.video.height != encoder->height || frame->pts_us < 0 ||
        (encoder->stats.submitted && frame->pts_us <= encoder->stats.last_pts_us) ||
        frame->info.video.y_stride < encoder->width || frame->info.video.uv_stride < encoder->width)
        return -EINVAL;
    /* 用除法先校验范围，避免恶意 stride/offset 使乘加溢出。 */
    if (frame->size < encoder->width || frame->info.video.uv_offset > frame->size - encoder->width ||
        frame->info.video.y_stride > (frame->size - encoder->width) / (encoder->height - 1) ||
        frame->info.video.uv_stride > (frame->size - frame->info.video.uv_offset - encoder->width) /
                                      (encoder->height / 2 - 1)) return -EINVAL;
    y_end = (encoder->height - 1) * frame->info.video.y_stride + encoder->width;
    uv_end = frame->info.video.uv_offset + (encoder->height / 2 - 1) * frame->info.video.uv_stride + encoder->width;
    if (frame->info.video.uv_offset < y_end || uv_end > frame->size) return -EINVAL;
    destination = mpp_buffer_get_ptr(encoder->input);
    if (destination == NULL) return -EIO;
    /* 默认 uncached DRM 分配，不申请 CACHABLE；不依赖新版 MPP cache-sync API。
     * 若今后改为 cached/imported DMA-BUF，必须成对增加 CPU/DMA 同步。 */
    memset(destination, 0, encoder->input_size);
    for (size_t row = 0; row < encoder->height; ++row)
        memcpy(destination + row * encoder->stride, frame->data + row * frame->info.video.y_stride, encoder->width);
    destination += (size_t)encoder->stride * encoder->vertical_stride;
    for (size_t row = 0; row < encoder->height / 2; ++row)
        memcpy(destination + row * encoder->stride,
               frame->data + frame->info.video.uv_offset + row * frame->info.video.uv_stride, encoder->width);
    return 0;
}

/** @brief 创建一份 MPP 帧描述，EOS 帧不挂载图像缓冲，避免重复编码末帧。 */
static int prepare_frame(IpcVideoEncoder *encoder, bool eos, int64_t pts)
{
    MPP_RET ret = mpp_frame_init(&encoder->pending);
    if (ret != MPP_OK) return mpp_error("mpp_frame_init", ret);
    mpp_frame_set_width(encoder->pending, encoder->width);
    mpp_frame_set_height(encoder->pending, encoder->height);
    mpp_frame_set_hor_stride(encoder->pending, encoder->stride);
    mpp_frame_set_ver_stride(encoder->pending, encoder->vertical_stride);
    mpp_frame_set_fmt(encoder->pending, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(encoder->pending, pts);
    mpp_frame_set_eos(encoder->pending, eos);
    if (!eos) mpp_frame_set_buffer(encoder->pending, encoder->input);
    return 0;
}
#endif

/** @brief 告知上层本次二进制是否启用 MPP，禁用版本不会假装完成编码。 */
bool ipc_video_encoder_available(void)
{
    return IPC_WITH_MPP != 0;
}

/** @brief 初始化编码器及输出头，任何失败均回收已获得的资源。 */
int ipc_video_encoder_init(IpcVideoEncoder **context, const IpcConfig *config,
                           unsigned int fps, IpcH264Sink sink, void *opaque)
{
#if IPC_WITH_MPP
    IpcVideoEncoder *encoder;
    MPP_RET ret;
    MppPollType timeout = MPP_POLL_NON_BLOCK;
    int result;
    if (!context || *context || !config || !sink || fps < 1 || fps > 30 ||
        config->video_width != 1280 || config->video_height != 720 ||
        strcmp(config->video_codec, "h264") || strcmp(config->video_pixel_format, "NV12") ||
        config->video_bitrate < 64000 || config->video_bitrate > 20000000 ||
        config->video_gop < 1 || config->video_gop > 300) return -EINVAL;
    encoder = calloc(1, sizeof(*encoder));
    if (!encoder) return -ENOMEM;
    encoder->width = config->video_width;
    encoder->height = config->video_height;
    encoder->stride = (encoder->width + 15U) & ~15U;
    encoder->vertical_stride = (encoder->height + 15U) & ~15U;
    encoder->input_size = (size_t)encoder->stride * encoder->vertical_stride * 3 / 2;
    encoder->stats.fps = fps;
    encoder->stats.first_pts_us = encoder->stats.last_pts_us = -1;
    encoder->sink = sink;
    encoder->opaque = opaque;
    ret = mpp_create(&encoder->ctx, &encoder->api);
    if (ret != MPP_OK) { result = mpp_error("mpp_create", ret); goto failed; }
    ret = encoder->api->control(encoder->ctx, MPP_SET_INPUT_TIMEOUT, &timeout);
    if (ret != MPP_OK) { result = mpp_error("MPP_SET_INPUT_TIMEOUT", ret); goto failed; }
    ret = encoder->api->control(encoder->ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret != MPP_OK) { result = mpp_error("MPP_SET_OUTPUT_TIMEOUT", ret); goto failed; }
    ret = mpp_init(encoder->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) { result = mpp_error("mpp_init (check MPP library/device)", ret); goto failed; }
    result = configure_encoder(encoder, config);
    if (result < 0) goto failed;
    ret = mpp_buffer_group_get_internal(&encoder->group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) { result = mpp_error("DRM buffer group", ret); goto failed; }
    ret = mpp_buffer_get(encoder->group, &encoder->input, encoder->input_size);
    if (ret != MPP_OK) { result = mpp_error("DMA input allocation", ret); goto failed; }
    result = emit_header(encoder);
    if (result < 0) goto failed;
    ipc_log_write(IPC_LOG_INFO, "encoder", "MPP H.264 Baseline: %ux%u stride=%u/%u fps=%u bitrate=%u gop=%u; Annex B, I/P, no frame drop",
                  encoder->width, encoder->height, encoder->stride, encoder->vertical_stride,
                  fps, config->video_bitrate, config->video_gop);
    *context = encoder;
    return 0;
failed:
    ipc_video_encoder_deinit(&encoder);
    return result;
#else
    (void)context; (void)config; (void)fps; (void)sink; (void)opaque;
    ipc_log_write(IPC_LOG_ERROR, "encoder", "MPP support disabled; rebuild with WITH_MPP=1 using the board SDK");
    return -ENOTSUP;
#endif
}

/** @brief 同步完成一帧编码；错误后保留输入资源到 MPP 销毁，防止提前释放。 */
int ipc_video_encoder_send(IpcVideoEncoder *encoder, const IpcRawFrame *frame)
{
#if IPC_WITH_MPP
    int result;
    if (!encoder || encoder->failed || encoder->stats.eos) return -EINVAL;
    result = copy_input(encoder, frame);
    if (result == 0) result = prepare_frame(encoder, false, frame->pts_us);
    if (result == 0) result = submit_frame(encoder);
    if (result == 0) {
        ++encoder->stats.submitted;
        result = receive_output(encoder, false, frame->pts_us);
    }
    if (result < 0) encoder->failed = true;

    return result;
#else
    (void)encoder; (void)frame;
    return -ENOTSUP;
#endif
}

/** @brief 排空阶段发送独立 EOS 并等待确认；EOS 不增加图像帧计数。 */
int ipc_video_encoder_finish(IpcVideoEncoder *encoder)
{
#if IPC_WITH_MPP
    int result;
    if (!encoder || encoder->failed) return -EINVAL;
    if (encoder->stats.eos) return 0;
    result = prepare_frame(encoder, true, encoder->stats.last_pts_us < 0 ? 0 : encoder->stats.last_pts_us);
    if (result == 0) result = submit_frame(encoder);
    if (result == 0) result = receive_output(encoder, true, 0);
    if (result < 0) encoder->failed = true;

    return result;
#else
    (void)encoder;
    return -ENOTSUP;
#endif
}

/** @brief 复制编码计数和时间戳，不在采集运行期间跨线程读取。 */
int ipc_video_encoder_get_stats(const IpcVideoEncoder *encoder, IpcVideoEncoderStats *stats)
{
#if IPC_WITH_MPP
    if (!encoder || !stats) return -EINVAL;
    *stats = encoder->stats;
    return 0;
#else
    (void)encoder; (void)stats;
    return -ENOTSUP;
#endif
}

/** @brief 销毁硬件上下文后再释放描述与缓冲；尽力清理所有资源并返回首个错误。 */
int ipc_video_encoder_deinit(IpcVideoEncoder **context)
{
#if IPC_WITH_MPP
    IpcVideoEncoder *encoder;
    int result = 0;
    MPP_RET ret;
    if (!context || !*context) return 0;
    encoder = *context;
    if (encoder->ctx) {
        ret = mpp_destroy(encoder->ctx);
        if (ret != MPP_OK) result = mpp_error("mpp_destroy", ret);
    }
    if (encoder->pending) mpp_frame_deinit(&encoder->pending);
    if (encoder->input) {
        ret = mpp_buffer_put(encoder->input);
        if (ret != MPP_OK && result == 0) result = mpp_error("mpp_buffer_put", ret);
    }
    if (encoder->group) {
        ret = mpp_buffer_group_put(encoder->group);
        if (ret != MPP_OK && result == 0) result = mpp_error("mpp_buffer_group_put", ret);
    }
    if (encoder->cfg) mpp_enc_cfg_deinit(encoder->cfg);
    free(encoder);
    *context = NULL;
    return result;
#else
    if (context) *context = NULL;
    return 0;
#endif
}
