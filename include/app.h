/* app.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_APP_H
#define IPC_APP_H

/* RTMP/双输出阶段的应用上下文预留类型。
 * 当前本地录像由 record_pipeline.c 的私有 RecordPipeline 管理五个工作线程、
 * 两个原始队列和两个编码包队列；不将尚未接入的 RTMP 状态混入现有运行入口。
 * 后续复用 AVPacket 独立引用接入第二路输出，禁止共享可变包元数据。 */
typedef struct IpcApp IpcApp;

#endif /* IPC_APP_H */
