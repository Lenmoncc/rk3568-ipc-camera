/** @file mock_rtmp_io.c
 * @brief 仅主机测试链接：替换网络子进程的 AVIO，模拟不响应任何中断回调的 librtmp。
 * 正式 FLV 封装、IPC、监督期限、并行编码/MP4 和资源清理均不替换。
 */
#define _POSIX_C_SOURCE 200809L
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int output_fd = -1;
static unsigned int writes;
/** @brief 匹配唯一网络故障场景，由测试驱动通过环境变量选择。 */
static bool scenario(const char *name)
{const char *s=getenv("IPC_RTMP_CASE");return s && !strcmp(s,name);}
/** @brief 模拟不理会 FFmpeg interrupt_callback 的无限网络阻塞，由父进程强制结束。 */
static void hang_forever(void) {for(;;) pause();}
/** @brief 将真实 FLV 保存到测试临时文件，或模拟网络写失败/长期阻塞/缓慢读取。 */
static int mock_write(void *opaque,uint8_t *data,int count)
{
    (void)opaque;++writes;
    if(scenario("hang-write") && writes>1)hang_forever();
    if(scenario("header-fail") || (scenario("write-fail")&&writes>4))return AVERROR(EPIPE);
    if(scenario("slow-write") && writes>1){struct timespec delay={0,300000000};nanosleep(&delay,NULL);}
    int left=count;
    while(left){ssize_t n=write(output_fd,data,(size_t)left);if(n<0){if(errno==EINTR)continue;return AVERROR(errno);}assert(n>0);data+=n;left-=(int)n;}
    return count;
}
extern int __real_avio_open2(AVIOContext **,const char *,int,const AVIOInterruptCB *,AVDictionary **);
/** @brief 模拟连接返回、拒绝或永久挂起；real 模式保留真实 RTMP/TCP 协议栈。 */
int __wrap_avio_open2(AVIOContext **io,const char *url,int flags,const AVIOInterruptCB *cb,AVDictionary **options)
{
    const char *pid_path=getenv("IPC_RTMP_PID_FILE");
    if(pid_path){FILE *f=fopen(pid_path,"w");assert(f);fprintf(f,"%ld\n",(long)getpid());fclose(f);}
    if(scenario("real"))return __real_avio_open2(io,url,flags,cb,options);
    assert(!strncmp(url,"rtmp://",7));
    if(scenario("hang-open"))hang_forever();
    if(scenario("open-fail"))return AVERROR(ECONNREFUSED);
    const char *path=getenv("IPC_RTMP_FLV");assert(path);
    output_fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(output_fd>=0);
    uint8_t *buffer=av_malloc(32768);assert(buffer);
    *io=avio_alloc_context(buffer,32768,1,NULL,NULL,mock_write,NULL);assert(*io);return 0;
}
extern int __real_avio_closep(AVIOContext **);
/** @brief 模拟网络关闭卡住或返回错误；成功时释放 AVIO 与文件句柄。 */
int __wrap_avio_closep(AVIOContext **io)
{
    if(scenario("real"))return __real_avio_closep(io);
    if(scenario("hang-close"))hang_forever();
    if(*io){av_freep(&(*io)->buffer);avio_context_free(io);}
    if(output_fd>=0){close(output_fd);output_fd=-1;}
    return scenario("close-fail")?AVERROR(EIO):0;
}
