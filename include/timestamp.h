/* timestamp.h — 单调时钟已实现，其余换算接口保留为后续草案。 */
#ifndef IPC_TIMESTAMP_H
#define IPC_TIMESTAMP_H

#include "media_types.h"

/** @brief 获得单调时钟的微秒值；失败返回 -1。 */
int64_t ipc_monotonic_us(void);
/** @brief 按累计每声道样本数换算微秒，向下取整；错误不修改输出。
 * @return 0 成功；-EINVAL 负偏移/零采样率/空指针；-EOVERFLOW 超出 int64_t。
 */
int ipc_audio_pts_us(int64_t start_offset_us, uint64_t sample_count,
    unsigned int sample_rate, int64_t *pts_us);
/* 待实现：通用时间基换算，供后续封装模块使用。 */
int ipc_timestamp_rescale(int64_t value, IpcTimeBase source,
    IpcTimeBase target, int64_t *result);

#endif /* IPC_TIMESTAMP_H */
