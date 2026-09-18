/**
 * @file v4l2_capture.h
 * @brief 正式工程的 V4L2 多平面采集接口，不依赖 demo 或编码库。
 *
 * init/start/read/stop/deinit 必须串行使用同一个上下文；read 由一个采集线程
 * 调用。管理线程应先 join 再查询统计或释放上下文，不支持并发 deinit。
 */
#ifndef IPC_V4L2_CAPTURE_H
#define IPC_V4L2_CAPTURE_H
#include "config.h"
#include "media_types.h"

typedef struct IpcVideoCapture IpcVideoCapture;

/** 驱动协商后的实际布局；初版仅支持逐行 NV12、一个内存平面。 */
typedef struct {
    unsigned int width, height;
    size_t stride;       /**< Y 与 UV 行步长，字节。 */
    size_t uv_offset;    /**< 相对图像起点，等于 stride * height。 */
    size_t frame_size;   /**< 应用帧分配大小，含行填充。 */
    size_t sizeimage;    /**< 驱动报告的缓冲区需求，可能大于实际图像。 */
    unsigned int buffers; /**< 实际申请的驱动缓冲区数。 */
} IpcVideoFormat;

/** 仅采集线程写；管理者在 join 后读取，不是线程安全的实时快照。 */
typedef struct {
    uint64_t dequeued, captured, invalid, timestamp_rejected;
    uint64_t sequence_gaps, sequence_resets, timestamp_fallback;
    uint64_t poll_timeouts, would_block;
    int64_t first_arrival_us, last_arrival_us;
} IpcVideoCaptureStats;

/** @brief 打开、配置、映射并排队缓冲区，尚不启动流。
 * @param context 必须非 NULL 且 *context == NULL；成功时接管新上下文。
 * @return 0 成功，负 errno 失败；失败清理部分资源且不修改输出指针。
 */
int ipc_video_capture_init(IpcVideoCapture **context, const IpcConfig *config);

/** @brief 以应用单调时钟起点启动流；epoch_us 必须在调用之前刚获取。
 * @return 0 成功，负 errno 失败；同一上下文不支持停止后重新启动。
 */
int ipc_video_capture_start(IpcVideoCapture *context, int64_t epoch_us);

/** @brief 等待并复制一帧，在返回前归还驱动缓冲区。
 * @param frame 非 NULL 且 *frame == NULL；成功后调用者独占拥有堆帧。
 * @param timeout_ms 单次 poll 等待上限，范围 1..1000 毫秒。
 * @return 1 成功；0 暂无帧（超时/EAGAIN/EINTR）；-EBADMSG 已丢弃并归还
 *         的损坏帧或无效时间戳；其他负 errno 为需要停止链路的错误。
 * @note 失败不交付帧；应用内存与 MMAP 分离，可安全在 QBUF 后使用。
 */
int ipc_video_capture_read(IpcVideoCapture *context, IpcRawFrame **frame, int timeout_ms);

/** @brief 停止流，可重复调用；只可在 read 结束后串行调用，返回负 errno 或 0。 */
int ipc_video_capture_stop(IpcVideoCapture *context);

/** @brief 获取实际布局；在 init 完成后且 deinit 前有效，不转移资源所有权。 */
int ipc_video_capture_get_format(const IpcVideoCapture *context, IpcVideoFormat *format);

/** @brief 在采集线程退出后复制统计信息；返回 0 或 -EINVAL。 */
int ipc_video_capture_get_stats(const IpcVideoCapture *context, IpcVideoCaptureStats *stats);

/** @brief 停流、解除映射、释放驱动缓冲区和设备，清空 *context。
 * @return 0 成功，否则返回第一个清理错误；其余资源仍继续尝试清理。
 * @pre 所有使用者已经 join；NULL 及重复调用安全。
 */
int ipc_video_capture_deinit(IpcVideoCapture **context);
#endif
