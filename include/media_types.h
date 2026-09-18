/**
 * @file media_types.h
 * @brief 采集、队列、编码模块共享的媒体描述，不依赖具体设备库。
 *
 * 原始帧由应用独占持有；编码包与流参数是后续阶段的接口草案。
 * 队列不检查格式正确性，采集端负责填充，编码端负责核对支持的布局。
 */
#ifndef IPC_MEDIA_TYPES_H
#define IPC_MEDIA_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* 前向声明使骨架不依赖本机的 FFmpeg 头文件。
 * 实现阶段必须包含 SDK 内相应的 FFmpeg 4.4.1 头文件。 */
struct AVPacket;
struct AVCodecParameters;

typedef enum { IPC_MEDIA_VIDEO, IPC_MEDIA_AUDIO } IpcMediaType;
typedef struct { int num; int den; } IpcTimeBase;

/** 初版录音数据格式。显式字段避免消费者仅凭字节数猜测 PCM 布局。 */
typedef enum {
    IPC_AUDIO_FORMAT_S16_LE = 0 /**< 每样本 16 位有符号小端、声道交错排列。 */
} IpcAudioFormat;

/**
 * @brief 一个 NV12 视频帧或一个 PCM 音频块。
 *
 * frame 对象与 data 分别使用 malloc/calloc 分配，由 ipc_raw_frame_free 释放。
 * data 不得指向驱动 MMAP、已 QBUF 的缓冲区、栈空间或其他帧共享的内存。
 * 建议 calloc 清零描述对象，再完整填写相应媒体分支。
 */
typedef struct {
    IpcMediaType type; /**< 决定 info 使用 video 还是 audio 分支。 */
    uint8_t *data;     /**< 应用自有连续内存的起始地址；入队后不可再访问。 */
    size_t size;       /**< data 中可供访问的有效字节范围，包含实际行步长所需填充。 */
    int64_t pts_us;    /**< 相对于应用共同起点的微秒时间戳，不是墙上时钟。 */
    union {
        struct {
            unsigned int width;  /**< 有效图像宽度，像素。 */
            unsigned int height; /**< 有效图像高度，像素。 */
            unsigned int fourcc; /**< 像素格式标识；初版由 V4L2 填入 NV12 FOURCC。 */
            size_t y_stride;     /**< 相邻 Y 行首地址间隔，字节，可大于 width。 */
            size_t uv_stride;    /**< 相邻 UV 行首地址间隔，字节。 */
            size_t uv_offset;    /**< UV 首地址相对于 data 的偏移，字节。 */
            uint32_t sequence;   /**< 驱动帧序号，可用于观察丢帧和顺序。 */
        } video;
        struct {
            unsigned int sample_rate;         /**< 每声道每秒样本数，Hz。 */
            unsigned int channels;            /**< 声道数。 */
            unsigned int samples_per_channel; /**< 本块每声道样本数，不是字节数。 */
            IpcAudioFormat sample_format;     /**< 初版固定 S16_LE，编码前再转换。 */
            /* S16_LE 的有效数据长度 = samples_per_channel * channels * 2；
             * 采集端计算时须检查溢出。AAC 重分帧由编码模块处理。 */
        } audio;
    } info;
} IpcRawFrame;

/* 每个输出拥有独立的 packet 对象；底层数据可通过 av_packet_ref 共享。
 * PTS/DTS/duration、关键帧标记存放在 AVPacket 中，time_base 描述其单位。 */
typedef struct {
    IpcMediaType type;
    struct AVPacket *packet;
    IpcTimeBase time_base;
} IpcEncodedPacket;

/* 编码器持有原始参数；输出模块初始化时复制参数。
 * 输出写头之前须确认 codecpar（包括必要的 extradata）已就绪。 */
typedef struct {
    IpcMediaType type;
    struct AVCodecParameters *codecpar;
    IpcTimeBase time_base;
} IpcStreamParams;

#endif /* IPC_MEDIA_TYPES_H */
