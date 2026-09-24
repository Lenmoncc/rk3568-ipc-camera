/** @file app.h
 * @brief 早期公共上下文的预留声明；实际生命周期由 record_pipeline.c 私有上下文管理。
 */
#ifndef IPC_APP_H
#define IPC_APP_H
/* 当前最多六个业务线程：双采集、双编码、MP4、RTMP；每个输出各有两条编码包队列。
 * RTMP 网络库在受监督的 exec 子进程运行，避免阻塞库调用拖住本地录像。
 * 不向 demo/ 暴露或借用正式上下文，AVPacket 元数据也不跨输出共享修改。 */
typedef struct IpcApp IpcApp;
#endif /* IPC_APP_H */
