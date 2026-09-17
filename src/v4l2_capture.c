/*
 * v4l2_capture.c — V4L2 视频采集
 *
 * 当前阶段：只建立模块文件和对外接口，尚未实现任何业务函数。
 * 不使用“直接返回成功”的空实现；接入 main 前必须补齐本模块定义。
 *
 * 后续实现任务：
 * 1. 复用 demo 中已验证的 VIDEO_CAPTURE_MPLANE + MMAP 流程。
 * 2. 设置并读回 1280x720 NV12，保存实际 stride、sizeimage 和时间戳。
 * 3. DQBUF 后复制到应用自有帧，再 QBUF；将原始帧交给视频队列。
 * 4. 停止采集、关闭生产队列，按逆序释放映射和设备。
 */
#include "v4l2_capture.h"

/* TODO：按头文件契约逐步实现。 */
