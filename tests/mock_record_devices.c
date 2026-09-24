/** @file mock_record_devices.c
 * @brief 录像集成测试替身：按真实时间产生 PCM/视频 PTS，用有效 H.264 固定图像替代 MPP。
 * 只进入主机测试；AAC、队列、桥接、MP4 与线程管理均使用正式实现。
 */
#define _POSIX_C_SOURCE 200809L
#include "v4l2_capture.h"
#include "alsa_capture.h"
#include "video_encoder.h"
#include "frame_queue.h"
#include "timestamp.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct IpcVideoCapture {int64_t epoch; bool running; IpcVideoCaptureStats stats; uint64_t index;};
struct IpcAudioCapture {int64_t epoch,offset; bool running; IpcAudioCaptureStats stats;};
struct IpcVideoEncoder {IpcH264Sink sink;void *opaque;uint8_t *fixture;size_t size;IpcVideoEncoderStats stats;};
static unsigned int live, creates;
/** @brief 检查本次进程选择的唯一故障场景。 */
static bool scenario(const char *name) {const char *s=getenv("IPC_RECORD_CASE");return s && !strcmp(s,name);}
/** @brief 测试进程退出时核对模拟设备资源均已释放。 */
static void check_clean(void) {assert(live==0);fprintf(stderr,"RECORD_DEVICES_CLEAN\n");}
/** @brief 只在首个设备创建前注册统一资源检查。 */
static void register_check(void) {static bool registered;if(!registered){atexit(check_clean);registered=true;}}
/** @brief 模拟有限设备等待，未到数据时刻返回 0；不将慢设备变成忙等。 */
static int wait_until(int64_t when,int timeout_ms)
{
    int64_t left=when-ipc_monotonic_us();
    if(left<=0)return 1;
    bool ready=left<=(int64_t)timeout_ms*1000;
    if(!ready)left=(int64_t)timeout_ms*1000;
    struct timespec delay={.tv_sec=left/1000000,.tv_nsec=(long)(left%1000000)*1000};
    while(nanosleep(&delay,&delay)<0 && errno==EINTR){}
    return ready ? 1:0;
}
/** @brief 为录像场景分配模拟视频设备。 */
int ipc_video_capture_init(IpcVideoCapture **c,const IpcConfig *config)
{(void)config;register_check();if(scenario("video-init-fail"))return -EIO;*c=calloc(1,sizeof(**c));if(!*c)return -ENOMEM;++live;return 0;}
/** @brief 将共同起点传入模拟视频采集器。 */
int ipc_video_capture_start(IpcVideoCapture *c,int64_t epoch)
{if(scenario("video-start-fail"))return -EIO;c->epoch=epoch;c->running=true;return 0;}
/** @brief 按 25fps 和可控起始偏差交付独占 NV12 帧，供正式录像调度使用。 */
int ipc_video_capture_read(IpcVideoCapture *c,IpcRawFrame **out,int timeout_ms)
{
    assert(c && c->running && out && !*out);
    if(scenario("video-timeout")){wait_until(ipc_monotonic_us()+200000,timeout_ms);return 0;}
    if(scenario("video-read-fail") && c->index>=4)return -EIO;
    int64_t offset=scenario("late-video") ? 400000:175000;
    int64_t pts=offset+(int64_t)c->index*40000;
    if(!wait_until(c->epoch+pts,timeout_ms))return 0;
    IpcRawFrame *f=calloc(1,sizeof(*f));assert(f);
    f->size=1280*720*3/2;f->data=calloc(1,f->size);assert(f->data);
    f->type=IPC_MEDIA_VIDEO;f->pts_us=pts;f->info.video.width=1280;f->info.video.height=720;
    f->info.video.sequence=(uint32_t)c->index++;
    ++c->stats.dequeued;++c->stats.captured;
    if(c->stats.captured==1)c->stats.first_arrival_us=c->epoch+pts;
    c->stats.last_arrival_us=c->epoch+pts;
    *out=f;return 1;
}
/** @brief 停止模拟视频设备，允许未启动和重复停止。 */
int ipc_video_capture_stop(IpcVideoCapture *c){if(c)c->running=false;return 0;}
/** @brief 返回模拟采集布局，兼容独立视频流水线的链接接口。 */
int ipc_video_capture_get_format(const IpcVideoCapture *c,IpcVideoFormat *f)
{if(!c||!f)return -EINVAL;*f=(IpcVideoFormat){1280,720,1280,1280*720,1280*720*3/2,1280*720*3/2,4};return 0;}
/** @brief 复制视频采集统计。 */
int ipc_video_capture_get_stats(const IpcVideoCapture *c,IpcVideoCaptureStats *s){*s=c->stats;return 0;}
/** @brief 释放模拟视频设备并减少资源计数。 */
int ipc_video_capture_deinit(IpcVideoCapture **c){if(c&&*c){free(*c);*c=NULL;--live;}return 0;}
/** @brief 测试替身提供音频采集能力。 */
bool ipc_audio_capture_available(void){return true;}
/** @brief 分配模拟 ALSA 设备，要求测试配置使用既定 48kHz 双声道。 */
int ipc_audio_capture_init(IpcAudioCapture **c,const IpcConfig *config)
{register_check();if(scenario("audio-init-fail"))return -EIO;assert(config->audio_sample_rate==48000&&config->audio_channels==2);*c=calloc(1,sizeof(**c));if(!*c)return -ENOMEM;++live;return 0;}
/** @brief 返回 10ms 周期的模拟音频布局。 */
int ipc_audio_capture_get_format(const IpcAudioCapture *c,IpcAudioCaptureFormat *f)
{(void)c;*f=(IpcAudioCaptureFormat){48000,2,480,1920};return 0;}
/** @brief 使用共同起点和可控声卡启动偏差。 */
int ipc_audio_capture_start(IpcAudioCapture *c,int64_t epoch)
{if(scenario("audio-start-fail"))return -EIO;c->epoch=epoch;c->offset=scenario("late-audio")?350000:1000;c->running=true;return 0;}
/** @brief 按硬件节奏生成正弦 PCM，保留实际样本下标与时间戳，支持读故障注入。 */
int ipc_audio_capture_read(IpcAudioCapture *c,IpcRawFrame **out,unsigned int max_samples,int timeout_ms)
{
    assert(c && c->running && out && !*out);
    if(scenario("audio-timeout")){wait_until(ipc_monotonic_us()+200000,timeout_ms);return 0;}
    if(scenario("audio-read-fail")&&c->stats.samples>=4800)return -EPIPE;
    unsigned int n=max_samples<480?max_samples:480;
    int64_t pts;assert(ipc_audio_pts_us(c->offset,c->stats.samples,48000,&pts)==0);
    if(!wait_until(c->epoch+pts+(int64_t)n*1000000/48000,timeout_ms))return 0;
    IpcRawFrame *f=calloc(1,sizeof(*f));assert(f);
    f->type=IPC_MEDIA_AUDIO;f->pts_us=pts;f->size=n*4;f->data=malloc(f->size);assert(f->data);
    f->info.audio.sample_rate=48000;f->info.audio.channels=2;f->info.audio.samples_per_channel=n;
    f->info.audio.sample_index=c->stats.samples;f->info.audio.sample_format=IPC_AUDIO_FORMAT_S16_LE;
    for(unsigned int i=0;i<n;++i)for(unsigned int ch=0;ch<2;++ch){
        int16_t sample=(int16_t)(10000*sin(6.283185307179586*(ch?880:440)*(double)(c->stats.samples+i)/48000));
        size_t at=(i*2+ch)*2;f->data[at]=(uint8_t)sample;f->data[at+1]=(uint8_t)((uint16_t)sample>>8);
    }
    if(!c->stats.blocks)c->stats.first_pts_us=pts;
    c->stats.last_pts_us=pts;++c->stats.blocks;c->stats.samples+=n;*out=f;return 1;
}
/** @brief 停止模拟 ALSA，允许未启动和重复停止。 */
int ipc_audio_capture_stop(IpcAudioCapture *c){if(c)c->running=false;return 0;}
/** @brief 复制音频采集统计。 */
int ipc_audio_capture_get_stats(const IpcAudioCapture *c,IpcAudioCaptureStats *s){*s=c->stats;return 0;}
/** @brief 释放模拟声卡并减少资源计数。 */
int ipc_audio_capture_deinit(IpcAudioCapture **c){if(c&&*c){free(*c);*c=NULL;--live;}return 0;}
/** @brief 声明测试替身提供 H.264 数据，不代表主机具有 MPP 硬件。 */
bool ipc_video_encoder_available(void){return true;}
/** @brief 读取主机生成的有效 Annex B 固定图像，初始化时同步交付 SPS/PPS。 */
int ipc_video_encoder_init(IpcVideoEncoder **c,const IpcConfig *config,unsigned int fps,IpcH264Sink sink,void *opaque)
{
    (void)config;if(scenario("encoder-init-fail"))return -EIO;
    const char *path=getenv("IPC_RECORD_FIXTURE");assert(path);
    FILE *f=fopen(path,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);long size=ftell(f);assert(size>0);rewind(f);
    IpcVideoEncoder *e=calloc(1,sizeof(*e));assert(e);e->fixture=malloc((size_t)size);assert(e->fixture);
    assert(fread(e->fixture,1,(size_t)size,f)==(size_t)size);fclose(f);e->size=(size_t)size;
    e->sink=sink;e->opaque=opaque;e->stats.fps=fps;e->stats.first_pts_us=e->stats.last_pts_us=-1;
    ++live;*c=e;
    IpcH264Packet packet={.data=e->fixture,.size=e->size,.header=true};
    int result=sink(opaque,&packet);
    if(result<0)ipc_video_encoder_deinit(c);
    return result;
}
/** @brief 用有效 IDR 图像替代 MPP 输出，分成两片，检验正式桥接器组帧及 PTS 传递。 */
int ipc_video_encoder_send(IpcVideoEncoder *e,const IpcRawFrame *f)
{
    if(scenario("encode-fail") && e->stats.encoded>=2)return -EIO;
    IpcH264Packet first={.data=e->fixture,.size=e->size/2,.pts_us=f->pts_us,.key_frame=true};
    IpcH264Packet last={.data=e->fixture+first.size,.size=e->size-first.size,.pts_us=f->pts_us,.frame_end=true};
    int result=e->sink(e->opaque,&first);if(!result)result=e->sink(e->opaque,&last);
    if(!result){if(!e->stats.encoded)e->stats.first_pts_us=f->pts_us;++e->stats.encoded;++e->stats.submitted;e->stats.last_pts_us=f->pts_us;}
    return result;
}
/** @brief 模拟无图像 EOS，可注入收尾失败。 */
int ipc_video_encoder_finish(IpcVideoEncoder *e)
{
    if(scenario("eos-fail"))return -EIO;
    IpcH264Packet eos={.eos=true,.frame_end=true};int result=e->sink(e->opaque,&eos);
    if(!result)e->stats.eos=true;
    return result;
}
/** @brief 复制编码统计，供正式流水线核对。 */
int ipc_video_encoder_get_stats(const IpcVideoEncoder *e,IpcVideoEncoderStats *s){*s=e->stats;return 0;}
/** @brief 回收测试固定图像及编码上下文。 */
int ipc_video_encoder_deinit(IpcVideoEncoder **e){if(e&&*e){free((*e)->fixture);free(*e);*e=NULL;--live;}return 0;}
extern int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
/** @brief 在五个线程创建位置逐个注入失败，验证未启动生产者的队列关闭补偿。 */
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{
    const char *n=getenv("IPC_RECORD_THREAD_FAIL");++creates;
    if(n && creates==(unsigned int)atoi(n))return EAGAIN;
    return __real_pthread_create(t,a,fn,arg);
}
extern ssize_t __real_write(int,const void *,size_t);
/** @brief 注入 header/packet 写失败与合法短写，检查 AVIO 错误不会被隐藏。 */
ssize_t __wrap_write(int fd,const void *p,size_t n)
{
    static unsigned int writes; ++writes;
    if(scenario("header-write-fail") || (scenario("packet-write-fail")&&writes>1)){errno=ENOSPC;return -1;}
    return __real_write(fd,p,scenario("short-write")&&n>5?5:n);
}
extern off_t __real_lseek(int,off_t,int);
/** @brief 注入 trailer 回填定位错误。 */
off_t __wrap_lseek(int fd,off_t offset,int whence)
{if(scenario("seek-fail")){errno=EIO;return -1;}return __real_lseek(fd,offset,whence);}
extern int __real_close(int);
/** @brief 底层描述符仍实际关闭，再模拟 close 报错，检验最终成功判定。 */
int __wrap_close(int fd){int result=__real_close(fd);if(scenario("close-fail")){errno=EIO;return -1;}return result;}
