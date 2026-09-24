/** @file rtmp_transport.c
 * @brief 主进程通过有界 socket 请求/应答监督网络进程，不依赖 librtmp 响应中断回调。
 * 子进程 exec 同一二进制，只调用 libavformat 网络 AVIO；没有外部 ffmpeg 编码进程。
 */
#define _POSIX_C_SOURCE 200809L
#include "rtmp_transport.h"
#include "timestamp.h"
#include "log.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if IPC_WITH_FFMPEG
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/dict.h>
extern char **environ;
enum { TRANSPORT_FD = 3, BLOCK_MAX = 32768 };
struct IpcRtmpTransport {
    int fd;
    pid_t child;
    const atomic_bool *cancel;
    const atomic_llong *deadline_us;
};
/** @brief 在同机同二进制的私有 IPC 中收发固定字节数；父侧非阻塞并检查整个操作截止时间。 */
static int transfer(int fd, void *buffer, size_t size, bool sending,
                    const IpcRtmpTransport *owner, int64_t deadline)
{
    uint8_t *p = buffer;
    while (size) {
        if (owner) {
            int64_t now = ipc_monotonic_us();
            int64_t drain = atomic_load(owner->deadline_us);
            if (atomic_load(owner->cancel)) return -ECANCELED;
            if (now < 0) return -EIO;
            if (now >= deadline || (drain && now >= drain)) return -ETIMEDOUT;
        }
        ssize_t n = sending ? send(fd,p,size,MSG_NOSIGNAL) : recv(fd,p,size,0);
        if (n > 0) { p += n; size -= (size_t)n; continue; }
        if (!n) return -EPIPE;
        if (errno == EINTR) continue;
        if (owner && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd item = {.fd=fd,.events=sending ? POLLOUT : POLLIN};
            int result = poll(&item,1,50);
            if (result < 0 && errno != EINTR) return -errno;
            continue;
        }
        return -errno;
    }
    return 0;
}
/** @brief 同一截止时间内读子进程应答；应答是底层 FFmpeg 返回值，不使用墙上时间。 */
static int response(IpcRtmpTransport *t, int64_t deadline)
{
    int32_t status = 0;
    int result = transfer(t->fd,&status,sizeof(status),false,t,deadline);
    return result < 0 ? result : status;
}
/** @brief 回收唯一受管子进程；不取消线程，也不对其他进程发送信号。 */
void ipc_rtmp_transport_destroy(IpcRtmpTransport **context)
{
    if (!context || !*context) return;
    IpcRtmpTransport *t = *context; *context = NULL;
    if (t->fd >= 0) close(t->fd);
    if (t->child > 0) {
        int status;
        pid_t result;
        do { result = waitpid(t->child,&status,WNOHANG); } while (result < 0 && errno == EINTR);
        if (!result) {
            /* 网络库可能卡在连接/写入/关闭内，强制结束只影响专属子进程。 */
            kill(t->child,SIGKILL);
            do { result = waitpid(t->child,&status,0); } while (result < 0 && errno == EINTR);
        }
    }
    free(t);
}
/** @brief 用 posix_spawn+exec 启动网络隔离进程，CLOEXEC 防止采集/文件描述符被继承。 */
int ipc_rtmp_transport_open(IpcRtmpTransport **context, const char *url,
                            const atomic_bool *cancel, const atomic_llong *deadline_us)
{
    if (!context || *context || !url || strncmp(url,"rtmp://",7) || !cancel || !deadline_us) return -EINVAL;
    IpcRtmpTransport *t = calloc(1,sizeof(*t));
    if (!t) return -ENOMEM;
    t->fd = -1; t->cancel = cancel; t->deadline_us = deadline_us;
    int pair[2], result;
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair) < 0) { free(t); return -errno; }
    t->fd = pair[0];
    if (fcntl(t->fd,F_SETFL,O_NONBLOCK) < 0) { result = -errno; close(pair[1]); goto fail; }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    result = posix_spawn_file_actions_init(&actions);
    if (result) { result = -result; close(pair[1]); goto fail; }
    result = posix_spawnattr_init(&attr);
    if (result) { result = -result; posix_spawn_file_actions_destroy(&actions); close(pair[1]); goto fail; }
    sigset_t mask, defaults;
    sigemptyset(&mask); sigemptyset(&defaults);
    sigaddset(&defaults,SIGTERM); sigaddset(&defaults,SIGINT); sigaddset(&defaults,SIGPIPE);
    char parent[32]; snprintf(parent,sizeof(parent),"%ld",(long)getpid());
    char *args[] = {"ipc_camera","--internal-rtmp-transport","3",(char *)url,parent,NULL};
    result = posix_spawn_file_actions_addclose(&actions,pair[0]);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions,pair[1],TRANSPORT_FD);
    if (!result && pair[1] != TRANSPORT_FD) result = posix_spawn_file_actions_addclose(&actions,pair[1]);
    if (!result) result = posix_spawnattr_setsigmask(&attr,&mask);
    if (!result) result = posix_spawnattr_setsigdefault(&attr,&defaults);
    /* 独立进程组防止终端 Ctrl+C 先杀网络子进程；主进程统一安排排空/超时终止。 */
    if (!result) result = posix_spawnattr_setpgroup(&attr,0);
    if (!result) result = posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_SETPGROUP);
    if (!result) result = posix_spawn(&t->child,"/proc/self/exe",&actions,&attr,args,environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr); close(pair[1]);
    if (result) { result = -result; goto fail; }
    result = response(t,ipc_monotonic_us()+5000000);
    if (result < 0) goto fail;
    *context = t; return 0;
fail:
    ipc_rtmp_transport_destroy(&t); return result;
}
/** @brief 以一个有界块发送 FLV，写入和应答共用 3 秒期限，短写不会重置期限。 */
int ipc_rtmp_transport_write(IpcRtmpTransport *t, const uint8_t *data, size_t size)
{
    if (!t || !data || !size || size > BLOCK_MAX) return -EINVAL;
    uint32_t count = (uint32_t)size;
    int64_t deadline = ipc_monotonic_us()+3000000;
    int result = transfer(t->fd,&count,sizeof(count),true,t,deadline);
    if (!result) result = transfer(t->fd,(void *)data,size,true,t,deadline);
    if (!result) result = response(t,deadline);
    return result < 0 ? result : (int)size;
}
/** @brief 零长度块代表关闭，不让网络库的 close 阻塞主进程退出。 */
int ipc_rtmp_transport_finish(IpcRtmpTransport *t)
{
    if (!t) return -EINVAL;
    uint32_t count = 0;
    int64_t deadline = ipc_monotonic_us()+3000000;
    int result = transfer(t->fd,&count,sizeof(count),true,t,deadline);
    return result < 0 ? result : response(t,deadline);
}
/** @brief 子进程打开 RTMP 并逐块刷新；无条件短写/失败应答，父进程负责硬截止时间。 */
static int transport_child(const char *url)
{
    AVIOContext *io = NULL;
    AVDictionary *options = NULL;
    uint8_t data[BLOCK_MAX];
    int32_t status = avformat_network_init();
    if (status >= 0) status = av_dict_set(&options,"rw_timeout","3000000",0);
    if (status >= 0) status = avio_open2(&io,url,AVIO_FLAG_WRITE,NULL,&options);
    av_dict_free(&options);
    if (status < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(status,error,sizeof(error));
        ipc_log_write(IPC_LOG_ERROR,"rtmp-transport","connect: %s",error);
    }
    int result = transfer(TRANSPORT_FD,&status,sizeof(status),true,NULL,0);
    while (!result && status >= 0) {
        uint32_t count;
        result = transfer(TRANSPORT_FD,&count,sizeof(count),false,NULL,0);
        if (result < 0) break;
        if (!count) {
            status = avio_closep(&io);
            result = transfer(TRANSPORT_FD,&status,sizeof(status),true,NULL,0);
            break;
        }
        if (count > sizeof(data)) { status = AVERROR(EINVAL); break; }
        result = transfer(TRANSPORT_FD,data,count,false,NULL,0);
        if (result < 0) break;
        avio_write(io,data,(int)count); avio_flush(io);
        status = io->error < 0 ? io->error : 0;
        result = transfer(TRANSPORT_FD,&status,sizeof(status),true,NULL,0);
    }
    if (io) avio_closep(&io);
    avformat_network_deinit(); close(TRANSPORT_FD);
    return result < 0 || status < 0 ? 1 : 0;
}
/** @brief exec 后单线程关闭非 IPC 描述符，防止第三方硬件库未设置 CLOEXEC 导致设备被子进程持有。 */
static int close_inherited_fds(void)
{
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -errno;
    int scan_fd = dirfd(directory);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        long fd = strtol(entry->d_name,&end,10);
        if (*entry->d_name && !*end && fd > TRANSPORT_FD && fd != scan_fd) close((int)fd);
    }
    closedir(directory); return 0;
}
/** @brief 只在内部 exec 参数完整、描述符为 Unix socket 时运行；父进程消失则由内核终止。 */
int ipc_rtmp_transport_dispatch(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1],"--internal-rtmp-transport")) return -1;
    if (argc != 5 || strcmp(argv[2],"3") || strncmp(argv[3],"rtmp://",7)) return 2;
    char *end;
    errno = 0; long parent = strtol(argv[4],&end,10);
    if (errno || *end || parent <= 1) return 2;
    struct sockaddr address; socklen_t length = sizeof(address);
    if (getsockname(TRANSPORT_FD,&address,&length) < 0 || address.sa_family != AF_UNIX) return 2;
    if (prctl(PR_SET_PDEATHSIG,SIGKILL) < 0 || (long)getppid() != parent) return 1;
    struct sigaction ignore = {.sa_handler=SIG_IGN}; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE,&ignore,NULL) < 0) return 1;
    if (close_inherited_fds() < 0) return 1;
    return transport_child(argv[3]);
}
#else
/** @brief 无 FFmpeg 时没有网络传输，拒绝打开。 */
int ipc_rtmp_transport_open(IpcRtmpTransport **t,const char *u,const atomic_bool *c,const atomic_llong *d)
{(void)t;(void)u;(void)c;(void)d;return -ENOTSUP;}
/** @brief 无 FFmpeg 时拒绝网络写入。 */
int ipc_rtmp_transport_write(IpcRtmpTransport *t,const uint8_t *d,size_t s){(void)t;(void)d;(void)s;return -ENOTSUP;}
/** @brief 无 FFmpeg 时拒绝网络收尾。 */
int ipc_rtmp_transport_finish(IpcRtmpTransport *t){(void)t;return -ENOTSUP;}
/** @brief 无 FFmpeg 时允许空资源清理。 */
void ipc_rtmp_transport_destroy(IpcRtmpTransport **t){if(t)*t=NULL;}
/** @brief 无 FFmpeg 时拒绝内部网络入口，其他参数交给普通命令行。 */
int ipc_rtmp_transport_dispatch(int n,char **v){return n>1&&!strcmp(v[1],"--internal-rtmp-transport")?2:-1;}
#endif
