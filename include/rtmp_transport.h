/** @file rtmp_transport.h
 * @brief 受控 RTMP 传输子进程；隔离 BSP librtmp 内部不可中断的阻塞调用。
 */
#ifndef IPC_RTMP_TRANSPORT_H
#define IPC_RTMP_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
typedef struct IpcRtmpTransport IpcRtmpTransport;
/** @brief 启动同一可执行文件的内部传输入口，最多等待 5 秒连接；借用两项原子控制状态。
 * @param cancel true 立即中止；deadline_us 非零时为整个排空过程的单调截止时间。
 * @return 0 成功，负错误失败；失败自动回收子进程，*context 保持 NULL。
 */
int ipc_rtmp_transport_open(IpcRtmpTransport **context, const char *url,
                            const atomic_bool *cancel, const atomic_llong *deadline_us);
/** @brief 发送一块 FLV 并等底层刷新确认，整次调用最多 3 秒；仅网络输出线程调用。 */
int ipc_rtmp_transport_write(IpcRtmpTransport *context, const uint8_t *data, size_t size);
/** @brief 请求关闭网络并等待确认，最多 3 秒；不表示服务器或播放器已消费全部数据。 */
int ipc_rtmp_transport_finish(IpcRtmpTransport *context);
/** @brief 关闭 IPC，终止并 waitpid 回收本模块启动的子进程；NULL 安全。 */
void ipc_rtmp_transport_destroy(IpcRtmpTransport **context);
/** @brief main 在加载设备之前调用的内部入口；只处理继承的 Unix socket 和 RTMP URL。
 * @return 未匹配内部参数返回 -1；匹配后返回进程退出码，不再进入正式采集流程。
 */
int ipc_rtmp_transport_dispatch(int argc, char **argv);
#endif
