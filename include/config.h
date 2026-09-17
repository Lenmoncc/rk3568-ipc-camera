/**
 * @file config.h
 * @brief 配置加载、静态校验和摘要输出；不访问设备、文件输出目录或网络。
 */
#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include "log.h"

/* 所有路径和部署参数集中管理，不在采集或输出模块中散落硬编码。 */
typedef struct {
    char video_device[128];
    unsigned int video_width, video_height;
    char video_pixel_format[8];
    char video_codec[8];
    unsigned int video_fps;       /* 目标值，不代表实测帧率。 */
    unsigned int video_bitrate;
    unsigned int video_gop;
    char audio_device[128];
    unsigned int audio_sample_rate, audio_channels;
    char audio_sample_format[16];
    char audio_codec[8];
    unsigned int audio_bitrate;
    bool rtmp_enabled;            /* false 时允许 URL 为空或明确的 SRS 占位主机。 */
    char rtmp_url[512];
    char record_path[512];
    size_t video_raw_capacity, audio_raw_capacity;
    size_t video_packet_capacity, audio_packet_capacity;
    IpcLogLevel log_level;
} IpcConfig;

/**
 * @brief 读取并校验配置；全部成功后才更新 *config。
 * @param path 配置文件路径，支持 LF、CRLF、可选 UTF-8 BOM。
 * @param config 调用者提供的配置对象，失败时保持调用前内容不变。
 * @return 0 成功，负 errno 风格值表示参数、格式、范围或 I/O 错误。
 * @note 错误日志包含文件、行号及键名。缺失键因不存在行号而只报告文件和键。
 *       每行最大 1023 字节（不含 LF），只支持整行 # 注释，不解析引号或变量。
 */
int ipc_config_load(const char *path, IpcConfig *config);

/**
 * @brief 对已初始化的配置对象执行静态校验，不探测硬件和服务器。
 * @return 0 成功，负 errno 风格值表示错误。
 * @note 同一对象不能同时被其他线程修改；从文件加载时已经自动执行此校验。
 */
int ipc_config_validate(const IpcConfig *config);

/** @brief 对已通过校验的配置输出 INFO 摘要；被禁用的 RTMP 明确标记为 false。 */
void ipc_config_dump(const IpcConfig *config);

#endif /* IPC_CONFIG_H */
