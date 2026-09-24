/** @file mp4_output.c
 * @brief FFmpeg 4.4 MP4 输出；自定义可定位 AVIO 确保 O_EXCL 和完整错误传递。
 * 普通 MP4 的 moov 在收尾写入，断电/强制 kill 不保证可恢复。
 */
#define _POSIX_C_SOURCE 200809L
#include "mp4_output.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#if IPC_WITH_FFMPEG
#include <libavformat/avformat.h>
struct IpcMp4Output {
    AVFormatContext *format;
    AVIOContext *io;
    AVPacket *scratch;
    int fd;
    int io_error; /**< 某些封装路径不检查 seek 返回值，独立锁存首个底层 I/O 错误。 */
    bool failed, finished;
    int64_t last_dts[2];
    IpcMp4Stats stats;
};
/** @brief 把库错误转为应用负 errno，同时保留 FFmpeg 的可读原因。 */
static int mux_error(const char *operation, int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, text, sizeof(text));
    ipc_log_write(IPC_LOG_ERROR, "mp4", "%s: %s (%d)", operation, text, error);
    return error == AVERROR(ENOMEM) ? -ENOMEM : -EIO;
}
/** @brief 完整写入 AVIO 缓冲；短写继续、EINTR 重试，其余失败交回 FFmpeg。 */
static int write_bytes(void *opaque, uint8_t *data, int size)
{
    IpcMp4Output *m = opaque;
    int offset = 0;
    while (offset < size) {
        ssize_t written = write(m->fd, data + offset, (size_t)(size - offset));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            int error = AVERROR(written < 0 ? errno : EIO);
            if (!m->io_error) m->io_error = error;
            return error;
        }
        offset += (int)written;
    }
    return size;
}
/** @brief 提供普通文件定位和大小查询，供 MP4 回填 mdat 等字段。 */
static int64_t seek_bytes(void *opaque, int64_t offset, int whence)
{
    IpcMp4Output *m = opaque;
    if (whence & AVSEEK_SIZE) {
        struct stat st;
        if (fstat(m->fd, &st) < 0) {
            int error = AVERROR(errno); if (!m->io_error) m->io_error = error; return error;
        }
        return st.st_size;
    }
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return AVERROR(EINVAL);
    off_t result = lseek(m->fd, (off_t)offset, whence);
    if (result < 0) {
        int error = AVERROR(errno); if (!m->io_error) m->io_error = error; return error;
    }
    return result;
}
/** @brief 复制编码器参数到独立输出流；时间基在写头后可能由封装器调整。 */
static int add_stream(IpcMp4Output *m, const IpcStreamParams *params, unsigned int fps)
{
    AVStream *stream = avformat_new_stream(m->format, NULL);
    if (!stream) return -ENOMEM;
    int result = avcodec_parameters_copy(stream->codecpar, params->codecpar);
    if (result < 0) return mux_error("copy codec parameters", result);
    stream->codecpar->codec_tag = 0;
    stream->time_base = (AVRational){params->time_base.num, params->time_base.den};
    if (params->type == IPC_MEDIA_VIDEO) stream->avg_frame_rate = (AVRational){(int)fps,1};
    return 0;
}
/** @brief 验证双轨参数，再独占创建文件并写头；AVIO 由本模块而非 avio_open 管理。 */
int ipc_mp4_output_init(IpcMp4Output **context, const char *path,
                        const IpcStreamParams *video, const IpcStreamParams *audio, unsigned int fps)
{
    int result;
    AVDictionary *options = NULL;
    if (!context || *context || !path || !*path || !video || !audio || !video->codecpar || !audio->codecpar ||
        video->type != IPC_MEDIA_VIDEO || audio->type != IPC_MEDIA_AUDIO ||
        video->codecpar->codec_id != AV_CODEC_ID_H264 || audio->codecpar->codec_id != AV_CODEC_ID_AAC ||
        video->codecpar->extradata_size <= 0 || audio->codecpar->extradata_size <= 0 ||
        video->time_base.num <= 0 || video->time_base.den <= 0 || audio->time_base.num <= 0 || audio->time_base.den <= 0 ||
        !fps || fps > 30) return -EINVAL;
    IpcMp4Output *m = calloc(1, sizeof(*m));
    if (!m) return -ENOMEM;
    m->fd = -1; m->last_dts[0] = m->last_dts[1] = AV_NOPTS_VALUE;
    m->stats.first_video_us = m->stats.last_video_us = m->stats.first_audio_us = m->stats.last_audio_us = AV_NOPTS_VALUE;
    result = avformat_alloc_output_context2(&m->format, NULL, "mp4", NULL);
    if (result < 0) { result = mux_error("alloc MP4", result); goto fail; }
    result = add_stream(m, video, fps); if (result < 0) goto fail;
    result = add_stream(m, audio, fps); if (result < 0) goto fail;
    m->scratch = av_packet_alloc();
    if (!m->scratch) { result = -ENOMEM; goto fail; }
    m->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (m->fd < 0) { result = -errno; goto fail; }
    uint8_t *buffer = av_malloc(32768);
    if (!buffer) { result = -ENOMEM; goto fail; }
    m->io = avio_alloc_context(buffer, 32768, 1, m, NULL, write_bytes, seek_bytes);
    if (!m->io) { av_free(buffer); result = -ENOMEM; goto fail; }
    m->io->seekable = AVIO_SEEKABLE_NORMAL;
    m->format->pb = m->io; m->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    /* 不按流分别归零；允许 AAC 编码延迟产生负 DTS，使用 MP4 edit list 表达起始裁剪/偏移。 */
    m->format->avoid_negative_ts = 0 /* FFmpeg 4.4 的 disabled 值，保留负时间戳 */;
    m->format->max_interleave_delta = 500000;
    result = av_dict_set(&options, "use_editlist", "1", 0);
    if (result < 0) { result = mux_error("edit list options", result); goto fail; }
    result = avformat_write_header(m->format, &options);
    av_dict_free(&options);
    if (result < 0) { result = mux_error("write header", result); goto fail; }
    avio_flush(m->io);
    if (m->io->error < 0 || m->io_error < 0) {
        result = mux_error("flush header", m->io_error ? m->io_error : m->io->error); goto fail;
    }
    ipc_log_write(IPC_LOG_INFO, "mp4", "header ready: video_tb=%d/%d audio_tb=%d/%d output=%s",
                  m->format->streams[0]->time_base.num,m->format->streams[0]->time_base.den,
                  m->format->streams[1]->time_base.num,m->format->streams[1]->time_base.den,path);
    *context = m; return 0;
fail:
    av_dict_free(&options); ipc_mp4_output_deinit(&m); return result;
}
/** @brief 对独立引用重标定时间戳，检查每条轨 DTS 单调，库接管引用后及时清空 scratch。 */
int ipc_mp4_output_write(IpcMp4Output *m, const IpcEncodedPacket *packet)
{
    if (!m || m->failed || m->finished || !packet || !packet->packet ||
        (packet->type != IPC_MEDIA_VIDEO && packet->type != IPC_MEDIA_AUDIO) ||
        packet->time_base.num <= 0 || packet->time_base.den <= 0 || packet->packet->size <= 0 ||
        !packet->packet->data || packet->packet->pts == AV_NOPTS_VALUE || packet->packet->dts == AV_NOPTS_VALUE ||
        packet->packet->duration <= 0) return -EINVAL;
    int index = packet->type == IPC_MEDIA_VIDEO ? 0 : 1;
    AVRational input = {packet->time_base.num, packet->time_base.den};
    int64_t pts_us = av_rescale_q(packet->packet->pts, input, AV_TIME_BASE_Q);
    int result = av_packet_ref(m->scratch, packet->packet);
    if (result < 0) goto fail;
    av_packet_rescale_ts(m->scratch, input, m->format->streams[index]->time_base);
    m->scratch->stream_index = index; m->scratch->pos = -1;
    if (m->scratch->duration <= 0 || (m->last_dts[index] != AV_NOPTS_VALUE && m->scratch->dts <= m->last_dts[index])) {
        result = AVERROR(EINVAL); goto fail;
    }
    m->last_dts[index] = m->scratch->dts;
    result = av_interleaved_write_frame(m->format, m->scratch);
    av_packet_unref(m->scratch);
    if (result >= 0 && m->io->error < 0) result = m->io->error;
    if (result >= 0 && m->io_error < 0) result = m->io_error;
    if (result < 0) goto fail;
    if (!index) {
        if (!m->stats.video_packets++) m->stats.first_video_us = pts_us;
        m->stats.last_video_us = pts_us;
    } else {
        if (!m->stats.audio_packets++) m->stats.first_audio_us = pts_us;
        m->stats.last_audio_us = pts_us;
    }
    return 0;
fail:
    av_packet_unref(m->scratch); m->failed = true; return mux_error("write packet", result);
}
/** @brief 完成容器尾部并检查缓冲写入/close；成功标记仅在文件关闭成功后设置。 */
int ipc_mp4_output_finish(IpcMp4Output *m)
{
    if (!m || m->failed) return -EINVAL;
    if (m->finished) return 0;
    int result = av_write_trailer(m->format);
    avio_flush(m->io);
    if (result >= 0 && m->io->error < 0) result = m->io->error;
    if (result >= 0 && m->io_error < 0) result = m->io_error;
    if (close(m->fd) < 0 && result >= 0) result = AVERROR(errno);
    m->fd = -1;
    if (result < 0) { m->failed = true; return mux_error("finalize", result); }
    m->finished = true;
    m->stats.trailer_written = true;
    if (!m->stats.video_packets || !m->stats.audio_packets) { m->failed = true; return -ENODATA; }
    return 0;
}
/** @brief 复制双轨包数与原始时间轴统计，不改变封装状态。 */
int ipc_mp4_output_get_stats(const IpcMp4Output *m, IpcMp4Stats *stats)
{ if (!m || !stats) return -EINVAL; *stats = m->stats; return 0; }
/** @brief 释放自定义 AVIO 当前缓冲和上下文；未 finish 的文件不冒充已完成。 */
int ipc_mp4_output_deinit(IpcMp4Output **context)
{
    if (!context || !*context) return 0;
    IpcMp4Output *m = *context; *context = NULL;
    int result = 0;
    av_packet_free(&m->scratch);
    if (m->format) m->format->pb = NULL;
    avformat_free_context(m->format);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    if (m->fd >= 0 && close(m->fd) < 0) result = -errno;
    free(m); return result;
}
#else
/** @brief 无 FFmpeg 构建拒绝创建 MP4，不产生空成功。 */
int ipc_mp4_output_init(IpcMp4Output **m,const char *p,const IpcStreamParams *v,const IpcStreamParams *a,unsigned int f)
{(void)m;(void)p;(void)v;(void)a;(void)f;return -ENOTSUP;}
/** @brief 无 FFmpeg 构建拒绝写入编码包。 */
int ipc_mp4_output_write(IpcMp4Output *m,const IpcEncodedPacket *p){(void)m;(void)p;return -ENOTSUP;}
/** @brief 无 FFmpeg 构建拒绝文件收尾。 */
int ipc_mp4_output_finish(IpcMp4Output *m){(void)m;return -ENOTSUP;}
/** @brief 无 FFmpeg 构建无封装统计。 */
int ipc_mp4_output_get_stats(const IpcMp4Output *m,IpcMp4Stats *s){(void)m;(void)s;return -ENOTSUP;}
/** @brief 无 FFmpeg 构建允许空句柄释放。 */
int ipc_mp4_output_deinit(IpcMp4Output **m){if(m)*m=NULL;return 0;}
#endif
