/** @file video_encoder.h
 * @brief 单线程使用的 MPP H.264 编码器；对外不暴露 MPP/FFmpeg 类型。
 * init/send/finish/deinit 必须串行；查询统计应在编码线程 join 后进行。
 */
#ifndef IPC_VIDEO_ENCODER_H
#define IPC_VIDEO_ENCODER_H
#include "config.h"
#include "media_types.h"

typedef struct IpcVideoEncoder IpcVideoEncoder;

/** 输出数据只在回调期间有效；异步输出必须自行复制，不能保留 data 指针。
 * pts_us 沿用采集时间戳。当前 Baseline I/P 无重排，未来封装时 DTS=PTS。
 * header 表示初始化 SPS/PPS；frame_end 表示当前图像的最后一个输出分片。
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t pts_us;
    bool header, key_frame, frame_end, eos;
} IpcH264Packet;

/** @brief 同步消费一个编码包；返回 0 或负 errno，失败将终止编码。 */
typedef int (*IpcH264Sink)(void *opaque, const IpcH264Packet *packet);

typedef struct {
    uint64_t submitted, encoded, packets, bytes, keyframes;
    int64_t first_pts_us, last_pts_us;
    unsigned int fps;
    bool eos;
} IpcVideoEncoderStats;

/** @brief 返回本次构建是否包含真实 MPP 支持，不探测硬件。 */
bool ipc_video_encoder_available(void);

/** @brief 配置 NV12→H.264 Baseline/CBR、分配 DMA 缓冲并向 sink 输出 SPS/PPS。
 * fps 为码控与码流声明帧率，不修改摄像头；1..30，须按实测输入设置。
 * *context 初始必须为 NULL；失败回滚，成功后 sink/opaque 必须保持有效。
 */
int ipc_video_encoder_init(IpcVideoEncoder **context, const IpcConfig *config,
                           unsigned int fps, IpcH264Sink sink, void *opaque);

/** @brief 复制并编码一帧，收齐该帧输出后返回；不接管原始帧所有权。
 * 输入为 NV12，可含行填充；PTS 必须非负且严格递增。
 * 应用层提交/取包各最多重试 3 秒；底层驱动调用的耗时仍由 BSP 决定。
 * 任一运行错误后上下文不可继续发送，应调用 deinit。
 */
int ipc_video_encoder_send(IpcVideoEncoder *context, const IpcRawFrame *frame);

/** @brief 发送不含图像的 EOS 标记并取到输出 EOS；成功后重复调用安全。 */
int ipc_video_encoder_finish(IpcVideoEncoder *context);

/** @brief 串行复制统计；bytes 含 SPS/PPS，encoded 不计空 EOS 包。 */
int ipc_video_encoder_get_stats(const IpcVideoEncoder *context, IpcVideoEncoderStats *stats);

/** @brief 销毁 MPP 回收在途帧，再释放未提交帧与 DMA 引用，清空指针；NULL/重复调用安全。
 * 返回首个清理错误；错误路径也不会在硬件仍可能访问时提前复用输入缓冲。
 */
int ipc_video_encoder_deinit(IpcVideoEncoder **context);
#endif
