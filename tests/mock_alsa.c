/** @file mock_alsa.c
 * @brief 基于 ALSA 官方公开头文件的故障注入；不会访问真实设备或 mixer。
 * 生成左右声道不同的确定性样本，退出时核对全部模拟 ALSA 对象已释放。
 */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#define MUST(x) do { if (!(x)) { fprintf(stderr, "ALSA mock invariant %d: %s\n", __LINE__, #x); abort(); } } while (0)
struct _snd_pcm_hw_params { unsigned int rate, channels; snd_pcm_format_t format; snd_pcm_access_t access; snd_pcm_uframes_t period, buffer; };
struct _snd_pcm_sw_params { snd_pcm_uframes_t avail; };
struct _snd_pcm { struct _snd_pcm_hw_params hw; uint64_t samples; unsigned int calls; int started; };
static int devices, hardware_params, software_params, registered, thread_calls;
static FILE *pcm_file;
/** @brief 获取当前子进程的故障场景。 */
static int is_case(const char *name) { const char *value = getenv("IPC_MOCK_ALSA_CASE"); return value && !strcmp(value, name); }
/** @brief 进程结束时验证所有 ALSA 对象归零，禁止伪造成功回收。 */
static void verify_cleanup(void)
{
    MUST(!devices && !hardware_params && !software_params);
    fprintf(stderr, "ALSA_MOCK_CLEANUP_OK\n");
}
/** @brief 模拟 ALSA 错误文本。 */
const char *snd_strerror(int error) { return strerror(error < 0 ? -error : error); }
/** @brief 打开模拟录音句柄，强制检查非阻塞模式和采集方向。 */
int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
    MUST(pcm && !*pcm && name && stream == SND_PCM_STREAM_CAPTURE && mode == SND_PCM_NONBLOCK);
    if (!registered) { atexit(verify_cleanup); registered = 1; }
    if (is_case("open-fail")) return -EBUSY;
    *pcm = calloc(1, sizeof(**pcm)); MUST(*pcm); ++devices; return 0;
}
/** @brief 关闭模拟设备；即使注入关闭错误也回收测试内存。 */
int snd_pcm_close(snd_pcm_t *pcm)
{
    MUST(pcm && !pcm->started); free(pcm); --devices; return is_case("close-fail") ? -EIO : 0;
}
/** @brief 分配可追踪的硬件参数对象。 */
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **params)
{
    if (is_case("hw-alloc-fail")) return -ENOMEM;
    *params = calloc(1, sizeof(**params)); MUST(*params); ++hardware_params; return 0;
}
/** @brief 回收硬件参数对象。 */
void snd_pcm_hw_params_free(snd_pcm_hw_params_t *params) { MUST(params); free(params); --hardware_params; }
/** @brief 模拟设备能力查询。 */
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{ MUST(pcm && params); return is_case("hw-any-fail") ? -EINVAL : 0; }
/** @brief 保存交错访问方式，可注入不支持错误。 */
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t value)
{ MUST(pcm); params->access = value; return is_case("access-fail") ? -EINVAL : 0; }
/** @brief 保存目标格式，可注入格式设置错误。 */
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t value)
{ MUST(pcm); params->format = value; return is_case("format-fail") ? -EINVAL : 0; }
/** @brief 模拟板端至少双声道的约束，单声道不得悄悄转为双声道。 */
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int value)
{ MUST(pcm); params->channels = value; return value != 2 || is_case("channels-fail") ? -EINVAL : 0; }
/** @brief 记录精确采样率设置，避免测试只覆盖 near 调用。 */
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int value, int direction)
{ MUST(pcm && direction == 0); params->rate = value; return is_case("rate-fail") ? -EINVAL : 0; }
/** @brief 协商 512 样本周期，刻意不整除 48000，验证最后一块截断。 */
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *value, int *direction)
{ MUST(pcm); *value = is_case("huge-period") ? 131072 : 512; *direction = 0; params->period = *value; return 0; }
/** @brief 模拟驱动采纳建议缓冲区大小。 */
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *value)
{ MUST(pcm); params->buffer = *value; return is_case("buffer-fail") ? -EINVAL : 0; }
/** @brief 应用硬件设置。 */
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{ pcm->hw = *params; return is_case("apply-hw-fail") ? -EIO : 0; }
/** @brief 读回实际参数，可注入协商结果与请求不一致。 */
int snd_pcm_hw_params_current(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    *params = pcm->hw;
    if (is_case("actual-rate")) params->rate = 44100;
    if (is_case("actual-channels")) params->channels = 1;
    if (is_case("actual-format")) params->format = SND_PCM_FORMAT_S32_LE;
    if (is_case("actual-access")) params->access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    if (is_case("actual-buffer")) params->buffer = 1;
    return is_case("read-hw-fail") ? -EIO : 0;
}
/** @brief 读取实际采样率。 */
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *p, unsigned int *v, int *d) { *v=p->rate; *d=0; return 0; }
/** @brief 读取实际声道数。 */
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *p, unsigned int *v) { *v=p->channels; return 0; }
/** @brief 读取实际样本格式。 */
int snd_pcm_hw_params_get_format(const snd_pcm_hw_params_t *p, snd_pcm_format_t *v) { *v=p->format; return 0; }
/** @brief 读取实际访问方式。 */
int snd_pcm_hw_params_get_access(const snd_pcm_hw_params_t *p, snd_pcm_access_t *v) { *v=p->access; return 0; }
/** @brief 读取周期大小。 */
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v, int *d) { *v=p->period; *d=0; return 0; }
/** @brief 读取硬件缓冲大小。 */
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *v) { *v=p->buffer; return 0; }
/** @brief 分配可追踪的软件参数对象。 */
int snd_pcm_sw_params_malloc(snd_pcm_sw_params_t **params)
{ if (is_case("sw-alloc-fail")) return -ENOMEM; *params=calloc(1,sizeof(**params)); MUST(*params); ++software_params; return 0; }
/** @brief 回收软件参数对象。 */
void snd_pcm_sw_params_free(snd_pcm_sw_params_t *params) { MUST(params); free(params); --software_params; }
/** @brief 读取软件参数初值。 */
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p)
{ MUST(pcm && p); return is_case("sw-current-fail") ? -EIO : 0; }
/** @brief 保存有限等待的唤醒阈值。 */
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v)
{ MUST(pcm); p->avail=v; return is_case("avail-fail") ? -EINVAL : 0; }
/** @brief 应用软件参数，并检查阈值与周期一致。 */
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *p)
{ MUST(p->avail == pcm->hw.period); return is_case("apply-sw-fail") ? -EIO : 0; }
/** @brief 准备设备；本阶段不支持静默恢复丢样后的时间线。 */
int snd_pcm_prepare(snd_pcm_t *pcm) { MUST(pcm); return is_case("prepare-fail") ? -EIO : 0; }
/** @brief 启动录音，检查重复启动。 */
int snd_pcm_start(snd_pcm_t *pcm)
{ MUST(!pcm->started); if (is_case("start-fail")) return -EIO; pcm->started=1; return 0; }
/** @brief 停止设备，支持注入停止失败。 */
int snd_pcm_drop(snd_pcm_t *pcm)
{ MUST(pcm->started); pcm->started=0; return is_case("drop-fail") ? -EIO : 0; }
/** @brief 模拟有限等待、唤醒、超时或设备断开。 */
int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
    struct timespec delay = {.tv_sec=0, .tv_nsec=10000000};
    MUST(pcm->started && timeout >= 0 && timeout <= 1000);
    nanosleep(&delay,NULL);
    if (is_case("wait-fail")) return -ENODEV;
    if (is_case("wait-eintr")) return -EINTR;
    return is_case("timeout") ? 0 : 1;
}
/** @brief 生成交错左右声道样本，短读仍保持连续下标，可注入 XRUN/挂起等错误。 */
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *data, snd_pcm_uframes_t frames)
{
    unsigned char *bytes=data;
    struct timespec delay={.tv_sec=0,.tv_nsec=2000000};
    MUST(pcm->started && frames > 0 && frames <= pcm->hw.period);
    unsigned int call=pcm->calls++;
    if (is_case("timeout") || is_case("wait-fail")) return -EAGAIN;
    if (!call && (is_case("eagain-once") || is_case("wait-eintr"))) return -EAGAIN;
    if (!call && is_case("zero-once")) return 0;
    if (!call && is_case("eintr-once")) return -EINTR;
    if (call > 1 && is_case("xrun")) return -EPIPE;
    if (call > 1 && is_case("suspend")) return -ESTRPIPE;
    if (call > 1 && is_case("read-fail")) return -ENODEV;
    if (is_case("too-many")) return (snd_pcm_sframes_t)frames + 1;
    if (is_case("short-read") && frames > 1) frames /= 2;
    nanosleep(&delay,NULL);
    for (snd_pcm_uframes_t i=0; i<frames; ++i) {
        int value=(int)((pcm->samples+i)%30000)+1;
        for (unsigned int ch=0; ch<2; ++ch) {
            uint16_t sample=(uint16_t)(is_case("silence") ? 0 : ch ? -value : value);
            bytes[(i*2+ch)*2]=(unsigned char)sample;
            bytes[(i*2+ch)*2+1]=(unsigned char)(sample>>8);
        }
    }
    pcm->samples+=frames;
    return (snd_pcm_sframes_t)frames;
}
size_t __real_fwrite(const void *,size_t,size_t,FILE *);
int __real_fclose(FILE *);
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
/** @brief 注入部分写入故障，验证应用不会把残缺录音报告为成功。 */
size_t __wrap_fwrite(const void *data,size_t size,size_t count,FILE *file)
{
    pcm_file=file;
    if (is_case("write-fail")) { size_t result=__real_fwrite(data,size,count/2,file); errno=ENOSPC; return result; }
    return __real_fwrite(data,size,count,file);
}
/** @brief 只对音频输出注入 fclose 错误，不干扰配置文件读取。 */
int __wrap_fclose(FILE *file)
{
    int is_pcm=file == pcm_file;
    int result=__real_fclose(file);
    if (is_pcm && is_case("flush-fail")) { errno=EIO; return EOF; }
    return result;
}
/** @brief 注入消费者或生产者启动失败，检查等待线程可被唤醒并回收。 */
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{
    ++thread_calls;
    if ((thread_calls==1 && is_case("consumer-thread-fail")) || (thread_calls==2 && is_case("producer-thread-fail"))) return EAGAIN;
    return __real_pthread_create(t,a,fn,arg);
}
