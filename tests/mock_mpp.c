/** @file mock_mpp.c
 * @brief 仅供主机测试的 MPP 故障注入后端；使用官方公开头文件，不做真实编码。
 *
 * 严格模拟非阻塞 MPP 的帧所有权：put 成功转交，输出 KEY_INPUT_FRAME 归还，
 * 未归还的在途帧在 destroy 时回收。像素在 get 阶段才检查，捕获过早改写。
 */
#define _POSIX_C_SOURCE 200809L
#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_venc_cfg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUST(x) do { if (!(x)) { fprintf(stderr, "MPP MOCK invariant: %s:%d %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef struct { uint8_t *data; size_t size; unsigned int refs; } Buffer;
typedef struct { Buffer *buffer; RK_U32 width, height, stride, vertical, eos; RK_S64 pts; MppFrameFormat fmt; } Frame;
typedef struct { uint8_t *data; size_t size, length; int owned, partition, eoi, eos, intra; RK_S64 pts; Frame *returned; } Packet;
typedef struct { char key[48]; RK_S32 value; } Setting;
typedef struct { Setting entries[48]; size_t count; } Configuration;
typedef struct { Frame *inflight; Configuration *config; unsigned int submitted, get_calls, part; } Context;
static unsigned int contexts, groups, buffers, frames, packets, configurations;
static int registered;

/** @brief 读取当前测试场景，每个子进程独立注入一种错误。 */
static int is_case(const char *name)
{
    const char *value = getenv("IPC_MOCK_MPP_CASE");
    return value && strcmp(value, name) == 0;
}

/** @brief 子进程退出时验证全部模拟句柄与引用已归零。 */
static void check_cleanup(void)
{
    MUST(!contexts && !groups && !buffers && !frames && !packets && !configurations);
    fprintf(stderr, "MPP_MOCK_CLEANUP_OK\n");
}

/** @brief 从记录的配置中查询一个键，缺少必需配置即失败。 */
static RK_S32 value(Configuration *config, const char *key)
{
    for (size_t i = 0; i < config->count; ++i)
        if (strcmp(config->entries[i].key, key) == 0) return config->entries[i].value;
    MUST(0);
    return 0;
}

/** @brief 模拟控制接口，并检查帧率、码控、配置头和非阻塞模式。 */
static MPP_RET mock_control(MppCtx opaque, MpiCmd command, MppParam parameter)
{
    Context *ctx = opaque;
    switch (command) {
    case MPP_SET_INPUT_TIMEOUT:
    case MPP_SET_OUTPUT_TIMEOUT:
        MUST(*(MppPollType *)parameter == MPP_POLL_NON_BLOCK);
        return is_case("control-fail") ? MPP_ERR_VALUE : MPP_OK;
    case MPP_ENC_GET_CFG:
        return is_case("get-cfg-fail") ? MPP_ERR_VALUE : MPP_OK;
    case MPP_ENC_SET_CFG:
        ctx->config = parameter;
        MUST(value(ctx->config, "prep:width") == 1280 && value(ctx->config, "prep:height") == 720);
        MUST(value(ctx->config, "prep:format") == MPP_FMT_YUV420SP);
        MUST(value(ctx->config, "rc:fps_in_num") == value(ctx->config, "rc:fps_out_num"));
        MUST(value(ctx->config, "rc:fps_in_denorm") == 1 && value(ctx->config, "rc:fps_out_denorm") == 1);
        MUST(value(ctx->config, "rc:fps_in_flex") == 0 && value(ctx->config, "rc:fps_out_flex") == 0);
        MUST(value(ctx->config, "rc:drop_mode") == MPP_ENC_RC_DROP_FRM_DISABLED);
        MUST(value(ctx->config, "h264:profile") == 66 && value(ctx->config, "split:mode") == 0);
        return is_case("set-cfg-fail") ? MPP_ERR_VALUE : MPP_OK;
    case MPP_ENC_SET_HEADER_MODE:
        MUST(*(MppEncHeaderMode *)parameter == MPP_ENC_HEADER_MODE_EACH_IDR);
        return is_case("header-mode-fail") ? MPP_ERR_VALUE : MPP_OK;
    case MPP_ENC_GET_HDR_SYNC: {
        Packet *packet = parameter;
        static const uint8_t header[] = {0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0xce};
        MUST(packet->length == 0 && packet->size >= sizeof(header));
        if (is_case("header-fail")) return MPP_ERR_VPUHW;
        memcpy(packet->data, header, sizeof(header));
        packet->length = is_case("empty-header") ? 0 : sizeof(header);
        return MPP_OK;
    }
    default: MUST(0); return MPP_ERR_VALUE;
    }
}

/** @brief 接管帧描述，延后读取像素以检测缓冲区过早改写。 */
static MPP_RET mock_put(MppCtx opaque, MppFrame frame)
{
    Context *ctx = opaque;
    Frame *input = frame;
    static unsigned int tries;
    if (is_case("put-fail")) return MPP_ERR_VPUHW;
    if (is_case("put-timeout")) return MPP_ERR_BUFFER_FULL;
    if (is_case("put-busy-once") && tries++ == 0) return MPP_NOK;
    MUST(!ctx->inflight && input);
    MUST(input->width == 1280 && input->height == 720 && input->fmt == MPP_FMT_YUV420SP);
    MUST(input->stride == 1280 && input->vertical == 720);
    MUST((input->eos && !input->buffer) || (!input->eos && input->buffer));
    ctx->inflight = input;
    ctx->part = 0;
    if (!input->eos) ++ctx->submitted;
    return MPP_OK;
}

/** @brief 验证整帧复制结果和异步存活，再生成带 PTS 的模拟编码包。 */
static MPP_RET mock_get(MppCtx opaque, MppPacket *output)
{
    Context *ctx = opaque;
    Frame *input = ctx->inflight;
    Packet *packet;
    *output = NULL;
    MUST(input != NULL);
    if (is_case("get-fail")) return MPP_ERR_VPUHW;
    if (is_case("get-timeout") || (input->eos && is_case("eos-timeout"))) return MPP_NOK;
    if (is_case("get-empty-once") && ctx->get_calls++ == 0) return MPP_OK;
    if (is_case("get-nok-once") && ctx->get_calls++ == 0) return MPP_NOK;
    if (!input->eos) {
        uint8_t first = input->buffer->data[0];
        MUST(input->buffer->size == 1280U * 720U * 3U / 2U);
        for (size_t i = 0; i < 1280U * 720U; ++i) MUST(input->buffer->data[i] == first);
        for (size_t i = 1280U * 720U; i < input->buffer->size; ++i) MUST(input->buffer->data[i] == 128);
    }
    packet = calloc(1, sizeof(*packet)); MUST(packet);
    ++packets;
    packet->owned = 1;
    packet->size = 32;
    packet->data = calloc(1, packet->size); MUST(packet->data);
    packet->length = input->eos || is_case("empty-packet") ? 0 : packet->size;
    if (input->eos && is_case("eos-with-data")) packet->length = packet->size;
    packet->eos = input->eos || is_case("unexpected-eos");
    packet->pts = input->pts + (is_case("wrong-pts") ? 1 : 0);
    packet->partition = is_case("partition");
    packet->eoi = !packet->partition || input->eos || ctx->part == 1;
    packet->intra = (ctx->submitted - 1) % (unsigned int)value(ctx->config, "rc:gop") == 0;
    packet->data[3] = 1;
    packet->data[4] = packet->intra ? 0x65 : 0x41;
    packet->data[5] = input->eos ? 0 : input->buffer->data[0];
    memcpy(packet->data + 8, &input->pts, sizeof(input->pts));
    /* 空 EOS 可不带输入帧：分别模拟库内部立即释放、保留到 destroy 两种生命周期。
     * 普通图像帧缺少归还元数据仍是错误，不能被 EOS 兼容分支放行。 */
    if (input->eos && is_case("eos-no-meta")) {
        MppFrame finished = input;
        ctx->inflight = NULL;
        mpp_frame_deinit(&finished);
    } else if (packet->eoi && !is_case("missing-returned-frame") &&
               !(input->eos && is_case("eos-no-frame"))) {
        packet->returned = input;
        ctx->inflight = NULL;
    }
    ++ctx->part;
    *output = packet;
    return MPP_OK;
}

/** @brief 创建模拟上下文，注册一次资源回收检查。 */
MPP_RET mpp_create(MppCtx *context, MppApi **api)
{
    static MppApi methods = {.encode_put_frame = mock_put, .encode_get_packet = mock_get, .control = mock_control};
    if (!registered) { MUST(atexit(check_cleanup) == 0); registered = 1; }
    if (is_case("create-fail")) return MPP_ERR_MALLOC;
    *context = calloc(1, sizeof(Context)); MUST(*context);
    ++contexts; *api = &methods;
    return MPP_OK;
}

/** @brief 确认请求的是 H.264 编码，并注入硬件初始化失败。 */
MPP_RET mpp_init(MppCtx context, MppCtxType type, MppCodingType coding)
{
    MUST(context && type == MPP_CTX_ENC && coding == MPP_VIDEO_CodingAVC);
    return is_case("init-fail") ? MPP_ERR_VPU_CODEC_INIT : MPP_OK;
}

/** @brief 销毁时回收尚在 MPP 内部的帧；应用不能再次释放该帧。 */
MPP_RET mpp_destroy(MppCtx opaque)
{
    Context *ctx = opaque;
    if (ctx->inflight) { MppFrame frame = ctx->inflight; mpp_frame_deinit(&frame); }
    free(ctx); --contexts;
    return is_case("destroy-fail") ? MPP_ERR_VPUHW : MPP_OK;
}

/** @brief 分配配置记录器。 */
MPP_RET mpp_enc_cfg_init(MppEncCfg *config)
{
    if (is_case("cfg-init-fail")) return MPP_ERR_MALLOC;
    *config = calloc(1, sizeof(Configuration)); MUST(*config); ++configurations;
    return MPP_OK;
}

/** @brief 释放配置记录器。 */
MPP_RET mpp_enc_cfg_deinit(MppEncCfg config)
{
    free(config); --configurations; return MPP_OK;
}

/** @brief 按官方配置键集合验证名称，避免模拟器无条件接受拼错/不支持的键。 */
static int supported_s32_key(const char *key)
{
    static const char *const supported[] = {
        "prep:width", "prep:height", "prep:hor_stride", "prep:ver_stride", "prep:format",
        "rc:mode", "rc:fps_in_flex", "rc:fps_in_num", "rc:fps_in_denorm",
        "rc:fps_out_flex", "rc:fps_out_num", "rc:fps_out_denorm",
        "rc:bps_target", "rc:bps_max", "rc:bps_min", "rc:gop", "rc:qp_init",
        "rc:qp_min", "rc:qp_max", "rc:qp_min_i", "rc:qp_max_i", "rc:qp_ip",
        "codec:type", "h264:profile", "h264:level", "h264:cabac_en", "h264:trans8x8"
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
        if (strcmp(supported[i], key) == 0) return 1;
    return 0;
}

/** @brief 记录配置键，旧库场景拒绝 denom 别名，现代库场景将别名映射到 denorm。 */
MPP_RET mpp_enc_cfg_set_s32(MppEncCfg opaque, const char *key, RK_S32 setting)
{
    Configuration *config = opaque;
    MUST(strcmp(key, "rc:drop_mode") && strcmp(key, "split:mode"));
    if (is_case("cfg-key-fail")) return MPP_ERR_VALUE;
    if (!strcmp(key, "rc:fps_in_denom") || !strcmp(key, "rc:fps_out_denom")) {
        if (is_case("legacy-fps-keys")) return MPP_NOK;
        key = !strcmp(key, "rc:fps_in_denom") ? "rc:fps_in_denorm" : "rc:fps_out_denorm";
    }
    if (!supported_s32_key(key)) return MPP_NOK;
    MUST(config->count < 48);
    snprintf(config->entries[config->count].key, 48, "%s", key);
    config->entries[config->count++].value = setting;
    return MPP_OK;
}

/** @brief 记录无符号配置项，并检查正式模块没有误用有符号访问接口。 */
MPP_RET mpp_enc_cfg_set_u32(MppEncCfg opaque, const char *key, RK_U32 setting)
{
    Configuration *config = opaque;
    MUST(!strcmp(key, "rc:drop_mode") || !strcmp(key, "split:mode"));
    MUST(config->count < 48);
    snprintf(config->entries[config->count].key, 48, "%s", key);
    config->entries[config->count++].value = (RK_S32)setting;
    return MPP_OK;
}

/** @brief 分配独立的非缓存 DRM 缓冲组，拒绝误用普通 malloc 类型。 */
MPP_RET mpp_buffer_group_get(MppBufferGroup *group, MppBufferType type, MppBufferMode mode, const char *tag, const char *caller)
{
    (void)tag; (void)caller;
    MUST(type == MPP_BUFFER_TYPE_DRM && mode == MPP_BUFFER_INTERNAL);
    if (is_case("group-fail")) return MPP_ERR_MALLOC;
    *group = malloc(1); MUST(*group); ++groups; return MPP_OK;
}

/** @brief 缓冲组释放前检查所有缓冲引用已回收。 */
MPP_RET mpp_buffer_group_put(MppBufferGroup group)
{
    MUST(!buffers); free(group); --groups; return MPP_OK;
}

/** @brief 创建引用计数输入缓冲，模拟分配失败。 */
MPP_RET mpp_buffer_get_with_tag(MppBufferGroup group, MppBuffer *output, size_t size, const char *tag, const char *caller)
{
    Buffer *buffer;
    (void)tag; (void)caller; MUST(group);
    if (is_case("buffer-fail")) return MPP_ERR_MALLOC;
    buffer = calloc(1, sizeof(*buffer)); MUST(buffer);
    buffer->data = malloc(size); MUST(buffer->data); buffer->size = size; buffer->refs = 1;
    *output = buffer; ++buffers; return MPP_OK;
}

/** @brief 释放一个缓冲引用，最后一个引用才释放像素内存。 */
MPP_RET mpp_buffer_put_with_caller(MppBuffer opaque, const char *caller)
{
    Buffer *buffer = opaque; (void)caller; MUST(buffer && buffer->refs);
    if (--buffer->refs == 0) { free(buffer->data); free(buffer); --buffers; }
    return MPP_OK;
}

/** @brief 返回可由 CPU 写入的模拟 DMA 地址。 */
void *mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char *caller)
{
    (void)caller; return ((Buffer *)buffer)->data;
}

/** @brief 创建帧描述并计数，可注入分配错误。 */
MPP_RET mpp_frame_init(MppFrame *frame)
{
    if (is_case("frame-init-fail")) return MPP_ERR_MALLOC;
    *frame = calloc(1, sizeof(Frame)); MUST(*frame); ++frames; return MPP_OK;
}

/** @brief 回收帧及其持有的缓冲引用。 */
MPP_RET mpp_frame_deinit(MppFrame *opaque)
{
    Frame *frame = *opaque; MUST(frame && frames);
    if (frame->buffer) mpp_buffer_put_with_caller(frame->buffer, __func__);
    free(frame); *opaque = NULL; --frames; return MPP_OK;
}

/** @brief 生成简单字段设置接口；每个生成函数只修改对应的帧元数据。 */
#define FRAME_SETTER(name, member, type) \
    void mpp_frame_set_##name(MppFrame frame, type input) { ((Frame *)frame)->member = input; }
FRAME_SETTER(width, width, RK_U32)
FRAME_SETTER(height, height, RK_U32)
FRAME_SETTER(hor_stride, stride, RK_U32)
FRAME_SETTER(ver_stride, vertical, RK_U32)
FRAME_SETTER(eos, eos, RK_U32)
FRAME_SETTER(pts, pts, RK_S64)
FRAME_SETTER(fmt, fmt, MppFrameFormat)

/** @brief 给帧增加输入缓冲引用，匹配真实 MPP 的 frame_set_buffer 契约。 */
void mpp_frame_set_buffer(MppFrame opaque, MppBuffer buffer)
{
    Frame *frame = opaque; MUST(!frame->buffer); frame->buffer = buffer;
    if (buffer) ++((Buffer *)buffer)->refs;
}

/** @brief 创建借用内存的头信息包，不取得调用者栈内存所有权。 */
MPP_RET mpp_packet_init(MppPacket *output, void *data, size_t size)
{
    Packet *packet;
    if (is_case("header-init-fail")) return MPP_ERR_MALLOC;
    packet = calloc(1, sizeof(*packet)); MUST(packet);
    packet->data = data; packet->size = packet->length = size; *output = packet; ++packets;
    return MPP_OK;
}

/** @brief 销毁编码包前要求调用者已取走并释放 KEY_INPUT_FRAME。 */
MPP_RET mpp_packet_deinit(MppPacket *opaque)
{
    Packet *packet = *opaque; MUST(packet && !packet->returned);
    if (packet->owned) { memset(packet->data, 0xdd, packet->size); free(packet->data); }
    free(packet); *opaque = NULL; --packets; return MPP_OK;
}

/** @brief 设置头包有效长度。 */
void mpp_packet_set_length(MppPacket packet, size_t size) { ((Packet *)packet)->length = size; }
/** @brief 取得包有效数据地址。 */
void *mpp_packet_get_pos(const MppPacket packet) { return ((Packet *)packet)->data; }
/** @brief 取得包有效长度。 */
size_t mpp_packet_get_length(const MppPacket packet) { return ((Packet *)packet)->length; }
/** @brief 取得透传的输入 PTS。 */
RK_S64 mpp_packet_get_pts(const MppPacket packet) { return ((Packet *)packet)->pts; }
/** @brief 取得 EOS 标记。 */
RK_U32 mpp_packet_get_eos(const MppPacket packet) { return ((Packet *)packet)->eos; }
/** @brief 取得分片标记。 */
RK_U32 mpp_packet_is_partition(const MppPacket packet) { return ((Packet *)packet)->partition; }
/** @brief 取得帧末分片标记。 */
RK_U32 mpp_packet_is_eoi(const MppPacket packet) { return ((Packet *)packet)->eoi; }
/** @brief 模拟普通输出元数据，以及部分 BSP 空 EOS 不带元数据的行为。 */
RK_S32 mpp_packet_has_meta(const MppPacket packet)
{
    return !(((Packet *)packet)->eos && is_case("eos-no-meta"));
}
/** @brief 返回包自身作为模拟元数据对象。 */
MppMeta mpp_packet_get_meta(const MppPacket packet) { return packet; }
/** @brief 从输出元数据中读取关键帧标记。 */
MPP_RET mpp_meta_get_s32(MppMeta meta, MppMetaKey key, RK_S32 *result)
{
    MUST(key == KEY_OUTPUT_INTRA); *result = ((Packet *)meta)->intra; return MPP_OK;
}
/** @brief 转交输入帧所有权并清除元数据中的引用，保证只释放一次。 */
MPP_RET mpp_meta_get_frame(MppMeta meta, MppMetaKey key, MppFrame *result)
{
    Packet *packet = meta; MUST(key == KEY_INPUT_FRAME);
    *result = packet->returned; packet->returned = NULL;
    return *result ? MPP_OK : MPP_NOK;
}
