/* config.h — 初版模块接口草案；业务函数尚未实现。 */
#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include <stddef.h>

/* 所有路径和部署参数集中管理，不在采集或输出模块中散落硬编码。 */
typedef struct {
    char video_device[128];
    unsigned int video_width, video_height;
    unsigned int video_fps;       /* 目标值，不代表实测帧率。 */
    unsigned int video_bitrate;
    unsigned int video_gop;
    char audio_device[128];
    unsigned int audio_sample_rate, audio_channels;
    unsigned int audio_bitrate;
    char rtmp_url[512];
    char record_path[512];
    size_t video_raw_capacity, audio_raw_capacity;
    size_t video_packet_capacity, audio_packet_capacity;
} IpcConfig;

/* 待实现：load 读取键值配置；validate 拒绝未填写的必要参数。
 * 返回 0 表示成功，负值表示错误；本阶段只有声明，没有成功占位实现。 */
int ipc_config_load(const char *path, IpcConfig *config);
int ipc_config_validate(const IpcConfig *config);
void ipc_config_dump(const IpcConfig *config);

#endif /* IPC_CONFIG_H */
