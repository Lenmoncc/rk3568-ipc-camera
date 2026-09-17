/*
 * audio_encoder.c — PCM 转换与 AAC 编码
 *
 * 当前阶段：只建立模块文件和对外接口，尚未实现任何业务函数。
 * 不使用“直接返回成功”的空实现；接入 main 前必须补齐本模块定义。
 *
 * 后续实现任务：
 * 1. 使用 swresample 将交错 S16_LE 转为 AAC 接受的 FLTP。
 * 2. 用采样缓冲重新分帧，不能假设 ALSA 一次读取等于一个 AAC 帧。
 * 3. 处理 send/receive 的多包和 EAGAIN 语义；分发到两路音频包队列。
 * 4. 输入结束后处理残留样本、重采样器和编码器延迟，再关闭包队列。
 */
#include "audio_encoder.h"

/* TODO：按头文件契约逐步实现。 */
