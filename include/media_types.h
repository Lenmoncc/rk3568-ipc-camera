/* media_types.h — 初版模块接口草案；业务函数尚未实现。 */
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

/* NV12 视频帧和 PCM 音频块共用的队列元素。
 * data 由应用持有，不得指向已 QBUF 的驱动内存或复用的栈数组。
 * pts_us：与应用共同起点对齐的微秒时间戳。 */
typedef struct {
    IpcMediaType type;
    uint8_t *data;
    size_t size;
    int64_t pts_us;
    union {
        struct {
            unsigned int width, height;
            unsigned int fourcc;
            size_t y_stride, uv_stride, uv_offset;
            uint32_t sequence;
        } video;
        struct {
            unsigned int sample_rate, channels;
            unsigned int samples_per_channel;
            /* 初版采用交错 S16_LE；其他格式接入时扩展类型。 */
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
