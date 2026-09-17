/* timestamp.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_TIMESTAMP_H
#define IPC_TIMESTAMP_H

#include "media_types.h"

/* 待实现：获得单调时钟的微秒值；失败返回负值。 */
int64_t ipc_monotonic_us(void);
/* 待实现：sample_count 为每声道累计样本数；统一换算为微秒。
 * 时间基和采样率必须有效，溢出应返回错误，不能悄悄回绕。 */
int ipc_audio_pts_us(int64_t start_offset_us, uint64_t sample_count,
    unsigned int sample_rate, int64_t *pts_us);
int ipc_timestamp_rescale(int64_t value, IpcTimeBase source,
    IpcTimeBase target, int64_t *result);

#endif /* IPC_TIMESTAMP_H */
